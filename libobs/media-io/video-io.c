/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <assert.h>
#include "../util/bmem.h"
#include "../util/platform.h"
#include "../util/profiler.h"
#include "../util/threading.h"
#include "../util/darray.h"

#include "format-conversion.h"
#include "video-io.h"
#include "video-frame.h"
#include "video-scaler.h"

extern profiler_name_store_t *obs_get_profiler_name_store(void);

#define MAX_CACHE_SIZE 16

struct video_data_container {
	volatile long refs;

	struct video_data data;
};

struct cached_video_data {
	struct video_data_container *container;
	bool expiring;
};

struct track_duplicated_frame {
	int count;
	video_tracked_frame_id id;
};

struct cached_frame_info {
	DARRAY(struct cached_video_data) frames;
	uint32_t frames_written;
	int count;
	uint64_t timestamp;
	video_tracked_frame_id tracked_id;
	DARRAY(struct track_duplicated_frame) tracked_ids;
};

struct video_input {
	DARRAY(struct video_scale_info) info;

	void (*callback)(void *param, struct video_data_container *container);
	void *param;
};

static inline void video_input_free(struct video_input *input)
{
	da_free(input->info);
}

struct video_output {
	struct video_output_info   info;

	pthread_t                  thread;
	pthread_mutex_t            data_mutex;
	bool                       stop;

	os_sem_t                   *update_semaphore;
	uint64_t                   frame_time;
	uint32_t                   skipped_frames;
	uint32_t                   total_frames;

	bool                       initialized;

	pthread_mutex_t            input_mutex;
	DARRAY(struct video_input) inputs;
	video_scale_info_ts        maybe_expired_scale_info;

	pthread_mutex_t            scale_info_mutex;
	video_scale_info_ts        new_scale_info;
	video_scale_info_ts        expiring_scale_info;
	video_scale_info_ts        removed_scale_info;

	DARRAY(struct video_conversion_info) infos;

	size_t                     available_frames;
	size_t                     first_added;
	size_t                     last_added;
	struct cached_frame_info   cache[MAX_CACHE_SIZE];
};

/* ------------------------------------------------------------------------- */

static struct cached_video_data *find_frame(struct cached_frame_info *cfi, struct video_scale_info *info)
{
	for (size_t i = 0; i < cfi->frames.num; i++) {
		if (!info->gpu_conversion && !cfi->frames.array[i].container->data.info.gpu_conversion)
			return cfi->frames.array + i;
		if (memcmp(&cfi->frames.array[i].container->data.info, info, sizeof(*info)) == 0)
			return cfi->frames.array + i;
	}

	return NULL;
}

static inline bool video_output_cur_frame(struct video_output *video)
{
	struct cached_frame_info *frame_info;
	bool complete;
	bool tracked_frame = false;
	video_tracked_frame_id tracked_id;

	/* -------------------------------- */

	pthread_mutex_lock(&video->data_mutex);

	frame_info = &video->cache[video->first_added];

	for (size_t i = 0; i < frame_info->tracked_ids.num; i++)
		if (--frame_info->tracked_ids.array[i].count == 0) {
			tracked_frame = true;
			tracked_id = frame_info->tracked_ids.array[i].id;
		}

	pthread_mutex_unlock(&video->data_mutex);

	/* -------------------------------- */

	pthread_mutex_lock(&video->input_mutex);

	for (size_t i = 0; i < video->inputs.num; i++) {
		struct video_input *input = video->inputs.array+i;

		struct cached_video_data *frame = NULL;
		for (size_t j = 0; j < input->info.num; j++) {
			if (!(frame = find_frame(frame_info, input->info.array + j)))
				continue;

			if (!frame->expiring && input->info.num > 1 && ((input->info.num - j) > 1)) {
				da_push_back_array(video->maybe_expired_scale_info, input->info.array + j + 1, input->info.num - j - 1);
				da_resize(input->info, j + 1);
			}
			break;
		}

		if (!frame)
			continue;

		if (tracked_frame) {
			frame->container->data.tracked_id = tracked_id;
			blog(LOG_INFO, "video-io: Outputting (duplicated) tracked frame %lld", tracked_id);
		}

		input->callback(input->param, frame->container);
	}

	for (size_t i = 0; i < video->maybe_expired_scale_info.num;) {
		struct video_scale_info *info = video->maybe_expired_scale_info.array + i;

		bool found = false;
		for (size_t j = 0; j < video->inputs.num; j++) {
			struct video_input *input = video->inputs.array + j;
			if (da_find(input->info, info, 0) != DARRAY_INVALID) {
				da_erase(video->maybe_expired_scale_info, i);
				found = true;
				break;
			}
		}
		if (found)
			continue;

		i += 1;
	}

	if (video->maybe_expired_scale_info.num) {
		pthread_mutex_lock(&video->scale_info_mutex);
		da_push_back_da(video->removed_scale_info, video->maybe_expired_scale_info);
		pthread_mutex_unlock(&video->scale_info_mutex);
		da_resize(video->maybe_expired_scale_info, 0);
	}

	pthread_mutex_unlock(&video->input_mutex);

	/* -------------------------------- */

	pthread_mutex_lock(&video->data_mutex);

	if (tracked_frame) {
		for (size_t i = 0; i < frame_info->tracked_ids.num; i++) {
			if (frame_info->tracked_ids.array[i].id == tracked_id) {
				da_erase(frame_info->tracked_ids, i);
				break;
			}
		}
	}

	complete = --frame_info->count == 0;

	if (complete) {
		if (++video->first_added == video->info.cache_size)
			video->first_added = 0;

		if (++video->available_frames == video->info.cache_size)
			video->last_added = video->first_added;

	} else {
		for (size_t i = 0; i < frame_info->frames.num; i++) {
			struct cached_video_data *frame = frame_info->frames.array + i;
			frame->container->data.timestamp += video->frame_time;
			frame->container->data.tracked_id = 0;
		}
		++video->skipped_frames;
	}

	pthread_mutex_unlock(&video->data_mutex);

	/* -------------------------------- */

	return complete;
}

static void *video_thread(void *param)
{
	struct video_output *video = param;

	os_set_thread_name("video-io: video thread");

	const char *video_thread_name =
		profile_store_name(obs_get_profiler_name_store(),
				"video_thread(%s)", video->info.name);

	while (os_sem_wait(video->update_semaphore) == 0) {
		if (video->stop)
			break;

		profile_start(video_thread_name);
		while (!video->stop && !video_output_cur_frame(video)) {
			video->total_frames++;
		}

		video->total_frames++;
		profile_end(video_thread_name);

		profile_reenable_thread();
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */

static inline bool valid_video_params(const struct video_output_info *info)
{
	return info->fps_den != 0 && info->fps_num != 0;
}

static inline void init_cache(struct video_output *video)
{
	if (video->info.cache_size > MAX_CACHE_SIZE)
		video->info.cache_size = MAX_CACHE_SIZE;

	for (size_t i = 0; i < video->info.cache_size; i++) {
		da_init(video->cache[i].frames);
		da_init(video->cache[i].tracked_ids);
	}

	video->available_frames = video->info.cache_size;
}

int video_output_open(video_t **video, struct video_output_info *info)
{
	struct video_output *out;
	pthread_mutexattr_t attr;

	if (!valid_video_params(info))
		return VIDEO_OUTPUT_INVALIDPARAM;

	out = bzalloc(sizeof(struct video_output));
	if (!out)
		goto fail;

	memcpy(&out->info, info, sizeof(struct video_output_info));
	out->frame_time = (uint64_t)(1000000000.0 * (double)info->fps_den /
		(double)info->fps_num);
	out->initialized = false;

	if (pthread_mutexattr_init(&attr) != 0)
		goto fail;
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
		goto fail;
	if (pthread_mutex_init(&out->data_mutex, &attr) != 0)
		goto fail;
	if (pthread_mutex_init(&out->input_mutex, &attr) != 0)
		goto fail;
	if (pthread_mutex_init(&out->scale_info_mutex, &attr) != 0)
		goto fail;
	if (os_sem_init(&out->update_semaphore, 0) != 0)
		goto fail;
	if (pthread_create(&out->thread, NULL, video_thread, out) != 0)
		goto fail;

	init_cache(out);

	out->initialized = true;
	*video = out;
	return VIDEO_OUTPUT_SUCCESS;

fail:
	video_output_close(out);
	return VIDEO_OUTPUT_FAIL;
}

void video_output_close(video_t *video)
{
	if (!video)
		return;

	video_output_stop(video);

	for (size_t i = 0; i < video->inputs.num; i++)
		video_input_free(&video->inputs.array[i]);
	da_free(video->inputs);

	for (size_t i = 0; i < video->info.cache_size; i++) {
		struct cached_frame_info *cfi = video->cache + i;
		for (size_t j = 0; j < cfi->frames.num; j++)
			video_data_container_release(cfi->frames.array[j].container);
		da_free(video->cache[i].frames);
		da_free(video->cache[i].tracked_ids);
	}

	os_sem_destroy(video->update_semaphore);
	pthread_mutex_destroy(&video->data_mutex);
	pthread_mutex_destroy(&video->input_mutex);
	pthread_mutex_destroy(&video->scale_info_mutex);
	bfree(video);
}

static size_t video_get_input_idx(const video_t *video,
		video_data_callback callback,
		void *param)
{
	for (size_t i = 0; i < video->inputs.num; i++) {
		struct video_input *input = video->inputs.array+i;
		if (input->callback == callback && input->param == param)
			return i;
	}

	return DARRAY_INVALID;
}

static inline bool video_input_init(struct video_input *input,
		struct video_output *video)
{
#if 0
	if (input->conversion.width  != video->info.width ||
	    input->conversion.height != video->info.height ||
	    input->conversion.format != video->info.format) {
		struct video_scale_info from = {
			.format = video->info.format,
			.width  = video->info.width,
			.height = video->info.height,
			.range  = video->info.range,
			.colorspace = video->info.colorspace,
		};

		int ret = video_scaler_create(&input->scaler,
				&input->conversion, &from,
				VIDEO_SCALE_FAST_BILINEAR);
		if (ret != VIDEO_SCALER_SUCCESS) {
			if (ret == VIDEO_SCALER_BAD_CONVERSION)
				blog(LOG_ERROR, "video_input_init: Bad "
				                "scale conversion type");
			else
				blog(LOG_ERROR, "video_input_init: Failed to "
				                "create scaler");

			return false;
		}

		for (size_t i = 0; i < MAX_CONVERT_BUFFERS; i++)
			video_frame_init(&input->frame[i],
					input->conversion.format,
					input->conversion.width,
					input->conversion.height, true);
	}
#endif

	return true;
}

bool video_output_connect(video_t *video,
		const struct video_scale_info *info,
		video_data_callback callback,
		void *param)
{
	bool success = false;

	if (!video || !callback)
		return false;

	if (!info || !info->width || !info->height)
		return false;

	assert(info->gpu_conversion == true);

	pthread_mutex_lock(&video->input_mutex);

	if (video->inputs.num == 0) {
		video->skipped_frames = 0;
		video->total_frames = 0;
	}

	if (video_get_input_idx(video, callback, param) == DARRAY_INVALID) {
		struct video_input input;
		memset(&input, 0, sizeof(input));

		input.callback = callback;
		input.param    = param;
		da_push_back(input.info, info);

		success = video_input_init(&input, video);
		if (success) {
			pthread_mutex_lock(&video->scale_info_mutex);

			bool found = false;
			for (size_t i = 0; i < video->inputs.num; i++) {
				struct video_input *other = video->inputs.array + i;
				if (da_find(other->info, info, 0) != DARRAY_INVALID) {
					found = true;
					break;
				}
			}

			if (!found)
				da_push_back(video->new_scale_info, info);

			pthread_mutex_unlock(&video->scale_info_mutex);

			da_push_back(video->inputs, &input);
		}
	}

	pthread_mutex_unlock(&video->input_mutex);

	return success;
}

void video_output_disconnect(video_t *video,
		video_data_callback callback,
		void *param)
{
	if (!video || !callback)
		return;

	pthread_mutex_lock(&video->input_mutex);

	size_t idx = video_get_input_idx(video, callback, param);
	if (idx != DARRAY_INVALID) {
		struct video_input input = video->inputs.array[idx];
		da_erase(video->inputs, idx);

		video_scale_info_ts removed = { 0 };
		for (size_t j = 0; j < input.info.num; j++) {
			bool found = false;
			for (size_t i = 0; i < video->inputs.num; i++) {
				if (da_find(video->inputs.array[i].info, &input.info.array[j], 0) == DARRAY_INVALID)
					continue;

				found = true;
				break;
			}

			if (!found)
				da_push_back(removed, &input.info.array[j]);
		}
		
		if (removed.num) {
			pthread_mutex_lock(&video->scale_info_mutex);
			da_push_back_da(video->removed_scale_info, removed);
			pthread_mutex_unlock(&video->scale_info_mutex);
		}

		da_free(removed);

		video_input_free(&input);
	}

	pthread_mutex_unlock(&video->input_mutex);
}

bool video_output_update(video_t *video,
	const struct video_scale_info *info,
	video_data_callback callback,
	void *param)
{
	bool success = false;

	if (!video || !callback)
		return false;

	if (!info || !info->width || !info->height)
		return false;

	assert(info->gpu_conversion == true);

	pthread_mutex_lock(&video->input_mutex);

	size_t idx = video_get_input_idx(video, callback, param);
	if (idx != DARRAY_INVALID) {
		struct video_input *input = &video->inputs.array[idx];
		struct video_scale_info *old = &input->info.array[0];

		size_t info_idx = da_find(input->info, info, 0);
		if (info_idx == DARRAY_INVALID)
			da_insert(input->info, 0, info);
		else if (info_idx > 0)
			da_move_item(input->info, info_idx, 0);

		if (info_idx != 0) {
			bool found_new = false;
			bool found_old = false;
			for (size_t i = 0; i < video->inputs.num; i++) {
				if (i == idx)
					continue;

				struct video_input *other = video->inputs.array + i;
				if (!found_new && memcmp(other->info.array, info, sizeof(*info)) == 0)
					found_new = true;
				if (!found_old && memcmp(other->info.array, old, sizeof(*info)) == 0)
					found_old = true;
				if (found_new && found_old)
					break;
			}

			if (!found_new || !found_old) {
				pthread_mutex_lock(&video->scale_info_mutex);
				if (!found_new)
					da_push_back(video->new_scale_info, info);
				if (!found_old)
					da_push_back(video->expiring_scale_info, old);
				pthread_mutex_unlock(&video->scale_info_mutex);
			}
		}

		success = true;
	}

	pthread_mutex_unlock(&video->input_mutex);

	return success;
}

bool video_output_active(const video_t *video)
{
	if (!video) return false;
	return video->inputs.num != 0;
}

const struct video_output_info *video_output_get_info(const video_t *video)
{
	return video ? &video->info : NULL;
}

static void alloc_frame(struct video_data *data)
{
	struct video_frame *frame = (struct video_frame*)data;

	video_frame_init(frame, data->info.format,
		data->info.width, data->info.height, false);
}

video_locked_frame video_output_lock_frame(video_t *video,
		size_t num_buffers_hint,
		int count, uint64_t timestamp, video_tracked_frame_id tracked_id)
{
	struct cached_frame_info *cfi = NULL;

	if (!video) return cfi;

	pthread_mutex_lock(&video->data_mutex);

	if (video->available_frames == 0) {
		video->cache[video->last_added].count += count;

		if (tracked_id) {
			struct track_duplicated_frame track = {
				video->cache[video->last_added].count,
				tracked_id
			};
			da_push_back(video->cache[video->last_added].tracked_ids, &track);
			blog(LOG_INFO, "video-io: Tracked frame %lld will be duplicated", tracked_id);
		}

	} else {
		if (video->available_frames != video->info.cache_size) {
			if (++video->last_added == video->info.cache_size)
				video->last_added = 0;
		}

		cfi = &video->cache[video->last_added];
		cfi->count = count;
		cfi->timestamp = timestamp;
		cfi->tracked_id = tracked_id;
		cfi->frames_written = 0;

		da_reserve(cfi->frames, num_buffers_hint);

		da_resize(cfi->tracked_ids, 0);
	}

	pthread_mutex_unlock(&video->data_mutex);

	return cfi;
}

static struct video_data_container *get_container(video_t *video, struct video_scale_info *info)
{
	struct video_data_container *container = bzalloc(sizeof(*container));

	container->data.info = *info;
	alloc_frame(&container->data);

	return container;
}

bool video_output_get_frame_buffer(video_t *video,
	struct video_frame *frame, struct video_scale_info *info, video_locked_frame locked, bool expiring)
{
	if (!locked || !info) return false;

	struct cached_frame_info *cfi = locked;

	struct cached_video_data *data = NULL;
	for (size_t i = 0; i < cfi->frames.num; i++) {
		if (memcmp(info, &cfi->frames.array[i].container->data.info, sizeof(*info)) != 0)
			continue;

		cfi->frames_written |= 1 << i;
		data = cfi->frames.array + i;
		if (data->container->refs > 0) {
			video_data_container_release(data->container);
			data->container = NULL;
		}
	}

	if (!data) {
		cfi->frames_written |= 1 << cfi->frames.num;
		data = da_push_back_new(cfi->frames);
	}

	if (!data->container)
		data->container = get_container(video, info);

	data->container->data.timestamp = cfi->timestamp;
	data->container->data.tracked_id = cfi->tracked_id;
	data->expiring = expiring;

	memcpy(frame, &data->container->data, sizeof(*frame));
	return true;
}

void video_output_unlock_frame(video_t *video, video_locked_frame locked)
{
	if (!video || !locked) return;

	struct cached_frame_info *cfi = locked;

	for (size_t i = 0, frame_num = 0; i < cfi->frames.num; frame_num++) {
		if (cfi->frames_written & (1 << frame_num)) {
			i++;
			continue;
		}

		video_data_container_release(cfi->frames.array[i].container);
		da_erase(cfi->frames, i);
	}

	pthread_mutex_lock(&video->data_mutex);

	video->available_frames--;
	os_sem_post(video->update_semaphore);

	pthread_mutex_unlock(&video->data_mutex);
}

uint64_t video_output_get_frame_time(const video_t *video)
{
	return video ? video->frame_time : 0;
}

void video_output_stop(video_t *video)
{
	void *thread_ret;

	if (!video)
		return;

	if (video->initialized) {
		video->initialized = false;
		video->stop = true;
		os_sem_post(video->update_semaphore);
		pthread_join(video->thread, &thread_ret);
	}
}

bool video_output_stopped(video_t *video)
{
	if (!video)
		return true;

	return video->stop;
}

double video_output_get_frame_rate(const video_t *video)
{
	if (!video)
		return 0.0;

	return (double)video->info.fps_num / (double)video->info.fps_den;
}

uint32_t video_output_get_skipped_frames(const video_t *video)
{
	return video->skipped_frames;
}

uint32_t video_output_get_total_frames(const video_t *video)
{
	return video->total_frames;
}

bool video_output_get_changes(video_t *video, video_scale_info_ts *added,
		video_scale_info_ts *expiring, video_scale_info_ts *removed)
{
	if (!video)
		return false;

	if (!added || !removed)
		return false;

	pthread_mutex_lock(&video->scale_info_mutex);

	if (added)
		da_move((*added), video->new_scale_info);
	if (expiring)
		da_move((*expiring), video->expiring_scale_info);
	if (removed)
		da_move((*removed), video->removed_scale_info);

	pthread_mutex_unlock(&video->scale_info_mutex);

	for (size_t i = 0; i < added->num; i++)
		da_erase_item((*expiring), added->array + i);

	return true;
}

/* ------------------------------------------------------------------------- */

struct video_data *video_data_from_container(struct video_data_container *container)
{
	return container ? &container->data : NULL;
}

void video_data_container_addref(struct video_data_container *container)
{
	if (container)
		os_atomic_inc_long(&container->refs);
}

void video_data_container_release(struct video_data_container *container)
{
	if (!container)
		return;

	if (os_atomic_dec_long(&container->refs) != -1)
		return;

	video_frame_free((struct video_frame*)&container->data);
	bfree(container);
}

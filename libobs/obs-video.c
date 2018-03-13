/******************************************************************************
    Copyright (C) 2013-2014 by Hugh Bailey <obs.jim@gmail.com>

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

#include "obs.h"
#include "obs-internal.h"
#include "graphics/matrix4.h"
#include "graphics/vec4.h"
#include "media-io/format-conversion.h"
#include "media-io/video-frame.h"

static inline void calculate_base_volume(struct obs_core_data *data,
		struct obs_view *view, obs_source_t *target)
{
	if (!target->activate_refs) {
		target->base_volume = 0.0f;

	/* only walk the tree if there are transitions active */
	} else if (data->active_transitions) {
		float best_vol = 0.0f;

		for (size_t i = 0; i < MAX_CHANNELS; i++) {
			struct obs_source *source = view->channels[i];
			float vol = 0.0f;

			if (!source)
				continue;

			vol = obs_source_get_target_volume(source, target);
			if (best_vol < vol)
				best_vol = vol;
		}

		target->base_volume = best_vol;

	} else {
		target->base_volume = 1.0f;
	}
}

static uint64_t tick_sources(uint64_t cur_time, uint64_t last_time)
{
	struct obs_core_data *data = &obs->data;
	struct obs_view      *view = &data->main_view;
	struct obs_source    *source;
	uint64_t             delta_time;
	float                seconds;

	if (!last_time)
		last_time = cur_time -
			video_output_get_frame_time(obs->video.video);

	delta_time = cur_time - last_time;
	seconds = (float)((double)delta_time / 1000000000.0);

	pthread_mutex_lock(&data->sources_mutex);

	/* call the tick function of each source */
	source = data->first_source;
	while (source) {
		obs_source_video_tick(source, seconds);
		source = (struct obs_source*)source->context.next;
	}

	/* calculate source volumes */
	pthread_mutex_lock(&view->channels_mutex);

	source = data->first_source;
	while (source) {
		calculate_base_volume(data, view, source);
		source = (struct obs_source*)source->context.next;
	}

	pthread_mutex_unlock(&view->channels_mutex);

	pthread_mutex_unlock(&data->sources_mutex);

	return cur_time;
}

/* in obs-display.c */
extern void render_display(struct obs_display *display);

static inline void render_displays(void)
{
	struct obs_display *display;

	if (!obs->data.valid)
		return;

	/* render extra displays/swaps */
	pthread_mutex_lock(&obs->data.displays_mutex);

	display = obs->data.first_display;
	while (display) {
		render_display(display);
		display = display->next;
	}

	pthread_mutex_unlock(&obs->data.displays_mutex);
}

static inline void set_render_size(uint32_t width, uint32_t height)
{
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);

	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
	gs_set_viewport(0, 0, width, height);
}

static inline void unmap_last_surfaces(struct obs_core_video *video)
{
	for (size_t i = 0; i < video->mapped_surfaces.num;) {
		if (video->mapped_surfaces.array[i]->refs != -1) {
			i++;
			continue;
		}

		gs_stagesurface_unmap(video->mapped_surfaces.array[i]->surf);
		da_erase(video->mapped_surfaces, i);
	}
}

static void free_output_texture(obs_output_texture_t *tex)
{
	switch (tex->type) {
	case OBS_OUTPUT_TEXTURE_TEX:
		gs_texture_destroy(tex->tex);
		break;
	case OBS_OUTPUT_TEXTURE_STAGESURF:
		gs_stagesurface_destroy(tex->surf);
		break;
	}

	bfree(tex);
}

static void free_unused_textures(obs_texture_pipeline_t *pipeline)
{
	for (size_t i = 0; i < pipeline->textures.num;) {
		obs_output_texture_t *tex = pipeline->textures.array[i];
		if (tex->refs >= 0) {
			i += 1;
			continue;
		}

		free_output_texture(tex);
		da_erase(pipeline->textures, i);
	}
}

static void free_unused_pipelines(obs_texture_pipelines_t *pipelines)
{
	for (size_t i = 0; i < pipelines->num;) {
		obs_texture_pipeline_t *pipeline = &pipelines->array[i];
		if (!pipeline->ready.num)
			free_unused_textures(pipeline);
		if (pipeline->textures.num) {
			i += 1;
			continue;
		}

		for (size_t i = 0; i < pipeline->idle_output_lists.num; i++)
			da_free(pipeline->idle_output_lists.array[i]);

		da_free(pipeline->textures);
		da_free(pipeline->active);
		da_free(pipeline->ready);
		da_free(pipeline->idle_output_lists);
		da_erase((*pipelines), i);
	}
}

static obs_active_texture_t *find_texture(obs_texture_pipeline_t *pipeline)
{
	obs_output_texture_t *tex = NULL;
	for (size_t i = 0; i < pipeline->textures.num; i++) {
		if (!os_atomic_compare_swap_long(&pipeline->textures.array[i]->refs, -1, 0))
			continue;

		tex = pipeline->textures.array[i];
		break;
	}

	if (!tex) {
		tex = bzalloc(sizeof(*tex));
		switch (pipeline->type) {
		case OBS_OUTPUT_TEXTURE_TEX:
			tex->tex = gs_texture_create(pipeline->width, pipeline->height,
				GS_RGBA, 1, NULL, GS_RENDER_TARGET);
			break;
		case OBS_OUTPUT_TEXTURE_STAGESURF:
			tex->surf = gs_stagesurface_create(pipeline->width, pipeline->height,
				GS_RGBA);
			break;
		}
		tex->type = pipeline->type;
		da_push_back(pipeline->textures, &tex);
	}

	if (!tex)
		return NULL;

	da_push_back_new(pipeline->active);
	obs_active_texture_t *active = da_end(pipeline->active);
	active->tex = tex;

	if (pipeline->idle_output_lists.num) {
		active->outputs = *(obs_video_outputs_t*)da_end(pipeline->idle_output_lists);
		da_pop_back(pipeline->idle_output_lists);
		da_resize(active->outputs, 0);
	}

	return active;
}

static obs_active_texture_t *find_texture_for_target(obs_texture_pipelines_t *pipelines,
	uint32_t width, uint32_t height, enum obs_output_texture_type type)
{
	obs_texture_pipeline_t *pipeline = NULL;
	for (size_t i = 0; i < pipelines->num; i++) {
		pipeline = &pipelines->array[i];
		if (pipeline->width == width && pipeline->height == height)
			break;
		pipeline = NULL;
	}

	if (!pipeline)
		pipeline = da_push_back_new((*pipelines));

	if (!pipeline) {
		blog(LOG_ERROR, "Tried to find texture (%dx%d) with no matching pipeline", width, height);
		return NULL;
	}

	pipeline->width = width;
	pipeline->height = height;
	pipeline->type = type;

	return find_texture(pipeline);
}

static obs_texture_pipeline_t *find_ready_pipeline_for_target(obs_texture_pipelines_t *pipelines,
	uint32_t width, uint32_t height)
{
	for (size_t i = 0; i < pipelines->num; i++) {
		obs_texture_pipeline_t *pipeline = &pipelines->array[i];
		if (pipeline->width != width || pipeline->height != height)
			continue;

		return pipeline;
	}

	return NULL;
}

static void release_ready_textures(obs_texture_pipeline_t *pipeline)
{
	for (size_t i = 0; i < pipeline->ready.num; i++)
		obs_output_texture_release(pipeline->ready.array[i].tex);
}

static void update_pipeline_ready_state(obs_texture_pipeline_t *pipeline)
{
	obs_active_textures_t tmp = pipeline->ready;
	pipeline->ready = pipeline->active;
	pipeline->active = tmp;

	for (size_t i = 0; i < pipeline->active.num; i++) {
		obs_video_outputs_t *outputs = &pipeline->active.array[i].outputs;
		if (!outputs->capacity)
			continue;
		da_push_back(pipeline->idle_output_lists, outputs);
	}

	da_resize(pipeline->active, 0);
}

static void free_activated_texture(obs_texture_pipeline_t *pipeline, obs_active_texture_t *active)
{
	da_erase_item(pipeline->textures, &active->tex);

	switch (active->tex->type) {
	case OBS_OUTPUT_TEXTURE_TEX:
		gs_texture_destroy(active->tex->tex);
		break;
	case OBS_OUTPUT_TEXTURE_STAGESURF:
		gs_stagesurface_destroy(active->tex->surf);
		break;
	}

	if (active->outputs.capacity)
		da_push_back(pipeline->idle_output_lists, &active->outputs);

	da_pop_back(pipeline->active);
}

static const char *render_main_texture_name = "render_main_texture";
static inline void render_main_texture(struct obs_core_video *video, struct obs_vframe_info *vframe_info)
{
	release_ready_textures(&video->render_textures);
	update_pipeline_ready_state(&video->render_textures);

	if (!video->active_outputs.num)
		return;

	profile_start(render_main_texture_name);

	obs_active_texture_t *active = NULL;
	while (active = find_texture(&video->render_textures)) {
		if (gs_texture_get_width(active->tex->tex) == video->render_textures.width &&
			gs_texture_get_height(active->tex->tex) == video->render_textures.height)
			break;

		free_activated_texture(&video->render_textures, active);
		active = NULL;
	}

	if (!active) {
		blog(LOG_ERROR, "Failed to find render texture");
		goto end;
	}

	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);

	gs_set_render_target(active->tex->tex, NULL);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);

	set_render_size(video->base_width, video->base_height);
	obs_view_render(&obs->data.main_view);

	da_push_back_da(active->outputs, video->active_outputs);

	active->vframe_info = vframe_info;
	vframe_info->uses++;

end:
	profile_end(render_main_texture_name);
}

static inline gs_effect_t *get_scale_effect_internal(
		struct obs_core_video *video,
		uint32_t width, uint32_t height,
		enum obs_scale_type scale_type)
{
	/* if the dimension is under half the size of the original image,
	 * bicubic/lanczos can't sample enough pixels to create an accurate
	 * image, so use the bilinear low resolution effect instead */
	if (width  < (video->base_width  / 2) &&
	    height < (video->base_height / 2)) {
		return video->bilinear_lowres_effect;
	}

	switch (scale_type) {
	case OBS_SCALE_BILINEAR: return video->default_effect;
	case OBS_SCALE_LANCZOS:  return video->lanczos_effect;
	case OBS_SCALE_BICUBIC:;
	}

	return video->bicubic_effect;
}

static inline bool resolution_close(struct obs_core_video *video,
		uint32_t width, uint32_t height)
{
	long width_cmp  = (long)video->base_width  - (long)width;
	long height_cmp = (long)video->base_height - (long)height;

	return labs(width_cmp) <= 16 && labs(height_cmp) <= 16;
}

static inline gs_effect_t *get_scale_effect(struct obs_core_video *video,
		uint32_t width, uint32_t height, enum obs_scale_type scale_type)
{
	if (resolution_close(video, width, height)) {
		return video->default_effect;
	} else {
		/* if the scale method couldn't be loaded, use either bicubic
		 * or bilinear by default */
		gs_effect_t *effect = get_scale_effect_internal(video,
			width, height, scale_type);
		if (!effect)
			effect = !!video->bicubic_effect ?
				video->bicubic_effect :
				video->default_effect;
		return effect;
	}
}

static obs_video_output_t *get_active_output(obs_active_texture_t *source, size_t i)
{
	obs_video_output_t *output = NULL;
	while (i < source->outputs.num) {
		output = source->outputs.array[i];
		if (!output->expired)
			return output;

		da_erase(source->outputs, i);
		output = NULL;
	}

	return output;
}

static void render_output_texture(struct obs_core_video *video, obs_active_texture_t *source)
{
	while (source->outputs.num) {
		obs_video_output_t *output = get_active_output(source, 0);
		if (!output)
			break;

		obs_active_texture_t *tex = find_texture_for_target(&video->output_textures, output->info.width, output->info.height, OBS_OUTPUT_TEXTURE_TEX);
		if (!tex) {
			blog(LOG_ERROR, "Failed to get output_frame texture for %p (%dx%d)", output, output->info.width, output->info.height);
			continue;
		}

		tex->vframe_info = source->vframe_info;
		tex->vframe_info->uses++;

		for (size_t i = 0; i < source->outputs.num;) {
			obs_video_output_t *out = source->outputs.array[i];
			if (out->expired ||
				out->info.width != output->info.width || out->info.height != output->info.height ||
				out->info.scale_type != output->info.scale_type ||
				out->info.colorspace != output->info.colorspace ||
				out->info.range != output->info.range) {
				i += 1;
				continue;
			}

			da_push_back(tex->outputs, &out);
			da_erase(source->outputs, i);
		}

		gs_texture_t *texture = source->tex->tex;
		gs_texture_t *target = tex->tex->tex;
		uint32_t     width = gs_texture_get_width(target);
		uint32_t     height = gs_texture_get_height(target);
		struct vec2  base_i;

		vec2_set(&base_i,
			1.0f / (float)video->base_width,
			1.0f / (float)video->base_height);

		gs_effect_t    *effect = get_scale_effect(video,
			width, height, output->info.scale_type);
		gs_technique_t *tech = gs_effect_get_technique(effect, "DrawMatrix");
		gs_eparam_t    *image = gs_effect_get_param_by_name(effect, "image");
		gs_eparam_t    *matrix = gs_effect_get_param_by_name(effect,
			"color_matrix");
		gs_eparam_t    *bres_i = gs_effect_get_param_by_name(effect,
			"base_dimension_i");
		size_t      passes, i;

		gs_set_render_target(target, NULL);
		set_render_size(width, height);

		if (bres_i)
			gs_effect_set_vec2(bres_i, &base_i);

		gs_effect_set_val(matrix, output->color_matrix, sizeof(float) * 16);
		gs_effect_set_texture(image, texture);

		gs_enable_blending(false);
		passes = gs_technique_begin(tech);
		for (i = 0; i < passes; i++) {
			gs_technique_begin_pass(tech, i);
			gs_draw_sprite(texture, 0, width, height);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
		gs_enable_blending(true);
	}
}

static const char *render_output_textures_name = "render_output_textures";
static inline void render_output_textures(struct obs_core_video *video)
{
	for (size_t i = 0; i < video->output_textures.num; i++) {
		release_ready_textures(&video->output_textures.array[i]);
		update_pipeline_ready_state(&video->output_textures.array[i]);
	}

	free_unused_pipelines(&video->output_textures);

	if (!video->render_textures.ready.num)
		return;

	profile_start(render_output_textures_name);

	for (size_t i = 0; i < video->render_textures.ready.num; i++) {
		render_output_texture(video, &video->render_textures.ready.array[i]);
		video->render_textures.ready.array[i].vframe_info->uses--;
	}

	profile_end(render_output_textures_name);
}

static inline void set_eparam(gs_effect_t *effect, const char *name, float val)
{
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, name);
	gs_effect_set_float(param, val);
}

static void render_convert_texture(struct obs_core_video *video, obs_active_texture_t *source)
{
	for (size_t i = 0; i < source->outputs.num;) {
		obs_video_output_t *output = get_active_output(source, i);
		if (!output)
			break;

		if (!output->info.gpu_conversion) {
			i += 1;
			continue;
		}

		obs_active_texture_t *tex = find_texture_for_target(&video->convert_textures, output->info.width, output->conversion_height, OBS_OUTPUT_TEXTURE_TEX);
		if (!tex) {
			blog(LOG_ERROR, "Failed to get convert texture for %p (%dx%d)", output, output->info.width, output->info.height);
			i += 1;
			continue;
		}

		tex->vframe_info = source->vframe_info;
		tex->vframe_info->uses++;

		for (size_t j = 0; j < source->outputs.num;) {
			obs_video_output_t *out = source->outputs.array[j];
			if (out->info.format != output->info.format) {
				j += 1;
				continue;
			}

			da_push_back(tex->outputs, &out);
			da_erase(source->outputs, j);
		}

		gs_texture_t *texture = source->tex->tex;
		gs_texture_t *target = tex->tex->tex;
		float        fwidth = (float)output->info.width;
		float        fheight = (float)output->info.height;
		size_t       passes;

		gs_effect_t    *effect = video->conversion_effect;
		gs_eparam_t    *image = gs_effect_get_param_by_name(effect, "image");
		gs_technique_t *tech = gs_effect_get_technique(effect,
			output->conversion_tech);

		set_eparam(effect, "u_plane_offset", (float)output->plane_offsets[1]);
		set_eparam(effect, "v_plane_offset", (float)output->plane_offsets[2]);
		set_eparam(effect, "width", fwidth);
		set_eparam(effect, "height", fheight);
		set_eparam(effect, "width_i", 1.0f / fwidth);
		set_eparam(effect, "height_i", 1.0f / fheight);
		set_eparam(effect, "width_d2", fwidth  * 0.5f);
		set_eparam(effect, "height_d2", fheight * 0.5f);
		set_eparam(effect, "width_d2_i", 1.0f / (fwidth  * 0.5f));
		set_eparam(effect, "height_d2_i", 1.0f / (fheight * 0.5f));
		set_eparam(effect, "input_height", (float)output->conversion_height);

		gs_effect_set_texture(image, texture);

		gs_set_render_target(target, NULL);
		set_render_size(output->info.width, output->conversion_height);

		gs_enable_blending(false);
		passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			gs_technique_begin_pass(tech, i);
			gs_draw_sprite(texture, 0, output->info.width,
				output->conversion_height);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
		gs_enable_blending(true);
	}
}

static const char *render_convert_textures_name = "render_convert_textures";
static void render_convert_textures(struct obs_core_video *video)
{
	for (size_t i = 0; i < video->convert_textures.num; i++) {
		release_ready_textures(&video->convert_textures.array[i]);
		update_pipeline_ready_state(&video->convert_textures.array[i]);
	}

	free_unused_pipelines(&video->convert_textures);

	if (!video->output_textures.num)
		return;

	profile_start(render_convert_textures_name);

	for (size_t i = 0; i < video->output_textures.num; i++) {
		obs_texture_pipeline_t *pipeline = &video->output_textures.array[i];
		for (size_t j = 0; j < pipeline->ready.num; j++) {
			render_convert_texture(video, &pipeline->ready.array[j]);
			pipeline->ready.array[j].vframe_info->uses--;
		}
	}

	profile_end(render_convert_textures_name);
}

static obs_ready_frame_t *add_ready_frame(obs_active_texture_t *tex, obs_video_output_t *output)
{
	tex->vframe_info->uses--;

	obs_ready_frame_t *ready = da_push_back_new(tex->vframe_info->data);
	ready->output = output;
	ready->tex = tex->tex;

	obs_output_texture_addref(tex->tex);

	return ready;
}

static inline void stage_output_texture(struct obs_core_video *video, obs_active_texture_t *source)
{
	assert(source->outputs.num == 1);

	obs_video_output_t *output = get_active_output(source, 0);
	if (!output)
		return;

	uint32_t height = gs_texture_get_height(source->tex->tex);
	obs_active_texture_t *tex = find_texture_for_target(&video->copy_surfaces,
		gs_texture_get_width(source->tex->tex), height, OBS_OUTPUT_TEXTURE_STAGESURF);
	if (!tex) {
		blog(LOG_ERROR, "Failed to get copy surface for %p (%dx%d)", output, output->info.width, height);
		return;
	}

	for (size_t i = 0; i < source->outputs.num; i++) {
		obs_video_output_t *out = source->outputs.array[i];
		if (out->info.format != output->info.format)
			continue;

		da_push_back(tex->outputs, &out);
		da_erase(source->outputs, 0);
	}

	tex->vframe_info = source->vframe_info;
	tex->vframe_info->uses++;

	gs_stage_texture(tex->tex->surf, source->tex->tex);
}

static const char *stage_output_textures_name = "stage_output_textures";
static void stage_output_textures(struct obs_core_video *video)
{
	for (size_t i = 0; i < video->copy_surfaces.num; i++) {
		release_ready_textures(&video->copy_surfaces.array[i]);
		unmap_last_surfaces(video);
		update_pipeline_ready_state(&video->copy_surfaces.array[i]);
	}

	free_unused_pipelines(&video->copy_surfaces);

	if (!video->output_textures.num && !video->convert_textures.num)
		return;

	profile_start(stage_output_textures_name);

	for (size_t i = 0; i < video->output_textures.num; i++) {
		obs_texture_pipeline_t *pipeline = &video->output_textures.array[i];
		for (size_t j = 0; j < pipeline->ready.num; j++) {
			if (!pipeline->ready.array[j].outputs.num)
				continue;
			stage_output_texture(video, &pipeline->ready.array[j]);
			pipeline->ready.array[j].vframe_info->uses--;
		}
	}

	for (size_t i = 0; i < video->convert_textures.num; i++) {
		obs_texture_pipeline_t *pipeline = &video->convert_textures.array[i];
		for (size_t j = 0; j < pipeline->ready.num; j++) {
			stage_output_texture(video, &pipeline->ready.array[j]);
			pipeline->ready.array[j].vframe_info->uses--;
		}
	}

	profile_end(stage_output_textures_name);
}

static inline void render_video(struct obs_core_video *video, struct obs_vframe_info *vframe_info)
{
	gs_begin_scene();

	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);

	render_main_texture(video, vframe_info);
	render_output_textures(video);
	render_convert_textures(video);

	stage_output_textures(video);

	gs_set_render_target(NULL, NULL);
	gs_enable_blending(true);

	gs_end_scene();
}

static inline void download_frames(struct obs_core_video *video)
{
	for (size_t i = 0; i < video->copy_surfaces.num; i++) {
		obs_texture_pipeline_t *pipeline = video->copy_surfaces.array + i;
		for (size_t j = 0; j < pipeline->ready.num; j++) {
			obs_active_texture_t *active = pipeline->ready.array + j;
			obs_video_output_t *output = active->outputs.array[0];

			struct video_frame frame = { 0 };

			if (!gs_stagesurface_map(active->tex->surf, &frame.data[0], &frame.linesize[0]))
				continue;

			da_push_back(video->mapped_surfaces, &active->tex);

			obs_ready_frame_t *ready = add_ready_frame(active, output);
			ready->frame = frame;
		}
	}
}

static inline uint32_t calc_linesize(uint32_t pos, uint32_t linesize)
{
	uint32_t size = pos % linesize;
	return size ? size : linesize;
}

static void copy_dealign(
		uint8_t *dst, uint32_t dst_pos, uint32_t dst_linesize,
		const uint8_t *src, uint32_t src_pos, uint32_t src_linesize,
		uint32_t remaining)
{
	while (remaining) {
		uint32_t src_remainder = src_pos % src_linesize;
		uint32_t dst_offset = dst_linesize - src_remainder;
		uint32_t src_offset = src_linesize - src_remainder;

		if (remaining < dst_offset) {
			memcpy(dst + dst_pos, src + src_pos, remaining);
			src_pos += remaining;
			dst_pos += remaining;
			remaining = 0;
		} else {
			memcpy(dst + dst_pos, src + src_pos, dst_offset);
			src_pos += src_offset;
			dst_pos += dst_offset;
			remaining -= dst_offset;
		}
	}
}

static inline uint32_t make_aligned_linesize_offset(uint32_t offset,
		uint32_t dst_linesize, uint32_t src_linesize)
{
	uint32_t remainder = offset % dst_linesize;
	return (offset / dst_linesize) * src_linesize + remainder;
}

static void fix_gpu_converted_alignment(struct video_frame *output_frame, const obs_video_output_t *output, struct video_frame *frame)
{
	uint32_t src_linesize = frame->linesize[0];
	uint32_t dst_linesize = output_frame->linesize[0] * 4;
	uint32_t src_pos      = 0;

	for (size_t i = 0; i < 3; i++) {
		if (output->plane_linewidth[i] == 0)
			break;

		src_pos = make_aligned_linesize_offset(output->plane_offsets[i],
				dst_linesize, src_linesize);

		copy_dealign(output_frame->data[i], 0, dst_linesize,
				frame->data[0], src_pos, src_linesize,
				output->plane_sizes[i]);
	}
}

static void set_gpu_converted_data(struct video_frame *output_frame, const obs_video_output_t *output, struct video_frame *frame)
{
	if (frame->linesize[0] == output->info.width*4) {
		struct video_frame input;

		for (size_t i = 0; i < 3; i++) {
			if (output->plane_linewidth[i] == 0)
				break;

			input.linesize[i] = output->plane_linewidth[i];
			input.data[i] = frame->data[0] + output->plane_offsets[i];
		}

		video_frame_copy(output_frame, &input, output->info.format, output->info.height);

	} else {
		fix_gpu_converted_alignment(output_frame, output, frame);
	}
}

static void convert_frame(
		struct video_frame *output_frame, const obs_video_output_t *output, const struct video_frame *frame)
{
	if (output->info.format == VIDEO_FORMAT_I420) {
		compress_uyvx_to_i420(
				frame->data[0], frame->linesize[0],
				0, output->info.height,
				output_frame->data, output_frame->linesize);

	} else if (output->info.format == VIDEO_FORMAT_NV12) {
		compress_uyvx_to_nv12(
				frame->data[0], frame->linesize[0],
				0, output->info.height,
				output_frame->data, output_frame->linesize);

	} else if (output->info.format == VIDEO_FORMAT_I444) {
		convert_uyvx_to_i444(
				frame->data[0], frame->linesize[0],
				0, output->info.height,
				output_frame->data, output_frame->linesize);

	} else {
		blog(LOG_ERROR, "convert_frame: unsupported texture format");
	}
}

static inline void copy_rgbx_frame(struct video_frame *output_frame,
	obs_video_output_t *output, const struct video_frame *input)
{
	uint8_t *in_ptr = input->data[0];
	uint8_t *out_ptr = output_frame->data[0];

	/* if the line sizes match, do a single copy */
	if (input->linesize[0] == output_frame->linesize[0]) {
		memcpy(out_ptr, in_ptr, input->linesize[0] * output->info.height);
	} else {
		for (size_t y = 0; y < output->info.height; y++) {
			memcpy(out_ptr, in_ptr, output->info.width * 4);
			in_ptr += input->linesize[0];
			out_ptr += output_frame->linesize[0];
		}
	}
}

static inline void output_video_data(video_t *video,
		struct obs_vframe_info *info)
{
	video_locked_frame locked;

	locked = video_output_lock_frame(video, info->data.num, info->count,
			info->timestamp, info->tracked_id);
	if (locked) {
		struct video_frame output_frame;
		for (size_t i = 0; i < info->data.num; i++) {
			obs_video_output_t *output = info->data.array[i].output;
			if (!video_output_get_frame_buffer(video, &output_frame, &output->info, locked, output->expiring || output->expired)) {
				blog(LOG_ERROR, "Failed to get frame buffer for output");
				continue;
			}

			struct video_frame *frame = &info->data.array[i].frame;
			if (output->info.gpu_conversion) {
				set_gpu_converted_data(&output_frame, output, frame);
			} else if (format_is_yuv(output->info.format)) {
				convert_frame(&output_frame, output, frame);
			} else {
				copy_rgbx_frame(&output_frame, output, frame);
			}

			if (info->data.array[i].tex)
				obs_output_texture_release(info->data.array[i].tex);
		}

		video_output_unlock_frame(video, locked);

	} else {
		for (size_t i = 0; i < info->data.num; i++)
			if (info->data.array[i].tex)
				obs_output_texture_release(info->data.array[i].tex);
	}

	da_resize(info->data, 0);
	info->timestamp = 0;
	info->count = 0;
}

static struct obs_vframe_info *get_vframe_info()
{
	struct obs_core_video *video = &obs->video;

	struct obs_vframe_info *info;
	if (video->vframe_info.num) {
		info = *(struct obs_vframe_info**)da_end(video->vframe_info);
		da_pop_back(video->vframe_info);
		return info;
	}

	return bzalloc(sizeof(*info));
}

static bool sleepto_imprecise(uint64_t target)
{
	uint64_t actual_time = os_gettime_ns();
	if (actual_time > target)
		return false;

	uint32_t sleep_time_ms = (uint32_t)((target - actual_time) / (1000 * 1000));
	os_sleep_ms(sleep_time_ms);
	return true;
}

static inline void video_sleep(struct obs_core_video *video,
		uint64_t *p_time, uint64_t interval_ns, struct obs_vframe_info **vframe_info)
{
	uint64_t cur_time = *p_time;
	uint64_t t = cur_time + interval_ns;
	int count;

	pthread_mutex_lock(&video->video_thread_time_mutex);
	video->video_thread_time = cur_time;
	pthread_mutex_unlock(&video->video_thread_time_mutex);

	struct obs_vframe_info *info = *vframe_info;
	if (info->uses)
		*vframe_info = get_vframe_info();

	bool precise_sleep = video->active_outputs.num > 0;
	bool did_sleep = precise_sleep ? os_sleepto_ns(t) : sleepto_imprecise(t);

	if (did_sleep) {
		*p_time = t;
		count = 1;
	} else {
		count = (int)((os_gettime_ns() - cur_time) / interval_ns);
		*p_time = cur_time + interval_ns * count;
	}

	video->total_frames += count;
	video->lagged_frames += count - 1;

	if (!info->uses)
		return;

	pthread_mutex_lock(&video->frame_tracker_mutex);
	info->tracked_id = video->tracked_frame_id;
	video->tracked_frame_id = 0;
	pthread_mutex_unlock(&video->frame_tracker_mutex);

	info->timestamp = cur_time;
	info->count = count;

	da_push_back(video->active_vframe_info, &info);
	if (video->active_vframe_info.num > 10)
		blog(LOG_ERROR, "video_sleep: Queued more than 10 frames, something's not quite right");
}

static const char *render_frame_render_video_name = "render_video";
static const char *render_frame_download_frame_name = "download_frames";
static const char *render_frame_gs_flush_name = "gs_flush";
static void render_frame(struct obs_vframe_info *vframe_info)
{
	struct obs_core_video *video = &obs->video;

	profile_start(render_frame_render_video_name);
	render_video(video, vframe_info);
	profile_end(render_frame_render_video_name);

	profile_start(render_frame_download_frame_name);
	download_frames(video);
	profile_end(render_frame_download_frame_name);

	profile_start(render_frame_gs_flush_name);
	gs_flush();
	profile_end(render_frame_gs_flush_name);
}

static const char *output_frame_output_video_data_name = "output_video_data";
static inline void output_frame()
{
	struct obs_core_video *video = &obs->video;

	if (!video->active_vframe_info.num)
		return;

	struct obs_vframe_info *info = video->active_vframe_info.array[0];
	if (info->uses)
		return;

	if (info->data.num) {
		profile_start(output_frame_output_video_data_name);
		output_video_data(video->video, info);
		profile_end(output_frame_output_video_data_name);
	}

	da_push_back(video->vframe_info, video->active_vframe_info.array);
	da_erase(video->active_vframe_info, 0);
}


#define PIXEL_SIZE 4

#define GET_ALIGN(val, align) \
	(((val) + (align-1)) & ~(align-1))

static inline void set_420p_sizes(obs_video_output_t *output)
{
	uint32_t chroma_pixels;
	uint32_t total_bytes;

	chroma_pixels = (output->info.width * output->info.height / 4);
	chroma_pixels = GET_ALIGN(chroma_pixels, PIXEL_SIZE);

	output->plane_offsets[0] = 0;
	output->plane_offsets[1] = output->info.width * output->info.height;
	output->plane_offsets[2] = output->plane_offsets[1] + chroma_pixels;

	output->plane_linewidth[0] = output->info.width;
	output->plane_linewidth[1] = output->info.width/2;
	output->plane_linewidth[2] = output->info.width/2;

	output->plane_sizes[0] = output->plane_offsets[1];
	output->plane_sizes[1] = output->plane_sizes[0]/4;
	output->plane_sizes[2] = output->plane_sizes[1];

	total_bytes = output->plane_offsets[2] + chroma_pixels;

	output->conversion_height =
		(total_bytes/PIXEL_SIZE + output->info.width-1) /
		output->info.width;

	output->conversion_height = GET_ALIGN(output->conversion_height, 2);
	output->conversion_tech = "Planar420";
}

static inline void set_nv12_sizes(obs_video_output_t *output)
{
	uint32_t chroma_pixels;
	uint32_t total_bytes;

	chroma_pixels = (output->info.width * output->info.height / 2);
	chroma_pixels = GET_ALIGN(chroma_pixels, PIXEL_SIZE);

	output->plane_offsets[0] = 0;
	output->plane_offsets[1] = output->info.width * output->info.height;

	output->plane_linewidth[0] = output->info.width;
	output->plane_linewidth[1] = output->info.width;

	output->plane_sizes[0] = output->plane_offsets[1];
	output->plane_sizes[1] = output->plane_sizes[0]/2;

	total_bytes = output->plane_offsets[1] + chroma_pixels;

	output->conversion_height =
		(total_bytes/PIXEL_SIZE + output->info.width-1) /
		output->info.width;

	output->conversion_height = GET_ALIGN(output->conversion_height, 2);
	output->conversion_tech = "NV12";
}

static inline void set_444p_sizes(obs_video_output_t *output)
{
	uint32_t chroma_pixels;
	uint32_t total_bytes;

	chroma_pixels = (output->info.width * output->info.height);
	chroma_pixels = GET_ALIGN(chroma_pixels, PIXEL_SIZE);

	output->plane_offsets[0] = 0;
	output->plane_offsets[1] = chroma_pixels;
	output->plane_offsets[2] = chroma_pixels + chroma_pixels;

	output->plane_linewidth[0] = output->info.width;
	output->plane_linewidth[1] = output->info.width;
	output->plane_linewidth[2] = output->info.width;

	output->plane_sizes[0] = chroma_pixels;
	output->plane_sizes[1] = chroma_pixels;
	output->plane_sizes[2] = chroma_pixels;

	total_bytes = output->plane_offsets[2] + chroma_pixels;

	output->conversion_height =
		(total_bytes/PIXEL_SIZE + output->info.width-1) /
		output->info.width;

	output->conversion_height = GET_ALIGN(output->conversion_height, 2);
	output->conversion_tech = "Planar444";
}

static inline void calc_gpu_conversion_sizes(obs_video_output_t *output)
{
	output->conversion_height = 0;
	memset(output->plane_offsets, 0, sizeof(output->plane_offsets));
	memset(output->plane_sizes, 0, sizeof(output->plane_sizes));
	memset(output->plane_linewidth, 0,
		sizeof(output->plane_linewidth));

	switch (output->info.format) {
	case VIDEO_FORMAT_I420:
		set_420p_sizes(output);
		break;
	case VIDEO_FORMAT_NV12:
		set_nv12_sizes(output);
		break;
	case VIDEO_FORMAT_I444:
		set_444p_sizes(output);
		break;
	}
}

static bool obs_init_gpu_conversion(obs_video_output_t *output)
{
	struct obs_core_video *video = &obs->video;

	calc_gpu_conversion_sizes(output);

	if (!output->conversion_height) {
		blog(LOG_INFO, "GPU conversion not available for format: %u",
			(unsigned int)output->info.format);
		return false;
#if 0
		video->gpu_conversion = false;
		return true;
#endif
	}

	return true;
}

static inline void set_video_matrix(obs_video_output_t *output)
{
	struct matrix4 mat;
	struct vec4 r_row;

	if (format_is_yuv(output->info.format)) {
		video_format_get_parameters(output->info.colorspace, output->info.range,
			(float*)&mat, NULL, NULL);
		matrix4_inv(&mat, &mat);

		/* swap R and G */
		r_row = mat.x;
		mat.x = mat.y;
		mat.y = r_row;
	} else {
		matrix4_identity(&mat);
	}

	memcpy(output->color_matrix, &mat, sizeof(float) * 16);
}

static void update_outputs()
{
	struct obs_core_video *video = &obs->video;

	while (video->expired_outputs.num) {
		obs_video_output_t *output = *(obs_video_output_t**)da_end(video->expired_outputs);
		da_pop_back(video->expired_outputs);
		da_erase_item(video->outputs, &output);
		bfree(output);
	}

	video_scale_info_ts added = { 0 }, expiring = { 0 }, removed = { 0 };
	if (!video_output_get_changes(video->video, &added, &expiring, &removed))
		return;

	for (size_t i = 0; i < removed.num; i++) {
		for (size_t j = 0; j < video->active_outputs.num; j++) {
			obs_video_output_t *output = video->active_outputs.array[j];
			if (memcmp(&output->info, removed.array + i, sizeof(output->info)) != 0)
				continue;

			da_erase_item(video->active_outputs, &output);
			da_push_back(video->expired_outputs, &output);
			output->expired = true;
			break;
		}
		for (size_t j = 0; j < video->expiring_outputs.num; j++) {
			obs_video_output_t *output = video->expiring_outputs.array[j];
			if (memcmp(&output->info, removed.array + i, sizeof(output->info)) != 0)
				continue;

			da_erase_item(video->expiring_outputs, &output);
			da_push_back(video->expired_outputs, &output);
			output->expired = true;
			break;
		}
	}

	for (size_t i = 0; i < expiring.num; i++) {
		for (size_t j = 0; j < video->active_outputs.num; j++) {
			obs_video_output_t *output = video->active_outputs.array[j];
			if (memcmp(&output->info, expiring.array + i, sizeof(output->info)) != 0)
				continue;

			da_erase_item(video->active_outputs, &output);
			da_push_back(video->expiring_outputs, &output);
			output->expiring = true;
			break;
		}
	}

	da_reserve(video->outputs, video->outputs.num + added.num);
	for (size_t i = 0; i < added.num; i++) {
		obs_video_output_t **output_ptr = da_push_back_new(video->outputs);
		*output_ptr = bzalloc(sizeof(obs_video_output_t));
		obs_video_output_t *output = *output_ptr;
		output->info = added.array[i];
		obs_init_gpu_conversion(output);
		set_video_matrix(output);
		da_push_back(video->active_outputs, &output);
	}

	da_free(added);
	da_free(expiring);
	da_free(removed);
}

static void update_render_size(void)
{
	struct obs_core_video *video = &obs->video;

	bool render_size_changed = false;

	pthread_mutex_lock(&video->resize_mutex);
	render_size_changed = video->render_textures.width != video->base_width ||
		video->render_textures.height != video->base_height;

	video->render_textures.width = video->base_width;
	video->render_textures.height = video->base_height;
	pthread_mutex_unlock(&video->resize_mutex);

	if (!render_size_changed)
		return;

	for (size_t i = 0; i < video->render_textures.textures.num;) {
		obs_output_texture_t *tex = video->render_textures.textures.array[i];
		if (tex->refs >= 0)
			goto next;
		if (gs_texture_get_width(tex->tex) == video->render_textures.width &&
			gs_texture_get_height(tex->tex) == video->render_textures.height)
			goto next;

		gs_texture_destroy(tex->tex);
		da_erase(video->render_textures.textures, i);
		continue;

	next:
		i++;
	}
}

#define NBSP "\xC2\xA0"

static const char *update_profiler_entry(bool active, uint64_t interval)
{
	const char *video_thread_name =
		profile_store_name(obs_get_profiler_name_store(),
			"obs_video_thread(%g"NBSP"ms%s)", interval / 1000000., active ? "" : " idle");
	profile_register_root(video_thread_name, interval);

	return video_thread_name;
}

static const char *tick_sources_name = "tick_sources";
static const char *gs_context_name = "gs_context(video->graphics)";
static const char *render_displays_name = "render_displays";
static const char *render_frame_name = "render_frame";
static const char *output_frame_name = "output_frame";
static const char *deferred_cleanup_name = "deferred_cleanup";
static const char *update_render_size_name = "update_render_size";
static const char *update_outputs_name = "update_outputs";
void *obs_video_thread(void *param)
{
	uint64_t last_time = 0;
	uint64_t interval = video_output_get_frame_time(obs->video.video);

	obs->video.video_time = os_gettime_ns();

	os_set_thread_name("libobs: graphics thread");

	bool outputs_were_active = obs->video.outputs.num > 0;

	const char *video_thread_name = update_profiler_entry(outputs_were_active, interval);

	struct obs_vframe_info *vframe_info = get_vframe_info();

	while (!video_output_stopped(obs->video.video)) {
		profile_start(video_thread_name);

		profile_start(tick_sources_name);
		last_time = tick_sources(obs->video.video_time, last_time);
		profile_end(tick_sources_name);

		profile_start(gs_context_name);
		gs_enter_context(obs->video.graphics);

		profile_start(render_displays_name);
		render_displays();
		profile_end(render_displays_name);

		profile_start(render_frame_name);
		render_frame(vframe_info);
		profile_end(render_frame_name);

		profile_start(deferred_cleanup_name);
		obs_free_deferred_gs_data();
		profile_end(deferred_cleanup_name);

		profile_start(update_render_size_name);
		update_render_size();
		profile_end(update_render_size_name);

		gs_leave_context();
		profile_end(gs_context_name);

		profile_start(output_frame_name);
		output_frame();
		profile_end(output_frame_name);

		profile_start(update_outputs_name);
		update_outputs();
		profile_end(update_outputs_name);

		profile_end(video_thread_name);

		bool outputs_active = obs->video.outputs.num > 0;
		if (outputs_active != outputs_were_active) {
			video_thread_name = update_profiler_entry(outputs_active, interval);
			outputs_were_active = outputs_active;
		}

		profile_reenable_thread();

		video_sleep(&obs->video, &obs->video.video_time, interval, &vframe_info);
	}

	UNUSED_PARAMETER(param);
	return NULL;
}

video_tracked_frame_id obs_track_next_frame(void)
{
	if (!obs)
		return 0;

	pthread_mutex_lock(&obs->video.frame_tracker_mutex);
	if (!obs->video.tracked_frame_id)
		obs->video.tracked_frame_id = ++obs->video.last_tracked_frame_id;

	video_tracked_frame_id tracked_id = obs->video.tracked_frame_id;
	pthread_mutex_unlock(&obs->video.frame_tracker_mutex);

	return tracked_id;
}

bool obs_get_video_thread_time(uint64_t *val)
{
	struct obs_core_video *video = &obs->video;

	if (!obs || !video->graphics || !val)
		return false;

	pthread_mutex_lock(&video->video_thread_time_mutex);
	*val = video->video_thread_time;
	pthread_mutex_unlock(&video->video_thread_time_mutex);

	return true;
}


void obs_defer_graphics_cleanup(size_t num,
		struct obs_graphics_defer_cleanup *items)
{
	struct obs_core_video *video = &obs->video;

	if (video->thread_initialized) {
		pthread_mutex_lock(&video->deferred_cleanup.mutex);

		for (size_t i = 0; i < num; i++) {
			switch (items[i].type) {
			case OBS_CLEANUP_DEFER_TEXTURE:
				da_push_back(video->deferred_cleanup.textures, &items[i].ptr);
				break;
			case OBS_CLEANUP_DEFER_STAGESURF:
				da_push_back(video->deferred_cleanup.stagesurfs, &items[i].ptr);
				break;
			case OBS_CLEANUP_DEFER_TEXRENDER:
				da_push_back(video->deferred_cleanup.texrenders, &items[i].ptr);
				break;
			}
		}

		pthread_mutex_unlock(&video->deferred_cleanup.mutex);

	} else {
		obs_enter_graphics();

		for (size_t i = 0; i < num; i++) {
			switch (items[i].type) {
			case OBS_CLEANUP_DEFER_TEXTURE:
				gs_texture_destroy(items[i].ptr);
				break;
			case OBS_CLEANUP_DEFER_STAGESURF:
				gs_stagesurface_destroy(items[i].ptr);
				break;
			case OBS_CLEANUP_DEFER_TEXRENDER:
				gs_texrender_destroy(items[i].ptr);
				break;
			}
		}

		obs_leave_graphics();
	}
}

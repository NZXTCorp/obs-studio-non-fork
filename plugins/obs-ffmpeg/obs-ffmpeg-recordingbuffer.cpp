/******************************************************************************
    Copyright (C) 2016 by Ruwen Hahn <palana@stunned.de>
    based on obs-ffmpeg-mux.c

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

#include <obs-module.h>
#include <obs-avc.h>
#include <util/dstr.hpp>
#include <util/pipe.h>
#include <util/platform.h>
#include "ffmpeg-mux/ffmpeg-mux.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4244)
#endif
#include <libavformat/avformat.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

const auto settings_buffer_length_name = "buffer_length";

using namespace std;

namespace std {
template <>
struct default_delete<os_process_pipe_t> {
	void operator()(os_process_pipe_t *pipe)
	{
		os_process_pipe_destroy(pipe);
	}
};

template <>
struct default_delete<calldata_t> {
	void operator()(calldata_t *data)
	{
		calldata_free(data);
		delete data;
	}
};
}

#define do_log(level, format, ...) \
	blog(level, "[ffmpeg recordingbuffer: '%s'] " format, \
			obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)

#define LOCK(x) lock_guard<decltype(x)> lock ## __LINE__{x}

namespace {

struct ffmpeg_muxer;

}

static bool build_command_line(struct ffmpeg_muxer *stream, const dstr *path,
		struct dstr *cmd);
static bool write_packet(struct ffmpeg_muxer *stream, os_process_pipe_t *pipe,
		struct encoder_packet *packet);

namespace {

struct packets_segment {
	using data_t = std::vector<uint8_t>;
	using offset_t = data_t::size_type;
	vector<encoder_packet> pkts;
	vector<offset_t>       offsets;
	data_t                 data;
	bool                   finalized = false;

	int64_t                keyframe_pts;
	double                 first_pts;
	double                 last_pts;
	bool                   have_pts = false;

	void AddPacket(const encoder_packet &pkt)
	{
		if (finalized)
			return;

		pkts.push_back(pkt);
		offsets.push_back(data.size());
		data.insert(end(data), pkt.data, pkt.data + pkt.size);

		auto pkt_pts = static_cast<double>(pkt.pts) * pkt.timebase_num / pkt.timebase_den;

		if (!have_pts) {
			have_pts = true;
			keyframe_pts = pkt.pts;
			first_pts = pkt_pts;
			last_pts = pkt_pts;
			return;
		}

		first_pts = min(first_pts, pkt_pts);
		last_pts = max(last_pts, pkt_pts);
	}

	void Finalize()
	{
		if (finalized)
			return;

		for (size_t i = 0, size = pkts.size(); i < size; i++)
			pkts[i].data = data.data() + offsets[i];

		finalized = true;
	}

	double Length() const
	{
		return last_pts - first_pts;
	}
};

struct buffer_output;

struct ffmpeg_muxer {
	obs_output_t      *output;
	bool              have_headers = false;
	bool              active = false;
	bool              capturing = false;
	double            buffer_length = 60.;

	signal_handler_t  *signal;

	mutex             buffers_mutex;
	vector<packets_segment::data_t> buffers;

	mutex             buffer_mutex;

	packets_segment   encoder_headers;
	deque<shared_ptr<packets_segment>> payload_data;
	shared_ptr<packets_segment> current_segment;

	vector<unique_ptr<buffer_output>> outputs;
	vector<unique_ptr<buffer_output>> complete_outputs;
};

struct buffer_output {
	ffmpeg_muxer      *stream;
	unique_ptr<os_process_pipe_t> pipe;
	DStr              path;
	video_tracked_frame_id tracked_id;
	bool              keep_recording = false;
	double            keep_recording_time = 0.;
	double            save_duration = 0.;

	int64_t           end_pts;
	bool              wait_for_end_time = false;
	int64_t           end_dts;
	bool              wait_for_dts = false;

	packets_segment   &headers;
	vector<shared_ptr<packets_segment>> initial_segments;
	vector<shared_ptr<packets_segment>> new_segments;
	packets_segment   final_segment;

	thread            output_thread;
	mutex             output_mutex;
	condition_variable output_update;
	bool              finish_output = false;
	bool              exit_thread = false;

	atomic<bool>      thread_finished{};
	int               total_frames = 0;

	unique_ptr<calldata_t> signal_data;

	buffer_output(ffmpeg_muxer *stream, const char *path_,
			video_tracked_frame_id tracked_id=0, double save_duration=0.)
		: stream(stream),
		  tracked_id(tracked_id),
		  headers(stream->encoder_headers),
		  save_duration(save_duration)
	{
		dstr_copy(path, path_);

		signal_data.reset(new calldata_t{});
		calldata_init(signal_data.get());
		calldata_set_ptr(signal_data.get(), "output", stream->output);
		calldata_set_string(signal_data.get(), "filename", path);

		DStr escaped_path;
		dstr_copy_dstr(escaped_path, path);
		dstr_replace(escaped_path, "\"", "\"\""); //?

		DStr cmd;
		if (!build_command_line(stream, escaped_path, cmd)) {
			warn("Failed to build command line");
			thread_finished = true;

			SignalFailure();
			return;
		}

		pipe.reset(os_process_pipe_create(cmd->array, "w"));
		if (!pipe) {
			warn("Failed to create process pipe");
			thread_finished = true;

			SignalFailure();
			return;
		}

		finish_output = !tracked_id;

		initial_segments.assign(begin(stream->payload_data),
			end(stream->payload_data));

		output_thread = thread([=]()
		{
			OutputThread();
		});
	}

	~buffer_output()
	{
		if (!output_thread.joinable())
			return;

		NotifyThread([&]
		{
			exit_thread = true;
		});

		output_thread.join();
	}

	template <typename Fun>
	void NotifyThread(Fun &&fun)
	{
		{
			LOCK(output_mutex);
			fun();
		}
		output_update.notify_one();
	}

	bool NewPacket(const encoder_packet &pkt, const packets_segment &seg)
	{
		if (finish_output)
			return false;

		if (keep_recording && keep_recording_time <= 0)
			return true;

		if (pkt.type != OBS_ENCODER_VIDEO)
			return true;

		if (wait_for_dts && end_dts > pkt.dts)
			return true;
		else if (!wait_for_dts) {
			if (tracked_id != pkt.tracked_id && (!wait_for_end_time || pkt.pts < end_pts))
				return true;
			if (keep_recording && !wait_for_end_time) {
				wait_for_end_time = true;
				end_pts = pkt.pts + static_cast<int64_t>(keep_recording_time * pkt.timebase_den / pkt.timebase_num);
				return true;
			}
			if (pkt.dts < pkt.pts) {
				wait_for_dts = true;
				end_dts = pkt.pts;
				return true;
			}
		}

		final_segment = seg;

		NotifyThread([&]
		{
			finish_output = true;
		});
		return false;
	}

	void AppendSegment(const shared_ptr<packets_segment> &seg)
	{
		if (finish_output)
			return;

		new_segments.push_back(seg);
	}

private:
	using stream_id_t = std::pair<obs_encoder_type, decltype(encoder_packet::track_idx)>;
	using first_stream_packet_t = std::map<stream_id_t, encoder_packet>;

	packets_segment *first_output_segment = nullptr;
	packets_segment *last_output_segment = nullptr;

	void RebaseTimestamp(encoder_packet &pkt,
			first_stream_packet_t *first_packets=nullptr)
	{
		if (!first_packets)
			return;

		// This can potentially introduce a minor desync
		// but then libobs behaves similarly, so the
		// desync shouldn't be noticable

		auto id = make_pair(pkt.type, pkt.track_idx);
		auto idx = first_packets->find(id);

		if (idx == end(*first_packets))
			idx = first_packets->emplace(id, pkt).first;

		if (idx == end(*first_packets))
			return;

		pkt.dts -= idx->second.dts;
		pkt.pts -= idx->second.dts;
	}

	bool OutputPackets(packets_segment &seg,
			first_stream_packet_t *first_packets=nullptr)
	{
		seg.Finalize();

		if (!first_output_segment)
			first_output_segment = &seg;

		last_output_segment = &seg;

		for (auto pkt : seg.pkts) {
			RebaseTimestamp(pkt, first_packets);
				
			if (!write_packet(stream, pipe.get(), &pkt))
				return false;

			if (pkt.type == OBS_ENCODER_VIDEO)
				total_frames += 1;
		}

		return true;
	}

	bool OutputSegments(const vector<shared_ptr<packets_segment>> &segments,
			first_stream_packet_t *first_packets=nullptr)
	{
		for (auto &seg : segments)
			if (!OutputPackets(*seg, first_packets))
				return false;

		return true;
	}

	bool WriteLimitedOutput()
	{
		first_stream_packet_t first_packets;

		auto find_and_output = [&](vector<shared_ptr<packets_segment>> &seg)
		{
			auto it = begin(seg);
			auto end_ = end(seg);
			if (first_packets.empty()) {
				for (; it != end_; it++) {
					if ((final_segment.last_pts - (*it)->last_pts) < save_duration)
						break;
				}
			}

			for (; it != end_; it++) {
				if (!OutputPackets(**it, &first_packets))
					return false;
			}

			return true;
		};

		if (!find_and_output(initial_segments)) {
			warn("Failed to write limited initial segments");
			return false;
		}

		if (!find_and_output(new_segments)) {
			warn("Failed to write limited new segments");
			return false;
		}

		if (!OutputPackets(final_segment, &first_packets)) {
			warn("Failed to write limited final segments");
			return false;
		}

		return true;
	}

	bool WriteOutput()
	{
		first_stream_packet_t first_packets;

		for (auto &pkt : headers.pkts) {
			if (!write_packet(stream, pipe.get(), &pkt)) {
				warn("Failed to write headers");
				return false;
			}
		}

		bool write_all_segments = save_duration < 1.;
		if (write_all_segments && !OutputSegments(initial_segments, &first_packets)) {
			warn("Failed to write initial segments");
			finish_output = false;
			return false;
		}

		{
			unique_lock<decltype(output_mutex)> lock(output_mutex);
			output_update.wait(lock, [&]
			{
				return exit_thread || finish_output;
			});

			if (exit_thread)
				return false;
		}

		if (!write_all_segments)
			return WriteLimitedOutput();
		
		if (!OutputSegments(new_segments, &first_packets)) {
			warn("Failed to write new segments");
			return false;
		}


		if (!OutputPackets(final_segment, &first_packets)) {
			warn("Failed to write final segment");
			return false;
		}

		return true;
	}

	int64_t GetStartPTS()
	{
		if (first_output_segment)
			return first_output_segment->keyframe_pts;

		if (initial_segments.size())
			return initial_segments.front()->keyframe_pts;
		if (new_segments.size())
			return new_segments.front()->keyframe_pts;
		return final_segment.keyframe_pts;
	}

	double CalculateDuration()
	{
		if (first_output_segment && last_output_segment)
			return first_output_segment->first_pts - last_output_segment->last_pts;

		auto start = DBL_MAX;
		auto end_ = 0.;

		auto update = [&](double val)
		{
			start = min(start, val);
			end_ = max(end_, val);
		};

		if (initial_segments.size())
			update(initial_segments.front()->first_pts);
		if (new_segments.size())
			update(new_segments.back()->first_pts);
		if (final_segment.pkts.size())
			update(final_segment.last_pts);

		return end_ - start;
	}

	void OutputThread()
	{
		if (WriteOutput()) {
			auto start_pts = GetStartPTS();
			auto duration = CalculateDuration();

			calldata_set_int(signal_data.get(), "frames", total_frames);
			calldata_set_int(signal_data.get(), "start_pts", start_pts);
			calldata_set_float(signal_data.get(), "duration", duration);

			if (tracked_id)
				calldata_set_int(signal_data.get(), "tracked_frame_id", tracked_id);

			signal_handler_signal(stream->signal,
					"buffer_output_finished", signal_data.get());
		} else {
			os_unlink(path);

			SignalFailure();
		}

		thread_finished = true;
	}

	void SignalFailure()
	{
		signal_handler_signal(stream->signal,
				"buffer_output_failed", signal_data.get());
	}
};

}

static const char *ffmpeg_mux_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegMuxer");
}

static void ffmpeg_mux_destroy(void *data)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);
	delete stream;
}

static void output_buffer_handler(void *data, calldata_t *calldata)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);
	DStr filename;
	dstr_copy(filename, calldata_string(calldata, "filename"));

	LOCK(stream->buffer_mutex);
	stream->complete_outputs.emplace_back(
			new buffer_output{stream, filename});
}

static void output_precise_buffer_handler(void *data, calldata_t *calldata)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);
	double duration = calldata_float(calldata, "save_duration");
	DStr filename;
	dstr_copy(filename, calldata_string(calldata, "filename"));

	LOCK(stream->buffer_mutex);
	auto frame_id = obs_track_next_frame();
	stream->outputs.emplace_back(
			new buffer_output{stream, filename, frame_id, duration});

	calldata_set_int(calldata, "tracked_frame_id", frame_id);
}

static void output_precise_buffer_and_keep_recording_handler(
		void *data, calldata_t *calldata)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);
	DStr filename;
	dstr_copy(filename, calldata_string(calldata, "filename"));

	LOCK(stream->buffer_mutex);
	auto frame_id = obs_track_next_frame();
	stream->outputs.emplace_back(
			new buffer_output{stream, filename, frame_id});
	auto &out = stream->outputs.back();
	out->keep_recording = true;
	out->keep_recording_time = calldata_float(calldata, "extra_recording_duration");

	calldata_set_int(calldata, "tracked_frame_id", frame_id);
}

static void *ffmpeg_mux_create(obs_data_t *settings, obs_output_t *output)
{
	auto stream = new ffmpeg_muxer;
	stream->output = output;
	stream->buffer_length = obs_data_get_double(settings, settings_buffer_length_name);

	if (stream->buffer_length <= 1) {
		warn("Supplied length (%g) is less than 1 second, using 1 second instead", stream->buffer_length);
		stream->buffer_length = 1.;
	}

	auto proc = obs_output_get_proc_handler(output);
	proc_handler_add(proc, "void output_buffer(string filename)",
			output_buffer_handler, stream);
	proc_handler_add(proc, "void output_precise_buffer(string filename, "
			"float save_duration, "
			"out int tracked_frame_id)",
			output_precise_buffer_handler, stream);
	proc_handler_add(proc, "void output_precise_buffer_and_keep_recording(string filename, "
			"out int tracked_frame_id, float extra_recording_duration)",
			output_precise_buffer_and_keep_recording_handler, stream);

	auto signal = obs_output_get_signal_handler(output);
	signal_handler_add(signal,
			"void buffer_output_finished(ptr output, string filename, "
			"int frames, float duration, int start_pts, int tracked_frame_id)");
	signal_handler_add(signal,
			"void buffer_output_failed(ptr output, string filename)");
	stream->signal = signal;

	UNUSED_PARAMETER(settings);
	return stream;
}

#ifdef _WIN32
#ifdef _WIN64
#define FFMPEG_MUX "ffmpeg-mux64.exe"
#else
#define FFMPEG_MUX "ffmpeg-mux32.exe"
#endif
#else
#define FFMPEG_MUX "ffmpeg-mux"
#endif

/* TODO: allow codecs other than h264 whenever we start using them */

static bool add_video_encoder_params(struct ffmpeg_muxer *stream,
		struct dstr *cmd, obs_encoder_t *vencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(vencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	video_t *video = obs_get_video();
	const struct video_output_info *info = video_output_get_info(video);

	obs_data_release(settings);

	if (!info)
		return false;

	dstr_catf(cmd, "%s %d %d %d %d %d ",
			"h264",
			bitrate,
			obs_output_get_width(stream->output),
			obs_output_get_height(stream->output),
			(int)info->fps_num,
			(int)info->fps_den);

	return true;
}

static bool add_audio_encoder_params(struct dstr *cmd, obs_encoder_t *aencoder)
{
	obs_data_t *settings = obs_encoder_get_settings(aencoder);
	int bitrate = (int)obs_data_get_int(settings, "bitrate");
	audio_t *audio = obs_get_audio();
	struct dstr name = {0};

	obs_data_release(settings);

	if (!audio)
		return false;

	dstr_copy(&name, obs_encoder_get_name(aencoder));
	dstr_replace(&name, "\"", "\"\"");

	dstr_catf(cmd, "\"%s\" %d %d %d ",
			name.array,
			bitrate,
			(int)obs_encoder_get_sample_rate(aencoder),
			(int)audio_output_get_channels(audio));

	dstr_free(&name);

	return true;
}

#ifdef _MSC_VER
#pragma warning(disable:4706)
#endif

static void log_muxer_params(struct ffmpeg_muxer *stream, const char *settings)
{
	int ret;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, settings, "=", " ", 0))) {
		char str[AV_ERROR_MAX_STRING_SIZE];
		warn("Failed to parse muxer settings: %s\n%s",
				av_make_error_string(str,
					AV_ERROR_MAX_STRING_SIZE, ret),
				settings);

		av_dict_free(&dict);
		return;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = {0};

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
						AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		info("Using muxer settings:%s", str.array);
		dstr_free(&str);
	}

	av_dict_free(&dict);
}

static void add_muxer_params(struct dstr *cmd, struct ffmpeg_muxer *stream)
{
	obs_data_t *settings = obs_output_get_settings(stream->output);
	struct dstr mux = {0};

	dstr_copy(&mux, obs_data_get_string(settings, "muxer_settings"));

	log_muxer_params(stream, mux.array);

	dstr_replace(&mux, "\"", "\\\"");
	obs_data_release(settings);

	dstr_catf(cmd, "\"%s\" ", mux.array ? mux.array : "");

	dstr_free(&mux);
}

static bool build_command_line(struct ffmpeg_muxer *stream, const dstr *path,
		struct dstr *cmd)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
	obs_encoder_t *aencoders[MAX_AUDIO_MIXES];
	int num_tracks = 0;

	for (;;) {
		obs_encoder_t *aencoder = obs_output_get_audio_encoder(
				stream->output, num_tracks);
		if (!aencoder)
			break;

		aencoders[num_tracks] = aencoder;
		num_tracks++;
	}

	dstr_init_move_array(cmd, obs_module_file(FFMPEG_MUX));
	dstr_insert_ch(cmd, 0, '\"');
	dstr_cat(cmd, "\" \"");
	dstr_cat_dstr(cmd, path);
	dstr_catf(cmd, "\" %d %d ", vencoder ? 1 : 0, num_tracks);

	if (vencoder && !add_video_encoder_params(stream, cmd, vencoder))
		return false;

	if (num_tracks) {
		dstr_cat(cmd, "aac ");

		for (int i = 0; i < num_tracks; i++) {
			if (!add_audio_encoder_params(cmd, aencoders[i]))
				return false;
		}
	}

	add_muxer_params(cmd, stream);

	return true;
}

static bool ffmpeg_mux_start(void *data)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	/* write headers and start capture */
	stream->active = true;
	stream->capturing = true;
	obs_output_begin_data_capture(stream->output, 0);

	return true;
}

static int deactivate(struct ffmpeg_muxer *stream)
{
	int ret = -1;

	if (stream->active) {
		stream->outputs.clear();
		stream->complete_outputs.clear();
		stream->active = false;
		stream->have_headers = false;

		info("stopped buffering");
	}

	return ret;
}

static void ffmpeg_mux_stop(void *data)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);

	if (stream->capturing) {
		obs_output_end_data_capture(stream->output);
		stream->capturing = false;
	}

	deactivate(stream);
}

static void signal_failure(struct ffmpeg_muxer *stream)
{
	int ret = deactivate(stream);
	int code;

	switch (ret) {
	case FFM_UNSUPPORTED:          code = OBS_OUTPUT_UNSUPPORTED; break;
	default:                       code = OBS_OUTPUT_ERROR;
	}

	obs_output_signal_stop(stream->output, code);
	stream->capturing = false;
}

static bool write_packet(struct ffmpeg_muxer *stream, os_process_pipe_t *pipe,
		struct encoder_packet *packet)
{
	bool is_video = packet->type == OBS_ENCODER_VIDEO;
	size_t ret;

	struct ffm_packet_info info;
	info.pts = packet->pts;
	info.dts = packet->dts;
	info.size = (uint32_t)packet->size;
	info.index = (int)packet->track_idx;
	info.type = is_video ? FFM_PACKET_VIDEO : FFM_PACKET_AUDIO;
	info.keyframe = packet->keyframe;

	if (packet->tracked_id)
		blog(LOG_INFO, "writing tracked packet %lld (%lld)", packet->pts,
				packet->tracked_id);

	ret = os_process_pipe_write(pipe, (const uint8_t*)&info,
			sizeof(info));
	if (ret != sizeof(info)) {
		warn("os_process_pipe_write for info structure failed");
		signal_failure(stream);
		return false;
	}

	ret = os_process_pipe_write(pipe, packet->data, packet->size);
	if (ret != packet->size) {
		warn("os_process_pipe_write for packet data failed");
		signal_failure(stream);
		return false;
	}

	return true;
}

static void gather_video_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);

	encoder_packet packet{};
	packet.type         = OBS_ENCODER_VIDEO;
	packet.timebase_den = 1;

	obs_encoder_get_extra_data(vencoder, &packet.data, &packet.size);

	stream->encoder_headers.AddPacket(packet);
}

static void gather_audio_headers(struct ffmpeg_muxer *stream,
		obs_encoder_t *aencoder, size_t idx)
{
	struct encoder_packet packet{};
	packet.type         = OBS_ENCODER_AUDIO;
	packet.timebase_den = 1;
	packet.track_idx    = idx;

	obs_encoder_get_extra_data(aencoder, &packet.data, &packet.size);

	stream->encoder_headers.AddPacket(packet);
}

static void gather_headers(struct ffmpeg_muxer *stream)
{
	obs_encoder_t *aencoder;
	size_t idx = 0;

	gather_video_headers(stream);

	do {
		aencoder = obs_output_get_audio_encoder(stream->output, idx);
		if (aencoder) {
			gather_audio_headers(stream, aencoder, idx);
			idx++;
		}
	} while (aencoder);

	stream->encoder_headers.Finalize();
}

/*static bool send_headers(ffmpeg_muxer *stream)
{
	stream->encoder_headers.Finalize();

	for (auto &pkt : stream->encoder_headers.pkts)
		if (!write_packet(stream, &pkt))
			return false;

	return true;
}*/

static double interval(const packets_segment &oldest, const packets_segment &youngest)
{
	return youngest.last_pts - oldest.first_pts;
}

static void prune_old_segments(ffmpeg_muxer *stream)
{
	if (stream->payload_data.empty())
		return;

	if (interval(*stream->payload_data.front(), *stream->current_segment)
			- stream->current_segment->Length() < stream->buffer_length)
		return;

	stream->payload_data.pop_front();
}

template <typename T, typename D, typename ... Ts>
static shared_ptr<T> make_shared_deleter(D &&d, Ts &&... ts)
{
	return {new T{forward<Ts>(ts)...}, forward<D>(d)};
}

static void push_buffer(ffmpeg_muxer *stream, packets_segment::data_t &&data)
{
	if (!data.capacity())
		return;

	data.clear();

	LOCK(stream->buffers_mutex);
	stream->buffers.emplace_back(move(data));
}

static packets_segment::data_t pop_buffer(ffmpeg_muxer *stream)
{
	LOCK(stream->buffers_mutex);
	if (stream->buffers.empty())
		return {};

	auto buffer = move(stream->buffers.back());
	stream->buffers.pop_back();

	return buffer;
}

static shared_ptr<packets_segment> create_segment(ffmpeg_muxer *stream)
{
	auto seg = make_shared_deleter<packets_segment>([&, stream](packets_segment *seg)
	{
		push_buffer(stream, move(seg->data));
		delete seg;
	});

	seg->data = pop_buffer(stream);

	return seg;
}

static void ffmpeg_mux_data(void *data, struct encoder_packet *packet)
{
	auto stream = static_cast<ffmpeg_muxer*>(data);

	if (!stream->active)
		return;

	LOCK(stream->buffer_mutex);

	if (!stream->have_headers) {
		gather_headers(stream);

		stream->have_headers = true;
	}

	if (!stream->current_segment)
		stream->current_segment = create_segment(stream);

	if (packet->keyframe) {
		prune_old_segments(stream);

		for (auto &output : stream->outputs)
			output->AppendSegment(stream->current_segment);

		if (!stream->current_segment->pkts.empty())
			stream->payload_data.emplace_back(move(stream->current_segment));

		stream->current_segment = create_segment(stream);
	}

	stream->current_segment->AddPacket(*packet);

	for (size_t i = 0; i < stream->outputs.size();) {
		auto &output = stream->outputs[i];
		if (!output->NewPacket(*packet, *stream->current_segment)) {
			stream->complete_outputs.emplace_back(move(output));
			stream->outputs.erase(begin(stream->outputs) + i);
		} else
			i += 1;
	}

	for (size_t i = 0; i < stream->complete_outputs.size();) {
		if (stream->complete_outputs[i]->thread_finished)
			stream->complete_outputs.erase(
					begin(stream->complete_outputs) + i);
		else
			i += 1;
	}
}

static obs_properties_t *ffmpeg_mux_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "path",
			obs_module_text("FilePath"),
			OBS_TEXT_DEFAULT);
	return props;
}

static void ffmpeg_mux_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, settings_buffer_length_name, 60.);
}

extern "C" void register_recordingbuffer(void)
{
	obs_output_info ffmpeg_recordingbuffer{};
	ffmpeg_recordingbuffer.id             = "ffmpeg_recordingbuffer";
	ffmpeg_recordingbuffer.flags          = OBS_OUTPUT_AV |
	                                        OBS_OUTPUT_ENCODED |
	                                        OBS_OUTPUT_MULTI_TRACK;
	ffmpeg_recordingbuffer.get_name       = ffmpeg_mux_getname;
	ffmpeg_recordingbuffer.create         = ffmpeg_mux_create;
	ffmpeg_recordingbuffer.destroy        = ffmpeg_mux_destroy;
	ffmpeg_recordingbuffer.start          = ffmpeg_mux_start;
	ffmpeg_recordingbuffer.stop           = ffmpeg_mux_stop;
	ffmpeg_recordingbuffer.encoded_packet = ffmpeg_mux_data;
	ffmpeg_recordingbuffer.get_properties = ffmpeg_mux_properties;
	ffmpeg_recordingbuffer.get_defaults   = ffmpeg_mux_defaults;

	obs_register_output(&ffmpeg_recordingbuffer);
};

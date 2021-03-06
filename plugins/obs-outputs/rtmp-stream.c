/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

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

#include "rtmp-stream.h"

static const char *rtmp_stream_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RTMPStream");
}

static void log_rtmp(int level, const char *format, va_list args)
{
	if (level > RTMP_LOGWARNING)
		return;

	blogva(LOG_INFO, format, args);
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream);

static inline void free_packets(struct rtmp_stream *stream)
{
	size_t num_packets;

	pthread_mutex_lock(&stream->packets_mutex);

	num_packets = num_buffered_packets(stream);
	if (num_packets)
		info("Freeing %d remaining packets", (int)num_packets);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));
		obs_free_encoder_packet(&packet);
	}
	pthread_mutex_unlock(&stream->packets_mutex);
}

static inline bool stopping(struct rtmp_stream *stream)
{
	return os_event_try(stream->stop_event) != EAGAIN;
}

static inline bool connecting(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->connecting);
}

static inline bool active(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->active);
}

static inline bool disconnected(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->disconnected);
}

static void rtmp_stream_destroy(void *data)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream) && !connecting(stream)) {
		pthread_join(stream->send_thread, NULL);

	} else if (connecting(stream) || active(stream)) {
		if (stream->connecting)
			pthread_join(stream->connect_thread, NULL);

		os_event_signal(stream->stop_event);

		if (active(stream)) {
			os_sem_post(stream->send_sem);
			obs_output_end_data_capture(stream->output);
			pthread_join(stream->send_thread, NULL);
		}
	}

	if (stream) {
		free_packets(stream);
		dstr_free(&stream->path);
		dstr_free(&stream->key);
		dstr_free(&stream->username);
		dstr_free(&stream->password);
		dstr_free(&stream->encoder_name_suffix);
		dstr_free(&stream->encoder_name);
		dstr_free(&stream->bind_ip);
		os_event_destroy(stream->stop_event);
		os_sem_destroy(stream->send_sem);
		pthread_mutex_destroy(&stream->packets_mutex);
		circlebuf_free(&stream->packets);

		os_event_destroy(stream->buffer_space_available_event);
		os_event_destroy(stream->buffer_has_data_event);
		os_event_destroy(stream->socket_available_event);
		os_event_destroy(stream->send_thread_signaled_exit);
		pthread_mutex_destroy(&stream->write_buf_mutex);

		circlebuf_free(&stream->packet_strain);
		circlebuf_free(&stream->sizes_sent);
		pthread_mutex_destroy(&stream->packet_strain_mutex);

#ifdef TEST_FRAMEDROPS
		circlebuf_free(&stream->droptest_info);
#endif
		if (stream->write_buf)
			bfree(stream->write_buf);
		bfree(stream);
	}
}

static void *rtmp_stream_create(obs_data_t *settings, obs_output_t *output)
{
	struct rtmp_stream *stream = bzalloc(sizeof(struct rtmp_stream));
	stream->output = output;
	pthread_mutex_init_value(&stream->packets_mutex);

	RTMP_Init(&stream->rtmp);
	RTMP_LogSetCallback(log_rtmp);
	RTMP_LogSetLevel(RTMP_LOGWARNING);

	if (pthread_mutex_init(&stream->packets_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&stream->stop_event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;

	if (pthread_mutex_init(&stream->write_buf_mutex, NULL) != 0) {
		warn("Failed to initialize write buffer mutex");
		goto fail;
	}

	if (pthread_mutex_init(&stream->packet_strain_mutex, NULL) != 0) {
		warn("Failed to initialize packet strain mutex");
		goto fail;
	}

	if (os_event_init(&stream->buffer_space_available_event,
		OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize write buffer event");
		goto fail;
	}
	if (os_event_init(&stream->buffer_has_data_event,
		OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize data buffer event");
		goto fail;
	}
	if (os_event_init(&stream->socket_available_event,
		OS_EVENT_TYPE_AUTO) != 0) {
		warn("Failed to initialize socket buffer event");
		goto fail;
	}
	if (os_event_init(&stream->send_thread_signaled_exit,
		OS_EVENT_TYPE_MANUAL) != 0) {
		warn("Failed to initialize socket exit event");
		goto fail;
	}

	UNUSED_PARAMETER(settings);
	return stream;

fail:
	rtmp_stream_destroy(stream);
	return NULL;
}

static void rtmp_stream_stop(void *data)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream))
		return;

	if (connecting(stream))
		pthread_join(stream->connect_thread, NULL);

	os_event_signal(stream->stop_event);

	if (active(stream)) {
		os_sem_post(stream->send_sem);
		obs_output_end_data_capture(stream->output);
	}
}

static inline void set_rtmp_str(AVal *val, const char *str)
{
	bool valid  = (str && *str);
	val->av_val = valid ? (char*)str       : NULL;
	val->av_len = valid ? (int)strlen(str) : 0;
}

static inline void set_rtmp_dstr(AVal *val, struct dstr *str)
{
	bool valid  = !dstr_is_empty(str);
	val->av_val = valid ? str->array    : NULL;
	val->av_len = valid ? (int)str->len : 0;
}

static inline bool get_next_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	bool new_packet = false;

	pthread_mutex_lock(&stream->packets_mutex);
	if (stream->packets.size) {
		circlebuf_pop_front(&stream->packets, packet,
				sizeof(struct encoder_packet));
		new_packet = true;
	}
	pthread_mutex_unlock(&stream->packets_mutex);

	return new_packet;
}

static bool discard_recv_data(struct rtmp_stream *stream, size_t size)
{
	RTMP *rtmp = &stream->rtmp;
	uint8_t buf[512];
#ifdef _WIN32
	int ret;
#else
	ssize_t ret;
#endif

	do {
		size_t bytes = size > 512 ? 512 : size;
		size -= bytes;

#ifdef _WIN32
		ret = recv(rtmp->m_sb.sb_socket, buf, (int)bytes, 0);
#else
		ret = recv(rtmp->m_sb.sb_socket, buf, bytes, 0);
#endif

		if (ret <= 0) {
#ifdef _WIN32
			int error = WSAGetLastError();
#else
			int error = errno;
#endif
			if (ret < 0) {
				do_log(LOG_ERROR, "recv error: %d (%d bytes)",
						error, (int)size);
			}
			return false;
		}
	} while (size > 0);

	return true;
}

#ifdef TEST_FRAMEDROPS
static void droptest_cap_data_rate(struct rtmp_stream *stream, size_t size)
{
	uint64_t ts = os_gettime_ns();
	struct droptest_info info;

	info.ts = ts;
	info.size = size;

	circlebuf_push_back(&stream->droptest_info, &info, sizeof(info));
	stream->droptest_size += size;

	if (stream->droptest_info.size) {
		circlebuf_peek_front(&stream->droptest_info,
				&info, sizeof(info));

		if (stream->droptest_size > DROPTEST_MAX_BYTES) {
			uint64_t elapsed = ts - info.ts;

			if (elapsed < 1000000000ULL) {
				elapsed = 1000000000ULL - elapsed;
				os_sleepto_ns(ts + elapsed);
			}

			while (stream->droptest_size > DROPTEST_MAX_BYTES) {
				circlebuf_pop_front(&stream->droptest_info,
						&info, sizeof(info));
				stream->droptest_size -= info.size;
			}
		}
	}
}
#endif

struct packet_strain_data {
	uint64_t time;
	union {
		int len;
		float strain;
	};
};

static void prune_packets_sent(struct circlebuf *buf)
{
	if (!buf->size)
		return;

	struct packet_strain_data last;
	circlebuf_peek_back(buf, &last, sizeof(last));

	while (buf->size) {
		struct packet_strain_data front;
		circlebuf_peek_front(buf, &front, sizeof(front));
		if ((buf->size > (sizeof(last) * 2)) && (last.time - front.time) > 1000000000)
			circlebuf_pop_front(buf, &front, sizeof(front));
		else
			break;
	}
}

void update_packets_sent(struct rtmp_stream *stream, int sent)
{
	if (!stream->autotune)
		return;

	struct packet_strain_data data = {
		os_gettime_ns()
	};
	data.len = sent;

	pthread_mutex_lock(&stream->packet_strain_mutex);
	circlebuf_push_back(&stream->sizes_sent, &data, sizeof(data));
	prune_packets_sent(&stream->sizes_sent);
	pthread_mutex_unlock(&stream->packet_strain_mutex);
}

static float sent_size(struct rtmp_stream *stream)
{
	pthread_mutex_lock(&stream->packet_strain_mutex);
	if (!stream->sizes_sent.size) {
		pthread_mutex_unlock(&stream->packet_strain_mutex);
		return 0.;
	}

	double sent = 0.;

	size_t elems = stream->sizes_sent.size / sizeof(struct packet_strain_data);
	struct packet_strain_data *data = stream->sizes_sent.data;
	for (size_t i = 0; i < elems; i++)
		sent += data[i].len;

	sent /= (data[elems - 1].time - data[0].time) / 1000000000.;
	pthread_mutex_unlock(&stream->packet_strain_mutex);

	return sent;
}

static float compute_strain(struct rtmp_stream *stream)
{
	pthread_mutex_lock(&stream->packet_strain_mutex);
	if (!stream->packet_strain.size) {
		pthread_mutex_unlock(&stream->packet_strain_mutex);
		return 0.;
	}

	double strain = 0.;

	size_t elems = stream->packet_strain.size / sizeof(struct packet_strain_data);
	struct packet_strain_data *data = stream->packet_strain.data;
	for (size_t i = 0; i < elems; i++)
		strain += data[i].strain;

	pthread_mutex_unlock(&stream->packet_strain_mutex);

	return strain / elems;
}

void update_packet_strain(struct rtmp_stream *stream)
{
	if (!stream->autotune)
		return;

	pthread_mutex_lock(&stream->packet_strain_mutex);
	float strain = stream->write_buf_len / (double)stream->write_buf_size;

	struct packet_strain_data data = {
		os_gettime_ns()
	};
	data.strain = strain;

	circlebuf_push_back(&stream->packet_strain, &data, sizeof(data));
	prune_packets_sent(&stream->packet_strain);
	pthread_mutex_unlock(&stream->packet_strain_mutex);
}

static int socket_queue_data(RTMPSockBuf *sb, const char *data, int len, void *arg)
{
	struct rtmp_stream *stream = arg;

retry_send:

	if (!RTMP_IsConnected(&stream->rtmp))
		return 0;

	pthread_mutex_lock(&stream->write_buf_mutex);

	if (stream->write_buf_len + len > stream->write_buf_size) {

		pthread_mutex_unlock(&stream->write_buf_mutex);

		if (os_event_wait(stream->buffer_space_available_event)) {
			return 0;
		}

		goto retry_send;
	}

	memcpy(stream->write_buf + stream->write_buf_len, data, len);
	stream->write_buf_len += len;

	update_packet_strain(stream);

	pthread_mutex_unlock(&stream->write_buf_mutex);

	os_event_signal (stream->buffer_has_data_event);

	return len;
}

static int send_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet, bool is_header, size_t idx)
{
	uint8_t *data;
	size_t  size;
	int     recv_size = 0;
	int     ret = 0;

	if (!stream->new_socket_loop) {
#ifdef _WIN32
		ret = ioctlsocket(stream->rtmp.m_sb.sb_socket, FIONREAD,
				(u_long*)&recv_size);
#else
		ret = ioctl(stream->rtmp.m_sb.sb_socket, FIONREAD, &recv_size);
#endif

		if (ret >= 0 && recv_size > 0) {
			if (!discard_recv_data(stream, (size_t)recv_size))
				return -1;
		}
	}

	flv_packet_mux(packet, &data, &size, is_header);

#ifdef TEST_FRAMEDROPS
	droptest_cap_data_rate(stream, size);
#endif

	ret = RTMP_Write(&stream->rtmp, (char*)data, (int)size, (int)idx);
	bfree(data);

	obs_free_encoder_packet(packet);

	stream->total_bytes_sent += size;
	return ret;
}

static inline bool send_headers(struct rtmp_stream *stream);

static bool send_remaining_packets(struct rtmp_stream *stream)
{
	struct encoder_packet packet;
	uint64_t max_ns = (uint64_t)stream->max_shutdown_time_sec * 1000000000;
	uint64_t begin_time_ns = os_gettime_ns();

	if (!stream->sent_headers) {
		if (!send_headers(stream))
			return false;
	}

	while (get_next_packet(stream, &packet)) {
		if (send_packet(stream, &packet, false, packet.track_idx) < 0)
			return false;

		/* Just disconnect if it takes too long to shut down */
		if ((os_gettime_ns() - begin_time_ns) > max_ns) {
			info("Took longer than %d second(s) to shut down, "
			     "automatically stopping connection",
			     stream->max_shutdown_time_sec);
			return false;
		}
	}

	return true;
}

static void *send_thread(void *data)
{
	struct rtmp_stream *stream = data;

	os_set_thread_name("rtmp-stream: send_thread");

	while (os_sem_wait(stream->send_sem) == 0) {
		struct encoder_packet packet;

		if (stopping(stream))
			break;
		if (!get_next_packet(stream, &packet))
			continue;

		if (!stream->sent_headers) {
			if (!send_headers(stream)) {
				os_atomic_set_bool(&stream->disconnected, true);
				break;
			}
		}

		if (send_packet(stream, &packet, false, packet.track_idx) < 0) {
			os_atomic_set_bool(&stream->disconnected, true);
			break;
		}
	}

	if (!disconnected(stream) && !send_remaining_packets(stream))
		os_atomic_set_bool(&stream->disconnected, true);

	if (disconnected(stream)) {
		info("Disconnected from %s", stream->path.array);
		free_packets(stream);
	} else {
		info("User stopped the stream");
	}

	if (stream->new_socket_loop) {
		os_event_signal(stream->send_thread_signaled_exit);
		os_event_signal(stream->buffer_has_data_event);
		pthread_join(stream->socket_thread, NULL);
		stream->socket_thread_active = false;
		stream->rtmp.m_bCustomSend = false;
	}

	RTMP_Close(&stream->rtmp);

	if (!stopping(stream)) {
		pthread_detach(stream->send_thread);
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
	}

	os_event_reset(stream->stop_event);
	os_atomic_set_bool(&stream->active, false);
	stream->sent_headers = false;
	return NULL;
}

static bool send_meta_data(struct rtmp_stream *stream, size_t idx, bool *next)
{
	uint8_t *meta_data;
	size_t  meta_data_size;
	bool    success = true;

	*next = flv_meta_data(stream->output, &meta_data,
			&meta_data_size, false, idx);

	if (*next) {
		success = RTMP_Write(&stream->rtmp, (char*)meta_data,
				(int)meta_data_size, (int)idx) >= 0;
		bfree(meta_data);
	}

	return success;
}

static bool send_audio_header(struct rtmp_stream *stream, size_t idx,
		bool *next)
{
	obs_output_t  *context  = stream->output;
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, idx);
	uint8_t       *header;

	struct encoder_packet packet   = {
		.type         = OBS_ENCODER_AUDIO,
		.timebase_den = 1
	};

	if (!aencoder) {
		*next = false;
		return true;
	}

	obs_encoder_get_extra_data(aencoder, &header, &packet.size);
	packet.data = bmemdup(header, packet.size);
	return send_packet(stream, &packet, true, idx) >= 0;
}

static bool send_video_header(struct rtmp_stream *stream)
{
	obs_output_t  *context  = stream->output;
	obs_encoder_t *vencoder = obs_output_get_video_encoder(context);
	uint8_t       *header;
	size_t        size;

	struct encoder_packet packet   = {
		.type         = OBS_ENCODER_VIDEO,
		.timebase_den = 1,
		.keyframe     = true
	};

	obs_encoder_get_extra_data(vencoder, &header, &size);
	packet.size = obs_parse_avc_header(&packet.data, header, size);
	return send_packet(stream, &packet, true, 0) >= 0;
}

static inline bool send_headers(struct rtmp_stream *stream)
{
	stream->sent_headers = true;
	size_t i = 0;
	bool next = true;
	bool fail = false;

	if (!send_audio_header(stream, i++, &next))
		return false;
	if (!send_video_header(stream))
		return false;

	while (next) {
		if (!send_audio_header(stream, i++, &next))
			return false;
	}

	return true;
}

static inline bool reset_semaphore(struct rtmp_stream *stream)
{
	os_sem_destroy(stream->send_sem);
	return os_sem_init(&stream->send_sem, 0) == 0;
}

#ifdef _WIN32
#define socklen_t int
#endif

#define MIN_SENDBUF_SIZE 65535

static void adjust_sndbuf_size(struct rtmp_stream *stream, int new_size)
{
	int cur_sendbuf_size = new_size;
	socklen_t int_size = sizeof(int);

	getsockopt(stream->rtmp.m_sb.sb_socket, SOL_SOCKET, SO_SNDBUF,
			(char*)&cur_sendbuf_size, &int_size);

	if (cur_sendbuf_size < new_size) {
		cur_sendbuf_size = new_size;
		setsockopt(stream->rtmp.m_sb.sb_socket, SOL_SOCKET, SO_SNDBUF,
				(const char*)&cur_sendbuf_size, int_size);
	}
}

static int init_send(struct rtmp_stream *stream)
{
	int ret;
	size_t idx = 0;
	bool next = true;

#if defined(_WIN32)
	adjust_sndbuf_size(stream, MIN_SENDBUF_SIZE);
#endif

	reset_semaphore(stream);

	ret = pthread_create(&stream->send_thread, NULL, send_thread, stream);
	if (ret != 0) {
		RTMP_Close(&stream->rtmp);
		warn("Failed to create send thread");
		return OBS_OUTPUT_ERROR;
	}

	if (stream->new_socket_loop) {
		int one = 1;
#ifdef _WIN32
		if (ioctlsocket(stream->rtmp.m_sb.sb_socket, FIONBIO, &one)) {
#else
		if (ioctl(stream->rtmp.m_sb.sb_socket, FIONBIO, &one)) {
#endif
			warn("Failed to set non-blocking socket");
			return OBS_OUTPUT_ERROR;
		}

		os_event_reset(stream->send_thread_signaled_exit);

		info("New socket loop enabled by user");
		if (stream->low_latency_mode)
			info("Low latency mode enabled by user");

		if (stream->write_buf)
			bfree(stream->write_buf);

		int total_bitrate = 0;
		obs_output_t  *context  = stream->output;

		obs_encoder_t *vencoder = obs_output_get_video_encoder(context);
		if (vencoder) {
			obs_data_t *params = obs_encoder_get_settings(vencoder);
			if (params) {
				int bitrate = obs_data_get_int(params, "bitrate");
				stream->current_bitrate = bitrate;
				total_bitrate += bitrate;
				obs_data_release(params);
			}
		}

		stream->audio_bitrate = 0;

		obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, 0);
		if (aencoder) {
			obs_data_t *params = obs_encoder_get_settings(aencoder);
			if (params) {
				int bitrate = obs_data_get_int(params, "bitrate");
				stream->audio_bitrate += bitrate;
				total_bitrate += bitrate;
				obs_data_release(params);
			}
		}

		// to bytes/sec
		int ideal_buffer_size = total_bitrate * 128;

		if (ideal_buffer_size < 131072)
			ideal_buffer_size = 131072;

		stream->write_buf_size = ideal_buffer_size;
		stream->write_buf = bmalloc(ideal_buffer_size);

		stream->target_write_buf_size = ideal_buffer_size;

		{
			obs_data_t *settings = obs_output_get_settings(stream->output);
			stream->autotune = obs_data_get_bool(settings,
				OPT_AUTOTUNE_ENABLED);
			if (stream->autotune) {
				obs_encoder_t *vencoder = obs_output_get_video_encoder(stream->output);
				if (vencoder && obs_encoder_can_update(vencoder)) {
					obs_data_item_t *target = obs_data_item_byname(settings, OPT_TARGET_BITRATE);
					if (target) {
						stream->target_bitrate = obs_data_item_get_int(target);
						if (!stream->current_bitrate)
							stream->current_bitrate = stream->target_bitrate;
					} else if (stream->current_bitrate)
						stream->target_bitrate = stream->current_bitrate;
					else
						stream->autotune = false;
				} else {
					stream->autotune = false;
				}
			}
			obs_data_release(settings);
		}

#ifdef _WIN32
		ret = pthread_create(&stream->socket_thread, NULL,
				socket_thread_windows, stream);
#else
		warn("New socket loop not supported on this platform");
		return OBS_OUTPUT_ERROR;
#endif

		if (ret != 0) {
			RTMP_Close(&stream->rtmp);
			warn("Failed to create socket thread");
			return OBS_OUTPUT_ERROR;
		}

		stream->socket_thread_active = true;
		stream->rtmp.m_bCustomSend = true;
		stream->rtmp.m_customSendFunc = socket_queue_data;
		stream->rtmp.m_customSendParam = stream;
	}

	os_atomic_set_bool(&stream->active, true);
	while (next) {
		if (!send_meta_data(stream, idx++, &next)) {
			warn("Disconnected while attempting to connect to "
			     "server.");
			return OBS_OUTPUT_DISCONNECTED;
		}
	}
	obs_output_begin_data_capture(stream->output, 0);

	return OBS_OUTPUT_SUCCESS;
}

#ifdef _WIN32
static void win32_log_interface_type(struct rtmp_stream *stream)
{
	RTMP *rtmp = &stream->rtmp;
	MIB_IPFORWARDROW route;
	uint32_t dest_addr, source_addr;
	char hostname[256];
	HOSTENT *h;

	if (rtmp->Link.hostname.av_len >= sizeof(hostname) - 1)
		return;

	strncpy(hostname, rtmp->Link.hostname.av_val, sizeof(hostname));
	hostname[rtmp->Link.hostname.av_len] = 0;

	h = gethostbyname(hostname);
	if (!h)
		return;

	dest_addr = *(uint32_t*)h->h_addr_list[0];

	if (rtmp->m_bindIP.addrLen == 0)
		source_addr = 0;
	else if (rtmp->m_bindIP.addr.ss_family == AF_INET)
		source_addr = (*(struct sockaddr_in*)&rtmp->m_bindIP)
			.sin_addr.S_un.S_addr;
	else
		return;

	if (!GetBestRoute(dest_addr, source_addr, &route)) {
		MIB_IFROW row;
		memset(&row, 0, sizeof(row));
		row.dwIndex = route.dwForwardIfIndex;

		if (!GetIfEntry(&row)) {
			uint32_t speed =row.dwSpeed / 1000000;
			char *type;
			struct dstr other = {0};

			if (row.dwType == IF_TYPE_ETHERNET_CSMACD) {
				type = "ethernet";
			} else if (row.dwType == IF_TYPE_IEEE80211) {
				type = "802.11";
			} else {
				dstr_printf(&other, "type %lu", row.dwType);
				type = other.array;
			}

			info("Interface: %s (%s, %lu mbps)", row.bDescr, type,
					speed);

			dstr_free(&other);
		}
	}
}
#endif

static int try_connect(struct rtmp_stream *stream)
{
	if (dstr_is_empty(&stream->path)) {
		warn("URL is empty");
		return OBS_OUTPUT_BAD_PATH;
	}

	info("Connecting to RTMP URL %s...", stream->path.array);

	RTMP_Init(&stream->rtmp);
	if (!RTMP_SetupURL(&stream->rtmp, stream->path.array))
		return OBS_OUTPUT_BAD_PATH;

	RTMP_EnableWrite(&stream->rtmp);

	dstr_copy(&stream->encoder_name, "FMLE/3.0 (compatible; obs-studio/");

#ifdef HAVE_OBSCONFIG_H
	dstr_cat(&stream->encoder_name, OBS_VERSION);
#else
	dstr_catf(&stream->encoder_name, "%d.%d.%d",
			LIBOBS_API_MAJOR_VER,
			LIBOBS_API_MINOR_VER,
			LIBOBS_API_PATCH_VER);
#endif

	dstr_cat(&stream->encoder_name, "; FMSc/1.0)");

	if (stream->encoder_name_suffix.len)
		dstr_catf(&stream->encoder_name, " %s",
				stream->encoder_name_suffix.array);

	set_rtmp_dstr(&stream->rtmp.Link.pubUser,   &stream->username);
	set_rtmp_dstr(&stream->rtmp.Link.pubPasswd, &stream->password);
	set_rtmp_dstr(&stream->rtmp.Link.flashVer,  &stream->encoder_name);
	stream->rtmp.Link.swfUrl = stream->rtmp.Link.tcUrl;

	if (dstr_is_empty(&stream->bind_ip) ||
	    dstr_cmp(&stream->bind_ip, "default") == 0) {
		memset(&stream->rtmp.m_bindIP, 0, sizeof(stream->rtmp.m_bindIP));
	} else {
		bool success = netif_str_to_addr(&stream->rtmp.m_bindIP.addr,
				&stream->rtmp.m_bindIP.addrLen,
				stream->bind_ip.array);
		if (success) {
			int len = stream->rtmp.m_bindIP.addrLen;
			bool ipv6 = len == sizeof(struct sockaddr_in6);
			info("Binding to IPv%d", ipv6 ? 6 : 4);
		}
	}

	RTMP_AddStream(&stream->rtmp, stream->key.array);

	for (size_t idx = 1;; idx++) {
		obs_encoder_t *encoder = obs_output_get_audio_encoder(
				stream->output, idx);
		const char *encoder_name;

		if (!encoder)
			break;

		encoder_name = obs_encoder_get_name(encoder);
		RTMP_AddStream(&stream->rtmp, encoder_name);
	}

	stream->rtmp.m_outChunkSize       = 4096;
	stream->rtmp.m_bSendChunkSizeInfo = true;
	stream->rtmp.m_bUseNagle          = true;

#ifdef _WIN32
	win32_log_interface_type(stream);
#endif

	if (!RTMP_Connect(&stream->rtmp, NULL))
		return OBS_OUTPUT_CONNECT_FAILED;
	if (!RTMP_ConnectStream(&stream->rtmp, 0))
		return OBS_OUTPUT_INVALID_STREAM;

	info("Connection to %s successful", stream->path.array);

	return init_send(stream);
}

static bool init_connect(struct rtmp_stream *stream)
{
	obs_service_t *service;
	obs_data_t *settings;
	const char *bind_ip;
	int64_t drop_p;
	int64_t drop_b;

	if (stopping(stream)) {
		pthread_join(stream->send_thread, NULL);
	}

	free_packets(stream);

	service = obs_output_get_service(stream->output);
	if (!service)
		return false;

	os_atomic_set_bool(&stream->disconnected, false);
	stream->total_bytes_sent = 0;
	stream->dropped_frames   = 0;
	stream->min_priority     = 0;

	settings = obs_output_get_settings(stream->output);
	dstr_copy(&stream->path,     obs_service_get_url(service));
	dstr_copy(&stream->key,      obs_service_get_key(service));
	dstr_copy(&stream->username, obs_service_get_username(service));
	dstr_copy(&stream->password, obs_service_get_password(service));
	dstr_copy(&stream->encoder_name_suffix,
		obs_data_get_string(settings, OPT_ENCODER_NAME));
	dstr_depad(&stream->path);
	dstr_depad(&stream->key);
	drop_b = (int64_t)obs_data_get_int(settings, OPT_DROP_THRESHOLD);
	drop_p = (int64_t)obs_data_get_int(settings, OPT_PFRAME_DROP_THRESHOLD);
	stream->max_shutdown_time_sec =
		(int)obs_data_get_int(settings, OPT_MAX_SHUTDOWN_TIME_SEC);

	if (drop_p < (drop_b + 200))
		drop_p = drop_b + 200;

	stream->drop_threshold_usec = 1000 * drop_b;
	stream->pframe_drop_threshold_usec = 1000 * drop_p;

	bind_ip = obs_data_get_string(settings, OPT_BIND_IP);
	dstr_copy(&stream->bind_ip, bind_ip);

	stream->new_socket_loop = obs_data_get_bool(settings,
			OPT_NEWSOCKETLOOP_ENABLED);
	stream->low_latency_mode = obs_data_get_bool(settings,
			OPT_LOWLATENCY_ENABLED);

	obs_data_release(settings);
	return true;
}

static void *connect_thread(void *data)
{
	struct rtmp_stream *stream = data;
	int ret;

	os_set_thread_name("rtmp-stream: connect_thread");

	if (!init_connect(stream)) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_BAD_PATH);
		return NULL;
	}

	ret = try_connect(stream);

	if (ret != OBS_OUTPUT_SUCCESS) {
		obs_output_signal_stop(stream->output, ret);
		info("Connection to %s failed: %d", stream->path.array, ret);
	}

	if (!stopping(stream))
		pthread_detach(stream->connect_thread);

	os_atomic_set_bool(&stream->connecting, false);
	return NULL;
}

static bool rtmp_stream_start(void *data)
{
	struct rtmp_stream *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	RTMP_Init(&stream->rtmp);

	os_atomic_set_bool(&stream->connecting, true);
	return pthread_create(&stream->connect_thread, NULL, connect_thread,
			stream) == 0;
}

static inline bool add_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	circlebuf_push_back(&stream->packets, packet,
			sizeof(struct encoder_packet));
	return true;
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream)
{
	return stream->packets.size / sizeof(struct encoder_packet);
}

static void drop_frames(struct rtmp_stream *stream, const char *name,
		int highest_priority, bool pframes)
{
	struct circlebuf new_buf            = {0};
	uint64_t         last_drop_dts_usec = 0;
	int              num_frames_dropped = 0;

#ifdef _DEBUG
	int start_packets = (int)num_buffered_packets(stream);
#else
	UNUSED_PARAMETER(name);
#endif

	circlebuf_reserve(&new_buf, sizeof(struct encoder_packet) * 8);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));

		last_drop_dts_usec = packet.dts_usec;

		/* do not drop audio data or video keyframes */
		if (packet.type          == OBS_ENCODER_AUDIO ||
		    packet.drop_priority >= highest_priority) {
			circlebuf_push_back(&new_buf, &packet, sizeof(packet));

		} else {
			num_frames_dropped++;
			obs_free_encoder_packet(&packet);
		}
	}

	circlebuf_free(&stream->packets);
	stream->packets = new_buf;

	if (stream->min_priority < highest_priority)
		stream->min_priority = highest_priority;
	if (!num_frames_dropped)
		return;

	stream->dropped_frames += num_frames_dropped;
#ifdef _DEBUG
	debug("Dropped %s, prev packet count: %d, new packet count: %d",
			name,
			start_packets,
			(int)num_buffered_packets(stream));
#endif
}

static bool find_first_video_packet(struct rtmp_stream *stream,
		struct encoder_packet *first)
{
	size_t count = stream->packets.size / sizeof(*first);

	for (size_t i = 0; i < count; i++) {
		struct encoder_packet *cur = circlebuf_data(&stream->packets,
				i * sizeof(*first));
		if (cur->type == OBS_ENCODER_VIDEO && !cur->keyframe) {
			*first = *cur;
			return true;
		}
	}

	return false;
}

static void check_to_drop_frames(struct rtmp_stream *stream, bool pframes)
{
	struct encoder_packet first;
	int64_t buffer_duration_usec;
	size_t num_packets = num_buffered_packets(stream);
	const char *name = pframes ? "p-frames" : "b-frames";
	int priority = pframes ?
		OBS_NAL_PRIORITY_HIGHEST : OBS_NAL_PRIORITY_HIGH;
	int64_t drop_threshold = pframes ?
		stream->pframe_drop_threshold_usec :
		stream->drop_threshold_usec;

	if (num_packets < 5)
		return;

	if (!find_first_video_packet(stream, &first))
		return;

	/* if the amount of time stored in the buffered packets waiting to be
	 * sent is higher than threshold, drop frames */
	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (buffer_duration_usec > drop_threshold) {
		debug("buffer_duration_usec: %" PRId64, buffer_duration_usec);
		drop_frames(stream, name, priority, pframes);
	}
}

static void update_bitrate(struct rtmp_stream *stream)
{
	obs_encoder_t *encoder = obs_output_get_video_encoder(stream->output);
	if (!encoder)
		return;

	obs_data_t *settings = obs_encoder_get_settings(encoder);
	obs_data_set_int(settings, "bitrate", stream->current_bitrate);
	obs_encoder_update(encoder, settings);
	obs_data_release(settings);

	stream->adjustment_frame_id = obs_track_next_frame();
	stream->adjustment_frame_id_valid = true;

	stream->last_adjustment_time = os_gettime_ns();
}

static void handle_packet_strain(struct rtmp_stream *stream, bool dropped_frames)
{
	float strain = compute_strain(stream);

	uint64_t current_time = os_gettime_ns();
	float sent_bitrate = sent_size(stream) * 8 / 1000;
	float diff = (sent_bitrate - stream->current_bitrate - stream->audio_bitrate) / stream->current_bitrate;

	uint32_t old_bitrate = stream->current_bitrate;
	if (stream->last_adjustment_time + 1500000000 < current_time && strain > .25 && stream->current_bitrate > 100) {
		stream->current_bitrate *= 1 - strain / 4;
		if (stream->current_bitrate < 100)
			stream->current_bitrate = 100;

		info("Lowering bitrate from %u to %u (strain: %g, sent: %g Mbit/s)",
			old_bitrate, stream->current_bitrate, strain, sent_bitrate / 1000);
		update_bitrate(stream);

	} else if (stream->last_adjustment_time + 5000000000 < current_time && stream->current_bitrate < stream->target_bitrate &&
		strain < .05 && stream->last_strain < .05 && !dropped_frames &&
		diff >= 0) {
		stream->current_bitrate += stream->target_bitrate * (0.05 - ((strain + stream->last_strain) / 2));
		if (stream->current_bitrate > stream->target_bitrate)
			stream->current_bitrate = stream->target_bitrate;

		info("Increasing bitrate from %u to %u (strain: %g, last: %g, sent: %g Mbit/s)",
			old_bitrate, stream->current_bitrate, strain, stream->last_strain, sent_bitrate / 1000);
		update_bitrate(stream);
	}

	stream->last_strain = strain;
}

static bool add_video_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	int prev_dropped = stream->dropped_frames;
	check_to_drop_frames(stream, false);
	check_to_drop_frames(stream, true);
	bool dropped_frames = stream->dropped_frames != prev_dropped;

	if (stream->adjustment_frame_id_valid) {
		if (packet->tracked_id == stream->adjustment_frame_id) {
			uint64_t buffer_length = 0;
			if (stream->packets.size) {
				struct encoder_packet first;
				circlebuf_peek_front(&stream->packets, &first, sizeof(first));
				buffer_length = packet->dts_usec - first.dts_usec;
			}

			stream->last_adjustment_time = os_gettime_ns() + buffer_length;
			stream->adjustment_frame_id_valid = false;


			// to bytes/sec
			uint32_t ideal_buffer_size = (stream->current_bitrate + stream->audio_bitrate) * 128;
			if (ideal_buffer_size < 131072)
				ideal_buffer_size = 131072;

			pthread_mutex_lock(&stream->packet_strain_mutex);
			stream->target_write_buf_size = ideal_buffer_size;
			pthread_mutex_unlock(&stream->packet_strain_mutex);
		}

	} else if (stream->autotune) {
		handle_packet_strain(stream, dropped_frames);
	}

	/* if currently dropping frames, drop packets until it reaches the
	 * desired priority */
	if (packet->drop_priority < stream->min_priority) {
		stream->dropped_frames++;
		return false;
	} else {
		stream->min_priority = 0;
	}

	stream->last_dts_usec = packet->dts_usec;
	return add_packet(stream, packet);
}

static void rtmp_stream_data(void *data, struct encoder_packet *packet)
{
	struct rtmp_stream    *stream = data;
	struct encoder_packet new_packet;
	bool                  added_packet = false;

	if (disconnected(stream))
		return;

	if (packet->type == OBS_ENCODER_VIDEO)
		obs_parse_avc_packet(&new_packet, packet);
	else
		obs_duplicate_encoder_packet(&new_packet, packet);

	pthread_mutex_lock(&stream->packets_mutex);

	if (!disconnected(stream)) {
		added_packet = (packet->type == OBS_ENCODER_VIDEO) ?
			add_video_packet(stream, &new_packet) :
			add_packet(stream, &new_packet);
	}

	pthread_mutex_unlock(&stream->packets_mutex);

	if (added_packet)
		os_sem_post(stream->send_sem);
	else
		obs_free_encoder_packet(&new_packet);
}

static void rtmp_stream_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, OPT_DROP_THRESHOLD, 500);
	obs_data_set_default_int(defaults, OPT_PFRAME_DROP_THRESHOLD, 800);
	obs_data_set_default_int(defaults, OPT_MAX_SHUTDOWN_TIME_SEC, 5);
	obs_data_set_default_string(defaults, OPT_ENCODER_NAME, "");
	obs_data_set_default_string(defaults, OPT_BIND_IP, "default");
	obs_data_set_default_bool(defaults, OPT_NEWSOCKETLOOP_ENABLED, false);
	obs_data_set_default_bool(defaults, OPT_LOWLATENCY_ENABLED, false);
}

static obs_properties_t *rtmp_stream_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	struct netif_saddr_data addrs = {0};
	obs_property_t *p;

	obs_properties_add_int(props, OPT_DROP_THRESHOLD,
			obs_module_text("RTMPStream.DropThreshold"),
			200, 10000, 100);

	p = obs_properties_add_list(props, OPT_BIND_IP,
			obs_module_text("RTMPStream.BindIP"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Default"), "default");

	netif_get_addrs(&addrs);
	for (size_t i = 0; i < addrs.addrs.num; i++) {
		struct netif_saddr_item item = addrs.addrs.array[i];
		obs_property_list_add_string(p, item.name, item.addr);
	}
	netif_saddr_data_free(&addrs);

	obs_properties_add_bool(props, OPT_NEWSOCKETLOOP_ENABLED,
			obs_module_text("RTMPStream.NewSocketLoop"));
	obs_properties_add_bool(props, OPT_LOWLATENCY_ENABLED,
			obs_module_text("RTMPStream.LowLatencyMode"));

	return props;
}

static uint64_t rtmp_stream_total_bytes_sent(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->total_bytes_sent;
}

static int rtmp_stream_dropped_frames(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->dropped_frames;
}

static float rtmp_stream_congestion(void *data)
{
	struct rtmp_stream *stream = data;

	if (stream->new_socket_loop)
		return (float)stream->write_buf_len /
			(float)stream->write_buf_size;
	else
		return stream->min_priority > 0 ? 1.0f : stream->congestion;
}

struct obs_output_info rtmp_output_info = {
	.id                 = "rtmp_output",
	.flags              = OBS_OUTPUT_AV |
	                      OBS_OUTPUT_ENCODED |
	                      OBS_OUTPUT_SERVICE |
	                      OBS_OUTPUT_MULTI_TRACK,
	.get_name           = rtmp_stream_getname,
	.create             = rtmp_stream_create,
	.destroy            = rtmp_stream_destroy,
	.start              = rtmp_stream_start,
	.stop               = rtmp_stream_stop,
	.encoded_packet     = rtmp_stream_data,
	.get_defaults       = rtmp_stream_defaults,
	.get_properties     = rtmp_stream_properties,
	.get_total_bytes    = rtmp_stream_total_bytes_sent,
	.get_dropped_frames = rtmp_stream_dropped_frames
};

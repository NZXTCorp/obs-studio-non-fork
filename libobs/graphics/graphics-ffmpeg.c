#include "graphics.h"

#include "util/platform.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "../obs-ffmpeg-compat.h"

struct ffmpeg_image {
	const char         *file;
	AVFormatContext    *fmt_ctx;
	AVCodecContext     *decoder_ctx;
	AVCodec            *decoder;
	AVStream           *stream;
	int                stream_idx;

	int                cx, cy;
	enum AVPixelFormat format;
};

static bool ffmpeg_image_open_decoder_context(struct ffmpeg_image *info)
{
	int ret = av_find_best_stream(info->fmt_ctx, AVMEDIA_TYPE_VIDEO,
			-1, 1, NULL, 0);
	if (ret < 0) {
		blog(LOG_WARNING, "Couldn't find video stream in file '%s': %s",
				info->file, av_err2str(ret));
		return false;
	}

	info->stream_idx  = ret;
	info->stream      = info->fmt_ctx->streams[ret];
	info->decoder_ctx = info->stream->codec;
	info->decoder     = avcodec_find_decoder(info->decoder_ctx->codec_id);

	if (!info->decoder) {
		blog(LOG_WARNING, "Failed to find decoder for file '%s'",
				info->file);
		return false;
	}

	ret = avcodec_open2(info->decoder_ctx, info->decoder, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open video codec for file '%s': "
		                  "%s", info->file, av_err2str(ret));
		return false;
	}

	return true;
}

static void ffmpeg_image_free(struct ffmpeg_image *info)
{
	avcodec_close(info->decoder_ctx);
	avformat_close_input(&info->fmt_ctx);
}

static bool ffmpeg_image_init(struct ffmpeg_image *info, const char *file)
{
	int ret;

	if (!file || !*file)
		return false;

	memset(info, 0, sizeof(struct ffmpeg_image));
	info->file       = file;
	info->stream_idx = -1;

	ret = avformat_open_input(&info->fmt_ctx, file, NULL, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open file '%s': %s",
				info->file, av_err2str(ret));
		return false;
	}

	ret = avformat_find_stream_info(info->fmt_ctx, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Could not find stream info for file '%s':"
		                  " %s", info->file, av_err2str(ret));
		goto fail;
	}

	if (!ffmpeg_image_open_decoder_context(info))
		goto fail;

	info->cx     = info->decoder_ctx->width;
	info->cy     = info->decoder_ctx->height;
	info->format = info->decoder_ctx->pix_fmt;
	return true;

fail:
	ffmpeg_image_free(info);
	return false;
}

static bool ffmpeg_image_reformat_frame(struct ffmpeg_image *info,
		AVFrame *frame, uint8_t *out, int linesize)
{
	struct SwsContext *sws_ctx = NULL;
	int               ret      = 0;

	if (info->format == AV_PIX_FMT_RGBA ||
	    info->format == AV_PIX_FMT_BGRA ||
	    info->format == AV_PIX_FMT_BGR0) {

		if (linesize != frame->linesize[0]) {
			int min_line = linesize < frame->linesize[0] ?
				linesize : frame->linesize[0];

			for (int y = 0; y < info->cy; y++)
				memcpy(out + y * linesize,
				       frame->data[0] + y * frame->linesize[0],
				       min_line);
		} else {
			memcpy(out, frame->data[0], linesize * info->cy);
		}

	} else {
		sws_ctx = sws_getContext(info->cx, info->cy, info->format,
				info->cx, info->cy, AV_PIX_FMT_BGRA,
				SWS_POINT, NULL, NULL, NULL);
		if (!sws_ctx) {
			blog(LOG_WARNING, "Failed to create scale context "
			                  "for '%s'", info->file);
			return false;
		}

		ret = sws_scale(sws_ctx, (const uint8_t *const*)frame->data,
				frame->linesize, 0, info->cy, &out, &linesize);
		sws_freeContext(sws_ctx);

		if (ret < 0) {
			blog(LOG_WARNING, "sws_scale failed for '%s': %s",
					info->file, av_err2str(ret));
			return false;
		}

		info->format = AV_PIX_FMT_BGRA;
	}

	return true;
}

static bool ffmpeg_image_decode(struct ffmpeg_image *info, uint8_t *out,
		int linesize)
{
	AVPacket          packet    = {0};
	bool              success   = false;
	AVFrame           *frame    = av_frame_alloc();
	int               got_frame = 0;
	int               ret;

	if (!frame) {
		blog(LOG_WARNING, "Failed to create frame data for '%s'",
				info->file);
		return false;
	}

	ret = av_read_frame(info->fmt_ctx, &packet);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to read image frame from '%s': %s",
				info->file, av_err2str(ret));
		goto fail;
	}

	while (!got_frame) {
		ret = avcodec_decode_video2(info->decoder_ctx, frame,
				&got_frame, &packet);
		if (ret < 0) {
			blog(LOG_WARNING, "Failed to decode frame for '%s': %s",
					info->file, av_err2str(ret));
			goto fail;
		}
	}

	success = ffmpeg_image_reformat_frame(info, frame, out, linesize);

fail:
	av_free_packet(&packet);
	av_frame_free(&frame);
	return success;
}

void gs_init_image_deps(void)
{
	av_register_all();
}

void gs_free_image_deps(void)
{
}

static inline enum gs_color_format convert_format(enum AVPixelFormat format)
{
	switch ((int)format) {
	case AV_PIX_FMT_RGBA: return GS_RGBA;
	case AV_PIX_FMT_BGRA: return GS_BGRA;
	case AV_PIX_FMT_BGR0: return GS_BGRX;
	}

	return GS_BGRX;
}

gs_texture_t *gs_texture_create_from_file(const char *file)
{
	struct ffmpeg_image image;
	gs_texture_t           *tex = NULL;

	if (ffmpeg_image_init(&image, file)) {
		uint8_t *data = malloc(image.cx * image.cy * 4);
		if (ffmpeg_image_decode(&image, data, image.cx * 4)) {
			tex = gs_texture_create(image.cx, image.cy,
					convert_format(image.format),
					1, (const uint8_t**)&data, 0);
		}

		ffmpeg_image_free(&image);
		free(data);
	}
	return tex;
}

static bool write_buffer(const char *file, AVPacket *packet)
{
	FILE *f = os_fopen(file, "wb+");
	if (!f)
		return false;

	size_t written = fwrite(packet->data, sizeof(*packet->data), packet->size, f);

	fclose(f);

	return true;
}

bool gs_stagesurface_save_to_file(gs_stagesurf_t *surf, const char *file)
{
	bool success = false;
	AVFormatContext *ctx = NULL;
	AVCodec *codec = NULL;
	AVCodecContext *cctx = NULL;
	AVFrame *frame = NULL;
	AVPacket packet = { 0 };
	enum AVPixelFormat format;

	if (!surf || !file)
		return success;

	switch (gs_stagesurface_get_color_format(surf)) {
	case GS_UNKNOWN:
	case GS_A8:
	case GS_R8:
	case GS_R10G10B10A2:
	case GS_RGBA16:
	case GS_R16:
	case GS_RGBA16F:
	case GS_RGBA32F:
	case GS_RG16F:
	case GS_RG32F:
	case GS_R16F:
	case GS_R32F:
	case GS_DXT1:
	case GS_DXT3:
	case GS_DXT5:
		return success;

	case GS_RGBA: format = AV_PIX_FMT_RGBA; break;
	case GS_BGRX: format = AV_PIX_FMT_BGR0; break;
	case GS_BGRA: format = AV_PIX_FMT_BGRA; break;
	}

	if (avformat_alloc_output_context2(&ctx, NULL, NULL, file) < 0)
		return success;

	if (ctx->oformat->video_codec == AV_CODEC_ID_NONE)
		goto err;

	enum AVCodecID id = av_guess_codec(ctx->oformat, NULL, file,
		NULL, AVMEDIA_TYPE_VIDEO);
	if (id == AV_CODEC_ID_NONE)
		goto err;

	codec = avcodec_find_encoder(id);
	if (!codec)
		goto err;

	cctx = avcodec_alloc_context3(codec);
	if (!cctx)
		goto err;

	cctx->pix_fmt = format;
	cctx->height = gs_stagesurface_get_height(surf);
	cctx->width = gs_stagesurface_get_width(surf);
	cctx->time_base.den = 1;
	cctx->time_base.num = 1;

	int res = 0;
	if ((res = avcodec_open2(cctx, codec, NULL)) < 0) {
		blog(LOG_WARNING, "gs_stagesurface_save_to_file: avcodec_open2(cctx, codec, NULL) -> %s (%d)", av_err2str(res), res);
		goto err;
	}

	frame = av_frame_alloc();
	if (!frame)
		goto err;

	frame->pts = 1;

	frame->height = cctx->height;
	frame->width = cctx->width;
	frame->format = format;

	frame->sample_aspect_ratio.den = 1;

	uint8_t *data = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(surf, &data, &linesize))
		goto err;

	graphics_t *context = gs_get_context();
	gs_leave_context();

	frame->linesize[0] = linesize;
	frame->data[0] = data;
	frame->extended_data = frame->data;

	int got_packet = 0;
	res = avcodec_encode_video2(cctx, &packet, frame, &got_packet);

	gs_enter_context(context);
	gs_stagesurface_unmap(surf);

	if (res < 0 || got_packet == 0)
		goto err;

	if (write_buffer(file, &packet))
		success = true;

err:
	av_free_packet(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&cctx);
	avformat_free_context(ctx);
	return success;
}
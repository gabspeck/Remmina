/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2026 Gabriel Speckhahn
 *
 * The protocol handling in this plugin is a port of remotex
 * (https://github.com/andrewtheguy/remotex), Copyright (c) 2026 Andrew Chen,
 * released under the MIT License.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */

#include "ard_decode.h"
#include "ard_log.h"

#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#ifdef HAVE_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

/* Units the decoder may fall behind by: half a second of a frame in four
 * strips at 30 frames a second. */
#define VIDEO_QUEUE 60
/* Where in its picture each unit sent goes, by its pts. */
#define SLOTS 64

typedef struct {
	gint strip;
	guint16 display_width;
	guint16 display_height;
} Slot;

struct _ArdVideo {
	GThread *thread;
	GAsyncQueue *queue;
	ArdVideoCallbacks callbacks;
	ArdCodec codec;
	AVCodecContext *ctx;
	struct SwsContext *sws;
	Slot slots[SLOTS];
	gint64 pts;
	gboolean waiting_keyframe;
	guint64 failures;
};

/* The sentinel that stops a thread, a pointer no unit has. */
static gint stop_marker;
#define STOP ((gpointer) & stop_marker)

static gboolean open_decoder(ArdVideo *v, GError **error)
{
	const AVCodec *codec = avcodec_find_decoder(v->codec == ARD_CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);

	if (!codec) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "this FFmpeg has no %s decoder",
			    v->codec == ARD_CODEC_HEVC ? "HEVC" : "H.264");
		return FALSE;
	}
	v->ctx = avcodec_alloc_context3(codec);
	/* Slice threads add no delay, and the Mac's HEVC has wavefront parallel
	 * processing on for them. Frame threads would hold pictures back. */
	v->ctx->thread_count = 0;
	v->ctx->thread_type = FF_THREAD_SLICE;
	v->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(v->ctx, codec, NULL) < 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "could not open FFmpeg's %s decoder", codec->name);
		avcodec_free_context(&v->ctx);
		return FALSE;
	}
	return TRUE;
}

static int colorspace(const AVFrame *frame)
{
	switch (frame->colorspace) {
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return SWS_CS_ITU601;
	default:
		/* The Mac tags both codecs BT.709. */
		return SWS_CS_ITU709;
	}
}

static void draw(ArdVideo *v, const AVFrame *frame, const Slot *slot)
{
	guint8 *pixels;
	gint stride, width, height, y, rows, cols;
	guint8 *dst[4] = { NULL };
	int dst_stride[4] = { 0 };

	if (!v->callbacks.lock(v->callbacks.user, &pixels, &stride, &width, &height))
		return;
	if (slot->strip >= 0) {
		guint strip_rows = ard_strip_rows(slot->display_height);

		/* A picture that is not a strip of this display is from before it
		 * was offered, or the display is not the size it was offered at yet. */
		if (frame->width != slot->display_width || (guint)frame->height != strip_rows
		    || width != slot->display_width || height != slot->display_height) {
			v->callbacks.unlock(v->callbacks.user, 0, 0);
			return;
		}
		y = slot->strip * strip_rows;
		if (y >= height) {
			v->callbacks.unlock(v->callbacks.user, 0, 0);
			return;
		}
		/* The last strip runs past the display's last row. */
		rows = MIN((gint)strip_rows, height - y);
		cols = width;
	} else {
		/* A Mac's H.264 encoder may code an odd side a pixel short. */
		if (ABS(frame->width - width) > 1 || ABS(frame->height - height) > 1) {
			v->callbacks.unlock(v->callbacks.user, 0, 0);
			return;
		}
		y = 0;
		rows = MIN(frame->height, height);
		cols = MIN(frame->width, width);
	}
	v->sws = sws_getCachedContext(v->sws, cols, rows, frame->format, cols, rows, AV_PIX_FMT_BGR0,
				      SWS_POINT, NULL, NULL, NULL);
	if (!v->sws) {
		v->callbacks.unlock(v->callbacks.user, 0, 0);
		return;
	}
	sws_setColorspaceDetails(v->sws, sws_getCoefficients(colorspace(frame)),
				 frame->color_range == AVCOL_RANGE_JPEG, sws_getCoefficients(SWS_CS_DEFAULT), 1,
				 0, 1 << 16, 1 << 16);
	dst[0] = pixels + (gsize)y * stride;
	dst[1] = NULL;
	dst_stride[0] = stride;
	sws_scale(v->sws, (const uint8_t *const *)frame->data, frame->linesize, 0, rows, dst, dst_stride);
	v->callbacks.unlock(v->callbacks.user, y, rows);
}

static void decode(ArdVideo *v, ArdUnit *unit, AVPacket *packet, AVFrame *frame)
{
	gsize len = unit->data->len;
	Slot *slot;
	int ret;

	/* After a failure the queue may still hold units predicted from what was
	 * lost; the receiver resyncs at the keyframe it asks for. */
	if (v->waiting_keyframe && !unit->keyframe)
		return;
	v->waiting_keyframe = FALSE;
	g_byte_array_set_size(unit->data, len + AV_INPUT_BUFFER_PADDING_SIZE);
	memset(unit->data->data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	packet->data = unit->data->data;
	packet->size = len;
	packet->pts = v->pts++;
	slot = &v->slots[packet->pts % SLOTS];
	slot->strip = unit->strip;
	slot->display_width = unit->display_width;
	slot->display_height = unit->display_height;
	ret = avcodec_send_packet(v->ctx, packet);
	if (ret < 0) {
		if (++v->failures <= 3)
			ARD_WARNING("the decoder refused a picture of the Mac's screen (%s); asking for a keyframe",
				    av_err2str(ret));
		avcodec_flush_buffers(v->ctx);
		v->waiting_keyframe = TRUE;
		v->callbacks.want_keyframe(v->callbacks.user);
		return;
	}
	while (avcodec_receive_frame(v->ctx, frame) == 0) {
		draw(v, frame, &v->slots[frame->pts % SLOTS]);
		av_frame_unref(frame);
	}
}

static gpointer video_thread(gpointer data)
{
	ArdVideo *v = data;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	gpointer item;

	while ((item = g_async_queue_pop(v->queue)) != STOP) {
		ArdUnit *unit = item;

		decode(v, unit, packet, frame);
		ard_unit_free(unit);
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	return NULL;
}

ArdVideo *ard_video_start(ArdCodec codec, const ArdVideoCallbacks *callbacks, GError **error)
{
	ArdVideo *v = g_new0(ArdVideo, 1);

	v->codec = codec;
	v->callbacks = *callbacks;
	if (!open_decoder(v, error)) {
		g_free(v);
		return NULL;
	}
	v->queue = g_async_queue_new();
	v->thread = g_thread_new("applehp-video", video_thread, v);
	return v;
}

gboolean ard_video_push(ArdVideo *v, ArdUnit *unit)
{
	if (g_async_queue_length(v->queue) >= VIDEO_QUEUE) {
		ard_unit_free(unit);
		return FALSE;
	}
	g_async_queue_push(v->queue, unit);
	return TRUE;
}

void ard_video_stop(ArdVideo *v)
{
	gpointer item;

	if (!v)
		return;
	g_async_queue_push_front(v->queue, STOP);
	g_thread_join(v->thread);
	while ((item = g_async_queue_try_pop(v->queue)))
		if (item != STOP)
			ard_unit_free(item);
	g_async_queue_unref(v->queue);
	avcodec_free_context(&v->ctx);
	sws_freeContext(v->sws);
	g_free(v);
}

/* ------------------------------------------------------------------------ */
/* Sound */

#ifdef HAVE_PULSE
/* AudioSpecificConfig for what the Mac sends, which RTP never says: ER AAC ELD
 * (object type 39), 48 kHz, stereo, 480-sample frames, no SBR. */
static const guint8 audio_specific_config[] = { 0xf8, 0xe6, 0x50, 0x00 };
#endif
#define SAMPLE_RATE 48000
#define CHANNELS 2
/* The most sound queued before the oldest is dropped: what keeps a backlog
 * from turning into a delay behind the picture. */
#define AUDIO_QUEUE 25

struct _ArdAudio {
	GThread *thread;
	GAsyncQueue *queue;
#ifdef HAVE_PULSE
	pa_simple *pulse;
#endif
	AVCodecContext *ctx;
};

#ifdef HAVE_PULSE
static void play(ArdAudio *a, const AVFrame *frame)
{
	gsize samples = frame->nb_samples;
	gfloat *interleaved;
	int error;

	if (frame->format == AV_SAMPLE_FMT_FLT) {
		pa_simple_write(a->pulse, frame->data[0], samples * CHANNELS * sizeof(gfloat), &error);
		return;
	}
	if (frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != CHANNELS)
		return;
	interleaved = g_new(gfloat, samples * CHANNELS);
	for (gsize i = 0; i < samples; i++)
		for (int c = 0; c < CHANNELS; c++)
			interleaved[i * CHANNELS + c] = ((const gfloat *)frame->data[c])[i];
	pa_simple_write(a->pulse, interleaved, samples * CHANNELS * sizeof(gfloat), &error);
	g_free(interleaved);
}

static gpointer audio_thread(gpointer data)
{
	ArdAudio *a = data;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	gpointer item;
	guint64 failures = 0;

	while ((item = g_async_queue_pop(a->queue)) != STOP) {
		GByteArray *unit = item;
		gsize len = unit->len;

		while (g_async_queue_length(a->queue) > AUDIO_QUEUE) {
			gpointer old = g_async_queue_try_pop(a->queue);

			if (old == STOP || !old) {
				g_async_queue_push_front(a->queue, STOP);
				break;
			}
			g_byte_array_unref(old);
		}
		g_byte_array_set_size(unit, len + AV_INPUT_BUFFER_PADDING_SIZE);
		memset(unit->data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		packet->data = unit->data;
		packet->size = len;
		if (avcodec_send_packet(a->ctx, packet) == 0) {
			while (avcodec_receive_frame(a->ctx, frame) == 0) {
				play(a, frame);
				av_frame_unref(frame);
			}
		} else if (++failures <= 3) {
			ARD_WARNING("the AAC-ELD decoder refused a unit of the Mac's sound");
		}
		g_byte_array_unref(unit);
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	return NULL;
}
#endif

ArdAudio *ard_audio_start(void)
{
#ifdef HAVE_PULSE
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	pa_sample_spec spec = { PA_SAMPLE_FLOAT32NE, SAMPLE_RATE, CHANNELS };
	/* A short target keeps the sound close to the picture. */
	pa_buffer_attr attr = { (uint32_t)-1, SAMPLE_RATE * CHANNELS * sizeof(gfloat) * 60 / 1000,
				(uint32_t)-1, (uint32_t)-1, (uint32_t)-1 };
	ArdAudio *a;
	int error;

	if (!codec) {
		ARD_WARNING("this FFmpeg has no AAC decoder; the Mac's sound will not play");
		return NULL;
	}
	a = g_new0(ArdAudio, 1);
	a->ctx = avcodec_alloc_context3(codec);
	a->ctx->extradata = av_mallocz(sizeof(audio_specific_config) + AV_INPUT_BUFFER_PADDING_SIZE);
	memcpy(a->ctx->extradata, audio_specific_config, sizeof(audio_specific_config));
	a->ctx->extradata_size = sizeof(audio_specific_config);
	a->ctx->sample_rate = SAMPLE_RATE;
	av_channel_layout_default(&a->ctx->ch_layout, CHANNELS);
	if (avcodec_open2(a->ctx, codec, NULL) < 0) {
		ARD_WARNING("FFmpeg's AAC decoder does not take AAC-ELD; the Mac's sound will not play");
		avcodec_free_context(&a->ctx);
		g_free(a);
		return NULL;
	}
	a->pulse = pa_simple_new(NULL, "Remmina", PA_STREAM_PLAYBACK, NULL, "Apple Screen Sharing", &spec, NULL, &attr, &error);
	if (!a->pulse) {
		ARD_WARNING("could not open PulseAudio for the Mac's sound: %s", pa_strerror(error));
		avcodec_free_context(&a->ctx);
		g_free(a);
		return NULL;
	}
	a->queue = g_async_queue_new();
	a->thread = g_thread_new("applehp-sound", audio_thread, a);
	return a;
#else
	ARD_INFO("built without PulseAudio; the Mac's sound will not play");
	return NULL;
#endif
}

void ard_audio_push(ArdAudio *a, const guint8 *data, gsize len)
{
	if (!a)
		return;
	g_async_queue_push(a->queue, g_byte_array_append(g_byte_array_sized_new(len + AV_INPUT_BUFFER_PADDING_SIZE), data, len));
}

void ard_audio_stop(ArdAudio *a)
{
	gpointer item;

	if (!a)
		return;
#ifdef HAVE_PULSE
	g_async_queue_push_front(a->queue, STOP);
	g_thread_join(a->thread);
	while ((item = g_async_queue_try_pop(a->queue)))
		if (item != STOP)
			g_byte_array_unref(item);
	g_async_queue_unref(a->queue);
	pa_simple_free(a->pulse);
#else
	(void)item;
#endif
	avcodec_free_context(&a->ctx);
	g_free(a);
}

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

#include "ard_receiver.h"
#include "ard_log.h"
#include "ard_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

/* The Mac sends a picture as one burst at the link's speed, and on a large
 * display a keyframe's burst overflows Linux's default receive buffer however
 * promptly it is read. Linux grants at most net.core.rmem_max. */
#define VIDEO_RECEIVE_BUFFER (4 << 20)
/* The least time between two keyframe requests, which is also how often one the
 * Mac dropped is made again: it drops one made too soon after its last keyframe,
 * and a still screen sends nothing more to show that it did. */
#define KEYFRAME_INTERVAL (500 * G_TIME_SPAN_MILLISECOND)
#define RATE_FEEDBACK (50 * G_TIME_SPAN_MILLISECOND)
#define RECEIVER_REPORT G_TIME_SPAN_SECOND
#define SILENT_START (5 * G_TIME_SPAN_SECOND)

typedef enum {
	ASK_NONE,
	/* A picture predicted from one already handed on, after lost packets. */
	ASK_REFRESH,
	/* An IDR: the decoder has nothing to predict from. */
	ASK_KEYFRAME
} Ask;

struct _ArdReceiver {
	GThread *thread;
	int wake[2];
	gint stop;
	gint keyframe_wanted;

	int audio_fd;
	int video_fd;
	guint16 audio_port;
	guint16 video_port;

	ArdSrtpReceiver audio_srtp;
	ArdSrtcpSender audio_rtcp;
	ArdSrtcpReceiver audio_reports;
	guint32 audio_ssrc;

	ArdSrtpReceiver video_srtp;
	ArdSrtcpSender video_rtcp;
	ArdSrtcpReceiver video_reports;
	guint32 video_ssrc;
	guint32 media_ssrc;
	ArdCodec codec;
	ArdAssembly assembly;
	ArdRateFeedback feedback;
	Ask owed;
	gint64 asked_at;
	gboolean have_unacknowledged;
	guint32 unacknowledged;

	ArdReceiverCallbacks callbacks;

	GMutex lock;
	guint16 display_width;
	guint16 display_height;
	ArdReceiverTimes times;

	guint64 video_packets;
	guint64 audio_packets;
	guint64 forged;
	guint64 behind;
};

static int bind_leg(const struct sockaddr_storage *local, const struct sockaddr_storage *peer, guint16 port,
		    int buffer, GError **error)
{
	struct sockaddr_storage at = *local, to = *peer;
	socklen_t len = local->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	int one = 1;
	int fd;

	if (local->ss_family == AF_INET6) {
		((struct sockaddr_in6 *)&at)->sin6_port = htons(port);
		((struct sockaddr_in6 *)&to)->sin6_port = htons(port);
	} else {
		((struct sockaddr_in *)&at)->sin_port = htons(port);
		((struct sockaddr_in *)&to)->sin_port = htons(port);
	}
	fd = socket(local->ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "could not open a UDP socket: %s", g_strerror(errno));
		return -1;
	}
	/* Every Mac names the same ports, so another session on this host binds
	 * them too; each socket being connected to its own Mac is what has the
	 * kernel hand each session its own Mac's packets. */
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
	if (buffer > 0) {
		int granted = 0;
		socklen_t granted_len = sizeof(granted);

		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
		getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &granted, &granted_len);
#ifdef __linux__
		/* Linux reads back twice what it grants. */
		granted /= 2;
#endif
		if (granted < buffer)
			ARD_WARNING("UDP %u got a %d-byte receive buffer of the %d asked for, so a keyframe of the Mac's "
				    "screen can overflow it; raise net.core.rmem_max to %d", port, granted, buffer, buffer);
	}
	if (bind(fd, (struct sockaddr *)&at, len) < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "could not bind UDP port %u for the Mac's media stream: %s",
			    port, g_strerror(errno));
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&to, len) < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "could not connect UDP port %u to the Mac: %s",
			    port, g_strerror(errno));
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

static void send_rtcp(int fd, ArdSrtcpSender *sender, const guint8 *report, gsize len)
{
	guint8 out[64];

	g_assert(len + 4 + ARD_SRTP_TAG <= sizeof(out));
	len = ard_srtcp_protect(sender, report, len, out);
	if (send(fd, out, len, 0) < 0 && errno != ECONNREFUSED && errno != EAGAIN)
		ARD_DEBUG("could not send RTCP: %s", g_strerror(errno));
}

static void want(ArdReceiver *r, Ask ask)
{
	if (ask > r->owed)
		r->owed = ask;
}

/* Ask the Mac for the picture owed, once its stream has named an SSRC and the
 * last request is old enough. It stays owed until a picture comes. */
static void ask_keyframe(ArdReceiver *r, gint64 now)
{
	guint8 request[16];
	gsize len;

	if (r->owed == ASK_NONE || r->media_ssrc == 0)
		return;
	if (r->asked_at && now - r->asked_at < KEYFRAME_INTERVAL)
		return;
	r->asked_at = now;
	if (r->owed == ASK_REFRESH) {
		guint16 w, h;

		/* The Mac takes its own size in place of the one named. */
		g_mutex_lock(&r->lock);
		w = r->display_width;
		h = r->display_height;
		g_mutex_unlock(&r->lock);
		len = ard_rtcp_refresh(r->video_ssrc, r->media_ssrc, w, h, request);
	} else {
		len = ard_rtcp_pli(r->video_ssrc, r->media_ssrc, request);
	}
	send_rtcp(r->video_fd, &r->video_rtcp, request, len);
}

static void acknowledge(ArdReceiver *r)
{
	guint8 ack[16];

	if (!r->have_unacknowledged)
		return;
	r->have_unacknowledged = FALSE;
	send_rtcp(r->video_fd, &r->video_rtcp, ack, ard_rtcp_reference_ack(r->video_ssrc, r->unacknowledged, ack));
}

static void take_video(ArdReceiver *r, guint8 *data, gsize len, gint64 arrived)
{
	ArdRtpHeader h;
	ArdUnit *unit = NULL;
	guint32 stream;

	switch (ard_srtp_unprotect(&r->video_srtp, data, len, &h)) {
	case ARD_SRTP_OK:
		break;
	case ARD_SRTP_RTCP:
		/* The Mac's report, which keeps the leg alive while a still screen
		 * sends no picture. A receiver's report where a sender's would be is
		 * the Mac saying it has sent nothing since its last. */
		if (ard_srtcp_authenticate(&r->video_reports, data, len)) {
			g_mutex_lock(&r->lock);
			r->times.video_heard = arrived;
			if (data[1] == ARD_RTCP_SENDER_REPORT)
				r->times.video_sender_report = arrived;
			else if (data[1] == ARD_RTCP_RECEIVER_REPORT)
				r->times.video_receiver_report = arrived;
			g_mutex_unlock(&r->lock);
		}
		return;
	case ARD_SRTP_FORGED:
		if (++r->forged <= 3)
			ARD_WARNING("dropped a screen video packet whose SRTP tag did not match");
		return;
	default:
		return;
	}
	g_mutex_lock(&r->lock);
	r->times.video_heard = arrived;
	r->times.video_pictured = arrived;
	g_mutex_unlock(&r->lock);

	stream = ard_assembly_stream(&r->assembly, h.ssrc);
	ard_rate_feedback_received(&r->feedback, stream, h.timestamp, arrived);
	if (++r->video_packets == 1)
		ARD_INFO("the Mac's screen video is flowing (SSRC 0x%08x)", h.ssrc);
	if (r->media_ssrc != stream) {
		/* A new stream, after an offer, starts with an IDR of its own. */
		r->owed = ASK_NONE;
		r->media_ssrc = stream;
	}

	switch (ard_assembly_push(&r->assembly, &h, data + h.payload_at, h.payload_end - h.payload_at, &unit)) {
	case ARD_DEPACK_PENDING:
		break;
	case ARD_DEPACK_LOST:
		want(r, ard_assembly_resumes_at_refresh(&r->assembly) ? ASK_REFRESH : ASK_KEYFRAME);
		break;
	case ARD_DEPACK_UNIT: {
		guint32 acknowledgement = unit->acknowledgement;

		/* One in strips can go on once it has the whole keyframe. Until then
		 * it is owed as if asked for now, so the rest has time to come. */
		if (ard_assembly_complete(&r->assembly)) {
			r->owed = ASK_NONE;
		} else {
			want(r, ASK_KEYFRAME);
			r->asked_at = arrived;
		}
		g_mutex_lock(&r->lock);
		unit->display_width = r->display_width;
		unit->display_height = r->display_height;
		g_mutex_unlock(&r->lock);
		if (!r->callbacks.video_unit(r->callbacks.user, unit)) {
			if (++r->behind <= 3)
				ARD_WARNING("the decoder fell behind the Mac's screen video; dropping to its next keyframe");
			ard_assembly_resync(&r->assembly);
			want(r, ASK_KEYFRAME);
		} else if (r->codec == ARD_CODEC_HEVC) {
			/* The HEVC's pictures ask to be acknowledged; the H.264's cannot
			 * be, inside the Mac. */
			r->have_unacknowledged = TRUE;
			r->unacknowledged = acknowledgement;
		}
		break;
	}
	}
}

static void take_audio(ArdReceiver *r, guint8 *data, gsize len, gint64 arrived)
{
	ArdRtpHeader h;

	switch (ard_srtp_unprotect(&r->audio_srtp, data, len, &h)) {
	case ARD_SRTP_OK:
		g_mutex_lock(&r->lock);
		r->times.audio_heard = arrived;
		r->times.audio_sounded = arrived;
		g_mutex_unlock(&r->lock);
		if (++r->audio_packets == 1)
			ARD_INFO("the Mac's sound is flowing: RTP payload type %u, %" G_GSIZE_FORMAT " bytes per unit",
				 h.payload_type, h.payload_end - h.payload_at);
		r->callbacks.audio_unit(r->callbacks.user, data + h.payload_at, h.payload_end - h.payload_at);
		break;
	case ARD_SRTP_RTCP:
		if (ard_srtcp_authenticate(&r->audio_reports, data, len)) {
			g_mutex_lock(&r->lock);
			r->times.audio_heard = arrived;
			g_mutex_unlock(&r->lock);
		}
		break;
	case ARD_SRTP_FORGED:
		if (++r->forged <= 3)
			ARD_WARNING("dropped a sound packet whose SRTP tag did not match");
		break;
	default:
		break;
	}
}

static void fail(ArdReceiver *r, const gchar *what)
{
	g_mutex_lock(&r->lock);
	if (!r->times.failure)
		r->times.failure = g_strdup_printf("%s: %s", what, g_strerror(errno));
	g_mutex_unlock(&r->lock);
}

/* Drain one socket. FALSE when it failed for good. A refused or reset receive
 * is an ICMP unreachable for a report sent before the Mac's side was up, which
 * UDP has no connection to fail on otherwise. */
static gboolean drain(ArdReceiver *r, int fd, guint8 *buffer, gsize size, gboolean video)
{
	for (;;) {
		ssize_t n = recv(fd, buffer, size, 0);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return TRUE;
			if (errno == EINTR || errno == ECONNREFUSED || errno == ECONNRESET)
				continue;
			fail(r, video ? "the Mac's screen video socket failed" : "the Mac's sound socket failed");
			return FALSE;
		}
		if (video)
			take_video(r, buffer, n, g_get_monotonic_time());
		else
			take_audio(r, buffer, n, g_get_monotonic_time());
		if (video) {
			acknowledge(r);
			ask_keyframe(r, g_get_monotonic_time());
		}
	}
}

static gpointer receiver_thread(gpointer data)
{
	ArdReceiver *r = data;
	guint8 *buffer = g_malloc(65536);
	gint64 started = g_get_monotonic_time();
	gint64 next_report = started, next_rate = started;
	gboolean warned_silent = FALSE;

	while (!g_atomic_int_get(&r->stop)) {
		struct pollfd fds[3] = {
			{ r->wake[0], POLLIN, 0 },
			{ r->video_fd, POLLIN, 0 },
			{ r->audio_fd, POLLIN, 0 },
		};
		gint64 now = g_get_monotonic_time();
		gint64 next = MIN(next_report, next_rate);
		int timeout = next > now ? (int)((next - now + 999) / 1000) : 0;

		if (poll(fds, G_N_ELEMENTS(fds), timeout) < 0 && errno != EINTR) {
			fail(r, "polling the media stream failed");
			break;
		}
		if (fds[0].revents & POLLIN) {
			guint8 drop[16];

			while (read(r->wake[0], drop, sizeof(drop)) > 0)
				;
		}
		if (g_atomic_int_compare_and_exchange(&r->keyframe_wanted, 1, 0)) {
			ard_assembly_resync(&r->assembly);
			want(r, ASK_KEYFRAME);
			r->asked_at = 0;
		}
		if ((fds[1].revents & POLLIN) && !drain(r, r->video_fd, buffer, 65536, TRUE))
			break;
		if ((fds[2].revents & POLLIN) && !drain(r, r->audio_fd, buffer, 65536, FALSE))
			break;
		now = g_get_monotonic_time();
		ask_keyframe(r, now);
		if (now >= next_rate) {
			guint8 report[32];

			if (ard_rate_feedback_report(&r->feedback, r->video_ssrc, now, report))
				send_rtcp(r->video_fd, &r->video_rtcp, report, sizeof(report));
			next_rate = now + RATE_FEEDBACK;
		}
		if (now >= next_report) {
			guint8 report[8];

			/* The Mac stops a stream that hears nothing from its receiver. */
			send_rtcp(r->audio_fd, &r->audio_rtcp, report, ard_rtcp_receiver_report(r->audio_ssrc, report));
			send_rtcp(r->video_fd, &r->video_rtcp, report, ard_rtcp_receiver_report(r->video_ssrc, report));
			next_report = now + RECEIVER_REPORT;
			if (!warned_silent && r->video_packets == 0 && now - started >= SILENT_START) {
				gboolean reported;

				g_mutex_lock(&r->lock);
				reported = r->times.video_receiver_report != 0;
				g_mutex_unlock(&r->lock);
				warned_silent = TRUE;
				if (!reported)
					ARD_WARNING("the Mac named UDP %u for its screen video but nothing has arrived in %ds; it "
						    "sends to this computer's address on that port, so a firewall or NAT between "
						    "them is what this looks like", r->video_port, (int)(SILENT_START / G_TIME_SPAN_SECOND));
			}
		}
	}
	g_free(buffer);
	return NULL;
}

ArdReceiver *ard_receiver_start(const ArdOffers *offers, const struct sockaddr_storage *local,
				const struct sockaddr_storage *peer, guint16 audio_port, guint16 video_port,
				const ArdReceiverCallbacks *callbacks, GError **error)
{
	ArdReceiver *r = g_new0(ArdReceiver, 1);
	gint64 now = g_get_monotonic_time();

	r->audio_fd = r->video_fd = r->wake[0] = r->wake[1] = -1;
	g_mutex_init(&r->lock);
	r->audio_port = audio_port;
	r->video_port = video_port;
	r->callbacks = *callbacks;
	r->codec = offers->codec;
	r->audio_ssrc = offers->audio_ssrc;
	r->video_ssrc = offers->video_ssrc;
	ard_srtp_receiver_init(&r->audio_srtp, offers->audio_keys.s2v);
	ard_srtcp_sender_init(&r->audio_rtcp, offers->audio_keys.v2s);
	ard_srtcp_receiver_init(&r->audio_reports, offers->audio_keys.s2v);
	ard_srtp_receiver_init(&r->video_srtp, offers->video_keys.s2v);
	ard_srtcp_sender_init(&r->video_rtcp, offers->video_keys.v2s);
	ard_srtcp_receiver_init(&r->video_reports, offers->video_keys.s2v);
	ard_assembly_init(&r->assembly, offers->codec, ard_offers_strips(offers));
	ard_rate_feedback_init(&r->feedback, now);

	if (pipe(r->wake) < 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "could not create a pipe: %s", g_strerror(errno));
		ard_receiver_stop(r);
		return NULL;
	}
	for (int i = 0; i < 2; i++) {
		fcntl(r->wake[i], F_SETFD, FD_CLOEXEC);
		fcntl(r->wake[i], F_SETFL, fcntl(r->wake[i], F_GETFL) | O_NONBLOCK);
	}
	r->video_fd = bind_leg(local, peer, video_port, VIDEO_RECEIVE_BUFFER, error);
	if (r->video_fd < 0) {
		ard_receiver_stop(r);
		return NULL;
	}
	r->audio_fd = bind_leg(local, peer, audio_port, 0, error);
	if (r->audio_fd < 0) {
		ard_receiver_stop(r);
		return NULL;
	}
	r->thread = g_thread_new("applehp-media", receiver_thread, r);
	return r;
}

void ard_receiver_stop(ArdReceiver *r)
{
	if (!r)
		return;
	g_atomic_int_set(&r->stop, 1);
	if (r->wake[1] >= 0 && write(r->wake[1], "x", 1) < 0)
		ARD_DEBUG("could not wake the media receiver");
	if (r->thread)
		g_thread_join(r->thread);
	for (int i = 0; i < 2; i++)
		if (r->wake[i] >= 0)
			close(r->wake[i]);
	if (r->audio_fd >= 0)
		close(r->audio_fd);
	if (r->video_fd >= 0)
		close(r->video_fd);
	ard_srtp_receiver_clear(&r->audio_srtp);
	ard_srtcp_sender_clear(&r->audio_rtcp);
	ard_srtcp_receiver_clear(&r->audio_reports);
	ard_srtp_receiver_clear(&r->video_srtp);
	ard_srtcp_sender_clear(&r->video_rtcp);
	ard_srtcp_receiver_clear(&r->video_reports);
	ard_assembly_clear(&r->assembly);
	g_free(r->times.failure);
	g_mutex_clear(&r->lock);
	g_free(r);
}

void ard_receiver_set_display(ArdReceiver *r, guint16 width, guint16 height)
{
	g_mutex_lock(&r->lock);
	r->display_width = width;
	r->display_height = height;
	g_mutex_unlock(&r->lock);
}

void ard_receiver_want_keyframe(ArdReceiver *r)
{
	g_atomic_int_set(&r->keyframe_wanted, 1);
	if (write(r->wake[1], "k", 1) < 0)
		ARD_DEBUG("could not wake the media receiver");
}

void ard_receiver_times(ArdReceiver *r, ArdReceiverTimes *times)
{
	g_mutex_lock(&r->lock);
	*times = r->times;
	times->failure = g_strdup(r->times.failure);
	g_mutex_unlock(&r->lock);
}

gboolean ard_receiver_matches(ArdReceiver *r, guint16 audio_port, guint16 video_port)
{
	return r->audio_port == audio_port && r->video_port == video_port;
}

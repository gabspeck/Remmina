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

#include "ard_session.h"
#include "ard_crypto.h"
#include "ard_decode.h"
#include "ard_log.h"
#include "ard_pasteboard.h"
#include "ard_receiver.h"
#include "ard_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/rand.h>
#include <zlib.h>

#define ENC_RAW 0
#define ENC_ZLIB 6
#define ENC_ZRLE 16
#define ENC_CURSOR_POS 0x44c
#define ENC_DISPLAY_INFO 0x44d
#define ENC_REKEY 0x44f
#define ENC_CURSOR_IMAGE 0x450
#define ENC_DISPLAY_LAYOUT 0x451
#define ENC_VENDOR_KEYSYMS 0x453
#define ENC_KEYBOARD_SOURCE 0x455
#define ENC_DESKTOP_SIZE -223
#define ENC_LAST_RECT -224

/* What this viewer advertises. Every entry is read or stepped over: a server
 * takes the list as a promise. Listing DisplayInfo and the layout together is
 * what makes the Mac report its displays as the layout. ZRLE is the one codec,
 * and its rectangles are stepped over unread: the picture is the media
 * stream, which a second list names once the first layout is in. */
static const gint32 encodings[] = {
	ENC_RAW, ENC_CURSOR_POS, ENC_DISPLAY_INFO, ENC_REKEY, ENC_CURSOR_IMAGE, ENC_DISPLAY_LAYOUT,
	ENC_VENDOR_KEYSYMS, ENC_KEYBOARD_SOURCE, ENC_DESKTOP_SIZE, ENC_LAST_RECT, ENC_ZRLE
};

#define SECURITY_ARD 30
#define MSG_SET_DISPLAY_CONFIGURATION 0x1d
/* The second event message, which a scroll is one kind of. */
#define MSG_EVENT_2 0x17
/* High Performance arms one push a second: its pixels are stepped over, and
 * the arming is what keeps cursor shapes coming across logins and locks. */
#define PUSH_INTERVAL_US 1000000
/* The least display Apple's viewer asks for, at 1x and at 2x: below it the Mac
 * sends no picture of some displays. */
#define MIN_POINTS_W 800
#define MIN_POINTS_H 600
/* The virtual display's fixed backing ceiling. It is the descriptor's maximum,
 * not its mode: deriving it from the opening mode caps every later resize. */
#define MAX_PIXELS_W 3840
#define MAX_PIXELS_H 2160
/* The virtual display's refresh, which bounds the stream's picture rate. */
#define DISPLAY_HZ 30.0
#define NOMINAL_DPI 132.0

#define STREAM_START (10 * G_TIME_SPAN_SECOND)
/* 16 of Apple's 3-second RTCP timeouts on a leg, at which its viewer gives up. */
#define STREAM_SILENCE (48 * G_TIME_SPAN_SECOND)
#define RESIZE_TIMEOUT (10 * G_TIME_SPAN_SECOND)
#define CONNECT_TIMEOUT_MS 15000
#define TICK (50 * G_TIME_SPAN_MILLISECOND)
#define MAX_CURSOR_DIM 512
#define MAX_CURSOR_BYTES (4 * 1024 * 1024)
#define MAX_SKIP_BYTES (256 * 1024 * 1024)
#define AUTH_ATTEMPTS 3

G_DEFINE_QUARK(ard-session-error-quark, ard_session_error)

typedef struct {
	guint16 px_w, px_h;
	guint16 pt_w, pt_h;
} Mode;

typedef struct {
	cairo_surface_t *surface;
	gint hot_x, hot_y;
} Cursor;

struct _ArdSession {
	ArdSessionParams params;
	ArdSessionCallbacks cb;
	int fd;
	struct sockaddr_storage local;
	struct sockaddr_storage peer;
	GThread *reader;
	GThread *writer;
	GAsyncQueue *outgoing;
	gint stopping;
	gint failed;

	/* The reader's: bytes off the socket not yet consumed, and once the
	 * record layer is on, the verified body of the record in hand. */
	GByteArray *raw;
	gsize raw_at;
	gboolean records;
	ArdCbc in;
	ArdCbc out;
	GByteArray *plain;
	gsize plain_at;
	gchar *username;
	gchar *password;
	GHashTable *cursors;
	guint16 server_w, server_h;

	GMutex lock;
	gboolean laid_out;
	gboolean on_virtual;
	gboolean moved_off;
	guint16 fb_w, fb_h;
	gdouble density;
	Mode mode;
	gboolean want_pending;
	Mode want;
	gboolean resize_out;
	gint64 resize_at;

	ArdOffers offers;
	/* The encodings naming the media stream have gone out, at asked_at. */
	gboolean asked;
	gint64 asked_at;
	gboolean ports_named;
	/* An offer is out that the Mac has not answered. Only one may be: a second
	 * one sent while the first's capture was starting left the capture failed
	 * and crashed WindowServer when the display went away. */
	gboolean pending;
	gint64 pending_at;
	/* The Mac has named its ports and no offer has gone out on that naming. */
	gboolean invited;
	gboolean offered;
	guint16 offered_w, offered_h;
	gint64 offered_at;
	gboolean withheld;
	ArdReceiver *receiver;
	ArdVideo *video;
	ArdAudio *audio;

	guint32 paste_session;
	gchar *local_text;
	gint64 last_tick;
};

static gint stop_marker;
#define STOP ((gpointer) & stop_marker)

/* ------------------------------------------------------------------------ */
/* Failure */

static gboolean stopping(ArdSession *s)
{
	return g_atomic_int_get(&s->stopping);
}

static void fail(ArdSession *s, GError *error)
{
	if (!stopping(s) && g_atomic_int_compare_and_exchange(&s->failed, 0, 1)) {
		ARD_WARNING("the session with the Mac ended: %s", error->message);
		s->cb.closed(s->cb.user, error);
	}
	g_error_free(error);
}

static void fail_with(ArdSession *s, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);
static void fail_with(ArdSession *s, const gchar *fmt, ...)
{
	va_list args;
	gchar *message;

	va_start(args, fmt);
	message = g_strdup_vprintf(fmt, args);
	va_end(args);
	fail(s, g_error_new_literal(ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, message));
	g_free(message);
}

/* ------------------------------------------------------------------------ */
/* Messages */

static gboolean send_all(int fd, const guint8 *data, gsize len)
{
	while (len > 0) {
		ssize_t n = send(fd, data, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return FALSE;
		}
		data += n;
		len -= n;
	}
	return TRUE;
}

static void queue(ArdSession *s, const guint8 *msg, gsize len)
{
	g_async_queue_push(s->outgoing, g_bytes_new(msg, len));
}

static void queue_array(ArdSession *s, GByteArray *msg)
{
	queue(s, msg->data, msg->len);
	g_byte_array_unref(msg);
}

/* An Apple control message: type, a reserved byte, the body's length, the body. */
static GByteArray *control(guint8 type, GByteArray *body)
{
	GByteArray *msg = g_byte_array_sized_new(4 + body->len);
	guint8 head[2] = { type, 0 };

	g_byte_array_append(msg, head, 2);
	ard_append16(msg, body->len);
	g_byte_array_append(msg, body->data, body->len);
	g_byte_array_unref(body);
	return msg;
}

/* Identify this viewer before enabling its optional control messages: two
 * numeric version triples and the server messages it handles. A mis-sized body
 * makes the Mac swallow the next message and hang. */
static void viewer_info(guint8 msg[66])
{
	memset(msg, 0, 66);
	msg[0] = 0x21;
	ard_put16(msg + 2, 62);
	ard_put16(msg + 4, 1);
	ard_put32(msg + 6, 2);
	ard_put32(msg + 10, 6);
	ard_put32(msg + 14, 1);
	/* The macOS the values above were measured on, 26.6.2: the daemon handles
	 * keys from a viewer it believes older than 10.15 the old way. */
	ard_put32(msg + 22, 26);
	ard_put32(msg + 26, 6);
	ard_put32(msg + 30, 2);
	msg[34] = 0xb0;
	msg[36] = 0x0c;
	msg[37] = 0x03;
	msg[38] = 0x90;
	msg[44] = 0x40;
}

static GByteArray *set_encodings(gboolean media_stream)
{
	GByteArray *msg = g_byte_array_new();
	guint8 head[2] = { 2, 0 };

	g_byte_array_append(msg, head, 2);
	ard_append16(msg, G_N_ELEMENTS(encodings) + (media_stream ? 1 : 0));
	if (media_stream)
		ard_append32(msg, ARD_ENCODING_MEDIA_STREAM);
	for (gsize i = 0; i < G_N_ELEMENTS(encodings); i++)
		ard_append32(msg, encodings[i]);
	return msg;
}

/* 32 bits a pixel, BGRx little-endian. Nothing is decoded in it: the Mac's
 * rectangles are stepped over. */
static void set_pixel_format(guint8 msg[20])
{
	memset(msg, 0, 20);
	msg[4] = 32;
	msg[5] = 24;
	msg[7] = 1;
	ard_put16(msg + 8, 255);
	ard_put16(msg + 10, 255);
	ard_put16(msg + 12, 255);
	msg[14] = 16;
	msg[15] = 8;
}

static void auto_framebuffer_update(guint16 w, guint16 h, guint8 msg[16])
{
	memset(msg, 0, 16);
	msg[0] = 0x09;
	ard_put16(msg + 2, 1);
	ard_put32(msg + 4, PUSH_INTERVAL_US);
	ard_put16(msg + 12, w);
	ard_put16(msg + 14, h);
}

static void update_request(gboolean incremental, guint8 msg[10])
{
	memset(msg, 0, 10);
	msg[0] = 3;
	msg[1] = incremental;
	/* One pixel at the origin, which every display has. A request sized for a
	 * display that has since shrunk crashes the Mac's capture agent, and the
	 * picture is the media stream anyway. */
	ard_put16(msg + 6, 1);
	ard_put16(msg + 8, 1);
}

static void auto_pasteboard(guint8 msg[8])
{
	static const guint8 start[8] = { 0x15, 0, 0, 1, 0, 0, 0, 0 };

	memcpy(msg, start, 8);
}

/* One virtual display with one mode, its name Apple's viewer's own, dynamic
 * resolution on so a new session restores the Mac's checkbox. */
static GByteArray *set_display_configuration(const Mode *mode)
{
	static const gchar name[] = "Screen Sharing Virtual Display";
	GByteArray *body = g_byte_array_new();
	gsize start;

	ard_append16(body, 1);		/* version */
	ard_append16(body, 1);		/* display count */
	ard_append32(body, 0);		/* flags */
	start = body->len;
	ard_append16(body, 0x9c + 0x1c);
	g_byte_array_append(body, (const guint8 *)name, strlen(name));
	g_byte_array_set_size(body, start + 0x7a);
	memset(body->data + start + 2 + strlen(name), 0, 0x7a - 2 - strlen(name));
	ard_append32(body, 1);		/* dynamic resolution */
	ard_append32(body, 4);		/* virtual */
	ard_append_f32(body, mode->pt_w / NOMINAL_DPI * 25.4);
	ard_append_f32(body, mode->pt_h / NOMINAL_DPI * 25.4);
	ard_append32(body, MAX_PIXELS_W);
	ard_append32(body, MAX_PIXELS_H);
	ard_append16(body, 0);		/* current mode */
	ard_append16(body, 0);		/* preferred mode */
	ard_append32(body, 7);		/* rotations, as Apple's viewer sends them */
	ard_append16(body, 1);		/* mode count */
	ard_append32(body, mode->px_w);
	ard_append32(body, mode->px_h);
	ard_append32(body, mode->pt_w);
	ard_append32(body, mode->pt_h);
	ard_append_f64(body, DISPLAY_HZ);
	ard_append32(body, 0);		/* mode flags: no HDR */
	return control(MSG_SET_DISPLAY_CONFIGURATION, body);
}

/* The mode `points` at `density` resolves to, at no less than the least the
 * Mac is asked for and held under the backing ceiling, with the points
 * re-derived from the pixels so the two state the same ratio. */
static Mode mode_for(guint16 w, guint16 h, gint density, ArdCodec codec)
{
	Mode m;
	gint pw = CLAMP((gint)lround((gdouble)MAX(w, MIN_POINTS_W) * density), 2, MAX_PIXELS_W);
	gint ph = CLAMP((gint)lround((gdouble)MAX(h, MIN_POINTS_H) * density), 2, MAX_PIXELS_H);

	/* A physical Mac sends no whole H.264 picture with an odd side. Its HEVC
	 * pads one itself. */
	if (codec == ARD_CODEC_H264) {
		pw &= ~1;
		ph &= ~1;
	}
	m.px_w = pw;
	m.px_h = ph;
	m.pt_w = MAX(1, lround((gdouble)pw / density));
	m.pt_h = MAX(1, lround((gdouble)ph / density));
	return m;
}

/* ------------------------------------------------------------------------ */
/* Reading */

static gboolean fill(ArdSession *s, gsize n, GError **error)
{
	while (s->raw->len - s->raw_at < n) {
		gsize old;
		ssize_t got;

		if (s->raw_at == s->raw->len) {
			g_byte_array_set_size(s->raw, 0);
			s->raw_at = 0;
		} else if (s->raw_at > 65536) {
			g_byte_array_remove_range(s->raw, 0, s->raw_at);
			s->raw_at = 0;
		}
		old = s->raw->len;
		g_byte_array_set_size(s->raw, old + 65536);
		got = recv(s->fd, s->raw->data + old, 65536, 0);
		g_byte_array_set_size(s->raw, old + MAX(got, 0));
		if (got > 0)
			continue;
		if (got < 0 && errno == EINTR)
			continue;
		if (got == 0)
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac closed the connection");
		else
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the connection to the Mac failed: %s",
				    g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static gboolean raw_read(ArdSession *s, guint8 *out, gsize n, GError **error)
{
	if (!fill(s, n, error))
		return FALSE;
	if (out)
		memcpy(out, s->raw->data + s->raw_at, n);
	s->raw_at += n;
	return TRUE;
}

/* The next record's body, verified before a byte of it is handed upward. */
static gboolean next_record(ArdSession *s, GError **error)
{
	guint8 len_be[2];
	gsize len, body_at, body_len;

	if (!raw_read(s, len_be, 2, error))
		return FALSE;
	len = ard_get16(len_be);
	if (!ard_record_len_ok(len)) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "a record claims %" G_GSIZE_FORMAT " ciphertext bytes", len);
		return FALSE;
	}
	if (!fill(s, len, error))
		return FALSE;
	if (!ard_record_open(&s->in, s->raw->data + s->raw_at, len, &body_at, &body_len, error))
		return FALSE;
	g_byte_array_set_size(s->plain, 0);
	g_byte_array_append(s->plain, s->raw->data + s->raw_at + body_at, body_len);
	s->plain_at = 0;
	s->raw_at += len;
	return TRUE;
}

/* Read `n` bytes of the session's plaintext, or skip them when `out` is NULL.
 * A message's records are concatenated: a rectangle whose bytes span several
 * records is one read. */
static gboolean read_bytes(ArdSession *s, guint8 *out, guint64 n, GError **error)
{
	if (!s->records) {
		while (n > 0) {
			gsize take = MIN(n, 65536);

			if (!raw_read(s, out, take, error))
				return FALSE;
			if (out)
				out += take;
			n -= take;
		}
		return TRUE;
	}
	while (n > 0) {
		gsize take;

		if (s->plain_at == s->plain->len && !next_record(s, error))
			return FALSE;
		take = MIN(n, s->plain->len - s->plain_at);
		if (out) {
			memcpy(out, s->plain->data + s->plain_at, take);
			out += take;
		}
		s->plain_at += take;
		n -= take;
	}
	return TRUE;
}

static gboolean read_u8(ArdSession *s, guint8 *v, GError **error)
{
	return read_bytes(s, v, 1, error);
}

static gboolean read_u16(ArdSession *s, guint16 *v, GError **error)
{
	guint8 b[2];

	if (!read_bytes(s, b, 2, error))
		return FALSE;
	*v = ard_get16(b);
	return TRUE;
}

static gboolean read_u32(ArdSession *s, guint32 *v, GError **error)
{
	guint8 b[4];

	if (!read_bytes(s, b, 4, error))
		return FALSE;
	*v = ard_get32(b);
	return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Connecting */

static gboolean connect_tcp(ArdSession *s, GError **error)
{
	struct addrinfo hints = { 0 }, *found = NULL;
	gchar port[16];
	int ret;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	g_snprintf(port, sizeof(port), "%d", s->params.port);
	ret = getaddrinfo(s->params.host, port, &hints, &found);
	if (ret != 0) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not resolve %s: %s",
			    s->params.host, gai_strerror(ret));
		return FALSE;
	}
	for (struct addrinfo *a = found; a; a = a->ai_next) {
		int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		struct pollfd pfd;
		int err = 0, one = 1;
		socklen_t len = sizeof(err);

		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (connect(fd, a->ai_addr, a->ai_addrlen) < 0 && errno != EINPROGRESS) {
			err = errno;
		} else {
			gint waited = 0, ready = 0;

			pfd.fd = fd;
			pfd.events = POLLOUT;
			/* In slices, so a session stopped meanwhile is not held up. */
			while (ready == 0 && waited < CONNECT_TIMEOUT_MS && !stopping(s)) {
				ready = poll(&pfd, 1, 200);
				waited += 200;
			}
			if (ready <= 0)
				err = stopping(s) ? ECANCELED : ETIMEDOUT;
			else
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
		}
		if (err != 0) {
			g_clear_error(error);
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not connect to %s:%d: %s",
				    s->params.host, s->params.port, g_strerror(err));
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		len = sizeof(s->local);
		getsockname(fd, (struct sockaddr *)&s->local, &len);
		len = sizeof(s->peer);
		getpeername(fd, (struct sockaddr *)&s->peer, &len);
		/* Under the lock ard_session_stop() shuts the socket down under, so
		 * either it sees this socket or this sees it stopping. */
		g_mutex_lock(&s->lock);
		if (stopping(s)) {
			g_mutex_unlock(&s->lock);
			close(fd);
			g_clear_error(error);
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the session was stopped");
			break;
		}
		s->fd = fd;
		g_mutex_unlock(&s->lock);
		g_clear_error(error);
		break;
	}
	freeaddrinfo(found);
	if (s->fd < 0 && error && !*error)
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not connect to %s:%d",
			    s->params.host, s->params.port);
	return s->fd >= 0;
}

/* Apple's Diffie-Hellman authentication, security type 30. Authenticated with
 * a password alone, a connection is nobody, and macOS answers it with a new
 * login window on a virtual display: the account named is what attaches the
 * session to that user's own. */
static gboolean authenticate(ArdSession *s, guint8 wrap_key[16], GError **error)
{
	guint8 head[4];
	guint16 generator;
	gsize key_len;
	guint8 *prime, *peer, *secret, *public_key;
	guint8 credentials[ARD_CREDENTIALS_LEN];
	gsize user_len = strlen(s->username), pass_len = strlen(s->password);
	gboolean ok = FALSE;

	if (user_len >= ARD_CREDENTIAL_FIELD || pass_len >= ARD_CREDENTIAL_FIELD) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "the username and the password can be at most %d bytes each", ARD_CREDENTIAL_FIELD - 1);
		return FALSE;
	}
	if (!read_bytes(s, head, 4, error))
		return FALSE;
	generator = ard_get16(head);
	key_len = ard_get16(head + 2);
	if (key_len < ARD_DH_MIN_KEY || key_len > ARD_DH_MAX_KEY) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "the Mac offered a %" G_GSIZE_FORMAT "-byte DH key", key_len);
		return FALSE;
	}
	prime = g_malloc(key_len);
	peer = g_malloc(key_len);
	secret = g_malloc(key_len);
	public_key = g_malloc(key_len);
	if (!read_bytes(s, prime, key_len, error) || !read_bytes(s, peer, key_len, error)
	    || !ard_dh_exchange(generator, prime, peer, key_len, secret, public_key, error))
		goto out;
	ard_md5(secret, key_len, wrap_key);
	/* The name at 0 and the password at 64, each NUL-terminated, the rest
	 * random so identical credentials do not encrypt alike. */
	RAND_bytes(credentials, sizeof(credentials));
	memcpy(credentials, s->username, user_len + 1);
	memcpy(credentials + ARD_CREDENTIAL_FIELD, s->password, pass_len + 1);
	ard_aes128_ecb(wrap_key, TRUE, credentials, sizeof(credentials));
	ok = send_all(s->fd, credentials, sizeof(credentials)) && send_all(s->fd, public_key, key_len);
	if (!ok)
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not send the credentials: %s",
			    g_strerror(errno));
out:
	OPENSSL_cleanse(credentials, sizeof(credentials));
	OPENSSL_cleanse(secret, key_len);
	g_free(prime);
	g_free(peer);
	g_free(secret);
	g_free(public_key);
	return ok;
}

/* Cleartext server messages until the rekey, whose key and IV start the record
 * layer both ways. The Mac may send a bell or a MiscStatus in this window,
 * after a restart with stale pasteboard state; anything else is a stream gone
 * somewhere unexpected. */
static gboolean await_rekey(ArdSession *s, const guint8 wrap_key[16], guint8 key[16], guint8 iv[16], GError **error)
{
	for (;;) {
		guint8 type, pad;
		guint16 count, len;

		if (!read_u8(s, &type, error))
			return FALSE;
		switch (type) {
		case 0: {
			guint8 header[12];
			guint8 body[ARD_REKEY_LEN];
			gint32 encoding;

			if (!read_u8(s, &pad, error) || !read_u16(s, &count, error))
				return FALSE;
			if (count == 0)
				continue;
			if (!read_bytes(s, header, 12, error))
				return FALSE;
			encoding = (gint32)ard_get32(header + 8);
			if (encoding != ENC_REKEY || count != 1) {
				g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
					    "the Mac sent encoding %d before the record layer was up", encoding);
				return FALSE;
			}
			if (!read_bytes(s, body, sizeof(body), error))
				return FALSE;
			ard_unwrap_rekey(wrap_key, body, key, iv);
			return TRUE;
		}
		case 2:
			continue;
		case 0x14:
			if (!read_u8(s, &pad, error) || !read_u16(s, &len, error) || !read_bytes(s, NULL, len, error))
				return FALSE;
			continue;
		default:
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
				    "the Mac sent message type 0x%02x before the record layer was up", type);
			return FALSE;
		}
	}
}

/* Whether the command bitmap of the Mac's extended ServerInit lists client
 * message `type`, most significant bit first. */
static gboolean accepts(const guint8 commands[16], guint8 type)
{
	return (commands[type >> 3] >> (7 - (type & 7))) & 1;
}

static gpointer writer_thread(gpointer data);

static gboolean handshake(ArdSession *s, GError **error)
{
	guint8 greeting[13] = { 0 };
	guint major = 0, minor = 0;
	guint8 count, types[256];
	guint8 wrap_key[16], key[16], iv[16];
	guint8 init[24];
	guint32 result, name_len;
	guint8 *name;
	guint8 commands[16];
	gboolean has_commands = FALSE;
	guint8 msg[66];
	static const guint8 client_init = 0x81;
	static const guint8 encryption_start[] = { 0x12, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1 };
	static const guint8 inbound_decryption[] = { 0x12, 0, 0, 2, 0, 1, 0, 0 };
	gboolean found = FALSE;

	if (!read_bytes(s, greeting, 12, error))
		return FALSE;
	if (sscanf((const gchar *)greeting, "RFB %3u.%3u\n", &major, &minor) != 2 || major != 3 || minor != 889) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "the server is not a Mac's Screen Sharing: it speaks RFB %u.%u, not Apple's 3.889", major, minor);
		return FALSE;
	}
	if (!send_all(s->fd, (const guint8 *)"RFB 003.889\n", 12) || !read_u8(s, &count, error))
		goto send_failed;
	if (count == 0) {
		guint32 len;
		gchar *reason;

		if (!read_u32(s, &len, error))
			return FALSE;
		reason = g_malloc0(MIN(len, 4096) + 1);
		if (!read_bytes(s, (guint8 *)reason, MIN(len, 4096), error)) {
			g_free(reason);
			return FALSE;
		}
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac refused the connection: %s", reason);
		g_free(reason);
		return FALSE;
	}
	if (!read_bytes(s, types, count, error))
		return FALSE;
	for (guint i = 0; i < count; i++)
		found |= types[i] == SECURITY_ARD;
	if (!found) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "the Mac does not offer its account login (type 30); its Remote Management settings may "
			    "limit it to another login type");
		return FALSE;
	}
	if (!send_all(s->fd, &(guint8){ SECURITY_ARD }, 1))
		goto send_failed;
	if (!authenticate(s, wrap_key, error) || !read_u32(s, &result, error))
		return FALSE;
	/* On 003.889 a refusal is the word alone, with no reason after it. */
	if (result != 0) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_AUTH,
			    "the Mac refused the login: check the password, and that Remote Management lets this "
			    "account Observe and Control");
		return FALSE;
	}
	/* 0x80 asks for Apple's extended ServerInit. */
	if (!send_all(s->fd, &client_init, 1))
		goto send_failed;
	if (!read_bytes(s, init, 24, error))
		return FALSE;
	s->server_w = ard_get16(init);
	s->server_h = ard_get16(init + 2);
	name_len = ard_get32(init + 20);
	if (name_len > 1024 * 1024) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac's ServerInit is malformed");
		return FALSE;
	}
	name = g_malloc0(name_len + 1);
	if (!read_bytes(s, name, name_len, error)) {
		g_free(name);
		return FALSE;
	}
	/* The extended "name" is 22 bytes of structure, then the name: a zero u16,
	 * a u32 of flags, and a bitmap of the client messages the Mac accepts. */
	if (name_len >= 22 && name[0] == 0) {
		memcpy(commands, name + 6, 16);
		has_commands = TRUE;
		ARD_INFO("connected to \"%s\" (Apple flags 0x%08x)", name + 22, ard_get32(name + 2));
	}
	g_free(name);
	if (!has_commands || !accepts(commands, MSG_SET_DISPLAY_CONFIGURATION)) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "this Mac does not offer High Performance Screen Sharing: it does not take "
			    "SetDisplayConfiguration, without which there is no virtual display");
		return FALSE;
	}
	if (!accepts(commands, MSG_EVENT_2)) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "this Mac does not take Screen Sharing's scroll message");
		return FALSE;
	}

	/* The prelude, back to back in cleartext: the Mac switches both of its
	 * ciphers the moment it sends the rekey, so nothing may wait for a reply
	 * in between. ViewerInfo, SetMode(control) and AutoPasteboard(start), in
	 * that order, are what the pasteboard's change notices need. Apple's viewer
	 * asks for the record layer only when a preference says to; this always
	 * does, so neither the keystrokes nor the media stream's keys cross the
	 * network in the clear. */
	viewer_info(msg);
	if (!send_all(s->fd, msg, 66) || !send_all(s->fd, (const guint8 *)"\x0a\0\0\x01", 4))
		goto send_failed;
	auto_pasteboard(msg);
	if (!send_all(s->fd, msg, 8) || !send_all(s->fd, encryption_start, sizeof(encryption_start))
	    || !send_all(s->fd, inbound_decryption, sizeof(inbound_decryption)))
		goto send_failed;
	if (!await_rekey(s, wrap_key, key, iv, error))
		return FALSE;
	if (!ard_cbc_init(&s->in, key, iv, FALSE) || !ard_cbc_init(&s->out, key, iv, TRUE)) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not start the record layer");
		return FALSE;
	}
	s->records = TRUE;
	ARD_INFO("Apple record layer active");
	return TRUE;

send_failed:
	g_clear_error(error);
	g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "could not write to the Mac: %s", g_strerror(errno));
	return FALSE;
}

static void reset_connection(ArdSession *s)
{
	g_mutex_lock(&s->lock);
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
	g_mutex_unlock(&s->lock);
	g_byte_array_set_size(s->raw, 0);
	s->raw_at = 0;
	g_byte_array_set_size(s->plain, 0);
	s->plain_at = 0;
	s->records = FALSE;
	ard_cbc_clear(&s->in);
	ard_cbc_clear(&s->out);
}

/* ------------------------------------------------------------------------ */
/* The media stream */

static gboolean video_unit(gpointer user, ArdUnit *unit)
{
	ArdSession *s = user;

	return ard_video_push(s->video, unit);
}

static void audio_unit(gpointer user, const guint8 *data, gsize len)
{
	ArdSession *s = user;

	ard_audio_push(s->audio, data, len);
}

static gboolean video_lock(gpointer user, guint8 **pixels, gint *stride, gint *width, gint *height)
{
	ArdSession *s = user;

	return s->cb.lock(s->cb.user, pixels, stride, width, height);
}

static void video_unlock(gpointer user, gint y, gint rows)
{
	ArdSession *s = user;

	s->cb.unlock(s->cb.user, y, rows);
}

static void video_wants_keyframe(gpointer user)
{
	ArdSession *s = user;

	g_mutex_lock(&s->lock);
	if (s->receiver)
		ard_receiver_want_keyframe(s->receiver);
	g_mutex_unlock(&s->lock);
}

/* Offer the stream for the display, when it has settled, nothing is out, and
 * the Mac has named its ports since the last offer: it names them for the
 * encodings naming the stream and after each display change, never in reply to
 * an offer. Called with the lock held. */
static void try_offer(ArdSession *s)
{
	if (!s->laid_out || s->resize_out || s->moved_off || s->pending || !s->invited)
		return;
	if (s->offered && s->offered_w == s->fb_w && s->offered_h == s->fb_h)
		return;
	s->invited = FALSE;
	s->pending = TRUE;
	s->pending_at = g_get_monotonic_time();
	s->offered = TRUE;
	s->offered_w = s->fb_w;
	s->offered_h = s->fb_h;
	s->offered_at = s->pending_at;
	s->withheld = FALSE;
	if (s->receiver)
		ard_receiver_set_display(s->receiver, s->fb_w, s->fb_h);
	ARD_INFO("offering the Mac's media stream for its %ux%u display", s->fb_w, s->fb_h);
	queue_array(s, ard_offers_configuration(&s->offers, s->fb_w, s->fb_h));
}

static gboolean on_media_reply(ArdSession *s, const guint8 *body, gsize len, GError **error)
{
	ArdReply reply;
	gboolean down = FALSE;

	if (!ard_parse_media_reply(body, len, &reply, error))
		return FALSE;
	switch (reply.kind) {
	case ARD_REPLY_PORTS: {
		ArdReceiverCallbacks callbacks = { video_unit, audio_unit, s };

		if (reply.second_video) {
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
				    "the Mac enabled a second video leg for the one display this session asked for");
			return FALSE;
		}
		g_mutex_lock(&s->lock);
		s->invited = TRUE;
		s->ports_named = TRUE;
		if (s->offered) {
			/* A display change, the Mac's own included, stops the stream
			 * until it is offered again. */
			s->offered = FALSE;
			down = TRUE;
		}
		if (!s->receiver || !ard_receiver_matches(s->receiver, reply.audio_port, reply.video_port)) {
			ArdReceiver *old = s->receiver;

			s->receiver = NULL;
			g_mutex_unlock(&s->lock);
			ard_receiver_stop(old);
			ARD_INFO("the Mac opened its media streams: screen video at UDP %u, sound at %u",
				 reply.video_port, reply.audio_port);
			old = ard_receiver_start(&s->offers, &s->local, &s->peer, reply.audio_port, reply.video_port,
						 &callbacks, error);
			if (!old)
				return FALSE;
			g_mutex_lock(&s->lock);
			s->receiver = old;
		}
		try_offer(s);
		g_mutex_unlock(&s->lock);
		if (down)
			s->cb.covered(s->cb.user, "Screen not available");
		return TRUE;
	}
	case ARD_REPLY_ANSWER:
		ARD_DEBUG("the Mac accepted the media-stream offer");
		g_mutex_lock(&s->lock);
		s->pending = FALSE;
		/* The display may have changed while the answer was owed. */
		try_offer(s);
		g_mutex_unlock(&s->lock);
		return TRUE;
	case ARD_REPLY_ERROR:
		if (reply.error_kind == 1 && reply.error_sub == 1)
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
				    "another viewer already has the Mac's High Performance stream, which it gives to one "
				    "at a time");
		else
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
				    "the Mac refused the media stream (error type %u, sub-code %u)",
				    reply.error_kind, reply.error_sub);
		return FALSE;
	default:
		ARD_DEBUG("ignoring media-stream message type %u", reply.other);
		return TRUE;
	}
}

/* ------------------------------------------------------------------------ */
/* The read loop */

static void cursor_free(gpointer data)
{
	Cursor *c = data;

	cairo_surface_destroy(c->surface);
	g_free(c);
}

/* Apple's cursor shapes arrive as a cache: stored once with pixels and then
 * selected by id. A store is BGRx pixels followed by a separate alpha plane,
 * each store its own zlib stream. A bad one is skipped without disturbing the
 * next. */
static void on_cursor(ArdSession *s, guint32 id, guint16 hot_x, guint16 hot_y, guint16 w, guint16 h,
		      const guint8 *deflated, gsize len)
{
	Cursor *c;
	guint8 *raw;
	uLongf raw_len;
	gsize pixels;
	guint8 *dst;
	int stride;

	if (len == 0) {
		c = g_hash_table_lookup(s->cursors, GUINT_TO_POINTER(id));
		if (c)
			s->cb.cursor(s->cb.user, c->surface, c->hot_x, c->hot_y);
		return;
	}
	if (w == 0 || h == 0 || w > MAX_CURSOR_DIM || h > MAX_CURSOR_DIM) {
		ARD_DEBUG("ignoring a %ux%u cursor shape", w, h);
		return;
	}
	pixels = (gsize)w * h;
	raw_len = pixels * 5;
	raw = g_malloc(raw_len);
	if (uncompress(raw, &raw_len, deflated, len) != Z_OK || raw_len != pixels * 5) {
		ARD_DEBUG("ignoring cursor shape %u, which did not inflate", id);
		g_free(raw);
		return;
	}
	c = g_new0(Cursor, 1);
	c->hot_x = hot_x;
	c->hot_y = hot_y;
	c->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_surface_flush(c->surface);
	dst = cairo_image_surface_get_data(c->surface);
	stride = cairo_image_surface_get_stride(c->surface);
	for (gsize i = 0; i < pixels; i++) {
		const guint8 *px = raw + i * 4;
		guint32 a = raw[pixels * 4 + i];
		guint32 r = px[2] * a / 255, g = px[1] * a / 255, b = px[0] * a / 255;

		*(guint32 *)(dst + (i / w) * stride + (i % w) * 4) = (a << 24) | (r << 16) | (g << 8) | b;
	}
	cairo_surface_mark_dirty(c->surface);
	g_free(raw);
	g_hash_table_replace(s->cursors, GUINT_TO_POINTER(id), c);
	s->cb.cursor(s->cb.user, c->surface, c->hot_x, c->hot_y);
}

/* The Mac's display layout: which screens it has, which it is sending, and at
 * what size. The u16 before it counts what follows; a header of 0x14 bytes,
 * then 0x38 bytes a display. */
static gboolean on_layout(ArdSession *s, const guint8 *p, gsize len, GError **error)
{
	guint count;
	guint32 flags;
	gboolean on_virtual, first, changed;
	guint16 fb_w, fb_h, logical_w, backing_w;
	gdouble density;
	const guint8 *record;
	guint8 msg[16];

	if (len < 0x14 || (count = ard_get16(p + 0x12)) < 1 || count > 25 || len < 0x14 + count * 0x38) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac sent a malformed display layout");
		return FALSE;
	}
	fb_w = ard_get16(p + 0x06);
	fb_h = ard_get16(p + 0x08);
	flags = ard_get32(p + 0x0e);
	on_virtual = flags & 1;
	record = p + 0x14;
	/* Bounds are edges, top, left, bottom, right: a size is a difference. */
	logical_w = ard_get16(record + 0x14 + 6) - ard_get16(record + 0x14 + 2);
	backing_w = ard_get16(record + 0x1c + 6) - ard_get16(record + 0x1c + 2);
	density = ard_get_f64(record);
	if (!(density >= 1.0 && density <= 4.0))
		density = logical_w ? (gdouble)backing_w / logical_w : 1.0;
	if (fb_w == 0 || fb_h == 0) {
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac's display layout is empty");
		return FALSE;
	}
	ARD_DEBUG("display layout: %ux%u at %gx, flags 0x%x, %u display(s)", fb_w, fb_h, density, flags, count);

	g_mutex_lock(&s->lock);
	if (s->laid_out && s->on_virtual && !on_virtual) {
		/* A login at the Mac moves the connection to the new user session's
		 * agent, which has only the physical display. That is not this
		 * session's desktop: ask for the virtual display again, as Apple's
		 * viewer does, and offer its stream on the ports named at the start. */
		s->on_virtual = FALSE;
		s->moved_off = TRUE;
		s->offered = FALSE;
		s->pending = FALSE;
		queue_array(s, set_display_configuration(&s->mode));
		g_mutex_unlock(&s->lock);
		ARD_INFO("the Mac moved the session off its virtual display; asking for it again");
		s->cb.covered(s->cb.user, "The Mac switched user sessions; waiting for its screen");
		return TRUE;
	}
	if (s->moved_off && on_virtual) {
		s->moved_off = FALSE;
		s->invited = TRUE;
	}
	first = !s->laid_out;
	changed = first || fb_w != s->fb_w || fb_h != s->fb_h || density != s->density;
	s->laid_out = TRUE;
	s->on_virtual = on_virtual;
	s->fb_w = fb_w;
	s->fb_h = fb_h;
	s->density = density;
	s->resize_out = FALSE;
	if (first) {
		/* The list naming the stream first, held for the rest of the session.
		 * The Mac names its ports for it, and sends no pixels to a viewer whose
		 * preferred codec is the stream, which keeps its updates from meeting
		 * the answer to an offer inside one record. */
		s->asked = TRUE;
		s->asked_at = g_get_monotonic_time();
		queue_array(s, set_encodings(TRUE));
	}
	/* A layout drops the Mac's update arming. */
	auto_framebuffer_update(1, 1, msg);
	queue(s, msg, 16);
	auto_pasteboard(msg);
	queue(s, msg, 8);
	try_offer(s);
	g_mutex_unlock(&s->lock);
	if (changed) {
		s->cb.display(s->cb.user, fb_w, fb_h, density);
		s->cb.covered(s->cb.user, "Screen not available");
	}
	return TRUE;
}

static gboolean read_rect(ArdSession *s, gboolean *last, GError **error)
{
	guint8 head[12];
	guint16 x, y, w, h, len16;
	gint32 encoding;
	guint32 len32;
	guint8 *body;

	if (!read_bytes(s, head, 12, error))
		return FALSE;
	x = ard_get16(head);
	y = ard_get16(head + 2);
	w = ard_get16(head + 4);
	h = ard_get16(head + 6);
	encoding = (gint32)ard_get32(head + 8);
	switch (encoding) {
	case ENC_RAW:
		return read_bytes(s, NULL, (guint64)w * h * 4, error);
	case ENC_ZRLE:
	case ENC_ZLIB:
		/* The picture is the media stream: the Mac's own rectangles are
		 * stepped over by their length, never inflated. */
		if (!read_u32(s, &len32, error))
			return FALSE;
		return read_bytes(s, NULL, len32, error);
	case ENC_CURSOR_POS:
	case ENC_DESKTOP_SIZE:
		return TRUE;
	case ENC_LAST_RECT:
		*last = TRUE;
		return TRUE;
	case ENC_DISPLAY_INFO: {
		guint8 info[10];

		if (!read_bytes(s, info, 10, error))
			return FALSE;
		return read_bytes(s, NULL, (guint64)ard_get16(info + 8) * 0x1c, error);
	}
	case ENC_VENDOR_KEYSYMS:
	case ENC_KEYBOARD_SOURCE:
		return read_u16(s, &len16, error) && read_bytes(s, NULL, len16, error);
	case ENC_REKEY:
		/* Records this side already framed under the old key would fail the
		 * Mac's check, so an unrequested rotation cannot be followed. */
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "the Mac re-keyed mid-session");
		return FALSE;
	case ENC_CURSOR_IMAGE: {
		guint32 id;
		gboolean ok;

		if (!read_u32(s, &id, error) || !read_u32(s, &len32, error))
			return FALSE;
		if (len32 > MAX_CURSOR_BYTES) {
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "a cursor shape of %u bytes", len32);
			return FALSE;
		}
		body = g_malloc(MAX(len32, 1));
		ok = read_bytes(s, body, len32, error);
		if (ok)
			on_cursor(s, id, x, y, w, h, body, len32);
		g_free(body);
		return ok;
	}
	case ENC_DISPLAY_LAYOUT:
	case ARD_ENCODING_MEDIA_STREAM: {
		gboolean ok;

		if (!read_u16(s, &len16, error))
			return FALSE;
		body = g_malloc(MAX(len16, 1));
		ok = read_bytes(s, body, len16, error)
		     && (encoding == ENC_DISPLAY_LAYOUT ? on_layout(s, body, len16, error) : on_media_reply(s, body, len16, error));
		g_free(body);
		return ok;
	}
	default:
		g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
			    "the Mac sent encoding %d (0x%x), which was not advertised", encoding, encoding);
		return FALSE;
	}
}

static gboolean on_status(ArdSession *s, GError **error)
{
	guint8 pad, body[64];
	guint16 len;

	if (!read_u8(s, &pad, error) || !read_u16(s, &len, error))
		return FALSE;
	if (len > sizeof(body))
		return read_bytes(s, NULL, len, error);
	if (!read_bytes(s, body, len, error))
		return FALSE;
	if (len < 4)
		return TRUE;
	switch (ard_get16(body + 2)) {
	case 1:
		ARD_INFO("the Mac's user ended the session");
		break;
	case 2: {
		/* The Mac's pasteboard changed. */
		guint8 fetch[8];

		g_mutex_lock(&s->lock);
		ard_pasteboard_fetch(++s->paste_session, fetch);
		g_mutex_unlock(&s->lock);
		queue(s, fetch, 8);
		break;
	}
	case 3:
		/* The Mac needs the data of a flavor this side promised. */
		g_mutex_lock(&s->lock);
		if (s->local_text)
			queue_array(s, ard_pasteboard_send(s->paste_session, s->local_text));
		g_mutex_unlock(&s->lock);
		break;
	case 17:
		ARD_INFO("the Mac's user session changed: a login or a logout at the Mac");
		break;
	default:
		break;
	}
	return TRUE;
}

static gboolean on_pasteboard(ArdSession *s, GError **error)
{
	guint8 header[ARD_PASTEBOARD_HEADER];
	guint32 uncompressed, compressed;
	guint8 *data;
	gchar *text;

	if (!read_bytes(s, header, sizeof(header), error))
		return FALSE;
	uncompressed = ard_get32(header + 7);
	compressed = ard_get32(header + 11);
	if (compressed > ARD_PASTEBOARD_MAX || uncompressed > ARD_PASTEBOARD_MAX) {
		ARD_WARNING("ignoring the Mac's %u-byte pasteboard", uncompressed);
		return read_bytes(s, NULL, compressed, error);
	}
	data = g_malloc(MAX(compressed, 1));
	if (!read_bytes(s, data, compressed, error)) {
		g_free(data);
		return FALSE;
	}
	text = ard_pasteboard_text(data, compressed, uncompressed);
	g_free(data);
	if (text) {
		s->cb.clipboard(s->cb.user, text);
		g_free(text);
	}
	return TRUE;
}

static gboolean read_loop(ArdSession *s, GError **error)
{
	guint8 msg[10];

	while (!stopping(s)) {
		guint8 type, pad[3];
		guint16 count;
		guint32 len;

		if (!read_u8(s, &type, error))
			return FALSE;
		switch (type) {
		case 0: {
			gboolean last = FALSE;

			if (!read_u8(s, pad, error) || !read_u16(s, &count, error))
				return FALSE;
			for (guint i = 0; i < count && !last; i++)
				if (!read_rect(s, &last, error))
					return FALSE;
			update_request(TRUE, msg);
			queue(s, msg, 10);
			break;
		}
		case 2:
			break;
		case 3:
			if (!read_bytes(s, pad, 3, error) || !read_u32(s, &len, error) || !read_bytes(s, NULL, len, error))
				return FALSE;
			break;
		case 0x14:
			if (!on_status(s, error))
				return FALSE;
			break;
		case 0x1f:
			if (!on_pasteboard(s, error))
				return FALSE;
			break;
		default:
			g_set_error(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED,
				    "the Mac sent message type 0x%02x, which this viewer does not know", type);
			return FALSE;
		}
	}
	return TRUE;
}

/* Everything the session sends once the record layer is up, before it reads:
 * the virtual display, the pixel format, the encodings, and the arming. */
static void opening(ArdSession *s)
{
	guint8 msg[20];

	g_mutex_lock(&s->lock);
	queue_array(s, set_display_configuration(&s->mode));
	g_mutex_unlock(&s->lock);
	set_pixel_format(msg);
	queue(s, msg, 20);
	queue_array(s, set_encodings(FALSE));
	auto_framebuffer_update(s->server_w, s->server_h, msg);
	queue(s, msg, 16);
	update_request(FALSE, msg);
	queue(s, msg, 10);
}

static gpointer reader_thread(gpointer data)
{
	ArdSession *s = data;
	GError *error = NULL;
	ArdVideoCallbacks video = { video_lock, video_unlock, video_wants_keyframe, s };
	gboolean retry = FALSE;

	for (guint attempt = 0;; attempt++) {
		if (stopping(s))
			return NULL;
		if (retry || !s->username || !s->password) {
			g_free(s->username);
			g_free(s->password);
			s->username = s->password = NULL;
			if (!s->cb.credentials(s->cb.user, retry, &s->username, &s->password)) {
				fail(s, g_error_new_literal(ARD_SESSION_ERROR, ARD_SESSION_ERROR_FAILED, "no credentials were given"));
				return NULL;
			}
		}
		if (connect_tcp(s, &error) && handshake(s, &error))
			break;
		if (g_error_matches(error, ARD_SESSION_ERROR, ARD_SESSION_ERROR_AUTH) && attempt + 1 < AUTH_ATTEMPTS) {
			ARD_INFO("%s", error->message);
			g_clear_error(&error);
			reset_connection(s);
			retry = TRUE;
			continue;
		}
		fail(s, error);
		return NULL;
	}

	s->video = ard_video_start(s->params.codec, &video, &error);
	if (!s->video) {
		fail(s, error);
		return NULL;
	}
	if (s->params.sound)
		s->audio = ard_audio_start();
	s->writer = g_thread_new("applehp-writer", writer_thread, s);
	opening(s);
	s->cb.connected(s->cb.user);
	s->cb.covered(s->cb.user, "Waiting for the Mac's screen");
	if (!read_loop(s, &error))
		fail(s, error);
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* The writer, and the session's clock */

/* What the session owes the Mac and the stream owes the session, each a
 * deadline the session ends at. Apple's viewer ends its session on the same
 * failures and has no way back to RFB pixels. Called with the lock held;
 * returns the failure, if any. */
static gchar *check_stream(ArdSession *s, gint64 now)
{
	ArdReceiverTimes t;
	gchar *failure = NULL;

	if (s->asked && !s->ports_named && now - s->asked_at > STREAM_START)
		return g_strdup("the Mac named no ports for its media stream within 10 s");
	if (s->pending && now - s->pending_at > STREAM_START)
		return g_strdup("the Mac did not answer the media-stream offer within 10 s");
	if (!s->offered || s->pending || !s->receiver)
		return NULL;
	ard_receiver_times(s->receiver, &t);
	if (t.failure)
		return g_strdup_printf("the media stream failed: %s", t.failure);
	if (t.video_pictured < s->offered_at) {
		/* The Mac reports as a receiver, which is what a sender that has sent
		 * nothing sends: it withholds the display, as it does one too low for
		 * its strips. The stream otherwise runs, so the session waits. */
		if (t.video_receiver_report >= s->offered_at && t.video_sender_report < s->offered_at) {
			if (!s->withheld) {
				s->withheld = TRUE;
				g_mutex_unlock(&s->lock);
				s->cb.covered(s->cb.user, "The Mac sends no picture at this size; try another window size");
				g_mutex_lock(&s->lock);
			}
		} else if (now - s->offered_at > STREAM_START) {
			failure = g_strdup(t.video_heard >= s->offered_at
					   ? "no picture came over the Mac's media stream within 10 s, though its packets arrived"
					   : "no picture came over the Mac's media stream within 10 s: nothing reached this "
					     "computer's UDP port, which is what a firewall or NAT between it and the Mac looks like");
		}
	} else if (now - t.video_heard > STREAM_SILENCE) {
		failure = g_strdup("the Mac's media stream sent nothing on the picture's leg for 48 s");
	}
	if (!failure && t.audio_sounded < s->offered_at && now - s->offered_at > STREAM_START)
		failure = g_strdup("no sound came over the Mac's media stream within 10 s");
	if (!failure && t.audio_sounded >= s->offered_at && now - t.audio_heard > STREAM_SILENCE)
		failure = g_strdup("the Mac's media stream sent nothing on the sound's leg for 48 s");
	g_free(t.failure);
	return failure;
}

static void tick(ArdSession *s, gint64 now)
{
	gchar *failure;
	gboolean resized = FALSE;

	if (g_atomic_int_get(&s->failed))
		return;
	g_mutex_lock(&s->lock);
	/* A display change goes out once the display has settled and no offer is
	 * out: one sent while an offer's capture starts can crash WindowServer. */
	if (s->want_pending && s->laid_out && !s->resize_out && !s->pending && !s->moved_off) {
		s->want_pending = FALSE;
		if (s->want.px_w != s->fb_w || s->want.px_h != s->fb_h) {
			s->mode = s->want;
			s->resize_out = TRUE;
			s->resize_at = now;
			queue_array(s, set_display_configuration(&s->mode));
			resized = TRUE;
			ARD_DEBUG("asking the Mac for a %ux%u display (%ux%u points)", s->mode.px_w, s->mode.px_h,
				  s->mode.pt_w, s->mode.pt_h);
		}
	}
	if (s->resize_out && now - s->resize_at > RESIZE_TIMEOUT) {
		ARD_WARNING("the Mac did not confirm the display size asked for");
		s->resize_out = FALSE;
		try_offer(s);
	}
	failure = check_stream(s, now);
	g_mutex_unlock(&s->lock);
	if (resized)
		s->cb.covered(s->cb.user, "Resizing");
	if (failure) {
		fail_with(s, "%s", failure);
		g_free(failure);
	}
}

static gpointer writer_thread(gpointer data)
{
	ArdSession *s = data;
	GByteArray *framed = g_byte_array_new();
	gpointer item;

	while (!stopping(s)) {
		gint64 now;

		item = g_async_queue_timeout_pop(s->outgoing, TICK);
		if (item == STOP)
			break;
		if (item) {
			gsize len;
			const guint8 *msg = g_bytes_get_data(item, &len);

			g_byte_array_set_size(framed, 0);
			ard_record_frame(&s->out, msg, len, framed);
			g_bytes_unref(item);
			if (!send_all(s->fd, framed->data, framed->len)) {
				fail_with(s, "could not write to the Mac: %s", g_strerror(errno));
				break;
			}
		}
		now = g_get_monotonic_time();
		if (now - s->last_tick >= TICK) {
			s->last_tick = now;
			tick(s, now);
		}
	}
	g_byte_array_unref(framed);
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* The owner's side */

ArdSession *ard_session_start(const ArdSessionParams *params, const ArdSessionCallbacks *callbacks)
{
	ArdSession *s = g_new0(ArdSession, 1);

	s->params = *params;
	s->params.host = g_strdup(params->host);
	s->params.density = CLAMP(params->density, 1, 2);
	s->cb = *callbacks;
	s->fd = -1;
	s->raw = g_byte_array_new();
	s->plain = g_byte_array_new();
	s->outgoing = g_async_queue_new_full((GDestroyNotify)g_bytes_unref);
	s->cursors = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cursor_free);
	g_mutex_init(&s->lock);
	s->mode = mode_for(params->width, params->height, s->params.density, params->codec);
	ard_offers_init(&s->offers, params->codec);
	s->reader = g_thread_new("applehp-session", reader_thread, s);
	return s;
}

void ard_session_stop(ArdSession *s)
{
	gpointer item;
	ArdReceiver *receiver;

	if (!s)
		return;
	g_atomic_int_set(&s->stopping, 1);
	g_mutex_lock(&s->lock);
	if (s->fd >= 0)
		shutdown(s->fd, SHUT_RDWR);
	g_mutex_unlock(&s->lock);
	g_async_queue_push_front(s->outgoing, STOP);
	g_thread_join(s->reader);
	if (s->writer)
		g_thread_join(s->writer);
	g_mutex_lock(&s->lock);
	receiver = s->receiver;
	s->receiver = NULL;
	g_mutex_unlock(&s->lock);
	ard_receiver_stop(receiver);
	ard_video_stop(s->video);
	ard_audio_stop(s->audio);
	while ((item = g_async_queue_try_pop(s->outgoing)))
		if (item != STOP)
			g_bytes_unref(item);
	g_async_queue_unref(s->outgoing);
	reset_connection(s);
	g_byte_array_unref(s->raw);
	g_byte_array_unref(s->plain);
	g_hash_table_destroy(s->cursors);
	g_mutex_clear(&s->lock);
	if (s->password)
		OPENSSL_cleanse(s->password, strlen(s->password));
	g_free(s->username);
	g_free(s->password);
	g_free(s->local_text);
	g_free(s->params.host);
	g_free(s);
}

static gboolean accepting_input(ArdSession *s)
{
	return s->writer && !stopping(s) && !g_atomic_int_get(&s->failed);
}

void ard_session_pointer(ArdSession *s, guint8 buttons, gint x, gint y)
{
	guint8 msg[6] = { 5, buttons };

	if (!accepting_input(s))
		return;
	ard_put16(msg + 2, CLAMP(x, 0, G_MAXUINT16));
	ard_put16(msg + 4, CLAMP(y, 0, G_MAXUINT16));
	queue(s, msg, 6);
}

void ard_session_key(ArdSession *s, guint32 keysym, gboolean down)
{
	guint8 msg[8] = { 4, down };

	if (!accepting_input(s))
		return;
	ard_put32(msg + 4, keysym);
	queue(s, msg, 8);
}

/* The scroll-wheel event of the second event message: a distance on both
 * axes, which the wheel bits of the pointer mask cannot carry. The Mac counts
 * a scroll up and to the left as positive. The distance goes as the point
 * delta, a tenth of it as the line delta an older application reads, with the
 * continuous flag and no phase: a precise scroll with no gesture around it. */
void ard_session_scroll(ArdSession *s, gint dx, gint dy, gint x, gint y)
{
	gint axes[3] = { -dx, -dy, 0 };
	guint8 msg[58] = { MSG_EVENT_2, 0 };

	if (!accepting_input(s) || (dx == 0 && dy == 0))
		return;
	ard_put16(msg + 2, 54);
	ard_put16(msg + 4, 1);
	ard_put16(msg + 6, 11);
	for (int i = 0; i < 3; i++) {
		ard_put16(msg + 8 + 2 * i, (guint16)(gint16)(axes[i] / 10));
		ard_put32(msg + 14 + 4 * i, (guint32)(gint32)((gint64)axes[i] * 65536 / 10));
		ard_put32(msg + 26 + 4 * i, (guint32)axes[i]);
	}
	ard_put32(msg + 50, 0x02);
	ard_put16(msg + 54, CLAMP(x, 0, G_MAXUINT16));
	ard_put16(msg + 56, CLAMP(y, 0, G_MAXUINT16));
	queue(s, msg, sizeof(msg));
}

void ard_session_resize(ArdSession *s, guint16 width, guint16 height, gint density)
{
	Mode mode = mode_for(width, height, CLAMP(density, 1, 2), s->params.codec);

	g_mutex_lock(&s->lock);
	s->want = mode;
	s->want_pending = TRUE;
	g_mutex_unlock(&s->lock);
}

void ard_session_refresh(ArdSession *s)
{
	g_mutex_lock(&s->lock);
	if (s->receiver)
		ard_receiver_want_keyframe(s->receiver);
	g_mutex_unlock(&s->lock);
}

void ard_session_clipboard(ArdSession *s, const gchar *text)
{
	if (!accepting_input(s) || !text)
		return;
	g_mutex_lock(&s->lock);
	g_free(s->local_text);
	s->local_text = g_strdup(text);
	queue_array(s, ard_pasteboard_send(s->paste_session, text));
	g_mutex_unlock(&s->lock);
}

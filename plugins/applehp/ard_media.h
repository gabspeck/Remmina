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

/* High Performance's media stream: the picture and the sound the Mac sends
 * over UDP once the RFB session has negotiated it.
 *
 * The viewer lists encoding 1010, the Mac names its ports (message 1), and the
 * viewer offers the stream with message 0x1c: SRTP master keys for each leg and
 * two AVConference offers, a binary property list around a deflated protobuf
 * each. The Mac answers (message 2) and its ScreensharingAgent then sends
 * HEVC (or H.264) and AAC-ELD over SRTP to the viewer's address, from each port
 * it named to the same port number here.
 *
 * Nothing of this is documented by Apple. The formats and the rules here were
 * measured against macOS 26 by remotex, whose docs/apple-vnc-889.md records the
 * evidence. */

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <openssl/evp.h>

#define ARD_ENCODING_MEDIA_STREAM 1010

/* 0x1c flags: bit 0 configures the stream for 60 frames a second, and bit 2
 * has the agent capture without the pointer, which keeps arriving as its own
 * shape over RFB. */
#define ARD_MEDIA_FLAG_60FPS 0x1
#define ARD_MEDIA_FLAG_NO_CURSOR 0x4

/* The strips the Mac codes an HEVC display in when offered four tiles, as
 * Apple's viewer offers them. */
#define ARD_STRIPS 4

#define ARD_SRTP_TAG 10

typedef enum {
	ARD_CODEC_HEVC,
	ARD_CODEC_H264
} ArdCodec;

typedef struct {
	guint8 v2s[46];
	guint8 s2v[46];
} ArdKeyPair;

/* One session's negotiation identity: the keys, SSRCs and ids every offer of
 * the session carries. Each offer starts a fresh stream under new SSRCs on the
 * Mac's side, so these need not change between offers. */
typedef struct {
	guint8 session_uuid[16];
	gchar call_id[37];
	ArdKeyPair audio_keys;
	ArdKeyPair video_keys;
	guint32 audio_ssrc;
	guint32 video_ssrc;
	ArdCodec codec;
} ArdOffers;

void ard_offers_init(ArdOffers *offers, ArdCodec codec);

/* Whether the display is offered in strips: the HEVC is, the H.264 is not,
 * since in strips the Mac codes it again from scratch every frame. */
gboolean ard_offers_strips(const ArdOffers *offers);

/* The 0x1c message offering the stream for a display of `width`×`height`
 * backing pixels. */
GByteArray *ard_offers_configuration(const ArdOffers *offers, guint16 width, guint16 height);

/* The rows of each strip of a display `height` rows high: a quarter of them,
 * rounded down and then up to a multiple of 16. */
guint ard_strip_rows(guint height);

typedef enum {
	ARD_REPLY_PORTS,
	ARD_REPLY_ANSWER,
	ARD_REPLY_ERROR,
	ARD_REPLY_OTHER
} ArdReplyKind;

typedef struct {
	ArdReplyKind kind;
	guint16 audio_port;
	guint16 video_port;
	gboolean second_video;
	guint answered_videos;
	guint32 error_kind;
	guint32 error_sub;
	guint16 other;
} ArdReply;

/* The body of an encoding-1010 rectangle, after its u16 size. */
gboolean ard_parse_media_reply(const guint8 *body, gsize len, ArdReply *reply, GError **error);

/* ------------------------------------------------------------------------ */
/* SRTP and SRTCP (RFC 3711): AES-256 counter mode, HMAC-SHA1-80 */

typedef struct {
	EVP_CIPHER_CTX *ctr;
	guint8 auth[20];
	guint8 salt[14];
} ArdSrtpKeys;

typedef struct {
	gboolean used;
	guint32 ssrc;
	guint16 seq;
	guint32 roc;
} ArdSrtpSeen;

typedef struct {
	guint8 payload_type;
	gboolean marker;
	guint16 sequence;
	guint32 timestamp;
	guint32 ssrc;
	/* The Mac's mark on a picture predicted from one this side acknowledged. */
	gboolean refresh;
	gsize payload_at;
	gsize payload_end;
} ArdRtpHeader;

typedef enum {
	ARD_SRTP_OK,
	ARD_SRTP_NOT_RTP,
	ARD_SRTP_RTCP,
	ARD_SRTP_FORGED,
	ARD_SRTP_STALE
} ArdSrtpResult;

/* The receiving side of one leg's SRTP. A picture in strips comes under an
 * SSRC for each strip, each with sequence numbers of its own. */
typedef struct {
	ArdSrtpKeys keys;
	ArdSrtpSeen seen[ARD_STRIPS];
	guint next_slot;
} ArdSrtpReceiver;

typedef struct {
	ArdSrtpKeys keys;
	guint32 index;
} ArdSrtcpSender;

typedef struct {
	ArdSrtpKeys keys;
	GHashTable *last;
} ArdSrtcpReceiver;

void ard_srtp_receiver_init(ArdSrtpReceiver *r, const guint8 master[46]);
void ard_srtp_receiver_clear(ArdSrtpReceiver *r);
/* Authenticate and decrypt one datagram in place. */
ArdSrtpResult ard_srtp_unprotect(ArdSrtpReceiver *r, guint8 *data, gsize len, ArdRtpHeader *header);

void ard_srtcp_sender_init(ArdSrtcpSender *s, const guint8 master[46]);
void ard_srtcp_sender_clear(ArdSrtcpSender *s);
/* `report` protected into `out`, which needs len + 14 bytes. Returns the
 * protected length. */
gsize ard_srtcp_protect(ArdSrtcpSender *s, const guint8 *report, gsize len, guint8 *out);

void ard_srtcp_receiver_init(ArdSrtcpReceiver *r, const guint8 master[46]);
void ard_srtcp_receiver_clear(ArdSrtcpReceiver *r);
/* Whether `data` is an authentic report newer than any received from its
 * sender. It is never decrypted: only its header, which is in the clear, is
 * read. */
gboolean ard_srtcp_authenticate(ArdSrtcpReceiver *r, const guint8 *data, gsize len);

#define ARD_RTCP_SENDER_REPORT 200
#define ARD_RTCP_RECEIVER_REPORT 201

gsize ard_rtcp_receiver_report(guint32 ssrc, guint8 out[8]);
gsize ard_rtcp_reference_ack(guint32 ssrc, guint32 picture, guint8 out[16]);
gsize ard_rtcp_refresh(guint32 ssrc, guint32 media_ssrc, guint16 width, guint16 height, guint8 out[16]);
gsize ard_rtcp_pli(guint32 ssrc, guint32 media_ssrc, guint8 out[12]);

/* ------------------------------------------------------------------------ */
/* Rate feedback: the RCTL report Apple's viewer sends every 50 ms */

typedef struct {
	gboolean started;
	guint32 previous;
	gint64 first_at;
	guint64 ticks;
	gdouble shorter;
	gdouble longer;
	gdouble owrd;
} ArdOneWayDelay;

typedef struct {
	gint64 epoch;
	guint32 ssrc;
	gboolean have_last;
	guint32 last_timestamp;
	gint64 last_at;
	guint32 received;
	ArdOneWayDelay delay;
} ArdRateFeedback;

void ard_rate_feedback_init(ArdRateFeedback *f, gint64 epoch);
/* An authentic picture packet from `ssrc`, stamped `timestamp`, that arrived
 * at `at` (monotonic microseconds). */
void ard_rate_feedback_received(ArdRateFeedback *f, guint32 ssrc, guint32 timestamp, gint64 at);
/* The report from `ssrc` at `now`, once a picture packet has arrived to echo. */
gboolean ard_rate_feedback_report(const ArdRateFeedback *f, guint32 ssrc, gint64 now, guint8 out[32]);

/* ------------------------------------------------------------------------ */
/* HEVC over RTP (RFC 7798, with Apple's layout of the decoding order numbers)
 * and H.264 (RFC 6184), into Annex B access units */

typedef struct {
	/* The picture's NAL units as Annex B. */
	GByteArray *data;
	/* A picture a decoder can start at: HEVC's IRAP, H.264's IDR. */
	gboolean keyframe;
	/* Which strip of the display, or -1 for a picture of the whole display. */
	gint strip;
	guint32 timestamp;
	/* What the Mac files the picture under for its acknowledgement: its
	 * decoding order number in strips, its timestamp otherwise. */
	guint32 acknowledgement;
	/* The size the display was offered at, which describes a strip. */
	guint16 display_width;
	guint16 display_height;
} ArdUnit;

void ard_unit_free(ArdUnit *unit);

typedef struct {
	ArdCodec codec;
	/* Whether payloads carry decoding order numbers, as a stream in strips
	 * does: an aggregation packet has one after its payload header, and
	 * every fragment has one after its fragment header. */
	gboolean donl;
	gboolean have_seq;
	guint16 next_seq;
	gboolean have_ts;
	guint32 ts;
	GByteArray *unit;
	GByteArray *fragment;
	gboolean in_fragment;
	gboolean damaged;
	gboolean refresh;
	gboolean keyframe;
	gboolean params;
	gboolean have_don;
	guint16 don;
} ArdReassembler;

typedef enum {
	ARD_DEPACK_PENDING,
	ARD_DEPACK_UNIT,
	ARD_DEPACK_LOST
} ArdDepack;

/* A stream whose pictures come whole. A gap drops everything up to a picture
 * the decoder can start at or, of the HEVC, one the Mac marks as a refresh. */
typedef struct {
	ArdCodec codec;
	gboolean have_ssrc;
	guint32 ssrc;
	ArdReassembler packets;
	gboolean synced;
	gboolean resumable;
} ArdWhole;

/* A display in strips. One HEVC stream in one decoding order: a gap drops
 * everything up to strip 0's next IDR. */
typedef struct {
	gboolean have_base;
	guint32 base;
	ArdReassembler strips[ARD_STRIPS];
	gboolean have_next;
	guint16 next;
	/* Strips whose picture of the last keyframe is still to come. */
	guint8 owed;
} ArdStrips;

typedef struct {
	gboolean in_strips;
	ArdWhole whole;
	ArdStrips strips;
} ArdAssembly;

void ard_assembly_init(ArdAssembly *a, ArdCodec codec, gboolean in_strips);
void ard_assembly_clear(ArdAssembly *a);
/* The SSRC that names the stream `ssrc` belongs to: its own, or of a stream in
 * strips, the first strip's. */
guint32 ard_assembly_stream(ArdAssembly *a, guint32 ssrc);
/* Take one packet. On ARD_DEPACK_UNIT `*unit` holds a picture to decode. */
ArdDepack ard_assembly_push(ArdAssembly *a, const ArdRtpHeader *h, const guint8 *payload, gsize len, ArdUnit **unit);
void ard_assembly_resync(ArdAssembly *a);
/* Whether a refresh picture is enough to go on from. */
gboolean ard_assembly_resumes_at_refresh(const ArdAssembly *a);
/* Whether the unit just handed on leaves the stream with everything to go on
 * from: any picture sent whole, and in strips, the last of its keyframe's. */
gboolean ard_assembly_complete(const ArdAssembly *a);

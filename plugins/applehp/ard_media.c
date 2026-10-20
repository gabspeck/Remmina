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

#include "ard_media.h"
#include "ard_wire.h"

#include <string.h>
#include <zlib.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

/* ------------------------------------------------------------------------ */
/* The AVConference offers, rebuilt field by field from Apple's viewer's */

#define MODE_AUDIO 8
#define MODE_VIDEO 7

/* The viewer's model and builds, as Apple's viewer on macOS 26.6 reported them. */
static const guint8 remote_endpoint_info[] = {
	0x08, 0x00,
	0x10, 0x01,
	0x1a, 0x0d, 'V', 'i', 'r', 't', 'u', 'a', 'l', 'M', 'a', 'c', '2', ',', '1',
	0x22, 0x08, '2', '2', '1', '5', '.', '5', '.', '1',
	0x2a, 0x05, '2', '5', 'G', '8', '3',
};

#define VICEROY "Viceroy 1.7.0"
#define AUDIO_F13 G_GUINT64_CONSTANT(17169649764059066368)
#define VIDEO_F13 G_GUINT64_CONSTANT(17169649764516085760)

typedef struct {
	guint64 f1, f2, f3;
	gboolean has_f3;
} CodecEntry;

/* The audio codecs (AAC-ELD {16, 4100}, AMR-NB {1, 299}, EVS {4, 6500}) and,
 * with f1 = 0, bitrates in bits per second. The Mac's rate controller takes
 * its ceiling from the 60 Mbit/s entry, whose f3 is 262144; deleting it leaves
 * the encoder a bitrate of 0. */
static const CodecEntry codec_entries[] = {
	{ 1,   299,	    0,	     FALSE },
	{ 4074,	   0,	      16384,   TRUE  },
	{ 0,   75000000,  524288,  TRUE  },
	{ 0,   20000000,  98304,   TRUE  },
	{ 0,   40000000,  12288,   TRUE  },
	{ 0,   60000000,  262144,  TRUE  },
	{ 16,  4100,	    0,	     FALSE },
	{ 0,   100000000, 1048576, TRUE  },
	{ 4,   6500,	    0,	     FALSE },
	{ 0,   6000000,	  131072,  TRUE  },
};

static void pb_varint(GByteArray *a, guint64 v)
{
	while (v >= 0x80) {
		guint8 b = (v & 0x7f) | 0x80;

		g_byte_array_append(a, &b, 1);
		v >>= 7;
	}
	guint8 b = v;

	g_byte_array_append(a, &b, 1);
}

static void pb_uint(GByteArray *a, guint64 field, guint64 v)
{
	pb_varint(a, field << 3);
	pb_varint(a, v);
}

static void pb_bytes(GByteArray *a, guint64 field, const guint8 *data, gsize len)
{
	pb_varint(a, (field << 3) | 2);
	pb_varint(a, len);
	g_byte_array_append(a, data, len);
}

/* Write `inner` as a length-delimited field of `a`, and free it. */
static void pb_message(GByteArray *a, guint64 field, GByteArray *inner)
{
	pb_bytes(a, field, inner->data, inner->len);
	g_byte_array_unref(inner);
}

static GByteArray *codec_entry(const CodecEntry *e)
{
	GByteArray *entry = g_byte_array_new();

	pb_uint(entry, 1, e->f1);
	pb_uint(entry, 2, e->f2);
	if (e->has_f3)
		pb_uint(entry, 3, e->f3);
	return entry;
}

static void blob_tail(GByteArray *blob, const CodecEntry *entries, gsize n, guint64 f13)
{
	pb_bytes(blob, 6, (const guint8 *)VICEROY, strlen(VICEROY));
	pb_uint(blob, 8, 0);
	for (gsize i = 0; i < n; i++)
		pb_message(blob, 9, codec_entry(&entries[i]));
	pb_uint(blob, 13, f13);
	pb_uint(blob, 14, 2);
	pb_uint(blob, 16, 0);
}

static GByteArray *audio_offer_blob(guint32 ssrc)
{
	GByteArray *blob = g_byte_array_new();
	GByteArray *stream = g_byte_array_new();

	pb_uint(blob, 1, 1);
	pb_uint(blob, 2, 1);
	pb_uint(stream, 1, ssrc);
	pb_uint(stream, 2, 0);
	pb_uint(stream, 3, 0);
	/* The RTP payload types this side takes, a bit each: 0x1000 is AAC-ELD's
	 * 101. Apple's viewer sends the same 0x5E7F, and the Mac sets the rate. */
	pb_uint(stream, 4, 24191);
	pb_uint(stream, 5, 0);
	pb_uint(stream, 6, 0);
	pb_message(blob, 3, stream);
	blob_tail(blob, codec_entries, G_N_ELEMENTS(codec_entries), AUDIO_F13);
	return blob;
}

static GByteArray *video_codec(guint64 id, const guint64 *levels, gsize n_levels, const gchar *features, guint64 f4)
{
	GByteArray *codec = g_byte_array_new();

	pb_uint(codec, 1, id);
	for (gsize i = 0; i < n_levels; i++) {
		GByteArray *level = g_byte_array_new();

		pb_uint(level, 1, 1);
		pb_uint(level, 2, levels[i]);
		pb_uint(level, 3, 50115);
		pb_uint(level, 4, 0);
		pb_message(codec, 2, level);
	}
	pb_bytes(codec, 3, (const guint8 *)features, strlen(features));
	pb_uint(codec, 4, f4);
	return codec;
}

/* Offered H.264 (123) and HEVC (100), as Apple's viewer offers them, the Mac
 * sends HEVC; offered 123 alone, H.264. */
static GByteArray *video_offer_blob(guint32 ssrc, guint16 width, guint16 height, guint64 tiles, ArdCodec codec)
{
	static const guint64 h264_levels[] = { 1, 2, 1, 2 };
	static const guint64 hevc_levels[] = { 1, 2 };
	GByteArray *blob = g_byte_array_new();
	GByteArray *stream = g_byte_array_new();
	CodecEntry entries[G_N_ELEMENTS(codec_entries)];

	pb_uint(blob, 1, 1);
	pb_uint(blob, 2, 1);
	pb_uint(stream, 1, ssrc);
	pb_uint(stream, 2, 0);
	pb_message(stream, 3, video_codec(123, h264_levels, G_N_ELEMENTS(h264_levels),
					  "FLS;MS:-1;LF:-1;LTR;CABAC;POS:0;EOD:1;HTS:2;RR:3;AR:4/3,5/8;XR:4/3,5/8;", 1));
	if (codec == ARD_CODEC_HEVC)
		pb_message(stream, 3, video_codec(100, hevc_levels, G_N_ELEMENTS(hevc_levels),
						  "FLS;LF:-1;POS:5;EOD:1;HTS:2;RR:3;POSE:4;AR:4/3,5/8;XR:4/3,5/8;", 14));
	pb_uint(stream, 4, width);
	pb_uint(stream, 5, height);
	pb_uint(stream, 6, tiles);
	pb_uint(stream, 7, 1);
	pb_uint(stream, 8, 63);
	pb_uint(stream, 9, 1);
	pb_uint(stream, 12, 1);
	pb_message(blob, 5, stream);
	/* The audio offer's entries, with {0, 40000000, 12288} moved to the front. */
	entries[0] = codec_entries[4];
	memcpy(entries + 1, codec_entries, 4 * sizeof(CodecEntry));
	memcpy(entries + 5, codec_entries + 5, (G_N_ELEMENTS(codec_entries) - 5) * sizeof(CodecEntry));
	blob_tail(blob, entries, G_N_ELEMENTS(entries), VIDEO_F13);
	return blob;
}

typedef enum {
	PLIST_DATA,
	PLIST_INT,
	PLIST_STR
} PlistKind;

typedef struct {
	const gchar *key;
	PlistKind kind;
	const guint8 *data;
	gsize len;
	guint8 integer;
} PlistEntry;

static void plist_marker(GByteArray *out, guint8 kind, gsize count)
{
	guint8 b[4];

	if (count < 15) {
		b[0] = kind | count;
		g_byte_array_append(out, b, 1);
	} else if (count < 256) {
		b[0] = kind | 0x0f;
		b[1] = 0x10;
		b[2] = count;
		g_byte_array_append(out, b, 3);
	} else {
		b[0] = kind | 0x0f;
		b[1] = 0x11;
		ard_put16(b + 2, count);
		g_byte_array_append(out, b, 4);
	}
}

/* A binary property list of one dictionary: the dictionary, its keys and its
 * values as objects, an offset table of u16s, and the 32-byte trailer. */
static GByteArray *bplist_dict(const PlistEntry *entries, gsize n)
{
	GByteArray *out = g_byte_array_new();
	guint16 offsets[1 + 2 * 8];
	gsize count = 0, table;
	static const guint8 trailer[] = { 0, 0, 0, 0, 0, 0, 2, 1 };

	g_assert(n <= 8);
	g_byte_array_append(out, (const guint8 *)"bplist00", 8);
	offsets[count++] = out->len;
	plist_marker(out, 0xd0, n);
	for (gsize i = 0; i < n; i++) {
		guint8 ref = 1 + i;

		g_byte_array_append(out, &ref, 1);
	}
	for (gsize i = 0; i < n; i++) {
		guint8 ref = 1 + n + i;

		g_byte_array_append(out, &ref, 1);
	}
	for (gsize i = 0; i < n; i++) {
		offsets[count++] = out->len;
		plist_marker(out, 0x50, strlen(entries[i].key));
		g_byte_array_append(out, (const guint8 *)entries[i].key, strlen(entries[i].key));
	}
	for (gsize i = 0; i < n; i++) {
		offsets[count++] = out->len;
		switch (entries[i].kind) {
		case PLIST_DATA:
			plist_marker(out, 0x40, entries[i].len);
			g_byte_array_append(out, entries[i].data, entries[i].len);
			break;
		case PLIST_INT: {
			guint8 b[2] = { 0x10, entries[i].integer };

			g_byte_array_append(out, b, 2);
			break;
		}
		case PLIST_STR:
			plist_marker(out, 0x50, entries[i].len);
			g_byte_array_append(out, entries[i].data, entries[i].len);
			break;
		}
	}
	table = out->len;
	for (gsize i = 0; i < count; i++)
		ard_append16(out, offsets[i]);
	g_byte_array_append(out, trailer, sizeof(trailer));
	ard_append64(out, count);
	ard_append64(out, 0);
	ard_append64(out, table);
	return out;
}

static GByteArray *deflated(const GByteArray *blob)
{
	uLongf len = compressBound(blob->len);
	GByteArray *out = g_byte_array_sized_new(len);

	g_byte_array_set_size(out, len);
	if (compress2(out->data, &len, blob->data, blob->len, Z_DEFAULT_COMPRESSION) != Z_OK)
		len = 0;
	g_byte_array_set_size(out, len);
	return out;
}

static GByteArray *offer_plist(guint8 mode, GByteArray *blob, const gchar *call_id)
{
	GByteArray *compressed = deflated(blob);
	PlistEntry entries[] = {
		{ "avcMediaStreamOptionRemoteEndpointInfo", PLIST_DATA, remote_endpoint_info, sizeof(remote_endpoint_info), 0 },
		{ "avcMediaStreamNegotiatorMode",	     PLIST_INT,	 NULL,		      0,			    mode },
		{ "avcMediaStreamNegotiatorMediaBlob",	     PLIST_DATA, compressed->data,    compressed->len,		    0 },
		{ "avcMediaStreamOptionCallID",		     PLIST_STR,	 (const guint8 *)call_id, strlen(call_id),	    0 },
	};
	GByteArray *plist = bplist_dict(entries, G_N_ELEMENTS(entries));

	g_byte_array_unref(compressed);
	g_byte_array_unref(blob);
	return plist;
}

void ard_offers_init(ArdOffers *offers, ArdCodec codec)
{
	guint8 *uuid = offers->session_uuid;

	RAND_bytes(offers->audio_keys.v2s, 46);
	RAND_bytes(offers->audio_keys.s2v, 46);
	RAND_bytes(offers->video_keys.v2s, 46);
	RAND_bytes(offers->video_keys.s2v, 46);
	RAND_bytes((guint8 *)&offers->audio_ssrc, 4);
	RAND_bytes((guint8 *)&offers->video_ssrc, 4);
	RAND_bytes(uuid, 16);
	uuid[6] = (uuid[6] & 0x0f) | 0x40;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
	g_snprintf(offers->call_id, sizeof(offers->call_id),
		   "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		   uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
		   uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
	offers->codec = codec;
}

gboolean ard_offers_strips(const ArdOffers *offers)
{
	return offers->codec == ARD_CODEC_HEVC;
}

guint ard_strip_rows(guint height)
{
	return (height / ARD_STRIPS + 15) / 16 * 16;
}

/* RFBMediaStreamServerConfiguration, version 3:
 *
 *   +0x00 u8   0x1c
 *   +0x02 u16  body length, everything after this field
 *   +0x04 u16  version = 3
 *   +0x06 u32  flags
 *   +0x0a u16  audio offer length
 *   +0x0c u16  video1 offer length
 *   +0x0e u16  video2 offer length, 0 for one display
 *   +0x14 16B  session UUID
 *   +0x24 46B  audio SRTP master key, viewer -> server
 *   +0x52 46B  audio SRTP master key, server -> viewer
 *   +0x80      audio offer, then the video keys the same way and its offer */
GByteArray *ard_offers_configuration(const ArdOffers *offers, guint16 width, guint16 height)
{
	GByteArray *audio = offer_plist(MODE_AUDIO, audio_offer_blob(offers->audio_ssrc), offers->call_id);
	GByteArray *video = offer_plist(MODE_VIDEO,
					video_offer_blob(offers->video_ssrc, width, height,
							 ard_offers_strips(offers) ? ARD_STRIPS : 1, offers->codec),
					offers->call_id);
	GByteArray *msg = g_byte_array_sized_new(0x80 + audio->len + 92 + video->len);

	g_byte_array_set_size(msg, 0x80);
	memset(msg->data, 0, 0x80);
	msg->data[0] = 0x1c;
	ard_put16(msg->data + 4, 3);
	ard_put32(msg->data + 6, ARD_MEDIA_FLAG_60FPS | ARD_MEDIA_FLAG_NO_CURSOR);
	ard_put16(msg->data + 0x0a, audio->len);
	ard_put16(msg->data + 0x0c, video->len);
	memcpy(msg->data + 0x14, offers->session_uuid, 16);
	memcpy(msg->data + 0x24, offers->audio_keys.v2s, 46);
	memcpy(msg->data + 0x52, offers->audio_keys.s2v, 46);
	g_byte_array_append(msg, audio->data, audio->len);
	g_byte_array_append(msg, offers->video_keys.v2s, 46);
	g_byte_array_append(msg, offers->video_keys.s2v, 46);
	g_byte_array_append(msg, video->data, video->len);
	ard_put16(msg->data + 2, msg->len - 4);
	g_byte_array_unref(audio);
	g_byte_array_unref(video);
	return msg;
}

gboolean ard_parse_media_reply(const guint8 *body, gsize len, ArdReply *reply, GError **error)
{
	memset(reply, 0, sizeof(*reply));
	if (len < 8) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "a media-stream reply of %" G_GSIZE_FORMAT " bytes has no header", len);
		return FALSE;
	}
	switch (ard_get16(body)) {
	case 1:
		if (len < 36) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "media-stream message 1 is too short");
			return FALSE;
		}
		if (!(ard_get32(body + 10) & ard_get32(body + 16) & 1)) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				    "media-stream message 1 did not enable both its audio and video legs");
			return FALSE;
		}
		reply->kind = ARD_REPLY_PORTS;
		reply->audio_port = ard_get16(body + 8);
		reply->video_port = ard_get16(body + 14);
		reply->second_video = ard_get32(body + 22) & 1;
		return TRUE;
	case 2: {
		gsize a, v, v2;

		if (len < 18) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "media-stream answer is too short");
			return FALSE;
		}
		a = ard_get16(body + 8);
		v = ard_get16(body + 10);
		v2 = ard_get16(body + 12);
		if (len != 18 + a + v + v2) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				    "media-stream answer is %" G_GSIZE_FORMAT " bytes, not the %" G_GSIZE_FORMAT " its lengths describe",
				    len, 18 + a + v + v2);
			return FALSE;
		}
		reply->kind = ARD_REPLY_ANSWER;
		reply->answered_videos = v2 ? 2 : 1;
		return TRUE;
	}
	case 3:
		if (len < 16) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "media-stream error message is too short");
			return FALSE;
		}
		reply->kind = ARD_REPLY_ERROR;
		reply->error_kind = ard_get32(body + 8);
		reply->error_sub = ard_get32(body + 12);
		return TRUE;
	default:
		reply->kind = ARD_REPLY_OTHER;
		reply->other = ard_get16(body);
		return TRUE;
	}
}

/* ------------------------------------------------------------------------ */
/* SRTP and SRTCP */

/* RFC 3711 4.3 with key_derivation_rate 0: labels base (encryption), base + 1
 * (authentication) and base + 2 (salt); 0 for SRTP, 3 for SRTCP. */
static void srtp_derive(ArdSrtpKeys *k, const guint8 master[46], guint8 base)
{
	EVP_CIPHER_CTX *prf = EVP_CIPHER_CTX_new();
	guint8 key[32];
	guint8 salt[14];

	EVP_EncryptInit_ex(prf, EVP_aes_256_ctr(), NULL, master, NULL);
	for (guint8 label = base; label < base + 3; label++) {
		guint8 iv[16] = { 0 };
		guint8 zeros[32] = { 0 };
		guint8 *out = label == base ? key : label == base + 1 ? k->auth : salt;
		int n = label == base ? 32 : label == base + 1 ? 20 : 14;

		memcpy(iv, master + 32, 14);
		iv[7] ^= label;
		EVP_EncryptInit_ex(prf, NULL, NULL, NULL, iv);
		EVP_EncryptUpdate(prf, out, &n, zeros, n);
	}
	EVP_CIPHER_CTX_free(prf);
	memcpy(k->salt, salt, 14);
	k->ctr = EVP_CIPHER_CTX_new();
	EVP_EncryptInit_ex(k->ctr, EVP_aes_256_ctr(), NULL, key, NULL);
	OPENSSL_cleanse(key, sizeof(key));
}

static void srtp_keys_clear(ArdSrtpKeys *k)
{
	if (k->ctr)
		EVP_CIPHER_CTX_free(k->ctr);
	k->ctr = NULL;
}

static void srtp_crypt(ArdSrtpKeys *k, guint32 ssrc, guint64 index, guint8 *data, gsize len)
{
	guint8 iv[16] = { 0 };
	int n;

	memcpy(iv, k->salt, 14);
	for (int i = 0; i < 4; i++)
		iv[4 + i] ^= ssrc >> (24 - 8 * i);
	for (int i = 0; i < 6; i++)
		iv[8 + i] ^= index >> (40 - 8 * i);
	EVP_EncryptInit_ex(k->ctr, NULL, NULL, NULL, iv);
	EVP_EncryptUpdate(k->ctr, data, &n, data, len);
}

static void srtp_tag(const ArdSrtpKeys *k, const guint8 *data, gsize len, guint8 tag[ARD_SRTP_TAG])
{
	guint8 full[20];
	unsigned int n = sizeof(full);

	HMAC(EVP_sha1(), k->auth, sizeof(k->auth), data, len, full, &n);
	memcpy(tag, full, ARD_SRTP_TAG);
}

#define EXTENSION_PROFILE 0x9301
#define EXTENSION_VARIES 0x0030
#define EXTENSION_REFRESH 0x0020

static ArdSrtpResult rtp_header(const guint8 *data, gsize len, ArdRtpHeader *h)
{
	gsize at;

	if (len < 12 || data[0] >> 6 != 2)
		return ARD_SRTP_NOT_RTP;
	if (data[1] >= 200 && data[1] <= 207)
		return ARD_SRTP_RTCP;
	at = 12 + 4 * (data[0] & 0x0f);
	h->refresh = FALSE;
	if (data[0] & 0x10) {
		guint16 profile;

		if (len < at + 4)
			return ARD_SRTP_NOT_RTP;
		profile = ard_get16(data + at);
		h->refresh = (profile & ~EXTENSION_VARIES) == EXTENSION_PROFILE && (profile & EXTENSION_REFRESH);
		at += 4 + 4 * (gsize)ard_get16(data + at + 2);
	}
	if (len < at + ARD_SRTP_TAG)
		return ARD_SRTP_NOT_RTP;
	h->payload_type = data[1] & 0x7f;
	h->marker = data[1] & 0x80;
	h->sequence = ard_get16(data + 2);
	h->timestamp = ard_get32(data + 4);
	h->ssrc = ard_get32(data + 8);
	h->payload_at = at;
	h->payload_end = len - ARD_SRTP_TAG;
	return ARD_SRTP_OK;
}

void ard_srtp_receiver_init(ArdSrtpReceiver *r, const guint8 master[46])
{
	memset(r, 0, sizeof(*r));
	srtp_derive(&r->keys, master, 0);
}

void ard_srtp_receiver_clear(ArdSrtpReceiver *r)
{
	srtp_keys_clear(&r->keys);
}

/* RFC 3711 Appendix A: the rollover counter `seq` most likely belongs to. */
static guint32 guess_roc(const ArdSrtpSeen *seen, guint16 seq)
{
	if (!seen)
		return 0;
	if (seen->seq < 0x8000) {
		if ((guint16)(seq - seen->seq) > 0x8000 && seq > seen->seq)
			return seen->roc - 1;
		return seen->roc;
	}
	if (seq < (guint16)(seen->seq - 0x8000))
		return seen->roc + 1;
	return seen->roc;
}

ArdSrtpResult ard_srtp_unprotect(ArdSrtpReceiver *r, guint8 *data, gsize len, ArdRtpHeader *h)
{
	ArdSrtpResult result = rtp_header(data, len, h);
	ArdSrtpSeen *seen = NULL;
	guint8 tag[ARD_SRTP_TAG], expected[ARD_SRTP_TAG];
	guint32 roc;
	guint64 index;
	gsize end = h->payload_end;

	if (result != ARD_SRTP_OK)
		return result;
	for (guint i = 0; i < ARD_STRIPS; i++)
		if (r->seen[i].used && r->seen[i].ssrc == h->ssrc)
			seen = &r->seen[i];
	roc = guess_roc(seen, h->sequence);
	/* The tag covers the packet and then the rollover counter, which takes the
	 * tag's place while it is computed. */
	memcpy(tag, data + end, ARD_SRTP_TAG);
	ard_put32(data + end, roc);
	srtp_tag(&r->keys, data, end + 4, expected);
	memcpy(data + end, tag, ARD_SRTP_TAG);
	if (CRYPTO_memcmp(tag, expected, ARD_SRTP_TAG) != 0)
		return ARD_SRTP_FORGED;
	index = ((guint64)roc << 16) | h->sequence;
	if (seen) {
		if (index <= (((guint64)seen->roc << 16) | seen->seq))
			return ARD_SRTP_STALE;
	} else {
		/* Every offer starts a stream under new SSRCs: the oldest is let go. */
		seen = &r->seen[r->next_slot];
		r->next_slot = (r->next_slot + 1) % ARD_STRIPS;
		seen->used = TRUE;
		seen->ssrc = h->ssrc;
	}
	seen->seq = h->sequence;
	seen->roc = roc;
	srtp_crypt(&r->keys, h->ssrc, index, data + h->payload_at, end - h->payload_at);
	return ARD_SRTP_OK;
}

void ard_srtcp_sender_init(ArdSrtcpSender *s, const guint8 master[46])
{
	memset(s, 0, sizeof(*s));
	srtp_derive(&s->keys, master, 3);
}

void ard_srtcp_sender_clear(ArdSrtcpSender *s)
{
	srtp_keys_clear(&s->keys);
}

gsize ard_srtcp_protect(ArdSrtcpSender *s, const guint8 *report, gsize len, guint8 *out)
{
	memcpy(out, report, len);
	srtp_crypt(&s->keys, ard_get32(report + 4), s->index, out + 8, len - 8);
	ard_put32(out + len, 0x80000000 | s->index);
	srtp_tag(&s->keys, out, len + 4, out + len + 4);
	s->index = (s->index + 1) & 0x7fffffff;
	return len + 4 + ARD_SRTP_TAG;
}

void ard_srtcp_receiver_init(ArdSrtcpReceiver *r, const guint8 master[46])
{
	memset(r, 0, sizeof(*r));
	srtp_derive(&r->keys, master, 3);
	r->last = g_hash_table_new(g_direct_hash, g_direct_equal);
}

void ard_srtcp_receiver_clear(ArdSrtcpReceiver *r)
{
	srtp_keys_clear(&r->keys);
	if (r->last)
		g_hash_table_destroy(r->last);
	r->last = NULL;
}

gboolean ard_srtcp_authenticate(ArdSrtcpReceiver *r, const guint8 *data, gsize len)
{
	guint8 expected[ARD_SRTP_TAG];
	gsize end;
	guint32 ssrc, index;
	gpointer last;

	if (len < 8 + 4 + ARD_SRTP_TAG || data[0] >> 6 != 2)
		return FALSE;
	end = len - ARD_SRTP_TAG;
	srtp_tag(&r->keys, data, end, expected);
	if (CRYPTO_memcmp(expected, data + end, ARD_SRTP_TAG) != 0)
		return FALSE;
	ssrc = ard_get32(data + 4);
	index = ard_get32(data + end - 4) & 0x7fffffff;
	/* Stored plus one, so an index of 0 is told apart from no entry. */
	if (g_hash_table_lookup_extended(r->last, GUINT_TO_POINTER(ssrc), NULL, &last)
	    && index + 1 <= GPOINTER_TO_UINT(last))
		return FALSE;
	g_hash_table_insert(r->last, GUINT_TO_POINTER(ssrc), GUINT_TO_POINTER(index + 1));
	return TRUE;
}

gsize ard_rtcp_receiver_report(guint32 ssrc, guint8 out[8])
{
	out[0] = 0x80;
	out[1] = ARD_RTCP_RECEIVER_REPORT;
	ard_put16(out + 2, 1);
	ard_put32(out + 4, ssrc);
	return 8;
}

/* AVConference's acknowledgement of a picture this side has whole: an APP
 * packet whose name is the number 5. In strips an acknowledged strip is one
 * whose repeats of a still screen the Mac stops sending. */
gsize ard_rtcp_reference_ack(guint32 ssrc, guint32 picture, guint8 out[16])
{
	out[0] = 0x80;
	out[1] = 204;
	ard_put16(out + 2, 3);
	ard_put32(out + 4, ssrc);
	ard_put32(out + 8, 5);
	ard_put32(out + 12, picture);
	return 16;
}

/* Payload-specific feedback of format 2 with the stream's size: the Mac answers
 * with a picture predicted from the last one acknowledged. */
gsize ard_rtcp_refresh(guint32 ssrc, guint32 media_ssrc, guint16 width, guint16 height, guint8 out[16])
{
	out[0] = 0x82;
	out[1] = 206;
	ard_put16(out + 2, 3);
	ard_put32(out + 4, ssrc);
	ard_put32(out + 8, media_ssrc);
	ard_put16(out + 12, width);
	ard_put16(out + 14, height);
	return 16;
}

gsize ard_rtcp_pli(guint32 ssrc, guint32 media_ssrc, guint8 out[12])
{
	out[0] = 0x81;
	out[1] = 206;
	ard_put16(out + 2, 2);
	ard_put32(out + 4, ssrc);
	ard_put32(out + 8, media_ssrc);
	return 12;
}

/* ------------------------------------------------------------------------ */
/* Rate feedback */

#define VIDEO_CLOCK 24000.0
#define REPORTED_BANDWIDTH 60000
/* A lag this far, in seconds, from either average is a clock that jumped. */
#define SPURIOUS_LAG 30.0

static void delay_restart(ArdOneWayDelay *d, guint32 timestamp, gint64 at)
{
	memset(d, 0, sizeof(*d));
	d->started = TRUE;
	d->previous = timestamp;
	d->first_at = at;
}

/* Apple's receiver's estimate: each picture's lag is its arrival less its RTP
 * timestamp, both counted from the stream's first picture. A short average
 * follows the lag, a long one settles on its floor, and the delay reported is
 * how far the short one stands above it. */
static void delay_sample(ArdOneWayDelay *d, guint32 timestamp, gint64 at)
{
	gint32 step;
	gdouble lag;

	if (!d->started) {
		delay_restart(d, timestamp, at);
		return;
	}
	step = (gint32)(timestamp - d->previous);
	if (step <= 0)
		return;
	d->previous = timestamp;
	d->ticks += step;
	lag = (at - d->first_at) / 1e6 - d->ticks / VIDEO_CLOCK;
	if (lag - d->shorter > SPURIOUS_LAG || d->longer - lag > SPURIOUS_LAG) {
		delay_restart(d, timestamp, at);
		return;
	}
	d->longer = 0.9999 * d->longer + 0.0001 * lag;
	d->shorter = 0.9 * d->shorter + 0.1 * lag;
	d->owrd = d->shorter - d->longer;
	if (d->owrd < 0) {
		d->longer = d->shorter;
		d->owrd = 0;
	}
}

void ard_rate_feedback_init(ArdRateFeedback *f, gint64 epoch)
{
	memset(f, 0, sizeof(*f));
	f->epoch = epoch;
}

void ard_rate_feedback_received(ArdRateFeedback *f, guint32 ssrc, guint32 timestamp, gint64 at)
{
	if (ssrc != f->ssrc) {
		ard_rate_feedback_init(f, f->epoch);
		f->ssrc = ssrc;
	}
	f->have_last = TRUE;
	f->last_timestamp = timestamp;
	f->last_at = at;
	f->received++;
	delay_sample(&f->delay, timestamp, at);
}

/*   +0  u8   0x85
 *   +1  u8   0
 *   +2  u16  4, the payload's length in words after this field
 *   +4  u16  the last picture packet's RTP timestamp >> 8
 *   +6  u32  zero
 *   +10 u16  milliseconds since that packet arrived
 *   +12 u16  the viewer's clock, in 1/1024 s
 *   +14 u16  one-way relative delay, seconds × 8192
 *   +16 u16  loss (top 4 bits) | picture packets received mod 4096
 *   +18 u16  bandwidth estimate, kbit/s
 *
 * The Mac takes it only alone in its datagram. */
gboolean ard_rate_feedback_report(const ArdRateFeedback *f, guint32 ssrc, gint64 now, guint8 out[32])
{
	gint64 held = (now - f->last_at) / 1000;
	gdouble delay = f->delay.owrd * 8192.0;

	if (!f->have_last)
		return FALSE;
	memset(out, 0, 32);
	out[0] = 0x80;
	out[1] = 204;
	ard_put16(out + 2, 7);
	ard_put32(out + 4, ssrc);
	memcpy(out + 8, "RCTL", 4);
	out[12] = 0x85;
	ard_put16(out + 14, 4);
	ard_put16(out + 16, f->last_timestamp >> 8);
	ard_put16(out + 22, CLAMP(held, 0, 0xffff));
	ard_put16(out + 24, (guint64)((now - f->epoch) * 1024 / 1000000));
	ard_put16(out + 26, MIN(delay, 0xffff));
	ard_put16(out + 28, f->received & 0xfff);
	ard_put16(out + 30, REPORTED_BANDWIDTH);
	return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Depacketizing */

#define HEVC_AP 48
#define HEVC_FU 49
#define H264_STAP_A 24
#define H264_FU_A 28
/* The unit a Mac sends its H.264's parameter sets in: an MP4 avc1 sample
 * description, whose avcC box holds them. */
#define H264_DESCRIPTION 18

static const guint8 start_code[] = { 0, 0, 0, 1 };

void ard_unit_free(ArdUnit *unit)
{
	if (!unit)
		return;
	if (unit->data)
		g_byte_array_unref(unit->data);
	g_free(unit);
}

static void reassembler_init(ArdReassembler *r, ArdCodec codec, gboolean donl)
{
	memset(r, 0, sizeof(*r));
	r->codec = codec;
	r->donl = donl;
	r->unit = g_byte_array_new();
	r->fragment = g_byte_array_new();
}

static void reassembler_clear(ArdReassembler *r)
{
	if (r->unit)
		g_byte_array_unref(r->unit);
	if (r->fragment)
		g_byte_array_unref(r->fragment);
	r->unit = r->fragment = NULL;
}

static void reassembler_reset(ArdReassembler *r)
{
	ArdCodec codec = r->codec;
	gboolean donl = r->donl;

	reassembler_clear(r);
	reassembler_init(r, codec, donl);
}

static void take_nal(ArdReassembler *r, const guint8 *nal, gsize len)
{
	guint type;

	if (len == 0) {
		r->damaged = TRUE;
		return;
	}
	g_byte_array_append(r->unit, start_code, sizeof(start_code));
	g_byte_array_append(r->unit, nal, len);
	if (r->codec == ARD_CODEC_HEVC) {
		type = (nal[0] >> 1) & 0x3f;
		if (type >= 16 && type <= 23)
			r->keyframe = TRUE;
		if (type >= 32 && type <= 34)
			r->params = TRUE;
	} else {
		type = nal[0] & 0x1f;
		if (type == 5)
			r->keyframe = TRUE;
		if (type == 7 || type == 8)
			r->params = TRUE;
	}
}

/* The Mac sends its H.264 parameter sets inside an avc1 sample description:
 * after avcC's version, profile, compatibility, level and length size come
 * the sequence parameter sets behind a count in five bits and the picture
 * parameter sets behind one in eight, each behind its u16 length. */
static gboolean take_h264_description(ArdReassembler *r, const guint8 *d, gsize len)
{
	const guint8 *record = NULL;
	gsize rest, at = 5;
	gboolean any = FALSE;
	static const guint8 masks[] = { 0x1f, 0xff };

	for (gsize i = 0; i + 4 <= len; i++) {
		if (memcmp(d + i, "avcC", 4) == 0) {
			record = d + i + 4;
			rest = len - i - 4;
			break;
		}
	}
	if (!record)
		return FALSE;
	for (gsize m = 0; m < G_N_ELEMENTS(masks); m++) {
		guint count;

		if (at >= rest)
			return FALSE;
		count = record[at++] & masks[m];
		for (guint i = 0; i < count; i++) {
			gsize size;

			if (at + 2 > rest)
				return FALSE;
			size = ard_get16(record + at);
			if (size == 0 || at + 2 + size > rest)
				return FALSE;
			take_nal(r, record + at + 2, size);
			at += 2 + size;
			any = TRUE;
		}
	}
	return any;
}

static void take_h264(ArdReassembler *r, const guint8 *nal, gsize len)
{
	if (len == 0) {
		r->damaged = TRUE;
		return;
	}
	if ((nal[0] & 0x1f) != H264_DESCRIPTION) {
		take_nal(r, nal, len);
		return;
	}
	if (!take_h264_description(r, nal, len))
		r->damaged = TRUE;
}

static void parse_h264(ArdReassembler *r, const guint8 *p, gsize len)
{
	if (len == 0) {
		r->damaged = TRUE;
		return;
	}
	switch (p[0] & 0x1f) {
	case H264_STAP_A: {
		const guint8 *rest = p + 1;
		gsize left = len - 1;

		while (left >= 2) {
			gsize size = ard_get16(rest);

			if (size == 0 || left < 2 + size) {
				r->damaged = TRUE;
				return;
			}
			take_h264(r, rest + 2, size);
			rest += 2 + size;
			left -= 2 + size;
		}
		break;
	}
	case H264_FU_A: {
		guint8 fu;

		if (len < 2) {
			r->damaged = TRUE;
			return;
		}
		fu = p[1];
		if (fu & 0x80) {
			guint8 header = (p[0] & 0xe0) | (fu & 0x1f);

			g_byte_array_set_size(r->fragment, 0);
			g_byte_array_append(r->fragment, &header, 1);
			r->in_fragment = TRUE;
		} else if (!r->in_fragment) {
			r->damaged = TRUE;
			return;
		}
		g_byte_array_append(r->fragment, p + 2, len - 2);
		if (fu & 0x40) {
			take_h264(r, r->fragment->data, r->fragment->len);
			r->in_fragment = FALSE;
		}
		break;
	}
	default:
		take_h264(r, p, len);
	}
}

static void parse_hevc(ArdReassembler *r, const guint8 *p, gsize len)
{
	gsize numbered = r->donl ? 2 : 0;
	guint kind;
	gsize body;
	const guint8 *rest;
	gsize left;

	if (len < 2) {
		r->damaged = TRUE;
		return;
	}
	kind = (p[0] >> 1) & 0x3f;
	body = kind == HEVC_FU ? 3 : 2;
	if (len < body + numbered) {
		r->damaged = TRUE;
		return;
	}
	if (r->donl && !r->have_don) {
		r->don = ard_get16(p + body);
		r->have_don = TRUE;
	}
	rest = p + body + numbered;
	left = len - body - numbered;
	switch (kind) {
	case HEVC_AP:
		while (left >= 2) {
			gsize size = ard_get16(rest);

			if (size < 2 || left < 2 + size) {
				r->damaged = TRUE;
				return;
			}
			take_nal(r, rest + 2, size);
			rest += 2 + size;
			left -= 2 + size;
		}
		break;
	case HEVC_FU: {
		guint8 fu = p[2];

		if (fu & 0x80) {
			guint8 header[2] = { (p[0] & 0x81) | ((fu & 0x3f) << 1), p[1] };

			g_byte_array_set_size(r->fragment, 0);
			g_byte_array_append(r->fragment, header, 2);
			r->in_fragment = TRUE;
		} else if (!r->in_fragment) {
			r->damaged = TRUE;
			return;
		}
		g_byte_array_append(r->fragment, rest, left);
		if (fu & 0x40) {
			take_nal(r, r->fragment->data, r->fragment->len);
			r->in_fragment = FALSE;
		}
		break;
	}
	default: {
		g_byte_array_append(r->unit, start_code, sizeof(start_code));
		g_byte_array_append(r->unit, p, 2);
		g_byte_array_append(r->unit, rest, left);
		if (kind >= 16 && kind <= 23)
			r->keyframe = TRUE;
		if (kind >= 32 && kind <= 34)
			r->params = TRUE;
	}
	}
}

typedef struct {
	gboolean dup;
	gboolean lost;
	/* The picture the packet ended, if every packet of it came. */
	GByteArray *whole;
	gboolean keyframe;
	gboolean params;
	gboolean refresh;
	gboolean have_don;
	guint16 don;
} Pushed;

/* Close the picture in flight, handing it over if it is whole. */
static void reassembler_finish(ArdReassembler *r, Pushed *out)
{
	gboolean damaged = r->damaged || r->in_fragment;

	if (out && !damaged && r->unit->len > 0) {
		out->whole = r->unit;
		out->keyframe = r->keyframe;
		out->params = r->params;
		out->refresh = r->refresh;
		out->have_don = r->have_don;
		out->don = r->don;
		r->unit = g_byte_array_new();
	} else {
		g_byte_array_set_size(r->unit, 0);
	}
	r->damaged = r->in_fragment = r->refresh = r->keyframe = r->params = r->have_don = FALSE;
}

/* A picture ends at the packet with the marker bit, or where the timestamp
 * moves on. A gap in the sequence numbers damages the picture it fell in. */
static void reassembler_push(ArdReassembler *r, const ArdRtpHeader *h, const guint8 *payload, gsize len, Pushed *out)
{
	memset(out, 0, sizeof(*out));
	if (r->have_seq) {
		guint16 ahead = h->sequence - r->next_seq;

		if (ahead >= 0x8000) {
			out->dup = TRUE;
			return;
		}
		out->lost = ahead != 0;
	}
	r->next_seq = h->sequence + 1;
	r->have_seq = TRUE;
	if (r->have_ts && r->ts != h->timestamp && (r->damaged || r->unit->len > 0)) {
		/* The previous picture's marked last packet never came. */
		r->damaged = TRUE;
		reassembler_finish(r, NULL);
	}
	r->damaged |= out->lost;
	r->ts = h->timestamp;
	r->have_ts = TRUE;
	r->refresh |= h->refresh;
	if (r->codec == ARD_CODEC_HEVC)
		parse_hevc(r, payload, len);
	else
		parse_h264(r, payload, len);
	if (h->marker)
		reassembler_finish(r, out);
}

static ArdUnit *unit_new(GByteArray *data, gboolean keyframe, gint strip, guint32 timestamp, guint32 acknowledgement)
{
	ArdUnit *unit = g_new0(ArdUnit, 1);

	unit->data = data;
	unit->keyframe = keyframe;
	unit->strip = strip;
	unit->timestamp = timestamp;
	unit->acknowledgement = acknowledgement;
	return unit;
}

static ArdDepack whole_push(ArdWhole *w, const ArdRtpHeader *h, const guint8 *payload, gsize len, ArdUnit **unit)
{
	Pushed pushed;
	ArdDepack out = ARD_DEPACK_PENDING;

	if (!w->have_ssrc || w->ssrc != h->ssrc) {
		/* A new stream, after an offer, starts with an IDR of its own. */
		reassembler_reset(&w->packets);
		w->have_ssrc = TRUE;
		w->ssrc = h->ssrc;
		w->synced = w->resumable = FALSE;
	}
	reassembler_push(&w->packets, h, payload, len, &pushed);
	if (pushed.dup)
		return ARD_DEPACK_PENDING;
	if (pushed.lost) {
		w->resumable |= w->synced && w->codec == ARD_CODEC_HEVC;
		w->synced = FALSE;
		out = ARD_DEPACK_LOST;
	}
	if (h->marker) {
		if (pushed.whole) {
			if (!w->synced && ((pushed.refresh && w->resumable) || pushed.keyframe || pushed.params)) {
				w->synced = TRUE;
				w->resumable = FALSE;
			}
			if (w->synced) {
				*unit = unit_new(pushed.whole, pushed.keyframe, -1, h->timestamp, h->timestamp);
				return ARD_DEPACK_UNIT;
			}
			g_byte_array_unref(pushed.whole);
		}
		if (!w->synced)
			out = ARD_DEPACK_LOST;
	}
	return out;
}

static void strips_reset(ArdStrips *s, guint32 base)
{
	for (guint i = 0; i < ARD_STRIPS; i++)
		reassembler_reset(&s->strips[i]);
	s->have_base = TRUE;
	s->base = base;
	s->have_next = FALSE;
	s->owed = 0;
}

static guint32 strips_stream(ArdStrips *s, guint32 ssrc)
{
	/* A new stream, after an offer, starts with the first strip's IDR, so an
	 * SSRC outside the strips' is one's first. */
	if (!s->have_base || ssrc - s->base >= ARD_STRIPS)
		strips_reset(s, ssrc);
	return s->base;
}

static ArdDepack strips_push(ArdStrips *s, const ArdRtpHeader *h, const guint8 *payload, gsize len, ArdUnit **unit)
{
	guint strip = h->ssrc - strips_stream(s, h->ssrc);
	Pushed pushed;

	reassembler_push(&s->strips[strip], h, payload, len, &pushed);
	if (pushed.dup)
		return ARD_DEPACK_PENDING;
	if (pushed.lost)
		s->have_next = FALSE;
	if (pushed.whole) {
		gboolean starts = strip == 0 && pushed.keyframe;

		if (pushed.have_don && ((s->have_next && s->next == pushed.don) || starts)) {
			s->next = pushed.don + 1;
			s->have_next = TRUE;
			if (starts)
				s->owed = ((1 << ARD_STRIPS) - 1) & ~1;
			else
				s->owed &= ~(1 << strip);
			*unit = unit_new(pushed.whole, pushed.keyframe, strip, h->timestamp, pushed.don);
			return ARD_DEPACK_UNIT;
		}
		g_byte_array_unref(pushed.whole);
		s->have_next = FALSE;
		return ARD_DEPACK_LOST;
	}
	if (pushed.lost || (h->marker && !s->have_next))
		return ARD_DEPACK_LOST;
	return ARD_DEPACK_PENDING;
}

void ard_assembly_init(ArdAssembly *a, ArdCodec codec, gboolean in_strips)
{
	memset(a, 0, sizeof(*a));
	a->in_strips = in_strips;
	a->whole.codec = codec;
	reassembler_init(&a->whole.packets, codec, FALSE);
	for (guint i = 0; i < ARD_STRIPS; i++)
		reassembler_init(&a->strips.strips[i], ARD_CODEC_HEVC, TRUE);
}

void ard_assembly_clear(ArdAssembly *a)
{
	reassembler_clear(&a->whole.packets);
	for (guint i = 0; i < ARD_STRIPS; i++)
		reassembler_clear(&a->strips.strips[i]);
}

guint32 ard_assembly_stream(ArdAssembly *a, guint32 ssrc)
{
	return a->in_strips ? strips_stream(&a->strips, ssrc) : ssrc;
}

ArdDepack ard_assembly_push(ArdAssembly *a, const ArdRtpHeader *h, const guint8 *payload, gsize len, ArdUnit **unit)
{
	*unit = NULL;
	if (a->in_strips)
		return strips_push(&a->strips, h, payload, len, unit);
	return whole_push(&a->whole, h, payload, len, unit);
}

void ard_assembly_resync(ArdAssembly *a)
{
	if (a->in_strips) {
		a->strips.have_next = FALSE;
	} else {
		a->whole.synced = FALSE;
		a->whole.resumable = FALSE;
	}
}

gboolean ard_assembly_resumes_at_refresh(const ArdAssembly *a)
{
	return !a->in_strips && !a->whole.synced && a->whole.resumable;
}

gboolean ard_assembly_complete(const ArdAssembly *a)
{
	if (!a->in_strips)
		return TRUE;
	return a->strips.have_next && a->strips.owed == 0;
}

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

#include "ard_pasteboard.h"
#include "ard_log.h"
#include "ard_wire.h"

#include <string.h>
#include <zlib.h>

#define UTF8_TEXT "public.utf8-plain-text"
/* Text past this is not taken from the Mac: a clipboard that size is not one
 * a person pastes. */
#define MAX_TEXT (16 * 1024 * 1024)

void ard_pasteboard_fetch(guint32 session_id, guint8 out[8])
{
	memset(out, 0, 8);
	out[0] = 0x0b;
	ard_put32(out + 4, session_id);
}

static GByteArray *archive(const gchar *text)
{
	GByteArray *out = g_byte_array_new();
	gsize len = strlen(text);

	if (len == 0) {
		ard_append32(out, 0);
		return out;
	}
	ard_append32(out, 1);
	ard_append32(out, strlen(UTF8_TEXT));
	g_byte_array_append(out, (const guint8 *)UTF8_TEXT, strlen(UTF8_TEXT));
	ard_append32(out, 0);	/* reserved */
	ard_append32(out, 0);	/* tag count */
	ard_append32(out, len);
	g_byte_array_append(out, (const guint8 *)text, len);
	return out;
}

GByteArray *ard_pasteboard_send(guint32 session_id, const gchar *text)
{
	GByteArray *plain = archive(text);
	GByteArray *msg = g_byte_array_new();
	z_stream z = { 0 };
	gsize header;

	g_byte_array_append(msg, (const guint8 *)"\x1f\0\0\0", 4);
	ard_append32(msg, session_id);
	ard_append32(msg, plain->len);
	ard_append32(msg, 0);
	header = msg->len;
	/* One sync flush and no end of stream, as Apple's viewer sends it. */
	deflateInit(&z, Z_DEFAULT_COMPRESSION);
	g_byte_array_set_size(msg, header + deflateBound(&z, plain->len) + 64);
	z.next_in = plain->data;
	z.avail_in = plain->len;
	z.next_out = msg->data + header;
	z.avail_out = msg->len - header;
	deflate(&z, Z_SYNC_FLUSH);
	g_byte_array_set_size(msg, header + z.total_out);
	ard_put32(msg->data + 12, z.total_out);
	deflateEnd(&z);
	g_byte_array_unref(plain);
	return msg;
}

typedef struct {
	const guint8 *data;
	gsize len;
	gsize at;
} Cursor;

static gboolean take32(Cursor *c, guint32 *v)
{
	if (c->len - c->at < 4)
		return FALSE;
	*v = ard_get32(c->data + c->at);
	c->at += 4;
	return TRUE;
}

static gboolean take(Cursor *c, gsize n, const guint8 **p)
{
	if (c->len - c->at < n)
		return FALSE;
	*p = c->data + c->at;
	c->at += n;
	return TRUE;
}

/* A run of items, each a u32 flavor count and its flavors. A flavor is a
 * counted name, a reserved u32, a counted list of counted key and value tags,
 * and counted data, every count a big-endian u32. */
static gchar *find_text(const guint8 *data, gsize len)
{
	Cursor c = { data, len, 0 };

	while (c.at < c.len) {
		guint32 flavors;

		if (!take32(&c, &flavors))
			return NULL;
		for (guint32 f = 0; f < flavors; f++) {
			guint32 n, reserved, tags, size;
			const guint8 *name, *bytes;

			if (!take32(&c, &n) || !take(&c, n, &name) || !take32(&c, &reserved) || !take32(&c, &tags))
				return NULL;
			for (guint32 t = 0; t < tags * 2; t++)
				if (!take32(&c, &size) || !take(&c, size, &bytes))
					return NULL;
			if (!take32(&c, &size) || !take(&c, size, &bytes))
				return NULL;
			if (n == strlen(UTF8_TEXT) && memcmp(name, UTF8_TEXT, n) == 0 && size > 0) {
				if (size > MAX_TEXT || !g_utf8_validate((const gchar *)bytes, size, NULL))
					return NULL;
				return g_strndup((const gchar *)bytes, size);
			}
		}
	}
	return NULL;
}

gchar *ard_pasteboard_text(const guint8 *compressed, gsize len, guint32 uncompressed)
{
	z_stream z = { 0 };
	guint8 *plain;
	gchar *text = NULL;
	int ret;

	if (uncompressed > ARD_PASTEBOARD_MAX)
		return NULL;
	plain = g_malloc(uncompressed + 1);
	inflateInit(&z);
	z.next_in = (guint8 *)compressed;
	z.avail_in = len;
	z.next_out = plain;
	z.avail_out = uncompressed + 1;
	ret = inflate(&z, Z_SYNC_FLUSH);
	if ((ret == Z_OK || ret == Z_STREAM_END || ret == Z_BUF_ERROR) && z.total_out <= uncompressed)
		text = find_text(plain, z.total_out);
	else
		ARD_WARNING("the Mac's pasteboard did not inflate to its declared %u bytes", uncompressed);
	inflateEnd(&z);
	g_free(plain);
	return text;
}

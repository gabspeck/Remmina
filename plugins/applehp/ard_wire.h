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

/* Big-endian field access for the wire formats of Apple's RFB 003.889 and its
 * media stream. Everything Apple sends is big-endian, the flags word of the
 * media-stream offer included. */

#pragma once

#include <glib.h>

static inline guint16 ard_get16(const guint8 *p)
{
	return (guint16)((p[0] << 8) | p[1]);
}

static inline guint32 ard_get32(const guint8 *p)
{
	return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

static inline void ard_put16(guint8 *p, guint16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static inline void ard_put32(guint8 *p, guint32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static inline void ard_append16(GByteArray *a, guint16 v)
{
	guint8 b[2];

	ard_put16(b, v);
	g_byte_array_append(a, b, 2);
}

static inline void ard_append32(GByteArray *a, guint32 v)
{
	guint8 b[4];

	ard_put32(b, v);
	g_byte_array_append(a, b, 4);
}

static inline void ard_append64(GByteArray *a, guint64 v)
{
	ard_append32(a, v >> 32);
	ard_append32(a, (guint32)v);
}

static inline void ard_append_f32(GByteArray *a, gfloat f)
{
	union { gfloat f; guint32 u; } v = { .f = f };

	ard_append32(a, v.u);
}

static inline void ard_append_f64(GByteArray *a, gdouble d)
{
	union { gdouble d; guint64 u; } v = { .d = d };

	ard_append64(a, v.u);
}

static inline gdouble ard_get_f64(const guint8 *p)
{
	union { gdouble d; guint64 u; } v;

	v.u = ((guint64)ard_get32(p) << 32) | ard_get32(p + 4);
	return v.d;
}

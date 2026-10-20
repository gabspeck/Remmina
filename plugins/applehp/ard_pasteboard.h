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

/* Apple's pasteboard messages. A Mac does not bridge its pasteboard through
 * RFB cut text: the viewer turns AutoPasteboard on, the Mac says when its
 * pasteboard changes (MiscStatus 2), the viewer fetches it (0x0b), and the Mac
 * answers with a zlib archive of every flavor of every item (0x1f). The viewer
 * sends its own the same way. Only the UTF-8 text flavor is read or written. */

#pragma once

#include <glib.h>

#define ARD_PASTEBOARD_MAX (100 * 1024 * 1024)

/* 0x1f's fixed part after its type byte: a pad byte, the promises bit, a pad
 * byte, the echoed session id, and the uncompressed and compressed sizes. */
#define ARD_PASTEBOARD_HEADER 15

void ard_pasteboard_fetch(guint32 session_id, guint8 out[8]);

/* The 0x1f message putting `text` on the Mac's pasteboard. Empty text is an
 * item with no flavors, which clears it. */
GByteArray *ard_pasteboard_send(guint32 session_id, const gchar *text);

/* The text out of a compressed archive, or NULL where it holds none. */
gchar *ard_pasteboard_text(const guint8 *compressed, gsize len, guint32 uncompressed);

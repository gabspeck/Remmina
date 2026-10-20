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

/* Decoding the media stream: the Mac's HEVC (Range Extensions, 8-bit 4:4:4,
 * full-range BT.709) or H.264 with FFmpeg into the display's pixels, and its
 * AAC-ELD sound with FFmpeg into PulseAudio. Each runs on a thread of its own,
 * so a slow decode never holds up the sockets the stream is read from. */

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include "ard_media.h"

typedef struct _ArdVideo ArdVideo;
typedef struct _ArdAudio ArdAudio;

typedef struct {
	/* Lock the display's pixels for writing: 32-bit BGRx rows of `stride`
	 * bytes, `width`×`height`. FALSE when there is nothing to draw on. */
	gboolean (*lock)(gpointer user, guint8 **pixels, gint *stride, gint *width, gint *height);
	/* Unlock them, `rows` rows from `y` having been drawn. */
	void (*unlock)(gpointer user, gint y, gint rows);
	/* The decoder lost its place and needs a keyframe. */
	void (*want_keyframe)(gpointer user);
	gpointer user;
} ArdVideoCallbacks;

ArdVideo *ard_video_start(ArdCodec codec, const ArdVideoCallbacks *callbacks, GError **error);
/* Queue a unit, which the decoder owns from here. FALSE when the queue is
 * full, and the unit has been dropped. */
gboolean ard_video_push(ArdVideo *v, ArdUnit *unit);
void ard_video_stop(ArdVideo *v);

/* NULL, with the reason logged, where sound cannot be played. */
ArdAudio *ard_audio_start(void);
void ard_audio_push(ArdAudio *a, const guint8 *data, gsize len);
void ard_audio_stop(ArdAudio *a);

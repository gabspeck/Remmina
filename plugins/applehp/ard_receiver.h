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

/* The UDP side of the media stream: the sound and picture legs read, every
 * packet authenticated and decrypted, the picture reassembled into access
 * units, and the RTCP the Mac expects sent back on each leg: receiver reports
 * every second, the rate controller's RCTL every 50 ms, acknowledgements of
 * every HEVC picture handed on, and keyframe requests. */

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include "ard_media.h"

typedef struct _ArdReceiver ArdReceiver;

typedef struct {
	/* A picture to decode, which the callee owns. FALSE when the decoder's
	 * queue is full: the stream then drops to its next keyframe. */
	gboolean (*video_unit)(gpointer user, ArdUnit *unit);
	/* One AAC-ELD access unit, 10 ms of 48 kHz stereo. */
	void (*audio_unit)(gpointer user, const guint8 *data, gsize len);
	gpointer user;
} ArdReceiverCallbacks;

/* When each leg last brought something, in monotonic microseconds, 0 for
 * never. The session judges the stream's liveness from them. */
typedef struct {
	gint64 video_heard;
	gint64 video_pictured;
	gint64 video_sender_report;
	gint64 video_receiver_report;
	gint64 audio_heard;
	gint64 audio_sounded;
	gchar *failure;
} ArdReceiverTimes;

/* Bind the ports the Mac named on `local`'s address and connect each socket to
 * the same port on `peer`'s, which is where the Mac sends from. */
ArdReceiver *ard_receiver_start(const ArdOffers *offers, const struct sockaddr_storage *local,
				const struct sockaddr_storage *peer, guint16 audio_port, guint16 video_port,
				const ArdReceiverCallbacks *callbacks, GError **error);
void ard_receiver_stop(ArdReceiver *r);

/* The display the stream was offered for, which every unit after it is
 * stamped with. */
void ard_receiver_set_display(ArdReceiver *r, guint16 width, guint16 height);

/* The decoder has nothing left to predict from: drop to the next keyframe and
 * ask the Mac for one. */
void ard_receiver_want_keyframe(ArdReceiver *r);

/* The caller frees `times->failure`. */
void ard_receiver_times(ArdReceiver *r, ArdReceiverTimes *times);

gboolean ard_receiver_matches(ArdReceiver *r, guint16 audio_port, guint16 video_port);

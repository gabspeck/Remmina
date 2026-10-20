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

/* One High Performance Screen Sharing session with a Mac: RFB 003.889 inside
 * Apple's record layer over TCP, one virtual display the Mac creates at the
 * size this side asks for, and the picture and sound from the media stream.
 *
 * The session runs on threads of its own and reports to its owner through
 * the callbacks, from those threads. */

#pragma once

#include <glib.h>
#include <cairo.h>
#include "ard_media.h"

typedef struct _ArdSession ArdSession;

#define ARD_SESSION_ERROR (ard_session_error_quark())
GQuark ard_session_error_quark(void);

typedef enum {
	/* The Mac refused the login: a wrong password, or an account Remote
	 * Management does not let in. */
	ARD_SESSION_ERROR_AUTH,
	ARD_SESSION_ERROR_FAILED
} ArdSessionError;

typedef struct {
	/* The account to log in as. `retry` is TRUE after the Mac refused the last
	 * one. FALSE to give up. The callee fills both with g_malloc'd strings. */
	gboolean (*credentials)(gpointer user, gboolean retry, gchar **username, gchar **password);
	/* The session is up: the record layer is on. */
	void (*connected)(gpointer user);
	/* The Mac's display is `width`×`height` backing pixels at `density`
	 * pixels per point. */
	void (*display)(gpointer user, guint16 width, guint16 height, gdouble density);
	/* The display has no picture to show, and why; NULL once it has one again
	 * is the owner's to decide, from the first picture drawn. */
	void (*covered)(gpointer user, const gchar *why);
	/* A pointer shape: an ARGB32 surface the size of the shape in points, which
	 * the callee does not own. */
	void (*cursor)(gpointer user, cairo_surface_t *shape, gint hot_x, gint hot_y);
	void (*clipboard)(gpointer user, const gchar *text);
	/* The session ended on its own. Not called once ard_session_stop() has
	 * begun. */
	void (*closed)(gpointer user, const GError *error);
	/* The display's pixels, for the picture decoder; see ArdVideoCallbacks. */
	gboolean (*lock)(gpointer user, guint8 **pixels, gint *stride, gint *width, gint *height);
	void (*unlock)(gpointer user, gint y, gint rows);
	gpointer user;
} ArdSessionCallbacks;

typedef struct {
	gchar *host;
	gint port;
	ArdCodec codec;
	gboolean sound;
	/* The display to open with, in points, and the density to ask for it
	 * at: 1 or 2, since the Mac does not round a fractional ratio. */
	guint16 width;
	guint16 height;
	gint density;
} ArdSessionParams;

ArdSession *ard_session_start(const ArdSessionParams *params, const ArdSessionCallbacks *callbacks);
/* End the session and wait for its threads. Not from the thread that runs the
 * owner's main loop while a callback may be waiting on it. */
void ard_session_stop(ArdSession *s);

/* Input, in the display's backing pixels. Each is queued, never blocking. */
void ard_session_pointer(ArdSession *s, guint8 buttons, gint x, gint y);
void ard_session_key(ArdSession *s, guint32 keysym, gboolean down);
/* A scroll by a distance in points, positive right and down. */
void ard_session_scroll(ArdSession *s, gint dx, gint dy, gint x, gint y);

/* Ask the Mac for a display of `width`×`height` points at `density`. Only the
 * last one asked for goes out, once the display has settled. */
void ard_session_resize(ArdSession *s, guint16 width, guint16 height, gint density);

/* Ask the Mac for a keyframe. */
void ard_session_refresh(ArdSession *s);

void ard_session_clipboard(ArdSession *s, const gchar *text);

/* The Apple mask bits for RFB's buttons: on 003.889 the Mac reads the mask as
 * macOS button numbers, so the middle and right bits trade places. */
#define ARD_BUTTON_LEFT 0x01
#define ARD_BUTTON_RIGHT 0x02
#define ARD_BUTTON_MIDDLE 0x04

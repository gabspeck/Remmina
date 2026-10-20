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

#pragma once

#include "common/remmina_plugin.h"
#include "ard_session.h"

#define APPLEHP_PLUGIN_NAME        "APPLEHP"
#define APPLEHP_PLUGIN_DESCRIPTION N_("Apple Screen Sharing, High Performance")
#define APPLEHP_PLUGIN_VERSION     VERSION
#define APPLEHP_PLUGIN_APPICON     "org.remmina.Remmina-applehp-symbolic"
#define APPLEHP_PLUGIN_SSH_APPICON "org.remmina.Remmina-applehp-symbolic"

#define APPLEHP_DEFAULT_PORT 5900

#define REMMINA_PLUGIN_AUDIT(fmt, ...) \
		remmina_plugin_service->_remmina_audit(__func__, fmt, ##__VA_ARGS__)

typedef struct _RemminaPluginApplehpData {
	RemminaProtocolWidget *gp;
	GtkWidget *drawing_area;
	ArdSession *session;
	/* Set on the main thread once teardown has begun. */
	gboolean closing;

	/* The display's pixels, which the decoder writes and the main thread
	 * draws: the Mac's backing pixels, drawn at its density. */
	GMutex buffer_lock;
	cairo_surface_t *surface;
	gint width;
	gint height;
	gdouble density;
	/* Why there is no picture to show, or NULL once there is one. */
	gchar *cover;
	gint redraw_queued;

	cairo_surface_t *cursor;
	gint cursor_x, cursor_y;
	gint cursor_queued;

	guint8 buttons;
	gdouble scroll_x, scroll_y;
	/* Hardware keycode to the keysym its press sent, so a release sends the
	 * same keysym whatever the modifiers have done since. */
	GHashTable *pressed;

	gulong clipboard_handler;
	gint64 clipboard_quiet_until;
	guint resize_source;
	/* Whether the Mac's display follows the window, which it does only once
	 * the connection is open: the panels Remmina shows while connecting
	 * shrink the window for a moment, and every display change restarts the
	 * Mac's stream. */
	gboolean following;
	gboolean first_credentials;
} RemminaPluginApplehpData;

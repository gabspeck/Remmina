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

/* Remmina's side of Apple High Performance Screen Sharing: the widget the
 * Mac's virtual display is drawn on, input, the clipboard, and the window size
 * the display follows. The protocol is ard_session.c's. */

#include "applehp_plugin.h"
#include "ard_log.h"

#include <math.h>
#include <gdk/gdkkeysyms.h>

#define APPLEHP_FEATURE_SCALE           1
#define APPLEHP_FEATURE_DYNRESUPDATE    2
#define APPLEHP_FEATURE_UNFOCUS         3
#define APPLEHP_FEATURE_VIEWONLY        4
#define APPLEHP_FEATURE_TOOL_REFRESH    5

/* The points one unit of a scroll is worth: a wheel notch, or about a notch's
 * worth of a touchpad glide. */
#define SCROLL_POINTS 50.0
/* The most one event may scroll, so a flick cannot flood the connection. */
#define SCROLL_MAX 512.0
/* How long a window's size has to hold before the Mac is asked for it. */
#define RESIZE_DELAY_MS 500
#define DEFAULT_WIDTH 1440
#define DEFAULT_HEIGHT 900

#define GET_PLUGIN_DATA(gp) (RemminaPluginApplehpData *)g_object_get_data(G_OBJECT(gp), "plugin-data")

static RemminaPluginService *remmina_plugin_service = NULL;

void ard_log(ArdLogLevel level, const gchar *func, const gchar *fmt, ...)
{
	va_list args;
	gchar *message;

	va_start(args, fmt);
	message = g_strdup_vprintf(fmt, args);
	va_end(args);
	switch (level) {
	case ARD_LOG_DEBUG:
		remmina_plugin_service->_remmina_debug(func, "%s", message);
		break;
	case ARD_LOG_INFO:
		remmina_plugin_service->_remmina_info("%s", message);
		break;
	case ARD_LOG_WARNING:
		remmina_plugin_service->_remmina_warning(func, "%s", message);
		break;
	}
	g_free(message);
}

/* ------------------------------------------------------------------------ */
/* The display */

static gboolean viewonly(RemminaProtocolWidget *gp)
{
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	return remmina_plugin_service->file_get_int(remminafile, "viewonly", FALSE);
}

static gboolean scaled(RemminaProtocolWidget *gp)
{
	return remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp) != REMMINA_PROTOCOL_WIDGET_SCALE_MODE_NONE;
}

static void update_scale(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gint width = remmina_plugin_service->protocol_plugin_get_width(gp);
	gint height = remmina_plugin_service->protocol_plugin_get_height(gp);

	if (scaled(gp))
		/* The drawing area takes its size from its parent. */
		gtk_widget_set_size_request(gpdata->drawing_area, -1, -1);
	else
		gtk_widget_set_size_request(gpdata->drawing_area, width, height);
	remmina_plugin_service->protocol_plugin_update_align(gp);
}

static gboolean redraw(gpointer data)
{
	RemminaProtocolWidget *gp = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	g_atomic_int_set(&gpdata->redraw_queued, 0);
	if (!gpdata->closing)
		gtk_widget_queue_draw(gpdata->drawing_area);
	return G_SOURCE_REMOVE;
}

/* From any thread. Draws queued meanwhile collapse into one. */
static void queue_redraw(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (g_atomic_int_compare_and_exchange(&gpdata->redraw_queued, 0, 1))
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, redraw, g_object_ref(gp), g_object_unref);
}

static void draw_cover(GtkWidget *widget, cairo_t *context, const gchar *text)
{
	GtkAllocation a;
	PangoLayout *layout;
	gint w, h;

	gtk_widget_get_allocation(widget, &a);
	cairo_identity_matrix(context);
	cairo_set_source_rgba(context, 0, 0, 0, 0.6);
	cairo_paint(context);
	layout = gtk_widget_create_pango_layout(widget, text);
	pango_layout_get_pixel_size(layout, &w, &h);
	cairo_set_source_rgb(context, 1, 1, 1);
	cairo_move_to(context, (a.width - w) / 2.0, (a.height - h) / 2.0);
	pango_cairo_show_layout(context, layout);
	g_object_unref(layout);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *context, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	GtkAllocation a;

	g_mutex_lock(&gpdata->buffer_lock);
	if (gpdata->surface) {
		gdouble w = gpdata->width / gpdata->density, h = gpdata->height / gpdata->density;

		cairo_save(context);
		if (scaled(gp)) {
			gtk_widget_get_allocation(widget, &a);
			cairo_scale(context, a.width / w, a.height / h);
		}
		/* The surface's device scale is the Mac's density: drawn at its
		 * points, a 2x display lands pixel for pixel on a 2x screen. */
		cairo_set_source_surface(context, gpdata->surface, 0, 0);
		cairo_rectangle(context, 0, 0, w, h);
		cairo_fill(context);
		cairo_restore(context);
	} else {
		cairo_set_source_rgb(context, 0, 0, 0);
		cairo_paint(context);
	}
	if (gpdata->cover)
		draw_cover(widget, context, gpdata->cover);
	g_mutex_unlock(&gpdata->buffer_lock);
	return TRUE;
}

typedef struct {
	RemminaProtocolWidget *gp;
	gint width;
	gint height;
} Resized;

static gboolean apply_display(gpointer data)
{
	Resized *r = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(r->gp);

	if (!gpdata->closing) {
		remmina_plugin_service->protocol_plugin_set_width(r->gp, r->width);
		remmina_plugin_service->protocol_plugin_set_height(r->gp, r->height);
		update_scale(r->gp);
		remmina_plugin_service->protocol_plugin_desktop_resize(r->gp);
		gtk_widget_queue_draw(gpdata->drawing_area);
	}
	g_object_unref(r->gp);
	g_free(r);
	return G_SOURCE_REMOVE;
}

/* From the session's thread. The surface is replaced here, at once, so the
 * new display's first picture never finds the old one's. */
static void on_display(gpointer user, guint16 width, guint16 height, gdouble density)
{
	RemminaProtocolWidget *gp = user;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cairo_surface_t *old;
	Resized *r = g_new0(Resized, 1);

	cairo_surface_set_device_scale(surface, density, density);
	g_mutex_lock(&gpdata->buffer_lock);
	old = gpdata->surface;
	gpdata->surface = surface;
	gpdata->width = width;
	gpdata->height = height;
	gpdata->density = density;
	g_mutex_unlock(&gpdata->buffer_lock);
	if (old)
		cairo_surface_destroy(old);
	r->gp = g_object_ref(gp);
	r->width = lround(width / density);
	r->height = lround(height / density);
	g_idle_add(apply_display, r);
}

static void on_covered(gpointer user, const gchar *why)
{
	RemminaProtocolWidget *gp = user;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	g_mutex_lock(&gpdata->buffer_lock);
	g_free(gpdata->cover);
	gpdata->cover = g_strdup(why);
	g_mutex_unlock(&gpdata->buffer_lock);
	queue_redraw(gp);
}

static gboolean pixels_lock(gpointer user, guint8 **pixels, gint *stride, gint *width, gint *height)
{
	RemminaProtocolWidget *gp = user;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	g_mutex_lock(&gpdata->buffer_lock);
	if (!gpdata->surface) {
		g_mutex_unlock(&gpdata->buffer_lock);
		return FALSE;
	}
	cairo_surface_flush(gpdata->surface);
	*pixels = cairo_image_surface_get_data(gpdata->surface);
	*stride = cairo_image_surface_get_stride(gpdata->surface);
	*width = gpdata->width;
	*height = gpdata->height;
	return TRUE;
}

static void pixels_unlock(gpointer user, gint y, gint rows)
{
	RemminaProtocolWidget *gp = user;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (rows > 0) {
		cairo_surface_mark_dirty(gpdata->surface);
		/* The first picture of the display lifts the notice. */
		g_clear_pointer(&gpdata->cover, g_free);
	}
	g_mutex_unlock(&gpdata->buffer_lock);
	if (rows > 0)
		queue_redraw(gp);
}

/* ------------------------------------------------------------------------ */
/* The session's other callbacks */

static void schedule_resize(RemminaProtocolWidget *gp);

static gboolean on_connected_idle(gpointer data)
{
	RemminaProtocolWidget *gp = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (!gpdata->closing) {
		remmina_plugin_service->protocol_plugin_signal_connection_opened(gp);
		remmina_plugin_service->protocol_plugin_unlock_dynres(gp);
		gpdata->following = TRUE;
		schedule_resize(gp);
	}
	g_object_unref(gp);
	return G_SOURCE_REMOVE;
}

static void on_connected(gpointer user)
{
	g_idle_add(on_connected_idle, g_object_ref(user));
}

/* The first time, the profile's account; after a refusal, or with none
 * saved, Remmina's prompt. */
static gboolean on_credentials(gpointer user, gboolean retry, gchar **username, gchar **password)
{
	RemminaProtocolWidget *gp = user;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	const gchar *saved_user = remmina_plugin_service->file_get_string(remminafile, "username");
	const gchar *saved_password = remmina_plugin_service->file_get_string(remminafile, "password");
	gboolean disablepasswordstoring;
	gint ret;

	if (!retry && gpdata->first_credentials && saved_user && *saved_user && saved_password && *saved_password) {
		gpdata->first_credentials = FALSE;
		*username = g_strdup(saved_user);
		*password = g_strdup(saved_password);
		return TRUE;
	}
	gpdata->first_credentials = FALSE;
	disablepasswordstoring = remmina_plugin_service->file_get_int(remminafile, "disablepasswordstoring", FALSE);
	ret = remmina_plugin_service->protocol_plugin_init_auth(gp,
								(disablepasswordstoring ? 0 : REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD) | REMMINA_MESSAGE_PANEL_FLAG_USERNAME,
								retry ? _("The Mac refused the login. Enter the Mac account's credentials")
								      : _("Enter the Mac account's credentials"),
								saved_user, saved_password, NULL, NULL);
	if (ret != GTK_RESPONSE_OK)
		return FALSE;
	*username = remmina_plugin_service->protocol_plugin_init_get_username(gp);
	*password = remmina_plugin_service->protocol_plugin_init_get_password(gp);
	if (remmina_plugin_service->protocol_plugin_init_get_savepassword(gp)) {
		remmina_plugin_service->file_set_string(remminafile, "username", *username);
		remmina_plugin_service->file_set_string(remminafile, "password", *password);
	}
	return *username && *password;
}

typedef struct {
	RemminaProtocolWidget *gp;
	cairo_surface_t *shape;
	gint hot_x, hot_y;
} CursorChange;

static gboolean apply_cursor(gpointer data)
{
	CursorChange *c = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(c->gp);
	GdkWindow *window;

	if (!gpdata->closing && (window = gtk_widget_get_window(gpdata->drawing_area))) {
		GdkCursor *cursor = gdk_cursor_new_from_surface(gtk_widget_get_display(gpdata->drawing_area), c->shape,
								c->hot_x, c->hot_y);

		gdk_window_set_cursor(window, cursor);
		g_object_unref(cursor);
	}
	cairo_surface_destroy(c->shape);
	g_object_unref(c->gp);
	g_free(c);
	return G_SOURCE_REMOVE;
}

/* The Mac captures its screen without the pointer and sends the pointer's
 * shape apart, which is drawn as the local cursor. */
static void on_cursor(gpointer user, cairo_surface_t *shape, gint hot_x, gint hot_y)
{
	CursorChange *c = g_new0(CursorChange, 1);

	c->gp = g_object_ref(user);
	c->shape = cairo_surface_reference(shape);
	c->hot_x = hot_x;
	c->hot_y = hot_y;
	g_idle_add(apply_cursor, c);
}

typedef struct {
	RemminaProtocolWidget *gp;
	gchar *text;
} ClipboardChange;

static gboolean apply_clipboard(gpointer data)
{
	ClipboardChange *c = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(c->gp);

	if (!gpdata->closing) {
		/* The owner change this makes is not sent back to the Mac. */
		gpdata->clipboard_quiet_until = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), c->text, -1);
	}
	g_free(c->text);
	g_object_unref(c->gp);
	g_free(c);
	return G_SOURCE_REMOVE;
}

static void on_clipboard(gpointer user, const gchar *text)
{
	RemminaProtocolWidget *gp = user;
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	ClipboardChange *c;

	if (remmina_plugin_service->file_get_int(remminafile, "disableclipboard", FALSE))
		return;
	c = g_new0(ClipboardChange, 1);
	c->gp = g_object_ref(gp);
	c->text = g_strdup(text);
	g_idle_add(apply_clipboard, c);
}

/* ------------------------------------------------------------------------ */
/* Closing */

typedef struct {
	RemminaProtocolWidget *gp;
	ArdSession *session;
} Teardown;

static gboolean finish_teardown(gpointer data)
{
	Teardown *t = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(t->gp);

	g_mutex_lock(&gpdata->buffer_lock);
	if (gpdata->surface)
		cairo_surface_destroy(gpdata->surface);
	gpdata->surface = NULL;
	g_clear_pointer(&gpdata->cover, g_free);
	g_mutex_unlock(&gpdata->buffer_lock);
	g_hash_table_remove_all(gpdata->pressed);
	remmina_plugin_service->protocol_plugin_signal_connection_closed(t->gp);
	g_object_unref(t->gp);
	g_free(t);
	return G_SOURCE_REMOVE;
}

/* Stopping the session waits for its threads, one of which may be waiting on
 * this main loop for a prompt, so it is waited for off it. */
static gpointer teardown_thread(gpointer data)
{
	Teardown *t = data;

	ard_session_stop(t->session);
	g_idle_add(finish_teardown, t);
	return NULL;
}

static void start_teardown(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	Teardown *t;

	if (!gpdata || gpdata->closing)
		return;
	gpdata->closing = TRUE;
	if (gpdata->clipboard_handler) {
		g_signal_handler_disconnect(G_OBJECT(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)), gpdata->clipboard_handler);
		gpdata->clipboard_handler = 0;
	}
	if (gpdata->resize_source) {
		g_source_remove(gpdata->resize_source);
		gpdata->resize_source = 0;
	}
	t = g_new0(Teardown, 1);
	t->gp = g_object_ref(gp);
	t->session = gpdata->session;
	gpdata->session = NULL;
	g_thread_unref(g_thread_new("applehp-close", teardown_thread, t));
}

typedef struct {
	RemminaProtocolWidget *gp;
	gchar *message;
} Closed;

static gboolean on_closed_idle(gpointer data)
{
	Closed *c = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(c->gp);

	if (!gpdata->closing) {
		if (c->message)
			remmina_plugin_service->protocol_plugin_set_error(c->gp, "%s", c->message);
		start_teardown(c->gp);
	}
	g_free(c->message);
	g_object_unref(c->gp);
	g_free(c);
	return G_SOURCE_REMOVE;
}

static void on_closed(gpointer user, const GError *error)
{
	Closed *c = g_new0(Closed, 1);

	c->gp = g_object_ref(user);
	if (error)
		c->message = g_strdup_printf(_("Apple Screen Sharing: %s"), error->message);
	g_idle_add(on_closed_idle, c);
}

/* ------------------------------------------------------------------------ */
/* Input */

/* A widget position in the display's backing pixels. */
static void to_display(RemminaProtocolWidget *gp, GtkWidget *widget, gdouble x, gdouble y, gint *dx, gint *dy)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gint width, height;
	gdouble density;

	g_mutex_lock(&gpdata->buffer_lock);
	width = gpdata->width;
	height = gpdata->height;
	density = gpdata->density;
	g_mutex_unlock(&gpdata->buffer_lock);
	if (width <= 0 || height <= 0) {
		*dx = *dy = 0;
		return;
	}
	if (scaled(gp)) {
		GtkAllocation a;

		gtk_widget_get_allocation(widget, &a);
		x = x * (width / density) / MAX(a.width, 1);
		y = y * (height / density) / MAX(a.height, 1);
	}
	*dx = CLAMP((gint)lround(x * density), 0, width - 1);
	*dy = CLAMP((gint)lround(y * density), 0, height - 1);
}

/* Whether the screen is not available: input then goes nowhere, since it would
 * land on a display nobody can see. Releases still go, so nothing is left held. */
static gboolean covered(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gboolean is_covered;

	g_mutex_lock(&gpdata->buffer_lock);
	is_covered = gpdata->cover != NULL;
	g_mutex_unlock(&gpdata->buffer_lock);
	return is_covered;
}

static gboolean on_motion(GtkWidget *widget, GdkEventMotion *event, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gint x, y;

	if (!gpdata->session || viewonly(gp) || covered(gp))
		return FALSE;
	to_display(gp, widget, event->x, event->y, &x, &y);
	ard_session_pointer(gpdata->session, gpdata->buttons, x, y);
	return TRUE;
}

static gboolean on_button(GtkWidget *widget, GdkEventButton *event, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	guint8 bit;
	gint x, y;

	if (!gpdata->session || viewonly(gp))
		return FALSE;
	/* The Mac chains clicks into a double-click itself. */
	if (event->type != GDK_BUTTON_PRESS && event->type != GDK_BUTTON_RELEASE)
		return TRUE;
	switch (event->button) {
	case 1:
		bit = ARD_BUTTON_LEFT;
		break;
	case 2:
		bit = ARD_BUTTON_MIDDLE;
		break;
	case 3:
		bit = ARD_BUTTON_RIGHT;
		break;
	default:
		return FALSE;
	}
	if (event->type == GDK_BUTTON_PRESS) {
		if (covered(gp))
			return TRUE;
		gpdata->buttons |= bit;
	} else {
		gpdata->buttons &= ~bit;
	}
	to_display(gp, widget, event->x, event->y, &x, &y);
	ard_session_pointer(gpdata->session, gpdata->buttons, x, y);
	return TRUE;
}

/* Whole points out of a running total, the fraction kept for the next event
 * so a slow glide still adds up. A reversal starts over. */
static gint spend(gdouble *pending, gdouble add)
{
	gint whole;

	if (add == 0)
		return 0;
	if ((*pending > 0 && add < 0) || (*pending < 0 && add > 0))
		*pending = 0;
	*pending = CLAMP(*pending + add, -SCROLL_MAX, SCROLL_MAX);
	whole = (gint)*pending;
	*pending -= whole;
	return whole;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gdouble dx = 0, dy = 0;
	gint x, y, sx, sy;

	if (!gpdata->session || viewonly(gp) || covered(gp))
		return FALSE;
	switch (event->direction) {
	case GDK_SCROLL_UP:
		dy = -1;
		break;
	case GDK_SCROLL_DOWN:
		dy = 1;
		break;
	case GDK_SCROLL_LEFT:
		dx = -1;
		break;
	case GDK_SCROLL_RIGHT:
		dx = 1;
		break;
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy);
		break;
	default:
		return FALSE;
	}
	sx = spend(&gpdata->scroll_x, dx * SCROLL_POINTS);
	sy = spend(&gpdata->scroll_y, dy * SCROLL_POINTS);
	to_display(gp, widget, event->x, event->y, &x, &y);
	ard_session_scroll(gpdata->session, sx, sy, x, y);
	return TRUE;
}

/* The keysym a key sends a Mac. Without Caps Lock: the Mac brings Shift with
 * an uppercase keysym, so a shortcut under Caps Lock would arrive with Shift
 * added. The Mac maps Alt to Command; a keyboard's Alt keys are its Option
 * keys, which the Mac maps from Meta. */
static guint32 keysym_for(RemminaProtocolWidget *gp, GdkEventKey *event)
{
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_window_get_display(event->window));
	guint keyval = event->keyval;
	guint plain;

	if (gdk_keymap_translate_keyboard_state(keymap, event->hardware_keycode, event->state & ~GDK_LOCK_MASK,
						event->group, &plain, NULL, NULL, NULL))
		keyval = plain;
	keyval = remmina_plugin_service->pref_keymap_get_keyval(remmina_plugin_service->file_get_string(remminafile, "keymap"),
								keyval);
	switch (keyval) {
	case GDK_KEY_Alt_L:
		return GDK_KEY_Meta_L;
	case GDK_KEY_Alt_R:
	case GDK_KEY_ISO_Level3_Shift:
		return GDK_KEY_Meta_R;
	default:
		return keyval;
	}
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *event, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	gpointer key = GUINT_TO_POINTER(event->hardware_keycode);
	gpointer sent;

	if (!gpdata->session || viewonly(gp))
		return FALSE;
	gpdata->scroll_x = gpdata->scroll_y = 0;
	if (event->type == GDK_KEY_PRESS) {
		guint32 keysym;

		if (covered(gp))
			return TRUE;
		keysym = keysym_for(gp, event);
		g_hash_table_insert(gpdata->pressed, key, GUINT_TO_POINTER(keysym));
		ard_session_key(gpdata->session, keysym, TRUE);
	} else if (g_hash_table_lookup_extended(gpdata->pressed, key, NULL, &sent)) {
		g_hash_table_remove(gpdata->pressed, key);
		ard_session_key(gpdata->session, GPOINTER_TO_UINT(sent), FALSE);
	}
	return TRUE;
}

static void release_all(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	GHashTableIter iter;
	gpointer keysym;

	if (!gpdata)
		return;
	g_hash_table_iter_init(&iter, gpdata->pressed);
	while (g_hash_table_iter_next(&iter, NULL, &keysym))
		if (gpdata->session)
			ard_session_key(gpdata->session, GPOINTER_TO_UINT(keysym), FALSE);
	g_hash_table_remove_all(gpdata->pressed);
	gpdata->buttons = 0;
}

static void on_clipboard_text(GtkClipboard *clipboard, const gchar *text, gpointer data)
{
	RemminaProtocolWidget *gp = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (text && gpdata->session)
		ard_session_clipboard(gpdata->session, text);
	g_object_unref(gp);
}

static void on_clipboard_owner(GtkClipboard *clipboard, GdkEvent *event, RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (!gpdata->session || viewonly(gp) || g_get_monotonic_time() < gpdata->clipboard_quiet_until)
		return;
	gtk_clipboard_request_text(clipboard, on_clipboard_text, g_object_ref(gp));
}

/* ------------------------------------------------------------------------ */
/* The window's size */

static gint density_of(RemminaPluginApplehpData *gpdata)
{
	return CLAMP(gtk_widget_get_scale_factor(gpdata->drawing_area), 1, 2);
}

static gboolean resize_now(gpointer data)
{
	RemminaProtocolWidget *gp = data;
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	GtkAllocation a;

	gpdata->resize_source = 0;
	if (gpdata->session && remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp) == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES) {
		gtk_widget_get_allocation(GTK_WIDGET(gp), &a);
		if (a.width > 1 && a.height > 1)
			ard_session_resize(gpdata->session, MIN(a.width, G_MAXUINT16), MIN(a.height, G_MAXUINT16), density_of(gpdata));
	}
	return G_SOURCE_REMOVE;
}

static void schedule_resize(RemminaProtocolWidget *gp)
{
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (gpdata->closing || !gpdata->following)
		return;
	if (gpdata->resize_source)
		g_source_remove(gpdata->resize_source);
	gpdata->resize_source = g_timeout_add(RESIZE_DELAY_MS, resize_now, gp);
}

static void on_size_allocate(GtkWidget *widget, GtkAllocation *alloc, RemminaProtocolWidget *gp)
{
	if (remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp) == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES)
		schedule_resize(gp);
}

/* ------------------------------------------------------------------------ */
/* The plugin */

static void plugin_data_free(gpointer data)
{
	RemminaPluginApplehpData *gpdata = data;

	g_hash_table_destroy(gpdata->pressed);
	g_mutex_clear(&gpdata->buffer_lock);
	g_free(gpdata->cover);
	g_free(gpdata);
}

static void remmina_plugin_applehp_init(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginApplehpData *gpdata = g_new0(RemminaPluginApplehpData, 1);

	g_object_set_data_full(G_OBJECT(gp), "plugin-data", gpdata, plugin_data_free);
	gpdata->gp = gp;
	gpdata->density = 1;
	gpdata->first_credentials = TRUE;
	g_mutex_init(&gpdata->buffer_lock);
	gpdata->pressed = g_hash_table_new(g_direct_hash, g_direct_equal);

	gpdata->drawing_area = gtk_drawing_area_new();
	gtk_widget_show(gpdata->drawing_area);
	gtk_container_add(GTK_CONTAINER(gp), gpdata->drawing_area);
	gtk_widget_add_events(gpdata->drawing_area,
			      GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
			      | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
	gtk_widget_set_can_focus(gpdata->drawing_area, TRUE);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "draw", G_CALLBACK(on_draw), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "size-allocate", G_CALLBACK(on_size_allocate), gp);
}

static gboolean remmina_plugin_applehp_open_connection(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	const gchar *codec = remmina_plugin_service->file_get_string(remminafile, "codec");
	ArdSessionCallbacks callbacks = {
		on_credentials, on_connected, on_display, on_covered, on_cursor, on_clipboard, on_closed,
		pixels_lock, pixels_unlock, gp
	};
	ArdSessionParams params = { 0 };
	gchar *host = NULL;
	gint width, height;

	remmina_plugin_service->get_server_port(remmina_plugin_service->file_get_string(remminafile, "server"),
						APPLEHP_DEFAULT_PORT, &host, &params.port);
	if (!host || !*host) {
		g_free(host);
		remmina_plugin_service->protocol_plugin_set_error(gp, _("No server to connect to."));
		return FALSE;
	}
	params.host = host;
	params.codec = g_strcmp0(codec, "h264") == 0 ? ARD_CODEC_H264 : ARD_CODEC_HEVC;
	params.sound = !remmina_plugin_service->file_get_int(remminafile, "disableaudio", FALSE);
	params.density = density_of(gpdata);
	width = remmina_plugin_service->get_profile_remote_width(gp);
	height = remmina_plugin_service->get_profile_remote_height(gp);
	if (remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp) == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES) {
		GtkAllocation a;

		gtk_widget_get_allocation(GTK_WIDGET(gp), &a);
		if (a.width > 1 && a.height > 1) {
			width = a.width;
			height = a.height;
		}
	}
	params.width = width > 0 ? MIN(width, G_MAXUINT16) : DEFAULT_WIDTH;
	params.height = height > 0 ? MIN(height, G_MAXUINT16) : DEFAULT_HEIGHT;
	remmina_plugin_service->protocol_plugin_set_width(gp, params.width);
	remmina_plugin_service->protocol_plugin_set_height(gp, params.height);

	remmina_plugin_service->protocol_plugin_register_hostkey(gp, gpdata->drawing_area);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "motion-notify-event", G_CALLBACK(on_motion), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "button-press-event", G_CALLBACK(on_button), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "button-release-event", G_CALLBACK(on_button), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "scroll-event", G_CALLBACK(on_scroll), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "key-press-event", G_CALLBACK(on_key), gp);
	g_signal_connect(G_OBJECT(gpdata->drawing_area), "key-release-event", G_CALLBACK(on_key), gp);
	if (!remmina_plugin_service->file_get_int(remminafile, "disableclipboard", FALSE))
		gpdata->clipboard_handler = g_signal_connect(G_OBJECT(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)),
							     "owner-change", G_CALLBACK(on_clipboard_owner), gp);

	REMMINA_PLUGIN_AUDIT(_("Connected to %s:%d via Apple Screen Sharing"), host, params.port);
	gpdata->session = ard_session_start(&params, &callbacks);
	g_free(host);
	return TRUE;
}

static gboolean remmina_plugin_applehp_close_connection(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	start_teardown(gp);
	return FALSE;
}

static gboolean remmina_plugin_applehp_query_feature(RemminaProtocolWidget *gp, const RemminaProtocolFeature *feature)
{
	TRACE_CALL(__func__);
	return TRUE;
}

static void remmina_plugin_applehp_call_feature(RemminaProtocolWidget *gp, const RemminaProtocolFeature *feature)
{
	TRACE_CALL(__func__);
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	/* Remmina's window can send features, unfocus among them, before the
	 * plugin's init has run. */
	if (!gpdata)
		return;
	switch (feature->id) {
	case APPLEHP_FEATURE_SCALE:
	case APPLEHP_FEATURE_DYNRESUPDATE:
		update_scale(gp);
		if (remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp) == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES)
			schedule_resize(gp);
		gtk_widget_queue_draw(gpdata->drawing_area);
		break;
	case APPLEHP_FEATURE_UNFOCUS:
		release_all(gp);
		break;
	case APPLEHP_FEATURE_TOOL_REFRESH:
		if (gpdata->session)
			ard_session_refresh(gpdata->session);
		break;
	default:
		break;
	}
}

static void remmina_plugin_applehp_keystroke(RemminaProtocolWidget *gp, const guint keystrokes[], const gint keylen)
{
	TRACE_CALL(__func__);
	RemminaPluginApplehpData *gpdata = GET_PLUGIN_DATA(gp);

	if (!gpdata)
		return;
	remmina_plugin_service->protocol_plugin_send_keys_signals(gpdata->drawing_area, keystrokes, keylen,
								  GDK_KEY_PRESS | GDK_KEY_RELEASE);
}

static gpointer codec_list[] = {
	"hevc", N_("HEVC (sharper, needs more CPU)"),
	"h264", N_("H.264"),
	NULL
};

static gchar server_tooltip[] =
	N_("The Mac's address. Turn on Screen Sharing or Remote Management in the Mac's\n"
	   "System Settings → General → Sharing. The Mac sends its picture and sound\n"
	   "over UDP to this computer, from ports 5900 and 5901 to the same ports here,\n"
	   "so a firewall or NAT between them has to let that through.");

static gchar username_tooltip[] =
	N_("A macOS account on the Mac. With Remote Management on, the account has to be\n"
	   "in its access list with at least Observe and Control.");

static gchar codec_tooltip[] =
	N_("The Mac sends HEVC in 4:4:4 colour, which keeps text sharp and is decoded in\n"
	   "software. H.264 is lighter to decode, but on a still screen it sends more data.");

static const RemminaProtocolSetting remmina_plugin_applehp_basic_settings[] = {
	{ REMMINA_PROTOCOL_SETTING_TYPE_SERVER,	  "server",	NULL,		     FALSE, "_rfb._tcp", server_tooltip,   NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_TEXT,	  "username",	N_("Username"),	     FALSE, NULL,	 username_tooltip, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_PASSWORD, "password",	N_("User password"), FALSE, NULL,	 NULL,		   NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_RESOLUTION, "resolution", NULL,		     FALSE, NULL,	 NULL,		   NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_SELECT,	  "codec",	N_("Video codec"),   FALSE, codec_list,	 codec_tooltip,	   NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_KEYMAP,	  "keymap",	NULL,		     FALSE, NULL,	 NULL,		   NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_END,	  NULL,		NULL,		     FALSE, NULL,	 NULL,		   NULL, NULL }
};

static const RemminaProtocolSetting remmina_plugin_applehp_advanced_settings[] = {
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK, "disableaudio",	         N_("Turn off sound"),		  TRUE,	 NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK, "disableclipboard",       N_("Turn off clipboard sync"),	  TRUE,	 NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK, "disablepasswordstoring", N_("Forget passwords after use"), TRUE,	 NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK, "viewonly",	         N_("View only"),		  TRUE,	 NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_END,   NULL,		         NULL,				  FALSE, NULL, NULL }
};

static const RemminaProtocolFeature remmina_plugin_applehp_features[] = {
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,	      APPLEHP_FEATURE_TOOL_REFRESH, N_("Refresh"), NULL, NULL },
	{ REMMINA_PROTOCOL_FEATURE_TYPE_SCALE,	      APPLEHP_FEATURE_SCALE,	    NULL,	   NULL, NULL },
	{ REMMINA_PROTOCOL_FEATURE_TYPE_DYNRESUPDATE, APPLEHP_FEATURE_DYNRESUPDATE, NULL,	   NULL, NULL },
	{ REMMINA_PROTOCOL_FEATURE_TYPE_UNFOCUS,      APPLEHP_FEATURE_UNFOCUS,	    NULL,	   NULL, NULL },
	{ REMMINA_PROTOCOL_FEATURE_TYPE_VIEWONLY,     APPLEHP_FEATURE_VIEWONLY,
	  GINT_TO_POINTER(REMMINA_PROTOCOL_FEATURE_PREF_CHECK), "viewonly", N_("View only") },
	{ REMMINA_PROTOCOL_FEATURE_TYPE_END,	      0,			    NULL,	   NULL, NULL }
};

static RemminaProtocolPlugin remmina_plugin_applehp = {
	REMMINA_PLUGIN_TYPE_PROTOCOL,                   // Type
	APPLEHP_PLUGIN_NAME,                            // Name
	APPLEHP_PLUGIN_DESCRIPTION,                     // Description
	GETTEXT_PACKAGE,                                // Translation domain
	APPLEHP_PLUGIN_VERSION,                         // Version number
	APPLEHP_PLUGIN_APPICON,                         // Icon for normal connection
	APPLEHP_PLUGIN_SSH_APPICON,                     // Icon for SSH connection
	remmina_plugin_applehp_basic_settings,          // Array for basic settings
	remmina_plugin_applehp_advanced_settings,       // Array for advanced settings
	/* The media stream comes over UDP from the Mac's own address, which an
	 * SSH tunnel does not carry. */
	REMMINA_PROTOCOL_SSH_SETTING_NONE,              // SSH settings type
	remmina_plugin_applehp_features,                // Array for available features
	remmina_plugin_applehp_init,                    // Plugin initialization
	remmina_plugin_applehp_open_connection,         // Plugin open connection
	remmina_plugin_applehp_close_connection,        // Plugin close connection
	remmina_plugin_applehp_query_feature,           // Query for available features
	remmina_plugin_applehp_call_feature,            // Call a feature
	remmina_plugin_applehp_keystroke,               // Send a keystroke
	NULL,                                           // No screenshot support available
	NULL,                                           // RCW map event
	NULL                                            // RCW unmap event
};

G_MODULE_EXPORT gboolean remmina_plugin_entry(RemminaPluginService *service);

gboolean remmina_plugin_entry(RemminaPluginService *service)
{
	TRACE_CALL(__func__);
	remmina_plugin_service = service;

	bindtextdomain(GETTEXT_PACKAGE, REMMINA_RUNTIME_LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

	return service->register_plugin((RemminaPlugin *)&remmina_plugin_applehp);
}

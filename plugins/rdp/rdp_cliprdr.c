/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2012-2012 Jean-Louis Dupond
 * Copyright (C) 2014-2015 Antenore Gatta, Fabio Castelli, Giovanni Panozzo
 * Copyright (C) 2016-2023 Antenore Gatta, Giovanni Panozzo
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

#include "rdp_plugin.h"
#include "rdp_cliprdr.h"
#include "rdp_event.h"

#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/cliprdr.h>
#if FREERDP_VERSION_MAJOR >= 3
#include <freerdp/utils/cliprdr_utils.h>
#include <freerdp/client/client_cliprdr_file.h>
#endif
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define CLIPBOARD_TRANSFER_WAIT_TIME 6
#define CLIPBOARD_FILE_CHUNK_SIZE (4 * 1024 * 1024)
#define CLIPBOARD_MAX_LOCKS 16

#define CB_FORMAT_HTML 0xD010
#define CB_FORMAT_PNG 0xD011
#define CB_FORMAT_JPEG 0xD012
#define CB_FORMAT_GIF 0xD013
#define CB_FORMAT_FILEDESCRIPTORW 0xD015

static char file_group_descriptorw_name[] = "FileGroupDescriptorW";

UINT32 remmina_rdp_cliprdr_get_format_from_gdkatom(GdkAtom atom)
{
	TRACE_CALL(__func__);
	UINT32 rc;
	gchar *name = gdk_atom_name(atom);
	rc = 0;
	if (g_strcmp0("UTF8_STRING", name) == 0 || g_strcmp0("text/plain;charset=utf-8", name) == 0)
		rc = CF_UNICODETEXT;
	if (g_strcmp0("TEXT", name) == 0 || g_strcmp0("text/plain", name) == 0)
		rc = CF_TEXT;
	if (g_strcmp0("text/html", name) == 0)
		rc = CB_FORMAT_HTML;
	if (g_strcmp0("image/png", name) == 0)
		rc = CB_FORMAT_PNG;
	if (g_strcmp0("image/jpeg", name) == 0)
		rc = CB_FORMAT_JPEG;
	if (g_strcmp0("image/bmp", name) == 0)
		rc = CF_DIB;
	g_free(name);
	return rc;
}

/* Never used? */
void remmina_rdp_cliprdr_get_target_types(UINT32 **formats, UINT16 *size, GdkAtom *types, int count)
{
	TRACE_CALL(__func__);
	int i;
	*size = 1;
	*formats = (UINT32 *)malloc(sizeof(UINT32) * (count + 1));

	*formats[0] = 0;
	for (i = 0; i < count; i++) {
		UINT32 format = remmina_rdp_cliprdr_get_format_from_gdkatom(types[i]);
		if (format != 0) {
			(*formats)[*size] = format;
			(*size)++;
		}
	}

	*formats = realloc(*formats, sizeof(UINT32) * (*size));
}

static UINT8 *lf2crlf(UINT8 *data, int *size)
{
	TRACE_CALL(__func__);
	UINT8 c;
	UINT8 *outbuf;
	UINT8 *out;
	UINT8 *in_end;
	UINT8 *in;
	int out_size;

	out_size = (*size) * 2 + 1;
	outbuf = (UINT8 *)malloc(out_size);
	out = outbuf;
	in = data;
	in_end = data + (*size);

	while (in < in_end) {
		c = *in++;
		if (c == '\n') {
			*out++ = '\r';
			*out++ = '\n';
		} else {
			*out++ = c;
		}
	}

	*out++ = 0;
	*size = out - outbuf;

	return outbuf;
}

static void crlf2lf(UINT8 *data, size_t *size)
{
	TRACE_CALL(__func__);
	UINT8 c;
	UINT8 *out;
	UINT8 *in;
	UINT8 *in_end;

	out = data;
	in = data;
	in_end = data + (*size);

	while (in < in_end) {
		c = *in++;
		if (c != '\r')
			*out++ = c;
	}

	*size = out - data;
}

static void remmina_rdp_cliprdr_send_file_contents_response(CliprdrClientContext *context, UINT32 streamId,
	const BYTE *data, UINT32 size, gboolean ok)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE response = { 0 };

#if FREERDP_VERSION_MAJOR >= 3
	response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
#else
	response.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
#endif
	response.streamId = streamId;
	response.cbRequested = ok ? size : 0;
	response.requestedData = ok ? data : NULL;
	context->ClientFileContentsResponse(context, &response);
}

/* cliprdr_file_context_init() takes over CliprdrClientContext::custom for itself */
static rfClipboard *remmina_rdp_cliprdr_clipboard(CliprdrClientContext *context)
{
#if FREERDP_VERSION_MAJOR >= 3
	return (rfClipboard *)cliprdr_file_context_get_context((CliprdrFileContext *)context->custom);
#else
	return (rfClipboard *)context->custom;
#endif
}

/* what the GTK thread hands the cliprdr thread for a FileGroupDescriptorW request */
typedef struct {
	GStrv	paths;
	gint	gen;
} RemminaRdpLocalSelection;

#if FREERDP_VERSION_MAJOR >= 3
/* wakes the paste waiting in remmina_rdp_cliprdr_request_data() */
static void remmina_rdp_cliprdr_signal_transfer_done(rfClipboard *clipboard)
{
	pthread_mutex_lock(&clipboard->transfer_clip_mutex);
	pthread_cond_signal(&clipboard->transfer_clip_cond);
	clipboard->srv_clip_data_wait = SCDW_NONE;
	pthread_mutex_unlock(&clipboard->transfer_clip_mutex);
}

/* top-level entries of the mounted list; g_dir_read_name() never yields "." or "..", unlike WinPR's FGD->uri-list converter */
static GStrv remmina_rdp_cliprdr_list_received_files(rfClipboard *clipboard)
{
	gchar *base_path = NULL;
	GDir *dir;
	const gchar *name;
	GPtrArray *paths;

	ClipboardLock(clipboard->system);
	if (clipboard->server_file_list_mounted)
		base_path = g_strdup(ClipboardGetDelegate(clipboard->system)->basePath);
	ClipboardUnlock(clipboard->system);

	dir = base_path ? g_dir_open(base_path, 0, NULL) : NULL;
	paths = g_ptr_array_new();
	if (dir) {
		while ((name = g_dir_read_name(dir)) != NULL)
			g_ptr_array_add(paths, g_build_filename(base_path, name, NULL));
		g_dir_close(dir);
	}
	g_free(base_path);

	if (paths->len == 0) {
		g_ptr_array_free(paths, TRUE);
		return NULL;
	}
	g_ptr_array_add(paths, NULL);
	return (GStrv)g_ptr_array_free(paths, FALSE);
}

/* as gtk-frdp lock_current_local_files(): keeps this list served under clipDataId until the server unlocks it */
static void remmina_rdp_cliprdr_lock_local_files(rfClipboard *clipboard, UINT32 clipDataId, GArray *files)
{
	if (!clipboard->locked_files)
		clipboard->locked_files = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_array_unref);

	/* every lock can pin a list of its own */
	if (g_hash_table_size(clipboard->locked_files) >= CLIPBOARD_MAX_LOCKS &&
	    !g_hash_table_contains(clipboard->locked_files, GUINT_TO_POINTER(clipDataId))) {
		REMMINA_PLUGIN_DEBUG("too many clipboard locks, ignoring clipDataId %u", clipDataId);
		return;
	}
	g_array_ref(files);
	g_hash_table_insert(clipboard->locked_files, GUINT_TO_POINTER(clipDataId), files);
}

static UINT remmina_rdp_cliprdr_server_lock_clipboard_data(CliprdrClientContext *context, const CLIPRDR_LOCK_CLIPBOARD_DATA *lockClipboardData)
{
	rfClipboard *clipboard = remmina_rdp_cliprdr_clipboard(context);

	if (clipboard->local_files_flat && clipboard->local_files_gen == g_atomic_int_get(&clipboard->local_selection_gen)) {
		remmina_rdp_cliprdr_lock_local_files(clipboard, lockClipboardData->clipDataId, clipboard->local_files_flat);
		clipboard->pending_lock = FALSE;
	} else {
		/* no list yet: lock the one built for the next FileGroupDescriptorW request, as gtk-frdp */
		clipboard->pending_lock = TRUE;
		clipboard->pending_lock_id = lockClipboardData->clipDataId;
	}
	return CHANNEL_RC_OK;
}

static UINT remmina_rdp_cliprdr_server_unlock_clipboard_data(CliprdrClientContext *context, const CLIPRDR_UNLOCK_CLIPBOARD_DATA *unlockClipboardData)
{
	rfClipboard *clipboard = remmina_rdp_cliprdr_clipboard(context);

	if (clipboard->locked_files)
		g_hash_table_remove(clipboard->locked_files, GUINT_TO_POINTER(unlockClipboardData->clipDataId));
	if (clipboard->pending_lock && clipboard->pending_lock_id == unlockClipboardData->clipDataId)
		clipboard->pending_lock = FALSE;
	return CHANNEL_RC_OK;
}
#endif

/* cliprdr channel thread, or after it has been joined */
static void remmina_rdp_cliprdr_drop_local_files(rfClipboard *clipboard)
{
	g_clear_pointer(&clipboard->local_files_flat, g_array_unref);
#if FREERDP_VERSION_MAJOR >= 3
	g_clear_pointer(&clipboard->locked_files, g_hash_table_unref);
	clipboard->pending_lock = FALSE;
#endif
}

UINT remmina_rdp_cliprdr_server_file_contents_request(CliprdrClientContext *context, const CLIPRDR_FILE_CONTENTS_REQUEST *fileContentsRequest)
{
	TRACE_CALL(__func__);
	rfClipboard *clipboard = remmina_rdp_cliprdr_clipboard(context);
	RemminaRdpOutgoingFile *entry;
	GArray *files = NULL;
	struct stat st;
	int fd = -1;

	/* MS-RDPECLIP 2.2.5.3: FILECONTENTS_SIZE and FILECONTENTS_RANGE are mutually exclusive */
	if ((fileContentsRequest->dwFlags & (FILECONTENTS_SIZE | FILECONTENTS_RANGE)) == (FILECONTENTS_SIZE | FILECONTENTS_RANGE))
		goto fail;

#if FREERDP_VERSION_MAJOR >= 3
	/* a locked list outlives the selection it was built for, as gtk-frdp */
	if (fileContentsRequest->haveClipDataId && clipboard->locked_files)
		files = g_hash_table_lookup(clipboard->locked_files, GUINT_TO_POINTER(fileContentsRequest->clipDataId));
#endif
	if (!files && clipboard->local_files_gen == g_atomic_int_get(&clipboard->local_selection_gen))
		files = clipboard->local_files_flat;
	if (!files || fileContentsRequest->listIndex >= files->len)
		goto fail;

	/* one open per request, like client_cliprdr_file.c; below the user's selection never through a symlink */
	entry = &g_array_index(files, RemminaRdpOutgoingFile, fileContentsRequest->listIndex);
	fd = open(entry->abs_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (entry->top_level ? 0 : O_NOFOLLOW));
	if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		goto fail;

	if (fileContentsRequest->dwFlags & FILECONTENTS_SIZE) {
		guint64 sizeLe = GUINT64_TO_LE((guint64)st.st_size);
		close(fd);
		remmina_rdp_cliprdr_send_file_contents_response(context, fileContentsRequest->streamId, (BYTE *)&sizeLe, 8, TRUE);
		return CHANNEL_RC_OK;
	}

	if (fileContentsRequest->dwFlags & FILECONTENTS_RANGE) {
		guint64 offset = ((guint64)fileContentsRequest->nPositionHigh << 32) | fileContentsRequest->nPositionLow;
		size_t toRead = MIN(fileContentsRequest->cbRequested, CLIPBOARD_FILE_CHUNK_SIZE);
		BYTE *buf = (BYTE *)malloc(toRead ? toRead : 1);
		size_t got = 0;
		ssize_t r = 0;

		if (buf && offset <= G_MAXINT64) {
			while (got < toRead && (r = pread(fd, buf + got, toRead - got, (off_t)(offset + got))) > 0)
				got += (size_t)r;
			if (r >= 0) {
				close(fd);
				remmina_rdp_cliprdr_send_file_contents_response(context, fileContentsRequest->streamId, buf, (UINT32)got, TRUE);
				free(buf);
				return CHANNEL_RC_OK;
			}
		}
		free(buf);
	}

fail:
	if (fd >= 0)
		close(fd);
	remmina_rdp_cliprdr_send_file_contents_response(context, fileContentsRequest->streamId, NULL, 0, FALSE);
	return CHANNEL_RC_OK;
}

void remmina_rdp_cliprdr_send_client_format_list(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginRdpUiObject *ui;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	rfClipboard *clipboard;
	CLIPRDR_FORMAT_LIST *pFormatList;
	RemminaPluginRdpEvent rdp_event = { 0 };

	if (!rfi || !rfi->connected || rfi->is_reconnecting)
		return;

	clipboard = &(rfi->clipboard);

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.clipboard = clipboard;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_FORMATLIST;
	pFormatList = remmina_rdp_event_queue_ui_sync_retptr(gp, ui);

	rdp_event.type = REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_LIST;
	rdp_event.clipboard_formatlist.pFormatList = pFormatList;
	remmina_rdp_event_event_push(gp, &rdp_event);
}

static void remmina_rdp_cliprdr_send_client_capabilities(rfClipboard *clipboard)
{
	TRACE_CALL(__func__);
	CLIPRDR_CAPABILITIES capabilities;
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet;

	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = (CLIPRDR_CAPABILITY_SET *)&(generalCapabilitySet);

	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;

	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES;
#if FREERDP_VERSION_MAJOR >= 3
	/* as xf_cliprdr.c; locking keeps a list served after the selection changes, unlocked lists stop being served */
	generalCapabilitySet.generalFlags |= cliprdr_file_context_current_flags(clipboard->file_context);
	if ((generalCapabilitySet.generalFlags & CB_STREAM_FILECLIP_ENABLED) &&
	    (cliprdr_file_context_remote_get_flags(clipboard->file_context) & CB_CAN_LOCK_CLIPDATA))
		generalCapabilitySet.generalFlags |= CB_CAN_LOCK_CLIPDATA;
#endif

	clipboard->context->ClientCapabilities(clipboard->context, &capabilities);
}

static UINT remmina_rdp_cliprdr_monitor_ready(CliprdrClientContext *context, const CLIPRDR_MONITOR_READY *monitorReady)
{
	TRACE_CALL(__func__);
	rfClipboard *clipboard = remmina_rdp_cliprdr_clipboard(context);
	RemminaProtocolWidget *gp;

	/* locks are session scoped */
	remmina_rdp_cliprdr_drop_local_files(clipboard);
	remmina_rdp_cliprdr_send_client_capabilities(clipboard);
	gp = clipboard->rfi->protocol_widget;
	remmina_rdp_cliprdr_send_client_format_list(gp);

	return CHANNEL_RC_OK;
}

static UINT remmina_rdp_cliprdr_server_capabilities(CliprdrClientContext *context, const CLIPRDR_CAPABILITIES *capabilities)
{
	TRACE_CALL(__func__);
#if FREERDP_VERSION_MAJOR >= 3
	rfClipboard *clipboard = remmina_rdp_cliprdr_clipboard(context);
	const BYTE *capsPtr = (const BYTE *)capabilities->capabilitySets;
	UINT32 i;

	if (!cliprdr_file_context_remote_set_flags(clipboard->file_context, 0))
		return ERROR_INTERNAL_ERROR;

	for (i = 0; i < capabilities->cCapabilitiesSets; i++) {
		const CLIPRDR_CAPABILITY_SET *caps = (const CLIPRDR_CAPABILITY_SET *)capsPtr;

		if (caps->capabilitySetType == CB_CAPSTYPE_GENERAL) {
			const CLIPRDR_GENERAL_CAPABILITY_SET *generalCaps = (const CLIPRDR_GENERAL_CAPABILITY_SET *)caps;
			if (!cliprdr_file_context_remote_set_flags(clipboard->file_context, generalCaps->generalFlags))
				return ERROR_INTERNAL_ERROR;
		}
		capsPtr += caps->capabilitySetLength;
	}
#endif
	return CHANNEL_RC_OK;
}


void remmina_rdp_cliprdr_cached_clipboard_free(rfClipboard *clipboard)
{
	TRACE_CALL(__func__);
	guint fmt;

	pthread_mutex_lock(&clipboard->srv_data_mutex);
	if (clipboard->srv_data != NULL) {
		fmt = clipboard->format;
		if (fmt == CB_FORMAT_PNG || fmt == CF_DIB || fmt == CF_DIBV5 || fmt == CB_FORMAT_JPEG) {
			g_object_unref(clipboard->srv_data);
		} else if (clipboard->server_filedescriptorw_format_id != 0 && fmt == clipboard->server_filedescriptorw_format_id) {
			g_strfreev((GStrv)clipboard->srv_data);
		} else {
			free(clipboard->srv_data);
		}
		clipboard->srv_data = NULL;
	}
	pthread_mutex_unlock(&clipboard->srv_data_mutex);
}


static UINT remmina_rdp_cliprdr_server_format_list(CliprdrClientContext *context, const CLIPRDR_FORMAT_LIST *formatList)
{
	TRACE_CALL(__func__);

	/* Called when a user do a "Copy" on the server: we collect all formats
	 * the server send us and then setup the local clipboard with the appropriate
	 * targets to request server data */

	RemminaPluginRdpUiObject *ui;
	RemminaProtocolWidget *gp;
	rfClipboard *clipboard;
	CLIPRDR_FORMAT *format;
	CLIPRDR_FORMAT_LIST_RESPONSE formatListResponse;
	UINT rc;

	int has_dib_level = 0;

	int i;

	clipboard = remmina_rdp_cliprdr_clipboard(context);
	gp = clipboard->rfi->protocol_widget;

	REMMINA_PLUGIN_DEBUG("gp=%p: Received a new ServerFormatList from server clipboard. Remmina version = %s",
						 gp, VERSION);

	GtkTargetList *list = gtk_target_list_new(NULL, 0);

	if (clipboard->srv_clip_data_wait == SCDW_BUSY_WAIT) {
		REMMINA_PLUGIN_DEBUG("gp=%p: we already have a FormatDataRequest in progress to the server, aborting", gp);
		remmina_rdp_clipboard_abort_client_format_data_request(clipboard->rfi);
	}

	remmina_rdp_cliprdr_cached_clipboard_free(clipboard);
	clipboard->server_html_format_id = 0;
	clipboard->server_filedescriptorw_format_id = 0;

#if FREERDP_VERSION_MAJOR >= 3
	/* the server owns the clipboard now: our outgoing list and any mounted incoming list are stale, locked lists stay */
	g_clear_pointer(&clipboard->local_files_flat, g_array_unref);
	clipboard->pending_lock = FALSE;
	ClipboardLock(clipboard->system);
	clipboard->server_file_list_mounted = FALSE;
	rc = cliprdr_file_context_notify_new_server_format_list(clipboard->file_context);
	ClipboardUnlock(clipboard->system);
	if (rc != CHANNEL_RC_OK) {
		gtk_target_list_unref(list);
		return rc;
	}
#endif

	REMMINA_PLUGIN_DEBUG("gp=%p: format list from the server:", gp);
	for (i = 0; i < formatList->numFormats; i++) {
		format = &formatList->formats[i];
		const char *serverFormatName = format->formatName;
		gchar *gtkFormatName = NULL;
		if (format->formatId == CF_UNICODETEXT) {
			serverFormatName = "CF_UNICODETEXT";
			gtkFormatName = "text/plain;charset=utf-8";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, CF_UNICODETEXT);
			/* Add also the older UTF8_STRING format for older applications */
			atom = gdk_atom_intern("UTF8_STRING", TRUE);
			gtk_target_list_add(list, atom, 0, CF_UNICODETEXT);
		} else if (format->formatId == CF_TEXT) {
			serverFormatName = "CF_TEXT";
			gtkFormatName = "text/plain";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, CF_TEXT);
			/* Add also the older TEXT format for older applications */
			atom = gdk_atom_intern("TEXT", TRUE);
			gtk_target_list_add(list, atom, 0, CF_TEXT);
		} else if (format->formatId == CF_DIB) {
			serverFormatName = "CF_DIB";
			if (has_dib_level < 1)
				has_dib_level = 1;
		} else if (format->formatId == CF_DIBV5) {
			serverFormatName = "CF_DIBV5";
			if (has_dib_level < 5)
				has_dib_level = 5;
		} else if (format->formatId == CB_FORMAT_JPEG) {
			serverFormatName = "CB_FORMAT_JPEG";
			gtkFormatName = "image/jpeg";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_JPEG);
		} else if (format->formatId == CB_FORMAT_PNG) {
			serverFormatName = "CB_FORMAT_PNG";
			gtkFormatName = "image/png";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_PNG);
		} else if (format->formatId == CB_FORMAT_HTML) {
			serverFormatName = "CB_FORMAT_HTML";
			gtkFormatName = "text/html";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, CB_FORMAT_HTML);
		} else if (format->formatId == CF_LOCALE) {
			serverFormatName = "CF_LOCALE";
		} else if (format->formatId == CF_METAFILEPICT) {
			serverFormatName = "CF_METAFILEPICT";
		} else if (serverFormatName != NULL && strcmp(serverFormatName, "HTML Format") == 0) {
			gtkFormatName = "text/html";
			GdkAtom atom = gdk_atom_intern(gtkFormatName, TRUE);
			gtk_target_list_add(list, atom, 0, format->formatId);
			clipboard->server_html_format_id = format->formatId;
#if FREERDP_VERSION_MAJOR >= 3
		} else if (serverFormatName != NULL && strcmp(serverFormatName, "FileGroupDescriptorW") == 0 &&
			   cliprdr_file_context_has_local_support(clipboard->file_context)) {
			gtkFormatName = "text/uri-list";
			gtk_target_list_add(list, gdk_atom_intern("text/uri-list", TRUE), 0, format->formatId);
			gtk_target_list_add(list, gdk_atom_intern("x-special/gnome-copied-files", TRUE), 0, format->formatId);
			gtk_target_list_add(list, gdk_atom_intern("x-special/mate-copied-files", TRUE), 0, format->formatId);
			clipboard->server_filedescriptorw_format_id = format->formatId;
#endif
		}
		REMMINA_PLUGIN_DEBUG("the server has clipboard format %d: %s -> GTK %s", format->formatId, serverFormatName, gtkFormatName);
	}

	/* Keep only one DIB format, if present */
	if (has_dib_level) {
		GdkAtom atom = gdk_atom_intern("image/bmp", TRUE);
		if (has_dib_level == 5)
			gtk_target_list_add(list, atom, 0, CF_DIBV5);
		else
			gtk_target_list_add(list, atom, 0, CF_DIB);
	}


	REMMINA_PLUGIN_DEBUG("gp=%p: sending ClientFormatListResponse to server", gp);
#if FREERDP_VERSION_MAJOR >= 3
	formatListResponse.common.msgType = CB_FORMAT_LIST_RESPONSE;
	formatListResponse.common.msgFlags = CB_RESPONSE_OK; // Can be CB_RESPONSE_FAIL in case of error
	formatListResponse.common.dataLen = 0;
#else
	formatListResponse.msgType = CB_FORMAT_LIST_RESPONSE;
	formatListResponse.msgFlags = CB_RESPONSE_OK; // Can be CB_RESPONSE_FAIL in case of error
	formatListResponse.dataLen = 0;
#endif
	rc = clipboard->context->ClientFormatListResponse(clipboard->context, &formatListResponse);
	/* Schedule GTK event to tell GTK to change the local clipboard calling gtk_clipboard_set_with_owner
	 * via REMMINA_RDP_UI_CLIPBOARD_SET_DATA
	 * GTK will immediately fire an "owner-change" event, that we should ignore.
	 * And if we are putting text on the clipboard, mutter or other clipboard
	 * managers may try immediately request a clipboard transfer. */

	/* Ensure we have at least one target into list, or Gtk will not change owner
	  when setting clipboard owner later.
	  We put it directly in the clipboard cache (clipboard->srv_data),
	  so remmina will never ask it to the server via ClientFormatDataRequest */
	gint n_targets;
	GtkTargetEntry *target_table = gtk_target_table_new_from_list(list, &n_targets);
	if (target_table)
		gtk_target_table_free(target_table, n_targets);
	if (n_targets == 0) {
		REMMINA_PLUGIN_DEBUG("gp=%p adding a dummy text target (empty text) for local clipboard, because we have no interesting targets from the server. Putting it in the local clipboard cache.");
		GdkAtom atom = gdk_atom_intern("text/plain;charset=utf-8", TRUE);
		gtk_target_list_add(list, atom, 0, CF_UNICODETEXT);
		pthread_mutex_lock(&clipboard->srv_data_mutex);
		clipboard->srv_data = malloc(1);
		((char *)(clipboard->srv_data))[0] = 0;
		pthread_mutex_unlock(&clipboard->srv_data_mutex);
		clipboard->format = CF_UNICODETEXT;
	}


	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.clipboard = clipboard;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_SET_DATA;
	ui->clipboard.targetlist = list;
	remmina_rdp_event_queue_ui_async(gp, ui);

	REMMINA_PLUGIN_DEBUG("gp=%p: processing of ServerFormatList ended, returning rc=%u to libfreerdp", gp, rc);
	return rc;
}

static UINT remmina_rdp_cliprdr_server_format_list_response(CliprdrClientContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *formatListResponse)
{
	TRACE_CALL(__func__);
	return CHANNEL_RC_OK;
}

#if FREERDP_VERSION_MAJOR >= 3
static void remmina_rdp_cliprdr_send_local_file_list(rfClipboard *clipboard, RemminaProtocolWidget *gp, RemminaRdpLocalSelection *selection);
#endif

static UINT remmina_rdp_cliprdr_server_format_data_request(CliprdrClientContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *formatDataRequest)
{
	TRACE_CALL(__func__);

	RemminaPluginRdpUiObject *ui;
	RemminaProtocolWidget *gp;
	rfClipboard *clipboard;

	clipboard = remmina_rdp_cliprdr_clipboard(context);
	gp = clipboard->rfi->protocol_widget;

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CLIPBOARD;
	ui->clipboard.clipboard = clipboard;
	ui->clipboard.type = REMMINA_RDP_UI_CLIPBOARD_GET_DATA;
	ui->clipboard.format = formatDataRequest->requestedFormatId;
#if FREERDP_VERSION_MAJOR >= 3
	if (formatDataRequest->requestedFormatId == CB_FORMAT_FILEDESCRIPTORW) {
		remmina_rdp_cliprdr_send_local_file_list(clipboard, gp, remmina_rdp_event_queue_ui_sync_retptr(gp, ui));
		return CHANNEL_RC_OK;
	}
#endif
	remmina_rdp_event_queue_ui_sync_retint(gp, ui);

	return CHANNEL_RC_OK;
}

int timeval_diff(struct timeval *start, struct timeval *end)
{
	/* Returns the time elapsed from start to end, in ms */
	int64_t ms_end = (end->tv_sec * 1000) + (end->tv_usec / 1000);
	int64_t ms_start = (start->tv_sec * 1000) + (start->tv_usec / 1000);

	return (int)(ms_end - ms_start);
}


static UINT remmina_rdp_cliprdr_server_format_data_response(CliprdrClientContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *formatDataResponse)
{
	TRACE_CALL(__func__);

	const UINT8 *data;
	size_t size;
	gboolean ok;
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	rfClipboard *clipboard;
	gpointer output = NULL;
	struct timeval now;
	int mstrans;

	clipboard = remmina_rdp_cliprdr_clipboard(context);
	gp = clipboard->rfi->protocol_widget;
	rfi = GET_PLUGIN_DATA(gp);

	data = formatDataResponse->requestedFormatData;
#if FREERDP_VERSION_MAJOR >= 3
	size = formatDataResponse->common.dataLen;
	ok = (formatDataResponse->common.msgFlags & CB_RESPONSE_OK) != 0;
#else
	size = formatDataResponse->dataLen;
	ok = (formatDataResponse->msgFlags & CB_RESPONSE_OK) != 0;
#endif
	if (!ok) {
		REMMINA_PLUGIN_DEBUG("gp=%p server FormatDataResponse: CB_RESPONSE_FAIL, discarding", gp);
		size = 0;
	}

	REMMINA_PLUGIN_DEBUG("gp=%p server FormatDataResponse received: clipboard data arrived form server.", gp);
	gettimeofday(&now, NULL);
	remmina_rdp_cliprdr_cached_clipboard_free(clipboard);

	/* Calculate stats */
	mstrans = timeval_diff(&(clipboard->clientformatdatarequest_tv), &now);
	REMMINA_PLUGIN_DEBUG("gp=%p %zu bytes transferred from server in %d ms. Speed is %d bytes/sec",
		gp, (size_t)size, mstrans, mstrans != 0 ? (int)((int64_t)size * 1000 / mstrans) : 0);

#if FREERDP_VERSION_MAJOR >= 3
	if (clipboard->server_filedescriptorw_format_id != 0 && rfi->clipboard.format == clipboard->server_filedescriptorw_format_id) {
		GStrv paths = NULL;

		ClipboardLock(clipboard->system);
		clipboard->server_file_list_mounted = size > 0 &&
			cliprdr_file_context_update_server_data(clipboard->file_context, clipboard->system, data, size);
		ClipboardUnlock(clipboard->system);

		paths = remmina_rdp_cliprdr_list_received_files(clipboard);
		if (!paths) {
			g_warning("[RDP] rdp_cliprdr: failed to mount the server file list over FUSE (size=%zu), discarding", size);
			remmina_rdp_cliprdr_signal_transfer_done(clipboard);
			return CHANNEL_RC_OK;
		}
		REMMINA_PLUGIN_DEBUG("gp=%p: server file list mounted over FUSE", gp);

		pthread_mutex_lock(&clipboard->srv_data_mutex);
		clipboard->srv_data = paths;
		pthread_mutex_unlock(&clipboard->srv_data_mutex);
		remmina_rdp_cliprdr_signal_transfer_done(clipboard);
		return CHANNEL_RC_OK;
	}
#endif

	if (size > 0) {
		switch (rfi->clipboard.format) {
		case CF_UNICODETEXT:
		{
			output =
			g_utf16_to_utf8((const WCHAR *)data, size / sizeof(WCHAR), NULL, NULL, NULL);
			if (output) {
				size = strlen(output) + 1;
				crlf2lf(output, &size);
			}
			break;
		}

		case CF_TEXT:
		case CB_FORMAT_HTML:
		{
			output = (gpointer)calloc(1, size + 1);
			if (output) {
				memcpy(output, data, size);
				crlf2lf(output, &size);
			}
			break;
		}

		case CF_DIBV5:
		case CF_DIB:
		{
			wStream *s;
			UINT32 offset;
			GError *perr;
			BITMAPINFOHEADER *pbi;
			BITMAPV5HEADER *pbi5;

			pbi = (BITMAPINFOHEADER *)data;

			// offset calculation inspired by http://downloads.poolelan.com/MSDN/MSDNLibrary6/Disk1/Samples/VC/OS/WindowsXP/GetImage/BitmapUtil.cpp
			offset = 14 + pbi->biSize;
			if (pbi->biClrUsed != 0)
				offset += sizeof(RGBQUAD) * pbi->biClrUsed;
			else if (pbi->biBitCount <= 8)
				offset += sizeof(RGBQUAD) * (1 << pbi->biBitCount);
			if (pbi->biSize == sizeof(BITMAPINFOHEADER)) {
				if (pbi->biCompression == 3)         // BI_BITFIELDS is 3
					offset += 12;
			} else if (pbi->biSize >= sizeof(BITMAPV5HEADER)) {
				pbi5 = (BITMAPV5HEADER *)pbi;
				if (pbi5->bV5ProfileData <= offset)
					offset += pbi5->bV5ProfileSize;
			}
			s = Stream_New(NULL, 14 + size);
			Stream_Write_UINT8(s, 'B');
			Stream_Write_UINT8(s, 'M');
			Stream_Write_UINT32(s, 14 + size);
			Stream_Write_UINT32(s, 0);
			Stream_Write_UINT32(s, offset);
			Stream_Write(s, data, size);

			data = Stream_Buffer(s);
			size = Stream_Length(s);

			GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
			perr = NULL;
			if (!gdk_pixbuf_loader_write(loader, data, size, &perr)) {
				Stream_Free(s, TRUE);
				g_warning("[RDP] rdp_cliprdr: gdk_pixbuf_loader_write() returned error %s\n", perr->message);
			} else {
				if (!gdk_pixbuf_loader_close(loader, &perr)) {
					g_warning("[RDP] rdp_cliprdr: gdk_pixbuf_loader_close() returned error %s\n", perr->message);
					perr = NULL;
				}
				Stream_Free(s, TRUE);
				output = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
			}
			g_object_unref(loader);
			break;
		}

		case CB_FORMAT_PNG:
		case CB_FORMAT_JPEG:
		{
			GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
			gdk_pixbuf_loader_write(loader, data, size, NULL);
			output = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
			gdk_pixbuf_loader_close(loader, NULL);
			g_object_unref(loader);
			break;
		}
		default:
		{
			if (rfi->clipboard.format == clipboard->server_html_format_id) {
				/* Converting from Microsoft HTML Clipboard Format to pure text/html
				 * https://docs.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format */
				int p = 0, lstart = 0;
				size_t osize;
				char c;
				/* Find the 1st starting with '<' */
				while(p < size && (c = data[p]) != 0) {
					if (c == '<' && p == lstart) break;
					if (c == '\n') lstart = p + 1;
					p++;
				}
				if (p < size) {
					osize = size - lstart;
					output = (gpointer)calloc(1, osize + 1);
					if (output) {
						memcpy(output, data + lstart, osize);
						((char *)output)[osize] = 0;
					}
				}
			}
		}
		}
	}

	pthread_mutex_lock(&clipboard->srv_data_mutex);
	clipboard->srv_data = output;
	pthread_mutex_unlock(&clipboard->srv_data_mutex);

	if (output != NULL)
		REMMINA_PLUGIN_DEBUG("gp=%p: clipboard local cache data has been loaded", gp);
	else
		REMMINA_PLUGIN_DEBUG("gp=%p: data from server is not valid (size=%zu format=%d), cannot load into cache", gp, size, rfi->clipboard.format);


	REMMINA_PLUGIN_DEBUG("gp=%p: signalling main GTK thread that we have some clipboard data.", gp);

	pthread_mutex_lock(&clipboard->transfer_clip_mutex);
	pthread_cond_signal(&clipboard->transfer_clip_cond);
	if (clipboard->srv_clip_data_wait == SCDW_BUSY_WAIT) {
		REMMINA_PLUGIN_DEBUG("gp=%p: clipboard transfer from server completed.", gp);
	} else {
		// Clipboard data arrived from server when we are not busy waiting on main loop
		// Unfortunately, we must discard it
		REMMINA_PLUGIN_DEBUG("gp=%p: clipboard transfer from server completed, but no local application is requesting it. Data is on local cache now, try to paste later.", gp);
	}
	clipboard->srv_clip_data_wait = SCDW_NONE;
	pthread_mutex_unlock(&clipboard->transfer_clip_mutex);

	return CHANNEL_RC_OK;
}


void remmina_rdp_cliprdr_request_data(GtkClipboard *gtkClipboard, GtkSelectionData *selection_data, guint info, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	/* Called by GTK when someone presses "Paste" on the client side.
	 * We ask to the server the data we need */

	CLIPRDR_FORMAT_DATA_REQUEST *pFormatDataRequest;
	rfClipboard *clipboard;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	RemminaPluginRdpEvent rdp_event = { 0 };
	struct timespec to;
	struct timeval tv;
	int rc;
	time_t tlimit, tlimit1s, tnow, tstart;

	REMMINA_PLUGIN_DEBUG("gp=%p: A local application has requested remote clipboard data for remote format id %d", gp, info);

	clipboard = &(rfi->clipboard);
	if (clipboard->srv_clip_data_wait != SCDW_NONE) {
		g_message("[RDP] Cannot paste now, I’m already transferring clipboard data from server. Try again later\n");
		return;
	}

#if FREERDP_VERSION_MAJOR >= 3
	if (info == clipboard->server_filedescriptorw_format_id && (clipboard->format != info || clipboard->srv_data == NULL)) {
		/* mounted already for this server format list: list it again rather than re-request (xf_cliprdr.c caches per format list) */
		GStrv mounted = remmina_rdp_cliprdr_list_received_files(clipboard);
		if (mounted) {
			remmina_rdp_cliprdr_cached_clipboard_free(clipboard);
			pthread_mutex_lock(&clipboard->srv_data_mutex);
			clipboard->srv_data = mounted;
			pthread_mutex_unlock(&clipboard->srv_data_mutex);
			clipboard->format = info;
		}
	}
#endif

	if (clipboard->format != info || clipboard->srv_data == NULL) {
		/* We do not have a local cached clipboard, so we have to start a remote request */
		remmina_rdp_cliprdr_cached_clipboard_free(clipboard);

		clipboard->format = info;

		pthread_mutex_lock(&clipboard->transfer_clip_mutex);

		pFormatDataRequest = (CLIPRDR_FORMAT_DATA_REQUEST *)malloc(sizeof(CLIPRDR_FORMAT_DATA_REQUEST));
		ZeroMemory(pFormatDataRequest, sizeof(CLIPRDR_FORMAT_DATA_REQUEST));
		pFormatDataRequest->requestedFormatId = clipboard->format;

		clipboard->srv_clip_data_wait = SCDW_BUSY_WAIT;	// Annotate that we are waiting for ServerFormatDataResponse

		REMMINA_PLUGIN_DEBUG("gp=%p Requesting clipboard data with format %d from the server via ServerFormatDataRequest", gp, clipboard->format);
		rdp_event.type = REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_DATA_REQUEST;
		rdp_event.clipboard_formatdatarequest.pFormatDataRequest = pFormatDataRequest;
		remmina_rdp_event_event_push(gp, &rdp_event);

		/* Busy wait clipboard data for CLIPBOARD_TRANSFER_WAIT_TIME seconds.
		*      In the meanwhile allow GTK event loop to proceed */

		tstart = time(NULL);
		tlimit = tstart + CLIPBOARD_TRANSFER_WAIT_TIME;
		rc = 100000;
		tlimit1s = tstart + 1;
		while ((tnow = time(NULL)) < tlimit && rc != 0 && clipboard->srv_clip_data_wait == SCDW_BUSY_WAIT) {

			if (tnow >= tlimit1s) {
				REMMINA_PLUGIN_DEBUG("gp=%p, clipboard data is still not here after %u seconds", gp, (unsigned)(tnow - tstart));
				tlimit1s = time(NULL) + 1;
			}

			gettimeofday(&tv, NULL);
			to.tv_sec = tv.tv_sec;
			to.tv_nsec = tv.tv_usec * 1000 + 5000000;      // wait for data for 5ms
			if (to.tv_nsec >= 1000000000) {
				to.tv_nsec -= 1000000000;
				to.tv_sec++;
			}
			rc = pthread_cond_timedwait(&clipboard->transfer_clip_cond, &clipboard->transfer_clip_mutex, &to);
			if (rc == 0)
				break;

			gtk_main_iteration_do(FALSE);
			if (GET_PLUGIN_DATA(gp) == NULL)
				return;	/* disconnected mid-wait: clipboard/mutex may already be freed */
		}

		if (rc != 0) {
			/* Timeout, just log it and hope that data will arrive later */
			if (clipboard->srv_clip_data_wait == SCDW_ABORTING) {
				g_warning("[RDP] gp=%p Clipboard data wait aborted.",gp);
			} else {
				if (rc == ETIMEDOUT)
					g_warning("[RDP] gp=%p Clipboard data from the server is not available in %d seconds. No data will be available to user.",
						gp, CLIPBOARD_TRANSFER_WAIT_TIME);
				else
					g_warning("[RDP] gp=%p internal error: pthread_cond_timedwait() returned %d\n", gp, rc);
			}
		}
		pthread_mutex_unlock(&clipboard->transfer_clip_mutex);

	}

	pthread_mutex_lock(&clipboard->srv_data_mutex);
	if (clipboard->srv_data != NULL) {
		REMMINA_PLUGIN_DEBUG("gp=%p pasting data to local application", gp);
		/* We have data in cache, just paste it */
		if (info == CB_FORMAT_PNG || info == CF_DIB || info == CF_DIBV5 || info == CB_FORMAT_JPEG) {
			gtk_selection_data_set_pixbuf(selection_data, clipboard->srv_data);
		} else if (info == CB_FORMAT_HTML || info == clipboard->server_html_format_id) {
			REMMINA_PLUGIN_DEBUG("gp=%p returning %zu bytes of HTML in clipboard to requesting application", gp, strlen(clipboard->srv_data));
			GdkAtom atom = gdk_atom_intern("text/html", TRUE);
			gtk_selection_data_set(selection_data, atom, 8, clipboard->srv_data, strlen(clipboard->srv_data));
		} else if (info == clipboard->server_filedescriptorw_format_id) {
			GStrv paths = (GStrv)clipboard->srv_data;
			GdkAtom target = gtk_selection_data_get_target(selection_data);
			gchar *targetName = gdk_atom_name(target);
			guint count = g_strv_length(paths);
			gchar **uris = g_new0(gchar *, count + 1);
			guint i;

			for (i = 0; i < count; i++)
				uris[i] = g_filename_to_uri(paths[i], NULL, NULL);

			if (g_strcmp0(targetName, "text/uri-list") == 0) {
				gtk_selection_data_set_uris(selection_data, uris);
			} else {
				/* as WinPR's convert_filedescriptors_to_{gnome,mate}_copied_files: the gnome length counts the NUL, mate's doesn't */
				gchar *joined = g_strjoinv("\n", uris);
				gchar *body = g_strconcat("copy\n", joined, NULL);
				gtk_selection_data_set(selection_data, target, 8, (const guchar *)body,
					strlen(body) + (g_strcmp0(targetName, "x-special/gnome-copied-files") == 0 ? 1 : 0));
				g_free(joined);
				g_free(body);
			}
			g_strfreev(uris);
			g_free(targetName);
		} else {
			REMMINA_PLUGIN_DEBUG("gp=%p returning %zu bytes of text in clipboard to requesting application", gp, strlen(clipboard->srv_data));
			gtk_selection_data_set_text(selection_data, clipboard->srv_data, -1);
		}
	} else {
		REMMINA_PLUGIN_DEBUG("gp=%p cannot paste data to local application because ->srv_data is NULL", gp);
	}
	pthread_mutex_unlock(&clipboard->srv_data_mutex);

}

void remmina_rdp_cliprdr_empty_clipboard(GtkClipboard *gtkClipboard, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	rfContext *rfi = GET_PLUGIN_DATA(gp);

#if FREERDP_VERSION_MAJOR >= 3
	if (rfi && rfi->clipboard.file_context) {
		ClipboardLock(rfi->clipboard.system);
		cliprdr_file_context_clear(rfi->clipboard.file_context);
		rfi->clipboard.server_file_list_mounted = FALSE;
		ClipboardUnlock(rfi->clipboard.system);
	}
#endif
}

static GStrv remmina_rdp_cliprdr_local_file_paths(GtkClipboard *gtkClipboard)
{
	TRACE_CALL(__func__);
	/* the same sources xf_cliprdr.c registers for FileGroupDescriptorW */
	static const gchar *copied_files_targets[] = { "x-special/gnome-copied-files", "x-special/mate-copied-files" };
	GtkSelectionData *data;
	gchar **uris = NULL;
	gchar **paths;
	guint i, k;

	data = gtk_clipboard_wait_for_contents(gtkClipboard, gdk_atom_intern("text/uri-list", TRUE));
	if (data) {
		uris = gtk_selection_data_get_uris(data);
		gtk_selection_data_free(data);
	}
	for (i = 0; !uris && i < G_N_ELEMENTS(copied_files_targets); i++) {
		GdkAtom atom = gdk_atom_intern(copied_files_targets[i], TRUE);
		gint len;
		gchar *text;

		if (atom == GDK_NONE || !(data = gtk_clipboard_wait_for_contents(gtkClipboard, atom)))
			continue;
		len = gtk_selection_data_get_length(data);
		text = len > 0 ? g_strndup((const gchar *)gtk_selection_data_get_data(data), len) : NULL;
		gtk_selection_data_free(data);
		/* as WinPR's process_files(): "copy\n" then one URI per line; "cut\n" is not offered */
		if (text && g_str_has_prefix(text, "copy\n"))
			uris = g_strsplit_set(text + 5, "\n", -1);
		g_free(text);
	}
	if (!uris)
		return NULL;

	/* as WinPR's process_uri_list/process_files: one unusable entry fails the whole selection
	 * (a missing file fails later, in the walk's stat()) */
	paths = g_new0(gchar *, g_strv_length(uris) + 1);
	for (i = 0, k = 0; paths && uris[i] != NULL; i++) {
		if (uris[i][0] && !(paths[k++] = g_filename_from_uri(uris[i], NULL, NULL)))
			g_clear_pointer(&paths, g_strfreev);
	}
	g_strfreev(uris);
	if (paths && !paths[0])
		g_clear_pointer(&paths, g_free);
	return paths;
}

/* the cFileName for rel_path, or NULL where WinPR's add_file_to_list/convert_local_name_component_to_remote would fail */
static gunichar2 *remmina_rdp_cliprdr_outgoing_wire_name(wClipboardDelegate *delegate, const gchar *rel_path, glong *len)
{
	const gchar *component = strrchr(rel_path, '/');
	gunichar2 *wcomponent, *wname;
	gboolean ok;
	glong i;

	component = component ? component + 1 : rel_path;
	if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0)
		return NULL;
	wcomponent = g_utf8_to_utf16(component, -1, NULL, NULL, NULL);
	ok = wcomponent && delegate && delegate->IsFileNameComponentValid &&
	     delegate->IsFileNameComponentValid((LPCWSTR)wcomponent);
	g_free(wcomponent);
	wname = ok ? g_utf8_to_utf16(rel_path, -1, NULL, len, NULL) : NULL;
	/* MS-RDPECLIP 2.2.5.2.3.1: cFileName is 260 WCHARs including the terminator */
	if (!wname || *len <= 0 || *len > 259) {
		g_free(wname);
		return NULL;
	}
	for (i = 0; i < *len; i++)
		if (wname[i] == '/')
			wname[i] = '\\';
	return wname;
}

/* depth-first, parent before children, as WinPR's add_file_to_list; any failure fails the whole list, as there.
 * Symlinks are followed only for the user's own selection (POSIX -H, as cp -RH), never inside it,
 * and the 259-unit name limit bounds the depth. */
static gboolean remmina_rdp_cliprdr_walk_outgoing(GArray *out, wClipboardDelegate *delegate, const gchar *abs_path, const gchar *rel_path, gboolean top_level)
{
	struct stat st;
	RemminaRdpOutgoingFile entry = { 0 };
	gboolean ok = TRUE;

	if ((top_level ? stat(abs_path, &st) : lstat(abs_path, &st)) != 0)
		return FALSE;
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		return !top_level;	/* nested symlinks, FIFOs, devices, sockets are skipped */
	if (!(entry.wire_name = remmina_rdp_cliprdr_outgoing_wire_name(delegate, rel_path, &entry.wire_len)))
		return FALSE;

	entry.abs_path = g_strdup(abs_path);
	entry.is_directory = S_ISDIR(st.st_mode);
	entry.top_level = top_level;
	entry.size = entry.is_directory ? 0 : (guint64)st.st_size;
	entry.mtime = (gint64)st.st_mtime;
	g_array_append_val(out, entry);

	if (entry.is_directory) {
		GDir *dir = g_dir_open(abs_path, 0, NULL);
		const gchar *name;

		if (!dir)
			return FALSE;
		while (ok && (name = g_dir_read_name(dir)) != NULL) {
			gchar *child_abs = g_build_filename(abs_path, name, NULL);
			gchar *child_rel = g_build_path("/", rel_path, name, NULL);
			ok = remmina_rdp_cliprdr_walk_outgoing(out, delegate, child_abs, child_rel, FALSE);
			g_free(child_abs);
			g_free(child_rel);
		}
		g_dir_close(dir);
	}
	return ok;
}

static void remmina_rdp_cliprdr_outgoing_file_clear(gpointer data)
{
	RemminaRdpOutgoingFile *entry = data;

	g_free(entry->wire_name);
	g_free(entry->abs_path);
}

static GArray *remmina_rdp_cliprdr_build_outgoing_files(GStrv top_level, wClipboardDelegate *delegate)
{
	GArray *out;
	gboolean ok = TRUE;
	guint i;

	if (!top_level || !top_level[0])
		return NULL;

	out = g_array_new(FALSE, FALSE, sizeof(RemminaRdpOutgoingFile));
	g_array_set_clear_func(out, remmina_rdp_cliprdr_outgoing_file_clear);
	for (i = 0; ok && top_level[i] != NULL; i++) {
		gchar *base = g_path_get_basename(top_level[i]);
		ok = remmina_rdp_cliprdr_walk_outgoing(out, delegate, top_level[i], base, TRUE);
		g_free(base);
	}

	if (!ok) {
		g_warning("[RDP] rdp_cliprdr: local file selection cannot be represented as an RDP file list, not offering it");
		g_array_free(out, TRUE);
		return NULL;
	}
	return out;
}

#if FREERDP_VERSION_MAJOR >= 3
/* runs on the cliprdr channel thread, which alone owns local_files_flat */
static void remmina_rdp_cliprdr_send_local_file_list(rfClipboard *clipboard, RemminaProtocolWidget *gp, RemminaRdpLocalSelection *selection)
{
	RemminaPluginRdpEvent rdp_event = { 0 };
	FILEDESCRIPTORW *descriptors;
	BYTE *wireData = NULL;
	UINT32 wireSize = 0;
	guint i, count;

	g_clear_pointer(&clipboard->local_files_flat, g_array_unref);
	clipboard->local_files_flat = selection ? remmina_rdp_cliprdr_build_outgoing_files(selection->paths, ClipboardGetDelegate(clipboard->system)) : NULL;
	clipboard->local_files_gen = selection ? selection->gen : 0;
	/* as gtk-frdp: a lock that arrived before the list was built */
	if (clipboard->pending_lock && clipboard->local_files_flat)
		remmina_rdp_cliprdr_lock_local_files(clipboard, clipboard->pending_lock_id, clipboard->local_files_flat);
	clipboard->pending_lock = FALSE;
	if (selection) {
		g_strfreev(selection->paths);
		g_free(selection);
	}

	count = clipboard->local_files_flat ? clipboard->local_files_flat->len : 0;
	descriptors = count > 0 ? (FILEDESCRIPTORW *)calloc(count, sizeof(FILEDESCRIPTORW)) : NULL;
	if (descriptors) {
		for (i = 0; i < count; i++) {
			RemminaRdpOutgoingFile *entry = &g_array_index(clipboard->local_files_flat, RemminaRdpOutgoingFile, i);

			memcpy(descriptors[i].cFileName, entry->wire_name, entry->wire_len * sizeof(WCHAR));	/* 1..259 units */

			descriptors[i].dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
			if (entry->is_directory) {
				descriptors[i].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			} else {
				descriptors[i].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
				descriptors[i].nFileSizeLow = (DWORD)(entry->size & 0xFFFFFFFFu);
				descriptors[i].nFileSizeHigh = (DWORD)(entry->size >> 32);
			}
			guint64 ft = (guint64)(entry->mtime + 11644473600LL) * 10000000ULL;
			descriptors[i].ftLastWriteTime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu);
			descriptors[i].ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
		}
		if (cliprdr_serialize_file_list_ex(cliprdr_file_context_remote_get_flags(clipboard->file_context),
						   descriptors, count, &wireData, &wireSize) != NO_ERROR) {
			wireData = NULL;
			wireSize = 0;
		}
		free(descriptors);
	}

	rdp_event.type = REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_DATA_RESPONSE;
	rdp_event.clipboard_formatdataresponse.size = (int)MIN(wireSize, INT32_MAX);
	rdp_event.clipboard_formatdataresponse.data = wireData;
	remmina_rdp_event_event_push(gp, &rdp_event);
}
#endif

CLIPRDR_FORMAT_LIST *remmina_rdp_cliprdr_get_client_format_list(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	GtkClipboard *gtkClipboard;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	GdkAtom *targets;
	gboolean result = 0;
	gboolean has_local_files = FALSE;
	gint loccount, srvcount;
	gint formatId, i;
	CLIPRDR_FORMAT *formats;
	struct retp_t {
		CLIPRDR_FORMAT_LIST	pFormatList;
		CLIPRDR_FORMAT		formats[];
	} *retp;

	formats = NULL;
	retp = NULL;
	loccount = 0;

	/* a new local selection: stop serving the previous file list unless the server locked it (xf_cliprdr.c clears its file context here) */
	g_atomic_int_inc(&rfi->clipboard.local_selection_gen);

	/* cheap check only; the walk itself happens lazily in get_clipboard_data() */
	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard) {
		GdkAtom file_atoms[] = { gdk_atom_intern("text/uri-list", TRUE), gdk_atom_intern("x-special/gnome-copied-files", TRUE),
					 gdk_atom_intern("x-special/mate-copied-files", TRUE) };
		result = gtk_clipboard_wait_for_targets(gtkClipboard, &targets, &loccount);
		for (i = 0; result && !has_local_files && i < loccount; i++)
			for (guint a = 0; a < G_N_ELEMENTS(file_atoms); a++)
				if (targets[i] != GDK_NONE && targets[i] == file_atoms[a])
					has_local_files = TRUE;
	}
	REMMINA_PLUGIN_DEBUG("gp=%p sending to server the following local clipboard content formats", gp);
	if (result && loccount > 0) {
		formats = (CLIPRDR_FORMAT *)malloc((loccount + 1) * sizeof(CLIPRDR_FORMAT));
		srvcount = 0;
		for (i = 0; i < loccount; i++) {
			formatId = remmina_rdp_cliprdr_get_format_from_gdkatom(targets[i]);
			if (formatId != 0) {
				gchar *name = gdk_atom_name(targets[i]);
				REMMINA_PLUGIN_DEBUG("     local clipboard format %s will be sent to remote as %d", name, formatId);
				g_free(name);
				formats[srvcount].formatId = formatId;
				/* Registered formats are identified by name, not by id */
				if (formatId == CB_FORMAT_PNG)
					formats[srvcount].formatName = "image/png";
				else if (formatId == CB_FORMAT_JPEG)
					formats[srvcount].formatName = "image/jpeg";
				else
					formats[srvcount].formatName = NULL;
				srvcount++;
			}
		}
		if (has_local_files) {
#if FREERDP_VERSION_MAJOR >= 3
			REMMINA_PLUGIN_DEBUG("gp=%p: local clipboard has files, announcing FileGroupDescriptorW", gp);
			formats[srvcount].formatId = CB_FORMAT_FILEDESCRIPTORW;
			formats[srvcount].formatName = file_group_descriptorw_name;
			srvcount++;
#endif
		}
		if (srvcount > 0) {
			retp = (struct retp_t *)malloc(sizeof(struct retp_t) + sizeof(CLIPRDR_FORMAT) * srvcount);
			retp->pFormatList.formats = retp->formats;
			retp->pFormatList.numFormats = srvcount;
			memcpy(retp->formats, formats, sizeof(CLIPRDR_FORMAT) * srvcount);
		} else {
			retp = (struct retp_t *)malloc(sizeof(struct retp_t));
			retp->pFormatList.formats = NULL;
			retp->pFormatList.numFormats = 0;
		}
		free(formats);
	} else {
		retp = (struct retp_t *)malloc(sizeof(struct retp_t) + sizeof(CLIPRDR_FORMAT));
		retp->pFormatList.formats = NULL;
		retp->pFormatList.numFormats = 0;
	}

	if (result)
		g_free(targets);

#if FREERDP_VERSION_MAJOR >= 3
	retp->pFormatList.common.msgType = CB_FORMAT_LIST;
	retp->pFormatList.common.msgFlags = 0;
#else
	retp->pFormatList.msgType = CB_FORMAT_LIST;
	retp->pFormatList.msgFlags = 0;
#endif

	return (CLIPRDR_FORMAT_LIST *)retp;
}

static void remmina_rdp_cliprdr_mt_get_format_list(RemminaProtocolWidget *gp, RemminaPluginRdpUiObject *ui)
{
	TRACE_CALL(__func__);
	ui->retptr = (void *)remmina_rdp_cliprdr_get_client_format_list(gp);
}

void remmina_rdp_cliprdr_get_clipboard_data(RemminaProtocolWidget *gp, RemminaPluginRdpUiObject *ui)
{
	TRACE_CALL(__func__);
	GtkClipboard *gtkClipboard;
	UINT8 *inbuf = NULL;
	UINT8 *outbuf = NULL;
#if FREERDP_VERSION_MAJOR >= 3
	WCHAR *outbuf_wchar = NULL;
#endif
	GdkPixbuf *image = NULL;
	size_t size = 0;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	RemminaPluginRdpEvent rdp_event = { 0 };

#if FREERDP_VERSION_MAJOR >= 3
	/* only the selection is read here; the walk runs on the cliprdr thread (send_local_file_list) */
	if (ui->clipboard.format == CB_FORMAT_FILEDESCRIPTORW) {
		RemminaRdpLocalSelection *selection = g_new0(RemminaRdpLocalSelection, 1);

		selection->gen = g_atomic_int_get(&rfi->clipboard.local_selection_gen);
		gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
		selection->paths = gtkClipboard ? remmina_rdp_cliprdr_local_file_paths(gtkClipboard) : NULL;
		ui->retptr = selection;
		return;
	}
#endif

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard) {
		switch (ui->clipboard.format) {
		case CF_TEXT:
		case CF_UNICODETEXT:
		case CB_FORMAT_HTML:
		{
			inbuf = (UINT8 *)gtk_clipboard_wait_for_text(gtkClipboard);
			break;
		}

		case CB_FORMAT_PNG:
		case CB_FORMAT_JPEG:
		case CF_DIB:
		case CF_DIBV5:
		{
			image = gtk_clipboard_wait_for_image(gtkClipboard);
			break;
		}
		}
	}

	/* No data received, send nothing */
	if (inbuf != NULL || image != NULL) {
		switch (ui->clipboard.format) {
			case CF_TEXT:
			case CB_FORMAT_HTML:
			{
				size = strlen((char *)inbuf);
				outbuf = lf2crlf(inbuf, (int *) &size);
				g_free(inbuf);
				break;
			}
			case CF_UNICODETEXT:
			{
				size = strlen((const char *)inbuf);
				inbuf = lf2crlf(inbuf, (int *) &size);
#if FREERDP_VERSION_MAJOR >= 3
				size_t len = 0;
				outbuf_wchar = ConvertUtf8NToWCharAlloc((const char *)inbuf, (size_t)size, &len);
				size = (len + 1) * sizeof(WCHAR);
#else
				const int rc = (ConvertToUnicode(CP_UTF8, 0, (CHAR *)inbuf, -1, (WCHAR **)&outbuf, 0)) * sizeof(WCHAR);
				size = 0;
				if (rc >= 0) {
					size = (size_t)rc;
				}
#endif
				g_free(inbuf);
				break;
			}
			case CB_FORMAT_PNG:
			{
				gchar *data;
				gsize buffersize;
				if (gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "png", NULL, NULL) == FALSE){
					break;
				}
				outbuf = (UINT8 *)malloc(buffersize);
				memcpy(outbuf, data, buffersize);
				size = buffersize;
				g_object_unref(image);
				break;
			}
			case CB_FORMAT_JPEG:
			{
				gchar *data;
				gsize buffersize;
				if (gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "jpeg", NULL, NULL) == FALSE){
					break;
				}
				outbuf = (UINT8 *)malloc(buffersize);
				memcpy(outbuf, data, buffersize);
				size = buffersize;
				g_object_unref(image);
				break;
			}
			case CF_DIB:
			case CF_DIBV5:
			{
				gchar *data;
				gsize buffersize;
				if (gdk_pixbuf_save_to_buffer(image, &data, &buffersize, "bmp", NULL, NULL) == FALSE){
					break;
				}
				size = buffersize - 14;
				outbuf = (UINT8 *)malloc(size);
				memcpy(outbuf, data + 14, size);
				g_object_unref(image);
				break;
			}
		}
	}

	rdp_event.type = REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_DATA_RESPONSE;
	rdp_event.clipboard_formatdataresponse.size = (int)MIN(size, INT32_MAX);

#if FREERDP_VERSION_MAJOR >= 3
	// For unicode, use the wchar buffer
	if (outbuf == NULL && outbuf_wchar != NULL) {
		rdp_event.clipboard_formatdataresponse.data = (unsigned char *)outbuf_wchar;
	}
	else {
		rdp_event.clipboard_formatdataresponse.data = (unsigned char *)outbuf;
	}
#else
	rdp_event.clipboard_formatdataresponse.data = (unsigned char *)outbuf;
#endif

	remmina_rdp_event_event_push(gp, &rdp_event);
}

void remmina_rdp_cliprdr_set_clipboard_data(RemminaProtocolWidget *gp, RemminaPluginRdpUiObject *ui)
{
	TRACE_CALL(__func__);
	GtkClipboard *gtkClipboard;
	gint n_targets;
	rfContext *rfi = GET_PLUGIN_DATA(gp);

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard) {
		GtkTargetEntry *targets = gtk_target_table_new_from_list(ui->clipboard.targetlist, &n_targets);
		if (!targets) {
			/* If no targets exists, this is an internal error, because
			 * remmina_rdp_cliprdr_server_format_list() must have produced
			 * at least one target before calling REMMINA_RDP_UI_CLIPBOARD_SET_DATA */
			g_warning("[RDP] internal error: no targets to insert into the local clipboard");
		}

		REMMINA_PLUGIN_DEBUG("setting clipboard with owner to me: %p", gp);
		gtk_clipboard_set_with_owner(gtkClipboard, targets, n_targets,
							(GtkClipboardGetFunc)remmina_rdp_cliprdr_request_data,
							(GtkClipboardClearFunc)remmina_rdp_cliprdr_empty_clipboard, G_OBJECT(gp));
		gtk_target_table_free(targets, n_targets);
	}
}

void remmina_rdp_cliprdr_detach_owner(RemminaProtocolWidget *gp)
{
	/* When closing a rdp connection, we should check if gp is a clipboard owner.
	 * If it’s an owner, detach it from the clipboard */
	TRACE_CALL(__func__);
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	GtkClipboard *gtkClipboard;

	if (!rfi || !rfi->drawing_area) return;

	gtkClipboard = gtk_widget_get_clipboard(rfi->drawing_area, GDK_SELECTION_CLIPBOARD);
	if (gtkClipboard && gtk_clipboard_get_owner(gtkClipboard) == (GObject *)gp)
		gtk_clipboard_clear(gtkClipboard);

}

void remmina_rdp_event_process_clipboard(RemminaProtocolWidget *gp, RemminaPluginRdpUiObject *ui)
{
	TRACE_CALL(__func__);

	switch (ui->clipboard.type) {
	case REMMINA_RDP_UI_CLIPBOARD_FORMATLIST:
		remmina_rdp_cliprdr_mt_get_format_list(gp, ui);
		break;

	case REMMINA_RDP_UI_CLIPBOARD_GET_DATA:
		remmina_rdp_cliprdr_get_clipboard_data(gp, ui);
		break;

	case REMMINA_RDP_UI_CLIPBOARD_SET_DATA:
		remmina_rdp_cliprdr_set_clipboard_data(gp, ui);
		break;

	}
}

void remmina_rdp_clipboard_init(rfContext *rfi)
{
	TRACE_CALL(__func__);
	// Future: initialize rfi->clipboard
}
/* as xf_cliprdr_uninit(): the file context still calls into the channel context while clearing, so it goes before FreeRDP frees it */
void remmina_rdp_cliprdr_uninit(rfContext *rfi, CliprdrClientContext *cliprdr)
{
	TRACE_CALL(__func__);
#if FREERDP_VERSION_MAJOR >= 3
	if (rfi->clipboard.file_context) {
		ClipboardLock(rfi->clipboard.system);
		cliprdr_file_context_uninit(rfi->clipboard.file_context, cliprdr);
		ClipboardUnlock(rfi->clipboard.system);
	}
#endif
	rfi->clipboard.context = NULL;
}

void remmina_rdp_clipboard_free(rfContext *rfi)
{
	TRACE_CALL(__func__);

	remmina_rdp_cliprdr_cached_clipboard_free(&(rfi->clipboard));
	/* the cliprdr thread that owns this has been joined by now */
	remmina_rdp_cliprdr_drop_local_files(&rfi->clipboard);

#if FREERDP_VERSION_MAJOR >= 3
	if (rfi->clipboard.file_context) {
		cliprdr_file_context_free(rfi->clipboard.file_context);
		rfi->clipboard.file_context = NULL;
	}
#endif
	if (rfi->clipboard.system) {
		ClipboardDestroy(rfi->clipboard.system);
		rfi->clipboard.system = NULL;
	}
}

void remmina_rdp_clipboard_abort_client_format_data_request(rfContext *rfi)
{
	TRACE_CALL(__func__);
	if (rfi && rfi->clipboard.srv_clip_data_wait == SCDW_BUSY_WAIT) {
		REMMINA_PLUGIN_DEBUG("requesting clipboard data transfer from server to be ignored and busywait loop to exit");
		/* Allow clipboard transfer from server to terminate */
		rfi->clipboard.srv_clip_data_wait = SCDW_ABORTING;
		usleep(100000);
	}
}

void remmina_rdp_cliprdr_init(rfContext *rfi, CliprdrClientContext *cliprdr)
{
	TRACE_CALL(__func__);

	rfClipboard *clipboard;
	clipboard = &(rfi->clipboard);

	rfi->clipboard.rfi = rfi;
	cliprdr->custom = (void *)clipboard;

	clipboard->context = cliprdr;
	pthread_mutex_init(&clipboard->transfer_clip_mutex, NULL);
	pthread_cond_init(&clipboard->transfer_clip_cond, NULL);
	clipboard->srv_clip_data_wait = SCDW_NONE;

	pthread_mutex_init(&clipboard->srv_data_mutex, NULL);

	clipboard->system = ClipboardCreate();

#if FREERDP_VERSION_MAJOR >= 3
	/* as xf_cliprdr.c: no file context, no clipboard (cliprdr->custom has to point at one) */
	clipboard->file_context = cliprdr_file_context_new(clipboard);
	if (!clipboard->file_context) {
		g_warning("[RDP] rdp_cliprdr: cliprdr_file_context_new() failed, clipboard disabled");
		return;
	}
	if (ClipboardGetFormatId(clipboard->system, "text/uri-list") != 0)
		cliprdr_file_context_set_locally_available(clipboard->file_context, TRUE);
#endif

	cliprdr->MonitorReady = remmina_rdp_cliprdr_monitor_ready;
	cliprdr->ServerCapabilities = remmina_rdp_cliprdr_server_capabilities;
	cliprdr->ServerFormatList = remmina_rdp_cliprdr_server_format_list;
	cliprdr->ServerFormatListResponse = remmina_rdp_cliprdr_server_format_list_response;
	cliprdr->ServerFormatDataRequest = remmina_rdp_cliprdr_server_format_data_request;
	cliprdr->ServerFormatDataResponse = remmina_rdp_cliprdr_server_format_data_response;

#if FREERDP_VERSION_MAJOR >= 3
	cliprdr_file_context_init(clipboard->file_context, cliprdr);
#endif

	cliprdr->ServerFileContentsRequest = remmina_rdp_cliprdr_server_file_contents_request;
#if FREERDP_VERSION_MAJOR >= 3
	/* FreeRDP's own handlers only track its local_streams, which are not used here */
	cliprdr->ServerLockClipboardData = remmina_rdp_cliprdr_server_lock_clipboard_data;
	cliprdr->ServerUnlockClipboardData = remmina_rdp_cliprdr_server_unlock_clipboard_data;
#endif
}

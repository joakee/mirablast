/* mirablast - session controller
 *
 * Owns the whole pipeline end to end: the evdi virtual display, the WFD
 * server (vendored from gnome-network-displays), the capture bin with its
 * crop, and the reconnect machinery. Because everything is in-process the
 * region file and GND_X11_* environment plumbing of the script era are gone:
 * crop updates are direct g_object_set calls on the live element.
 *
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include "mb-settings.h"
#include "nd-meta-provider.h"
#include "nd-sink.h"

G_BEGIN_DECLS

typedef enum {
  MB_SESSION_STATE_IDLE,             /* no virtual display */
  MB_SESSION_STATE_DISPLAY_STARTING, /* helper spawned, waiting for RandR */
  MB_SESSION_STATE_DISPLAY_READY,    /* output up, not streaming */
  MB_SESSION_STATE_CONNECTING,       /* stream starting (or re-starting) */
  MB_SESSION_STATE_STREAMING,
  MB_SESSION_STATE_ERROR,
} MbSessionState;

#define MB_TYPE_SESSION (mb_session_get_type ())
G_DECLARE_FINAL_TYPE (MbSession, mb_session, MB, SESSION, GObject)

MbSession      *mb_session_new (MbSettings *settings, gboolean with_dummy);

NdMetaProvider *mb_session_get_provider (MbSession *self);
MbSessionState  mb_session_get_state (MbSession *self);
gchar          *mb_session_dup_status (MbSession *self);

/* Bring the virtual display up / tear it down. */
void            mb_session_display_start (MbSession *self);
void            mb_session_display_stop (MbSession *self);

/* Stream to a sink from the provider list (starts the display if needed). */
void            mb_session_connect_sink (MbSession *self, NdSink *sink);
void            mb_session_disconnect (MbSession *self);

G_END_DECLS

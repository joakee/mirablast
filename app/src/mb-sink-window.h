/* mirablast - sink chooser window
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "mb-session.h"

G_BEGIN_DECLS

#define MB_TYPE_SINK_WINDOW (mb_sink_window_get_type ())
G_DECLARE_FINAL_TYPE (MbSinkWindow, mb_sink_window, MB, SINK_WINDOW, GtkWindow)

MbSinkWindow *mb_sink_window_new (MbSession *session);

G_END_DECLS

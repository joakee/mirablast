/* mirablast - status tray applet
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "mb-session.h"

G_BEGIN_DECLS

#define MB_TYPE_TRAY (mb_tray_get_type ())
G_DECLARE_FINAL_TYPE (MbTray, mb_tray, MB, TRAY, GObject)

MbTray *mb_tray_new (GtkApplication *app, MbSession *session, MbSettings *settings);

G_END_DECLS

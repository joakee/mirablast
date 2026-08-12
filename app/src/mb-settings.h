/* mirablast - persistent settings
 *
 * Copyright (C) 2026 James Oakey
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * (this application links against code from gnome-network-displays)
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define MB_TYPE_SETTINGS (mb_settings_get_type ())
G_DECLARE_FINAL_TYPE (MbSettings, mb_settings, MB, SETTINGS, GObject)

MbSettings *mb_settings_new (void);

/* Loaded from / saved to ~/.config/mirablast/mirablast.conf */
gboolean    mb_settings_save (MbSettings *self, GError **error);

/* Virtual display */
gboolean    mb_settings_get_resolution (MbSettings *self, guint *w, guint *h);
const gchar *mb_settings_get_resolution_string (MbSettings *self);
void        mb_settings_set_resolution_string (MbSettings *self, const gchar *wxh);
guint       mb_settings_get_refresh (MbSettings *self);
void        mb_settings_set_refresh (MbSettings *self, guint hz);

/* Streaming */
guint       mb_settings_get_latency_ms (MbSettings *self);
void        mb_settings_set_latency_ms (MbSettings *self, guint ms);
guint       mb_settings_get_gop_seconds (MbSettings *self);
void        mb_settings_set_gop_seconds (MbSettings *self, guint s);
gboolean    mb_settings_get_use_damage (MbSettings *self);
void        mb_settings_set_use_damage (MbSettings *self, gboolean on);
gboolean    mb_settings_get_auto_reconnect (MbSettings *self);
void        mb_settings_set_auto_reconnect (MbSettings *self, gboolean on);

/* Environment */
const gchar *mb_settings_get_primary (MbSettings *self);      /* "" = autodetect */
void        mb_settings_set_primary (MbSettings *self, const gchar *output);
const gchar *mb_settings_get_helper (MbSettings *self);       /* "" = autodiscover */
void        mb_settings_set_helper (MbSettings *self, const gchar *path);

/* Push the values the vendored wfd factory reads from the environment. */
void        mb_settings_apply_environment (MbSettings *self);

G_END_DECLS

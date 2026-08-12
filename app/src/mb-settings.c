/* mirablast - persistent settings
 *
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-settings.h"

#include <stdio.h>

#define GROUP "mirablast"

struct _MbSettings
{
  GObject   parent_instance;

  GKeyFile *kf;
  gchar    *path;

  gchar    *resolution;
  guint     refresh;
  guint     latency_ms;
  guint     gop_seconds;
  gboolean  use_damage;
  gboolean  auto_reconnect;
  gchar    *primary;
  gchar    *helper;
};

G_DEFINE_TYPE (MbSettings, mb_settings, G_TYPE_OBJECT)

enum {
  CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
mb_settings_finalize (GObject *object)
{
  MbSettings *self = MB_SETTINGS (object);

  g_clear_pointer (&self->kf, g_key_file_unref);
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->resolution, g_free);
  g_clear_pointer (&self->primary, g_free);
  g_clear_pointer (&self->helper, g_free);

  G_OBJECT_CLASS (mb_settings_parent_class)->finalize (object);
}

static void
mb_settings_class_init (MbSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = mb_settings_finalize;

  signals[CHANGED] = g_signal_new ("changed",
                                   MB_TYPE_SETTINGS,
                                   G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static gchar *
kf_string (GKeyFile *kf, const gchar *key, const gchar *fallback)
{
  gchar *v = g_key_file_get_string (kf, GROUP, key, NULL);

  return v ? v : g_strdup (fallback);
}

static gint
kf_int (GKeyFile *kf, const gchar *key, gint fallback, gint min, gint max)
{
  GError *error = NULL;
  gint v = g_key_file_get_integer (kf, GROUP, key, &error);

  if (error)
    {
      g_error_free (error);
      return fallback;
    }
  return CLAMP (v, min, max);
}

static gboolean
kf_bool (GKeyFile *kf, const gchar *key, gboolean fallback)
{
  GError *error = NULL;
  gboolean v = g_key_file_get_boolean (kf, GROUP, key, &error);

  if (error)
    {
      g_error_free (error);
      return fallback;
    }
  return v;
}

static void
mb_settings_init (MbSettings *self)
{
  g_autofree gchar *dir = NULL;

  dir = g_build_filename (g_get_user_config_dir (), "mirablast", NULL);
  g_mkdir_with_parents (dir, 0700);
  self->path = g_build_filename (dir, "mirablast.conf", NULL);

  self->kf = g_key_file_new ();
  g_key_file_load_from_file (self->kf, self->path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  self->resolution     = kf_string (self->kf, "resolution", "1920x1080");
  self->refresh        = kf_int (self->kf, "refresh", 60, 1, 240);
  self->latency_ms     = kf_int (self->kf, "latency-ms", 100, 0, 2000);
  self->gop_seconds    = kf_int (self->kf, "gop-seconds", 1, 1, 60);
  self->use_damage     = kf_bool (self->kf, "use-damage", TRUE);
  self->auto_reconnect = kf_bool (self->kf, "auto-reconnect", TRUE);
  self->primary        = kf_string (self->kf, "primary", "");
  self->helper         = kf_string (self->kf, "helper", "");
}

MbSettings *
mb_settings_new (void)
{
  return g_object_new (MB_TYPE_SETTINGS, NULL);
}

gboolean
mb_settings_save (MbSettings *self, GError **error)
{
  g_key_file_set_string  (self->kf, GROUP, "resolution", self->resolution);
  g_key_file_set_integer (self->kf, GROUP, "refresh", self->refresh);
  g_key_file_set_integer (self->kf, GROUP, "latency-ms", self->latency_ms);
  g_key_file_set_integer (self->kf, GROUP, "gop-seconds", self->gop_seconds);
  g_key_file_set_boolean (self->kf, GROUP, "use-damage", self->use_damage);
  g_key_file_set_boolean (self->kf, GROUP, "auto-reconnect", self->auto_reconnect);
  g_key_file_set_string  (self->kf, GROUP, "primary", self->primary);
  g_key_file_set_string  (self->kf, GROUP, "helper", self->helper);

  if (!g_key_file_save_to_file (self->kf, self->path, error))
    return FALSE;

  g_signal_emit (self, signals[CHANGED], 0);
  return TRUE;
}

gboolean
mb_settings_get_resolution (MbSettings *self, guint *w, guint *h)
{
  guint pw = 0, ph = 0;
  gchar tail = '\0';

  if (sscanf (self->resolution, "%ux%u%c", &pw, &ph, &tail) != 2 ||
      pw < 64 || ph < 64 || pw > 4094 || ph > 4094)
    return FALSE;

  if (w) *w = pw;
  if (h) *h = ph;
  return TRUE;
}

const gchar *
mb_settings_get_resolution_string (MbSettings *self) { return self->resolution; }

void
mb_settings_set_resolution_string (MbSettings *self, const gchar *wxh)
{
  g_free (self->resolution);
  self->resolution = g_strdup (wxh);
}

guint mb_settings_get_refresh (MbSettings *self) { return self->refresh; }
void  mb_settings_set_refresh (MbSettings *self, guint hz) { self->refresh = CLAMP (hz, 1, 240); }

guint mb_settings_get_latency_ms (MbSettings *self) { return self->latency_ms; }
void  mb_settings_set_latency_ms (MbSettings *self, guint ms) { self->latency_ms = MIN (ms, 2000); }

guint mb_settings_get_gop_seconds (MbSettings *self) { return self->gop_seconds; }
void  mb_settings_set_gop_seconds (MbSettings *self, guint s) { self->gop_seconds = CLAMP (s, 1, 60); }

gboolean mb_settings_get_use_damage (MbSettings *self) { return self->use_damage; }
void     mb_settings_set_use_damage (MbSettings *self, gboolean on) { self->use_damage = on; }

gboolean mb_settings_get_auto_reconnect (MbSettings *self) { return self->auto_reconnect; }
void     mb_settings_set_auto_reconnect (MbSettings *self, gboolean on) { self->auto_reconnect = on; }

const gchar *mb_settings_get_primary (MbSettings *self) { return self->primary; }

void
mb_settings_set_primary (MbSettings *self, const gchar *output)
{
  g_free (self->primary);
  self->primary = g_strdup (output ? output : "");
}

const gchar *mb_settings_get_helper (MbSettings *self) { return self->helper; }

void
mb_settings_set_helper (MbSettings *self, const gchar *path)
{
  g_free (self->helper);
  self->helper = g_strdup (path ? path : "");
}

void
mb_settings_apply_environment (MbSettings *self)
{
  g_autofree gchar *gop = g_strdup_printf ("%u", self->gop_seconds);
  g_autofree gchar *lat = g_strdup_printf ("%u", self->latency_ms);

  /* The vendored wfd-media-factory reads these when it builds a pipeline;
   * setting them here keeps the vendored code untouched. */
  g_setenv ("GND_GOP_SECONDS", gop, TRUE);
  g_setenv ("GND_LATENCY_MS", lat, TRUE);
}

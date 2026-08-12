/* mirablast - use a Miracast sink as a real extended display
 *
 * Tray-resident GTK application. The WFD/Miracast backend is vendored from
 * gnome-network-displays (GPL-3.0-or-later); the display and capture logic
 * comes from the mirablast scripts, now in-process.
 *
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include <glib-unix.h>
#include <gst/gst.h>
#include <gtk/gtk.h>

#include "mb-session.h"
#include "nd-provider.h"
#include "mb-settings.h"
#include "mb-tray.h"

typedef struct
{
  MbSettings *settings;
  MbSession  *session;
  MbTray     *tray;
  gboolean    with_dummy;
} App;

static gboolean
sigterm_cb (gpointer user_data)
{
  App *a = user_data;

  g_message ("mirablast: signal received, tearing down");
  if (a->session)
    mb_session_display_stop (a->session);
  g_application_quit (g_application_get_default ());
  return G_SOURCE_REMOVE;
}

/* MIRABLAST_AUTO test hook: "display" brings the virtual display up at
 * startup; "cast:<substring>" additionally connects to the first discovered
 * sink whose name matches. Exists so the full flow can be driven and
 * regression-tested without poking at the tray menu. */
static void
auto_try_cast (App *a)
{
  const gchar *spec = g_getenv ("MIRABLAST_AUTO");
  g_autoptr(GList) sinks = NULL;

  if (!spec || !g_str_has_prefix (spec, "cast:"))
    return;
  if (mb_session_get_state (a->session) != MB_SESSION_STATE_DISPLAY_READY)
    return;

  sinks = nd_provider_get_sinks (ND_PROVIDER (mb_session_get_provider (a->session)));
  for (GList *l = sinks; l; l = l->next)
    {
      g_autofree gchar *name = NULL;

      g_object_get (l->data, "display-name", &name, NULL);
      if (name && strstr (name, spec + 5))
        {
          g_message ("mirablast: MIRABLAST_AUTO connecting to '%s'", name);
          mb_session_connect_sink (a->session, l->data);
          return;
        }
    }
}

static void
auto_state_cb (App *a, MbSession *session)
{
  auto_try_cast (a);
}

static void
auto_sink_cb (App *a, NdSink *sink, NdProvider *provider)
{
  auto_try_cast (a);
}

static void
activate_cb (GtkApplication *app, gpointer user_data)
{
  App *a = user_data;

  if (a->tray)
    return; /* second launch of the single instance: nothing to do */

  a->settings = mb_settings_new ();
  mb_settings_apply_environment (a->settings);

  if (g_strcmp0 (g_getenv ("MIRABLAST_DUMMY"), "1") == 0)
    a->with_dummy = TRUE;

  a->session = mb_session_new (a->settings, a->with_dummy);
  a->tray = mb_tray_new (app, a->session, a->settings);

  /* Tray-resident: no main window holds the app open. */
  g_application_hold (G_APPLICATION (app));

  g_unix_signal_add (SIGTERM, sigterm_cb, a);
  g_unix_signal_add (SIGINT, sigterm_cb, a);

  {
    const gchar *spec = g_getenv ("MIRABLAST_AUTO");

    if (spec && *spec)
      {
        g_signal_connect_swapped (a->session, "state-changed",
                                  (GCallback) auto_state_cb, a);
        g_signal_connect_swapped (mb_session_get_provider (a->session),
                                  "sink-added", (GCallback) auto_sink_cb, a);
        mb_session_display_start (a->session);
      }
  }
}

static void
shutdown_cb (GApplication *app, gpointer user_data)
{
  App *a = user_data;

  if (a->session)
    mb_session_display_stop (a->session);
  g_clear_object (&a->tray);
  g_clear_object (&a->session);
  g_clear_object (&a->settings);
}

int
main (int argc, char *argv[])
{
  g_autoptr(GtkApplication) app = NULL;
  App a = { 0 };
  int status;

  gst_init (&argc, &argv);

  for (int i = 1; i < argc; i++)
    if (g_strcmp0 (argv[i], "--dummy") == 0)
      a.with_dummy = TRUE;

  app = gtk_application_new ("org.mirablast.Mirablast",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate_cb), &a);
  g_signal_connect (app, "shutdown", G_CALLBACK (shutdown_cb), &a);

  status = g_application_run (G_APPLICATION (app), 0, NULL);
  return status;
}

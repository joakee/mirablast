/* mirablast - status tray applet
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-tray.h"
#include "mb-settings-dialog.h"
#include "nd-provider.h"

#include <libayatana-appindicator/app-indicator.h>

struct _MbTray
{
  GObject         parent_instance;

  GtkApplication *app;
  MbSession      *session;
  MbSettings     *settings;

  AppIndicator   *indicator;
  GtkWidget      *menu;
  GtkWidget      *status_item;
  GtkWidget      *cast_item;
  GtkWidget      *cast_menu;      /* submenu of discovered sinks */
  GtkWidget      *cast_empty;     /* placeholder shown while it has none */
  GtkWidget      *disconnect_item;
  GtkWidget      *display_item;
};

G_DEFINE_TYPE (MbTray, mb_tray, G_TYPE_OBJECT)

/* ------------------------------------------------------------------ */
/* the sink submenu                                                    */

static void
sink_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);
  NdSink *sink = g_object_get_data (G_OBJECT (item), "mb-sink");

  if (!sink)
    return;

  /* Brings the virtual display up first if it isn't already; the session
   * queues the connect until the output is ready. */
  mb_session_connect_sink (self->session, sink);
}

/* The placeholder keeps the submenu non-empty: an item libdbusmenu has
 * exported with no children is not drawn as a submenu at all, so the entry
 * would be dead until the first sink turned up. */
static void
update_placeholder (MbTray *self)
{
  g_autoptr(GList) children = gtk_container_get_children (GTK_CONTAINER (self->cast_menu));
  gboolean have_sink = FALSE;

  for (GList *l = children; l; l = l->next)
    if (l->data != self->cast_empty)
      {
        have_sink = TRUE;
        break;
      }

  gtk_widget_set_visible (self->cast_empty, !have_sink);
}

static void
sink_added_cb (MbTray *self, NdSink *sink, NdProvider *provider)
{
  GtkWidget *item;
  g_autofree gchar *name = NULL;

  g_object_get (sink, "display-name", &name, NULL);

  item = gtk_menu_item_new_with_label (name ? name : "(unnamed sink)");
  g_object_set_data_full (G_OBJECT (item), "mb-sink",
                          g_object_ref (sink), g_object_unref);
  /* Follow renames (the name can arrive after discovery). */
  g_object_bind_property (sink, "display-name", item, "label",
                          G_BINDING_DEFAULT);
  g_signal_connect (item, "activate", G_CALLBACK (sink_activate_cb), self);

  gtk_widget_show (item);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->cast_menu), item);
  update_placeholder (self);
}

static void
sink_removed_cb (MbTray *self, NdSink *sink, NdProvider *provider)
{
  g_autoptr(GList) children = gtk_container_get_children (GTK_CONTAINER (self->cast_menu));

  for (GList *l = children; l; l = l->next)
    {
      if (g_object_get_data (G_OBJECT (l->data), "mb-sink") == sink)
        {
          gtk_widget_destroy (GTK_WIDGET (l->data));
          break;
        }
    }
  update_placeholder (self);
}

static void
disconnect_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);

  mb_session_disconnect (self->session);
}

static void
display_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);
  MbSessionState state = mb_session_get_state (self->session);

  if (state == MB_SESSION_STATE_IDLE || state == MB_SESSION_STATE_ERROR)
    mb_session_display_start (self->session);
  else
    mb_session_display_stop (self->session);
}

static void
settings_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);

  mb_settings_dialog_show (self->settings);
}

static void
quit_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);

  /* Teardown restores the panel layout before we go. */
  mb_session_display_stop (self->session);
  g_application_quit (G_APPLICATION (self->app));
}

static void
update_cb (MbTray *self, MbSession *session)
{
  MbSessionState state = mb_session_get_state (session);
  g_autofree gchar *status = mb_session_dup_status (session);
  const gchar *icon;

  gtk_menu_item_set_label (GTK_MENU_ITEM (self->status_item), status);

  gtk_widget_set_sensitive (self->disconnect_item,
                            state == MB_SESSION_STATE_STREAMING ||
                            state == MB_SESSION_STATE_CONNECTING);

  /* Picking a second sink mid-cast is refused by the session, and discovery
   * is off while a connect is in flight, so the list is stale then anyway. */
  gtk_widget_set_sensitive (self->cast_item,
                            state != MB_SESSION_STATE_STREAMING &&
                            state != MB_SESSION_STATE_CONNECTING);

  switch (state)
    {
    case MB_SESSION_STATE_IDLE:
    case MB_SESSION_STATE_ERROR:
      gtk_menu_item_set_label (GTK_MENU_ITEM (self->display_item),
                               "Start virtual display");
      break;
    default:
      gtk_menu_item_set_label (GTK_MENU_ITEM (self->display_item),
                               "Stop virtual display");
      break;
    }

  switch (state)
    {
    case MB_SESSION_STATE_STREAMING:
      icon = "video-display-symbolic";
      app_indicator_set_status (self->indicator, APP_INDICATOR_STATUS_ATTENTION);
      break;
    case MB_SESSION_STATE_ERROR:
      icon = "dialog-warning-symbolic";
      app_indicator_set_status (self->indicator, APP_INDICATOR_STATUS_ACTIVE);
      break;
    default:
      icon = "video-display-symbolic";
      app_indicator_set_status (self->indicator, APP_INDICATOR_STATUS_ACTIVE);
      break;
    }
  (void) icon;

  app_indicator_set_title (self->indicator, status);
}

static void
mb_tray_dispose (GObject *object)
{
  MbTray *self = MB_TRAY (object);

  g_clear_object (&self->indicator);
  g_clear_object (&self->session);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (mb_tray_parent_class)->dispose (object);
}

static void
mb_tray_class_init (MbTrayClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = mb_tray_dispose;
}

static void
mb_tray_init (MbTray *self)
{
}

MbTray *
mb_tray_new (GtkApplication *app, MbSession *session, MbSettings *settings)
{
  MbTray *self = g_object_new (MB_TYPE_TRAY, NULL);
  GtkWidget *item;

  self->app = app;
  self->session = g_object_ref (session);
  self->settings = g_object_ref (settings);

  self->indicator = app_indicator_new ("mirablast",
                                       "video-display",
                                       APP_INDICATOR_CATEGORY_HARDWARE);
  app_indicator_set_status (self->indicator, APP_INDICATOR_STATUS_ACTIVE);
  app_indicator_set_attention_icon_full (self->indicator,
                                         "video-display", "casting");

  self->menu = gtk_menu_new ();

  self->status_item = gtk_menu_item_new_with_label ("Idle");
  gtk_widget_set_sensitive (self->status_item, FALSE);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), self->status_item);

  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu),
                         gtk_separator_menu_item_new ());

  /* Cast to > <sink>: a submenu rather than a window, kept in step with the
   * provider so it is right whenever the tray menu is opened. */
  self->cast_item = gtk_menu_item_new_with_label ("Cast to");
  self->cast_menu = gtk_menu_new ();
  self->cast_empty = gtk_menu_item_new_with_label ("Searching for sinks…");
  gtk_widget_set_sensitive (self->cast_empty, FALSE);
  gtk_widget_show (self->cast_empty);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->cast_menu), self->cast_empty);
  gtk_widget_show (self->cast_menu);
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (self->cast_item), self->cast_menu);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), self->cast_item);

  {
    NdMetaProvider *provider = mb_session_get_provider (session);
    g_autoptr(GList) sinks = nd_provider_get_sinks (ND_PROVIDER (provider));

    g_signal_connect_object (provider, "sink-added",
                             (GCallback) sink_added_cb, self, G_CONNECT_SWAPPED);
    g_signal_connect_object (provider, "sink-removed",
                             (GCallback) sink_removed_cb, self, G_CONNECT_SWAPPED);

    for (GList *l = sinks; l; l = l->next)
      sink_added_cb (self, l->data, ND_PROVIDER (provider));
  }

  self->disconnect_item = gtk_menu_item_new_with_label ("Disconnect");
  gtk_widget_set_sensitive (self->disconnect_item, FALSE);
  g_signal_connect (self->disconnect_item, "activate",
                    G_CALLBACK (disconnect_activate_cb), self);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), self->disconnect_item);

  self->display_item = gtk_menu_item_new_with_label ("Start virtual display");
  g_signal_connect (self->display_item, "activate",
                    G_CALLBACK (display_activate_cb), self);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), self->display_item);

  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu),
                         gtk_separator_menu_item_new ());

  item = gtk_menu_item_new_with_label ("Settings…");
  g_signal_connect (item, "activate", G_CALLBACK (settings_activate_cb), self);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), item);

  item = gtk_menu_item_new_with_label ("Quit");
  g_signal_connect (item, "activate", G_CALLBACK (quit_activate_cb), self);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), item);

  gtk_widget_show_all (self->menu);
  /* show_all does not descend into submenus; their items show themselves. */
  update_placeholder (self);
  app_indicator_set_menu (self->indicator, GTK_MENU (self->menu));

  g_signal_connect_object (session, "state-changed",
                           (GCallback) update_cb, self, G_CONNECT_SWAPPED);
  update_cb (self, session);

  return self;
}

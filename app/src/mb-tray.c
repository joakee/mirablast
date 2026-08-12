/* mirablast - status tray applet
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-tray.h"
#include "mb-settings-dialog.h"
#include "mb-sink-window.h"

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
  GtkWidget      *disconnect_item;
  GtkWidget      *display_item;

  MbSinkWindow   *sink_window;
};

G_DEFINE_TYPE (MbTray, mb_tray, G_TYPE_OBJECT)

static void
cast_activate_cb (GtkMenuItem *item, gpointer user_data)
{
  MbTray *self = MB_TRAY (user_data);
  MbSessionState state = mb_session_get_state (self->session);

  if (state == MB_SESSION_STATE_IDLE || state == MB_SESSION_STATE_ERROR)
    mb_session_display_start (self->session);

  if (!self->sink_window)
    {
      self->sink_window = mb_sink_window_new (self->session);
      gtk_application_add_window (self->app, GTK_WINDOW (self->sink_window));
      g_object_add_weak_pointer (G_OBJECT (self->sink_window),
                                 (gpointer *) &self->sink_window);
    }
  gtk_window_present (GTK_WINDOW (self->sink_window));
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

  self->cast_item = gtk_menu_item_new_with_label ("Cast to…");
  g_signal_connect (self->cast_item, "activate",
                    G_CALLBACK (cast_activate_cb), self);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), self->cast_item);

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
  app_indicator_set_menu (self->indicator, GTK_MENU (self->menu));

  g_signal_connect_object (session, "state-changed",
                           (GCallback) update_cb, self, G_CONNECT_SWAPPED);
  update_cb (self, session);

  return self;
}

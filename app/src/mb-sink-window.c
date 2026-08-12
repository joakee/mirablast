/* mirablast - sink chooser window
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-sink-window.h"
#include "nd-provider.h"

struct _MbSinkWindow
{
  GtkWindow   parent_instance;

  MbSession  *session;
  GtkListBox *list;
  GtkLabel   *status;
};

G_DEFINE_TYPE (MbSinkWindow, mb_sink_window, GTK_TYPE_WINDOW)

static GtkWidget *
row_for_sink (NdSink *sink)
{
  GtkWidget *row, *label;
  g_autofree gchar *name = NULL;

  g_object_get (sink, "display-name", &name, NULL);

  row = gtk_list_box_row_new ();
  label = gtk_label_new (name ? name : "(unnamed sink)");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_start (label, 12);
  gtk_widget_set_margin_end (label, 12);
  gtk_widget_set_margin_top (label, 10);
  gtk_widget_set_margin_bottom (label, 10);
  gtk_container_add (GTK_CONTAINER (row), label);
  g_object_set_data_full (G_OBJECT (row), "mb-sink",
                          g_object_ref (sink), g_object_unref);

  /* Follow renames (the name can arrive after discovery). */
  g_object_bind_property (sink, "display-name", label, "label",
                          G_BINDING_DEFAULT);

  gtk_widget_show_all (row);
  return row;
}

static void
sink_added_cb (MbSinkWindow *self, NdSink *sink, NdProvider *provider)
{
  gtk_container_add (GTK_CONTAINER (self->list), row_for_sink (sink));
}

static void
sink_removed_cb (MbSinkWindow *self, NdSink *sink, NdProvider *provider)
{
  g_autoptr(GList) children = gtk_container_get_children (GTK_CONTAINER (self->list));

  for (GList *l = children; l; l = l->next)
    {
      NdSink *row_sink = g_object_get_data (G_OBJECT (l->data), "mb-sink");
      if (row_sink == sink)
        {
          gtk_widget_destroy (GTK_WIDGET (l->data));
          return;
        }
    }
}

static void
row_activated_cb (MbSinkWindow *self, GtkListBoxRow *row, GtkListBox *list)
{
  NdSink *sink = g_object_get_data (G_OBJECT (row), "mb-sink");

  if (!sink)
    return;

  mb_session_connect_sink (self->session, sink);
  gtk_widget_hide (GTK_WIDGET (self));
}

static void
session_state_cb (MbSinkWindow *self, MbSession *session)
{
  g_autofree gchar *status = mb_session_dup_status (session);

  gtk_label_set_text (self->status, status);
}

static gboolean
hide_on_delete_cb (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide (widget);
  return TRUE; /* keep the window alive; the tray reopens it */
}

static void
mb_sink_window_dispose (GObject *object)
{
  MbSinkWindow *self = MB_SINK_WINDOW (object);

  g_clear_object (&self->session);
  G_OBJECT_CLASS (mb_sink_window_parent_class)->dispose (object);
}

static void
mb_sink_window_class_init (MbSinkWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = mb_sink_window_dispose;
}

static void
mb_sink_window_init (MbSinkWindow *self)
{
  GtkWidget *box, *scroll, *heading;

  gtk_window_set_title (GTK_WINDOW (self), "Mirablast — Cast to");
  gtk_window_set_default_size (GTK_WINDOW (self), 380, 340);
  gtk_window_set_position (GTK_WINDOW (self), GTK_WIN_POS_CENTER);
  g_signal_connect (self, "delete-event", G_CALLBACK (hide_on_delete_cb), NULL);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);

  heading = gtk_label_new ("Available video sinks");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
  gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

  scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_set_vexpand (scroll, TRUE);
  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_NONE);
  g_signal_connect_object (self->list, "row-activated",
                           (GCallback) row_activated_cb, self, G_CONNECT_SWAPPED);
  gtk_container_add (GTK_CONTAINER (scroll), GTK_WIDGET (self->list));
  gtk_box_pack_start (GTK_BOX (box), scroll, TRUE, TRUE, 0);

  self->status = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (self->status, 0.0);
  gtk_box_pack_end (GTK_BOX (box), GTK_WIDGET (self->status), FALSE, FALSE, 0);

  gtk_container_add (GTK_CONTAINER (self), box);
  gtk_widget_show_all (box);
}

MbSinkWindow *
mb_sink_window_new (MbSession *session)
{
  MbSinkWindow *self = g_object_new (MB_TYPE_SINK_WINDOW, NULL);
  NdMetaProvider *provider;
  g_autoptr(GList) sinks = NULL;

  self->session = g_object_ref (session);
  provider = mb_session_get_provider (session);

  g_signal_connect_object (provider, "sink-added",
                           (GCallback) sink_added_cb, self, G_CONNECT_SWAPPED);
  g_signal_connect_object (provider, "sink-removed",
                           (GCallback) sink_removed_cb, self, G_CONNECT_SWAPPED);
  g_signal_connect_object (session, "state-changed",
                           (GCallback) session_state_cb, self, G_CONNECT_SWAPPED);

  sinks = nd_provider_get_sinks (ND_PROVIDER (provider));
  for (GList *l = sinks; l; l = l->next)
    gtk_container_add (GTK_CONTAINER (self->list), row_for_sink (l->data));

  session_state_cb (self, session);
  return self;
}

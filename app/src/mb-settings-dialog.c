/* mirablast - settings dialog
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-settings-dialog.h"

typedef struct
{
  MbSettings *settings;
  GtkEntry   *resolution;
  GtkSpinButton *refresh;
  GtkSpinButton *latency;
  GtkSpinButton *gop;
  GtkSwitch  *damage;
  GtkSwitch  *reconnect;
  GtkEntry   *primary;
  GtkEntry   *helper;
} Widgets;

static GtkWidget *dialog_singleton;

static void
add_row (GtkGrid *grid, gint row, const gchar *text, GtkWidget *widget,
         const gchar *tooltip)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (grid, label, 0, row, 1, 1);
  gtk_widget_set_hexpand (widget, TRUE);
  gtk_grid_attach (grid, widget, 1, row, 1, 1);
  if (tooltip)
    {
      gtk_widget_set_tooltip_text (label, tooltip);
      gtk_widget_set_tooltip_text (widget, tooltip);
    }
}

static void
response_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
  Widgets *w = user_data;

  if (response == GTK_RESPONSE_OK)
    {
      g_autoptr(GError) error = NULL;
      guint pw, ph;

      mb_settings_set_resolution_string (w->settings,
                                         gtk_entry_get_text (w->resolution));
      if (!mb_settings_get_resolution (w->settings, &pw, &ph))
        {
          g_warning ("settings: invalid resolution, reverting to 1920x1080");
          mb_settings_set_resolution_string (w->settings, "1920x1080");
        }
      mb_settings_set_refresh (w->settings,
                               gtk_spin_button_get_value_as_int (w->refresh));
      mb_settings_set_latency_ms (w->settings,
                                  gtk_spin_button_get_value_as_int (w->latency));
      mb_settings_set_gop_seconds (w->settings,
                                   gtk_spin_button_get_value_as_int (w->gop));
      mb_settings_set_use_damage (w->settings, gtk_switch_get_active (w->damage));
      mb_settings_set_auto_reconnect (w->settings, gtk_switch_get_active (w->reconnect));
      mb_settings_set_primary (w->settings, gtk_entry_get_text (w->primary));
      mb_settings_set_helper (w->settings, gtk_entry_get_text (w->helper));

      if (!mb_settings_save (w->settings, &error))
        g_warning ("settings: save failed: %s", error->message);
      mb_settings_apply_environment (w->settings);
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

void
mb_settings_dialog_show (MbSettings *settings)
{
  GtkWidget *dialog, *content, *grid_w, *note;
  GtkGrid *grid;
  Widgets *w;
  gint row = 0;

  if (dialog_singleton)
    {
      gtk_window_present (GTK_WINDOW (dialog_singleton));
      return;
    }

  dialog = gtk_dialog_new_with_buttons ("Mirablast Settings", NULL, 0,
                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                        "_Save", GTK_RESPONSE_OK,
                                        NULL);
  dialog_singleton = dialog;
  g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &dialog_singleton);
  gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

  w = g_new0 (Widgets, 1);
  w->settings = settings;
  g_object_set_data_full (G_OBJECT (dialog), "mb-widgets", w, g_free);

  grid_w = gtk_grid_new ();
  grid = GTK_GRID (grid_w);
  gtk_grid_set_row_spacing (grid, 8);
  gtk_grid_set_column_spacing (grid, 18);
  gtk_container_set_border_width (GTK_CONTAINER (grid_w), 12);

  w->resolution = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_text (w->resolution, mb_settings_get_resolution_string (settings));
  add_row (grid, row++, "Resolution (WxH)", GTK_WIDGET (w->resolution),
           "Match what the sink negotiates (usually 1920x1080): a mismatch makes "
           "the encoder rescale every frame. Applies the next time the virtual "
           "display starts.");

  w->refresh = GTK_SPIN_BUTTON (gtk_spin_button_new_with_range (1, 240, 1));
  gtk_spin_button_set_value (w->refresh, mb_settings_get_refresh (settings));
  add_row (grid, row++, "Refresh rate (Hz)", GTK_WIDGET (w->refresh),
           "Refresh rate of the virtual display. Applies at next display start.");

  w->latency = GTK_SPIN_BUTTON (gtk_spin_button_new_with_range (0, 2000, 10));
  gtk_spin_button_set_value (w->latency, mb_settings_get_latency_ms (settings));
  add_row (grid, row++, "Pipeline latency (ms)", GTK_WIDGET (w->latency),
           "Lower is snappier; raise it if the picture stutters. Applies at next "
           "connect.");

  w->gop = GTK_SPIN_BUTTON (gtk_spin_button_new_with_range (1, 60, 1));
  gtk_spin_button_set_value (w->gop, mb_settings_get_gop_seconds (settings));
  add_row (grid, row++, "Keyframe interval (s)", GTK_WIDGET (w->gop),
           "Seconds between H.264 keyframes. Applies at next connect.");

  w->damage = GTK_SWITCH (gtk_switch_new ());
  gtk_switch_set_active (w->damage, mb_settings_get_use_damage (settings));
  gtk_widget_set_halign (GTK_WIDGET (w->damage), GTK_ALIGN_START);
  add_row (grid, row++, "XDamage tracking", GTK_WIDGET (w->damage),
           "Leave this ON. Turning it off makes the stream flicker and smear "
           "the cursor; the switch exists for experiments only.");

  w->reconnect = GTK_SWITCH (gtk_switch_new ());
  gtk_switch_set_active (w->reconnect, mb_settings_get_auto_reconnect (settings));
  gtk_widget_set_halign (GTK_WIDGET (w->reconnect), GTK_ALIGN_START);
  add_row (grid, row++, "Auto-reconnect", GTK_WIDGET (w->reconnect),
           "Re-establish the cast automatically when a display move resizes the "
           "desktop and breaks the capture (~20s outage).");

  w->primary = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_text (w->primary, mb_settings_get_primary (settings));
  gtk_entry_set_placeholder_text (w->primary, "autodetect");
  add_row (grid, row++, "Primary output", GTK_WIDGET (w->primary),
           "Output to extend from (e.g. eDP-1). Empty = whatever X calls primary.");

  w->helper = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_text (w->helper, mb_settings_get_helper (settings));
  gtk_entry_set_placeholder_text (w->helper, "autodiscover");
  add_row (grid, row++, "evdi helper path", GTK_WIDGET (w->helper),
           "Path to evdi-virtual-display. Empty = look next to the app, then "
           "~/.local/bin, then $PATH.");

  note = gtk_label_new ("Resolution and refresh apply at the next display start; "
                        "the rest at the next connect.");
  gtk_label_set_xalign (GTK_LABEL (note), 0.0);
  gtk_widget_set_margin_top (note, 6);
  gtk_grid_attach (grid, note, 0, row, 2, 1);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  gtk_container_add (GTK_CONTAINER (content), grid_w);

  g_signal_connect (dialog, "response", G_CALLBACK (response_cb), w);
  gtk_widget_show_all (dialog);
}

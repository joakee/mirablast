/* mirablast - about dialog
 *
 * Modelled on Thunar's About window (thunar_dialogs_show_about): a plain
 * GtkAboutDialog carrying the app icon as its logo, the program name and
 * version, a two-line comment, the copyright line, and Credits/License
 * buttons off the bottom row. Same field set, mirablast's contents.
 *
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mb-about-dialog.h"

#ifndef MIRABLAST_VERSION
#define MIRABLAST_VERSION "unknown"
#endif

/* Shown on the License page. GTK's built-in licence types cannot say "this
 * directory is GPL, that one is MIT", and their button is a no-op besides:
 * only a custom licence gives the slide-out page the Credits button gets. */
static const gchar license_text[] =
  "mirablast is licensed per directory; see the LICENSE files in the source "
  "tree for the authoritative terms.\n"
  "\n"
  "\n"
  "The application (app/), including the vendored gnome-network-displays "
  "backend it is built on, is licensed under the GNU General Public License, "
  "version 3 or later:\n"
  "\n"
  "This program is free software: you can redistribute it and/or modify it "
  "under the terms of the GNU General Public License as published by the Free "
  "Software Foundation, either version 3 of the License, or (at your option) "
  "any later version.\n"
  "\n"
  "This program is distributed in the hope that it will be useful, but "
  "WITHOUT ANY WARRANTY; without even the implied warranty of "
  "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General "
  "Public License for more details.\n"
  "\n"
  "You should have received a copy of the GNU General Public License along "
  "with this program. If not, see https://www.gnu.org/licenses/.\n"
  "\n"
  "\n"
  "The evdi helper (bin/) and the packaging (packaging/) are licensed under "
  "the MIT License:\n"
  "\n"
  "Copyright \302\251 2026 James Oakey\n"
  "\n"
  "Permission is hereby granted, free of charge, to any person obtaining a "
  "copy of this software and associated documentation files (the "
  "\"Software\"), to deal in the Software without restriction, including "
  "without limitation the rights to use, copy, modify, merge, publish, "
  "distribute, sublicense, and/or sell copies of the Software, and to permit "
  "persons to whom the Software is furnished to do so, subject to the "
  "following conditions:\n"
  "\n"
  "The above copyright notice and this permission notice shall be included in "
  "all copies or substantial portions of the Software.\n"
  "\n"
  "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS "
  "OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF "
  "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN "
  "NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, "
  "DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR "
  "OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE "
  "USE OR OTHER DEALINGS IN THE SOFTWARE.\n";

static GtkWidget *dialog_singleton;

static void
response_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

void
mb_about_dialog_show (void)
{
  static const gchar *authors[] = {
    "James Oakey",
    NULL
  };
  static const gchar *backend[] = {
    "the gnome-network-displays contributors",
    NULL
  };
  GtkAboutDialog *about;

  if (dialog_singleton)
    {
      gtk_window_present (GTK_WINDOW (dialog_singleton));
      return;
    }

  about = GTK_ABOUT_DIALOG (gtk_about_dialog_new ());
  dialog_singleton = GTK_WIDGET (about);
  g_object_add_weak_pointer (G_OBJECT (about), (gpointer *) &dialog_singleton);

  gtk_window_set_position (GTK_WINDOW (about), GTK_WIN_POS_CENTER);

  gtk_about_dialog_set_logo_icon_name (about, "video-display");
  gtk_about_dialog_set_program_name (about, "Mirablast");
  gtk_about_dialog_set_version (about, MIRABLAST_VERSION);
  gtk_about_dialog_set_comments (about,
                                 "Use a Miracast sink as a real extended display\n"
                                 "on X11.");
  gtk_about_dialog_set_copyright (about, "Copyright \302\251 2026 James Oakey");
  /* set_license implies GTK_LICENSE_CUSTOM, which is what puts the licence on
   * its own page behind the button instead of a one-line footer link. */
  gtk_about_dialog_set_license (about, license_text);
  gtk_about_dialog_set_wrap_license (about, TRUE);
  gtk_about_dialog_set_website (about, "https://github.com/joakee/mirablast");
  gtk_about_dialog_set_website_label (about, "mirablast on GitHub");
  gtk_about_dialog_set_authors (about, authors);

  /* The WFD half of the app is vendored, so it belongs in Credits. */
  gtk_about_dialog_add_credit_section (about, "Miracast backend", backend);

  g_signal_connect (about, "response", G_CALLBACK (response_cb), NULL);
  /* Not show_all: the dialog decides which of its own buttons belong on the
   * action row, and show_all would force the hidden ones back on. */
  gtk_widget_show (GTK_WIDGET (about));
}

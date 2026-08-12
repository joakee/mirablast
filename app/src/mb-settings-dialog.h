/* mirablast - settings dialog
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include "mb-settings.h"

G_BEGIN_DECLS

/* Modal-less dialog; writes back and saves on OK. */
void mb_settings_dialog_show (MbSettings *settings);

G_END_DECLS

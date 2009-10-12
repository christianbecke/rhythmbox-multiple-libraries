/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *  Copyright (C) 2009 Christian Becke <christianbecke@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  The Rhythmbox authors hereby grant permission for non-GPL compatible
 *  GStreamer plugins to be used and distributed together with GStreamer
 *  and Rhythmbox. This permission is above and beyond the permissions granted
 *  by the GPL license by which Rhythmbox is covered. If you modify this code
 *  you may extend this exception to your version of the code, but you are not
 *  obligated to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 */

#ifndef __RB_LIBRARY_PREFERENCES_H
#define __RB_LIBRARY_PREFERENCES_H

#include <gtk/gtk.h>

#include "eel-gconf-extensions.h"
#include "rb-shell.h"
#include "rb-library-source.h"

G_BEGIN_DECLS

#define CONF_UI_LIBRARY_DIR CONF_PREFIX "/ui/library"
#define CONF_STATE_LIBRARY_DIR CONF_PREFIX "/state/library"
#define CONF_STATE_LIBRARY_SORTING CONF_PREFIX "/state/library/sorting"
#define CONF_STATE_PANED_POSITION CONF_PREFIX "/state/library/paned_position"
#define CONF_STATE_SHOW_BROWSER   CONF_PREFIX "/state/library/show_browser"

GtkWidget *rb_library_prefs_get_config_widget (RBSource *source, RBShellPreferences *prefs);

/* gconf notification functions */
void rb_library_prefs_library_location_changed (GConfClient *client,
						    guint cnxn_id,
						    GConfEntry *entry,
						    RBLibrarySource *source);
void rb_library_prefs_layout_path_changed (GConfClient *client,
						   guint cnxn_id,
						   GConfEntry *entry,
						   RBLibrarySource *source);
void rb_library_prefs_layout_filename_changed (GConfClient *client,
						       guint cnxn_id,
						       GConfEntry *entry,
						       RBLibrarySource *source);
void rb_library_prefs_ui_pref_changed (GConfClient *client,
					       guint cnxn_id,
					       GConfEntry *entry,
					       RBLibrarySource *source);
void rb_library_prefs_monitor_library_locations_changed (GConfClient *client,
					    guint cnxn_id,
					    GConfEntry *entry,
					    RBLibrarySource *source);
void rb_library_prefs_default_library_location_changed (GConfClient *client,
					    guint cnxn_id,
					    GConfEntry *entry,
					    RBLibrarySource *source);

/* functions for syncing ui and gconf state */
void rb_library_prefs_ui_prefs_sync (RBLibrarySource *source);
void rb_library_prefs_sync_default_library_location (RBLibrarySource *source, gboolean disable_notification);
void rb_library_prefs_sync_monitored_locations (RBLibrarySource *source, gboolean disable_notification);

G_END_DECLS

#endif /* __RB_LIBRARY_PREFERENCES_H */

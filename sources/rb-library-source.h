/*
 *  Copyright (C) 2002 Jorn Baayen <jorn@nl.linux.org>
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

#ifndef __RB_LIBRARY_SOURCE_H
#define __RB_LIBRARY_SOURCE_H

#include <gtk/gtk.h>

#include <shell/rb-shell.h>
#include <sources/rb-browser-source.h>
#include <rhythmdb/rhythmdb.h>
#include "rb-library-child-source.h"

G_BEGIN_DECLS

#define RB_TYPE_LIBRARY_SOURCE         (rb_library_source_get_type ())
#define RB_LIBRARY_SOURCE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), RB_TYPE_LIBRARY_SOURCE, RBLibrarySource))
#define RB_LIBRARY_SOURCE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), RB_TYPE_LIBRARY_SOURCE, RBLibrarySourceClass))
#define RB_IS_LIBRARY_SOURCE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), RB_TYPE_LIBRARY_SOURCE))
#define RB_IS_LIBRARY_SOURCE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), RB_TYPE_LIBRARY_SOURCE))
#define RB_LIBRARY_SOURCE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), RB_TYPE_LIBRARY_SOURCE, RBLibrarySourceClass))

typedef struct _RBLibrarySource RBLibrarySource;
typedef struct _RBLibrarySourceClass RBLibrarySourceClass;

typedef struct _RBLibrarySourcePrivate RBLibrarySourcePrivate;

struct _RBLibrarySource
{
	RBBrowserSource parent;

	RBLibrarySourcePrivate *priv;
};

struct _RBLibrarySourceClass
{
	RBBrowserSourceClass parent;
};

struct _RBLibrarySourcePrivate
{
	RhythmDB *db;

	gboolean loading_prefs;
	RBShellPreferences *shell_prefs;

	GtkWidget *config_widget;

	GList *child_sources;

	GtkWidget *library_location_entry;
	GtkWidget *layout_path_menu;
	GtkWidget *layout_filename_menu;
	GtkWidget *preferred_format_menu;
	GtkWidget *layout_example_label;
	GtkWidget *locations_tree;

	GList *import_jobs;
	guint start_import_job_id;

	guint library_location_notify_id;
	guint default_library_location_notify_id;
	guint ui_dir_notify_id;
	guint layout_path_notify_id;
	guint layout_filename_notify_id;
	guint monitor_library_locations_notify_id;
};

GType		rb_library_source_get_type		(void);

RBSource *      rb_library_source_new			(RBShell *shell);

RBLibraryChildSource * rb_library_source_get_child_source_for_uri (RBLibrarySource *source, const char *uri);

void rb_library_source_sync_child_sources (RBLibrarySource *source);

void rb_library_source_move_files (RBLibrarySource *source, RBLibraryChildSource *dest_source, GList *entries);

G_END_DECLS

#endif /* __RB_LIBRARY_SOURCE_H */

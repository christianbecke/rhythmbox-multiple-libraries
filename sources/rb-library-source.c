/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *  Copyright (C) 2002 Jorn Baayen <jorn@nl.linux.org>
 *  Copyright (C) 2003,2004 Colin Walters <walters@verbum.org>
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

/**
 * SECTION:rb-library-source
 * @short_description: main library source, containing all local songs
 *
 * The library source contains all local songs that have been imported
 * into the database.
 *
 * It provides a preferences page for configuring the library location,
 * the directory structure to use when transferring new files into
 * the library from another source, and the preferred audio encoding
 * to use.
 *
 * If multiple library locations are set in GConf, the library source
 * creates a child source for each location, which will only show
 * files found under that location.
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <profiles/gnome-media-profiles.h>

#include "rhythmdb.h"
#include "rb-debug.h"
#include "rb-dialog.h"
#include "rb-file-helpers.h"
#include "rb-util.h"
#include "eel-gconf-extensions.h"
#include "rb-library-source.h"
#include "rb-library-preferences.h"
#include "rb-library-file-helpers.h"
#include "rb-removable-media-manager.h"
#include "rb-library-child-source.h"

static void rb_library_source_class_init (RBLibrarySourceClass *klass);
static void rb_library_source_init (RBLibrarySource *source);
static void rb_library_source_constructed (GObject *object);
static void rb_library_source_dispose (GObject *object);
static void rb_library_source_finalize (GObject *object);
static void rb_library_source_set_property (GObject *object,
		guint prop_id,
		const GValue *value,
		GParamSpec *pspec);
static void rb_library_source_get_property (GObject *object,
		guint prop_id,
		GValue *value,
		GParamSpec *pspec);

/* RBSource implementations */
static gboolean impl_show_popup (RBSource *source);
static char *impl_get_browser_key (RBSource *source);
static char *impl_get_paned_key (RBBrowserSource *source);
static gboolean impl_receive_drag (RBSource *source, GtkSelectionData *data);
static gboolean impl_can_paste (RBSource *asource);
static void impl_paste (RBSource *source, GList *entries);
static guint impl_want_uri (RBSource *source, const char *uri);
static gboolean impl_add_uri (RBSource *source, const char *uri, const char *title, const char *genre);
static void impl_get_status (RBSource *source, char **text, char **progress_text, float *progress);
static RhythmDBImportJob *maybe_create_import_job (RBLibrarySource *source);
static void rb_library_source_add_child_source (RBLibrarySource *library_source,
		const char *uri);
static void rb_library_source_remove_child_source (RBLibrarySource *library_source,
		RBLibraryChildSource *child_source);
void rb_library_source_sync_child_sources (RBLibrarySource *source);

#define RB_LIBRARY_SOURCE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), RB_TYPE_LIBRARY_SOURCE, RBLibrarySourcePrivate))
G_DEFINE_TYPE (RBLibrarySource, rb_library_source, RB_TYPE_BROWSER_SOURCE)

enum
{
	PROP_0,
	PROP_CHILD_SOURCES,
};

static void
rb_library_source_class_init (RBLibrarySourceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RBSourceClass *source_class = RB_SOURCE_CLASS (klass);
	RBBrowserSourceClass *browser_source_class = RB_BROWSER_SOURCE_CLASS (klass);

	object_class->dispose = rb_library_source_dispose;
	object_class->finalize = rb_library_source_finalize;
	object_class->constructed = rb_library_source_constructed;
	object_class->set_property = rb_library_source_set_property;
	object_class->get_property = rb_library_source_get_property;

	source_class->impl_show_popup = impl_show_popup;
	source_class->impl_get_config_widget = rb_library_prefs_get_config_widget;
	source_class->impl_get_browser_key = impl_get_browser_key;
	source_class->impl_receive_drag = impl_receive_drag;
	source_class->impl_can_copy = (RBSourceFeatureFunc) rb_true_function;
	source_class->impl_can_paste = (RBSourceFeatureFunc) impl_can_paste;
	source_class->impl_paste = impl_paste;
	source_class->impl_want_uri = impl_want_uri;
	source_class->impl_add_uri = impl_add_uri;
	source_class->impl_get_status = impl_get_status;

	browser_source_class->impl_get_paned_key = impl_get_paned_key;
	browser_source_class->impl_has_drop_support = (RBBrowserSourceFeatureFunc) rb_true_function;

	g_object_class_install_property (object_class,
			PROP_CHILD_SOURCES,
			g_param_spec_pointer ("child-sources",
				"child-sources",
				"list of RBLibraryChildSource objects",
				G_PARAM_READABLE));

	g_type_class_add_private (klass, sizeof (RBLibrarySourcePrivate));

	gnome_media_profiles_init (eel_gconf_client_get_global ());
}

static void
rb_library_source_init (RBLibrarySource *source)
{
	source->priv = RB_LIBRARY_SOURCE_GET_PRIVATE (source);
}

static void
rb_library_source_dispose (GObject *object)
{
	RBLibrarySource *source;
	source = RB_LIBRARY_SOURCE (object);

	if (source->priv->shell_prefs) {
		g_object_unref (source->priv->shell_prefs);
		source->priv->shell_prefs = NULL;
	}

	if (source->priv->db) {
		g_object_unref (source->priv->db);
		source->priv->db = NULL;
	}

	if (source->priv->ui_dir_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->ui_dir_notify_id);
		source->priv->ui_dir_notify_id = 0;
	}

	if (source->priv->library_location_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->library_location_notify_id);
		source->priv->library_location_notify_id = 0;
	}

	if (source->priv->default_library_location_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->default_library_location_notify_id);
		source->priv->default_library_location_notify_id = 0;
	}

	if (source->priv->layout_path_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->layout_path_notify_id);
		source->priv->layout_path_notify_id = 0;
	}

	if (source->priv->layout_filename_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->layout_filename_notify_id);
		source->priv->layout_filename_notify_id = 0;
	}

	if (source->priv->import_jobs != NULL) {
		GList *t;
		if (source->priv->start_import_job_id != 0) {
			g_source_remove (source->priv->start_import_job_id);
			source->priv->start_import_job_id = 0;
		}
		for (t = source->priv->import_jobs; t != NULL; t = t->next) {
			RhythmDBImportJob *job = RHYTHMDB_IMPORT_JOB (t->data);
			rhythmdb_import_job_cancel (job);
			g_object_unref (job);
		}
		g_list_free (source->priv->import_jobs);
		source->priv->import_jobs = NULL;
	}
	
	if (source->priv->monitor_library_locations_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->monitor_library_locations_notify_id);
		source->priv->monitor_library_locations_notify_id = 0;
	}

	G_OBJECT_CLASS (rb_library_source_parent_class)->dispose (object);
}

static void
rb_library_source_finalize (GObject *object)
{
	RBLibrarySource *source;

	g_return_if_fail (object != NULL);
	g_return_if_fail (RB_IS_LIBRARY_SOURCE (object));

	source = RB_LIBRARY_SOURCE (object);

	g_return_if_fail (source->priv != NULL);

	rb_debug ("finalizing library source");

	G_OBJECT_CLASS (rb_library_source_parent_class)->finalize (object);
}

static void
rb_library_source_set_property (GObject *object,
		guint prop_id,
		const GValue *value,
		GParamSpec *pspec)
{
	RBLibrarySource *source = RB_LIBRARY_SOURCE (object);

	switch (prop_id) {
		case PROP_CHILD_SOURCES:
			if (source->priv->child_sources != NULL) {
				g_list_free (source->priv->child_sources);
			}
			source->priv->child_sources = g_list_copy ((GList *) g_value_get_pointer (value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
rb_library_source_get_property (GObject *object,
		guint prop_id,
		GValue *value,
		GParamSpec *pspec)
{
	RBLibrarySource *source = RB_LIBRARY_SOURCE (object);

	switch (prop_id) {
		case PROP_CHILD_SOURCES:
			g_value_set_pointer (value, g_list_copy (source->priv->child_sources));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static gboolean
add_child_sources_idle (RBLibrarySource *source)
{
	GDK_THREADS_ENTER ();
	rb_library_source_sync_child_sources (source);
	GDK_THREADS_LEAVE ();

	return FALSE;
}

static void
db_load_complete_cb (RhythmDB *db, RBLibrarySource *source)
{
	/* once the database is loaded, we can run the query to populate the library source */
	g_object_set (source, "populate", TRUE, NULL);
}

static void
rb_library_source_constructed (GObject *object)
{
	RBLibrarySource *source;
	RBShell *shell;
	RBEntryView *songs;
	GSList *list, *monitor_locations;

	RB_CHAIN_GOBJECT_METHOD (rb_library_source_parent_class, constructed, object);
	source = RB_LIBRARY_SOURCE (object);

	g_object_get (source, "shell", &shell, NULL);
	g_object_get (shell, "db", &source->priv->db, NULL);

	g_signal_connect_object (source->priv->db, "load-complete", G_CALLBACK (db_load_complete_cb), source, 0);

	rb_library_prefs_ui_prefs_sync (source);

	/* Set up a library location if there's no library location set */
	list = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);
	if (g_slist_length (list) == 0) {
		char *music_dir_uri;

		music_dir_uri = g_filename_to_uri (rb_music_dir (), NULL, NULL);
		if (music_dir_uri != NULL) {
			list = g_slist_prepend (list, music_dir_uri);
			eel_gconf_set_string_list (CONF_LIBRARY_LOCATION, list);
		}
	} else {
		/* ensure all library locations are URIs and not file paths */
		GSList *t;
		gboolean update = FALSE;
		for (t = list; t != NULL; t = t->next) {
			char *location;

			location = (char *)t->data;
			if (location[0] == '/') {
				char *uri = g_filename_to_uri (location, NULL, NULL);
				if (uri != NULL) {
					rb_debug ("converting library location path %s to URI %s", location, uri);
					g_free (location);
					t->data = uri;
					update = TRUE;
				}
			}

			location = rb_library_check_append_slash ((char *)t->data);
			if (location && t->data && strcmp (location, (char *)t->data) != 0) {
				g_free (t->data);
				t->data = location;
				update = TRUE;
			}
		}

		if (update) {
			eel_gconf_set_string_list (CONF_LIBRARY_LOCATION, list);
		}
	}

	/* migrate CONF_MONITOR_LIBRARY setting */
	monitor_locations = eel_gconf_get_string_list (CONF_MONITOR_LIBRARY_LOCATIONS);
	if (monitor_locations == NULL && eel_gconf_get_boolean (CONF_MONITOR_LIBRARY)) {
		rb_debug ("CONF_MONITOR_LIBRARY was set, migrating all library locations to CONF_MONITOR_LIBRARY_LOCATIONS");
		eel_gconf_set_string_list (CONF_MONITOR_LIBRARY_LOCATIONS, list);
	}
	rb_slist_deep_free (monitor_locations);
	rb_slist_deep_free (list);

	/* make sure migration is only done once */
	eel_gconf_recursive_unset (CONF_MONITOR_LIBRARY);

	/* make sure default library location is sane */
	rb_library_prefs_sync_default_library_location (source, TRUE);

	/* make sure monitored library locations are sane */
	rb_library_prefs_sync_monitored_locations (source, TRUE);

	source->priv->library_location_notify_id =
		eel_gconf_notification_add (CONF_LIBRARY_LOCATION,
				    (GConfClientNotifyFunc) rb_library_prefs_library_location_changed, source);

	source->priv->default_library_location_notify_id =
		eel_gconf_notification_add (CONF_DEFAULT_LIBRARY_LOCATION,
				(GConfClientNotifyFunc) rb_library_prefs_default_library_location_changed, source);

	source->priv->ui_dir_notify_id =
		eel_gconf_notification_add (CONF_UI_LIBRARY_DIR,
				    (GConfClientNotifyFunc) rb_library_prefs_ui_pref_changed, source);

	source->priv->monitor_library_locations_notify_id =
		eel_gconf_notification_add (CONF_MONITOR_LIBRARY_LOCATIONS,
				(GConfClientNotifyFunc) rb_library_prefs_monitor_library_locations_changed, source);

	songs = rb_source_get_entry_view (RB_SOURCE (source));

	rb_entry_view_append_column (songs, RB_ENTRY_VIEW_COL_RATING, FALSE);
	rb_entry_view_append_column (songs, RB_ENTRY_VIEW_COL_LAST_PLAYED, FALSE);
	rb_entry_view_append_column (songs, RB_ENTRY_VIEW_COL_FIRST_SEEN, FALSE);

	g_idle_add ((GSourceFunc)add_child_sources_idle, source);

	g_object_unref (shell);
}

/**
 * rb_library_source_new:
 * @shell: the #RBShell
 *
 * Creates and returns the #RBLibrarySource instance
 *
 * Return value: the #RBLibrarySource
 */
RBSource *
rb_library_source_new (RBShell *shell)
{
	RBSource *source;
	GdkPixbuf *icon;
	gint size;
	RhythmDBEntryType entry_type;

	entry_type = RHYTHMDB_ENTRY_TYPE_SONG;

	gtk_icon_size_lookup (RB_SOURCE_ICON_SIZE, &size, NULL);
	icon = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
					 "audio-x-generic",
					 size,
					 0, NULL);
	source = RB_SOURCE (g_object_new (RB_TYPE_LIBRARY_SOURCE,
					  "name", _("Music"),
					  "entry-type", entry_type,
					  "source-group", RB_SOURCE_GROUP_LIBRARY,
					  "sorting-key", CONF_STATE_LIBRARY_SORTING,
					  "shell", shell,
					  "icon", icon,
					  "populate", FALSE,		/* wait until the database is loaded */
					  NULL));
	if (icon != NULL) {
		g_object_unref (icon);
	}

	rb_shell_register_entry_type_for_source (shell, source, entry_type);

	return source;
}

RBLibraryChildSource *
rb_library_source_get_child_source_for_uri (RBLibrarySource *source, const char *uri)
{
	GList *i;
	RBLibraryChildSource *child_source, *found = NULL;
	char *source_uri;

	rb_debug ("searching child_source for uri '%s'", uri);

	g_return_val_if_fail (uri != NULL, NULL);

	for (i = source->priv->child_sources; i != NULL; i = g_list_next (i)) {
		child_source = (RBLibraryChildSource *)i->data;
		g_object_get (G_OBJECT (child_source),
				"uri", &source_uri,
				NULL);

		g_assert (source_uri != NULL);

		if (strcmp (source_uri, uri) == 0) {
			rb_debug ("found child_source [%p]", child_source);
			found = child_source;
		}
		g_free (source_uri);

		if (found != NULL) {
			break;
		}
	}

	return found;
}

static char *
impl_get_browser_key (RBSource *source)
{
	return g_strdup (CONF_STATE_SHOW_BROWSER);
}

static char *
impl_get_paned_key (RBBrowserSource *status)
{
	return g_strdup (CONF_STATE_PANED_POSITION);
}

static gboolean
impl_receive_drag (RBSource *asource, GtkSelectionData *data)
{
	RBLibrarySource *source = RB_LIBRARY_SOURCE (asource);
	GList *list, *i;
	GList *entries = NULL;
	gboolean is_id;

	rb_debug ("parsing uri list");
	list = rb_uri_list_parse ((const char *) gtk_selection_data_get_data (data));
	is_id = (gtk_selection_data_get_data_type (data) == gdk_atom_intern ("application/x-rhythmbox-entry", TRUE));

	for (i = list; i != NULL; i = g_list_next (i)) {
		if (i->data != NULL) {
			char *uri = i->data;
			RhythmDBEntry *entry;

			entry = rhythmdb_entry_lookup_from_string (source->priv->db, uri, is_id);

			if (entry == NULL) {
				RhythmDBImportJob *job;
				/* add to the library */
				job = maybe_create_import_job (source);
				rhythmdb_import_job_add_uri (job, uri);
			} else {
				/* add to list of entries to copy */
				entries = g_list_prepend (entries, entry);
			}

			g_free (uri);
		}
	}

	if (entries) {
		entries = g_list_reverse (entries);
		if (rb_source_can_paste (asource))
			rb_source_paste (asource, entries);
		g_list_free (entries);
	}

	g_list_free (list);
	return TRUE;
}

static gboolean
impl_show_popup (RBSource *source)
{
	_rb_source_show_popup (source, "/LibrarySourcePopup");
	return TRUE;
}

static gboolean
impl_can_paste (RBSource *asource)
{
	gboolean can_paste = TRUE;
	char *str;

	str = eel_gconf_get_string (CONF_DEFAULT_LIBRARY_LOCATION);
	can_paste &= (str != NULL);
	g_free (str);

	str = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_PATH);
	can_paste &= (str != NULL);
	g_free (str);

	str = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_FILENAME);
	can_paste &= (str != NULL);
	g_free (str);

	str = eel_gconf_get_string (CONF_LIBRARY_PREFERRED_FORMAT);
	can_paste &= (str != NULL);
	g_free (str);
	return can_paste;
}

static void
paste_completed_cb (RhythmDBEntry *entry, const char *dest, guint64 dest_size, GError *error, RBLibrarySource *source)
{
	if (error == NULL) {
		rhythmdb_add_uri (source->priv->db, dest);
	} else {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
			rb_debug ("not displaying 'file exists' error for %s", dest);
		} else {
			rb_error_dialog (NULL, _("Error transferring track"), "%s", error->message);
		}
	}
}

static void
move_completed_cb (RhythmDBEntry *entry, const char *dest, GError *transfer_error, RBLibrarySource *source)
{
	if (transfer_error != NULL) {
		if (g_error_matches (transfer_error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
			rb_debug ("not displaying 'file exists' error for %s", dest);
		} else {
			rb_error_dialog (NULL, _("Error moving track"), "%s", transfer_error->message);
		}
		return;
	}

	char *uri;
	GFile *file;
	GError *err = NULL;

	uri = g_strdup (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION));
	rhythmdb_entry_delete (source->priv->db, entry);
	rhythmdb_add_uri (source->priv->db, dest);

	file = g_file_new_for_uri (uri);
	if (! g_file_delete (file, NULL, &err)) {
		if (err != NULL) {
			rb_debug ("Could not delete '%s': %s",
					uri, err->message);
			g_error_free (err);
		} else {
			rb_debug ("Could not delete '%s': unknown error", uri);
		}
	}
	g_object_unref (file);
	g_free (uri);
}

static void
do_paste (RBLibrarySource *source,
		const char *dest_dir,
		GList *entries,
		RBTransferCompleteCallback callback)
{
	RBRemovableMediaManager *rm_mgr;
	GList *l;
	RBShell *shell;
	RhythmDBEntryType source_entry_type;

	g_assert (dest_dir != NULL);
	g_assert (callback != NULL);

	if (impl_can_paste (RB_SOURCE (source)) == FALSE) {
		g_warning ("RBLibrarySource impl_paste called when gconf keys unset");
		return;
	}

	g_object_get (source,
		      "shell", &shell,
		      "entry-type", &source_entry_type,
		      NULL);
	g_object_get (shell, "removable-media-manager", &rm_mgr, NULL);
	g_object_unref (shell);

	for (l = entries; l != NULL; l = g_list_next (l)) {
		RhythmDBEntry *entry = (RhythmDBEntry *)l->data;
		RhythmDBEntryType entry_type;
		RBSource *source_source;
		const char *location;
		char *dest;
		char *sane_dest;

		location = rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION);

		rb_debug ("pasting entry %s", location);

		entry_type = rhythmdb_entry_get_entry_type (entry);
		if (entry_type == source_entry_type && g_str_has_prefix (location, dest_dir)) {
			rb_debug ("can't copy an entry from the library to itself");
			continue;
		}

		/* see if the responsible source lets us copy */
		source_source = rb_shell_get_source_by_entry_type (shell, entry_type);
		if ((source_source != NULL) && !rb_source_can_copy (source_source)) {
			rb_debug ("source for the entry doesn't want us to copy it");
			continue;
		}

		dest = rb_library_build_filename (source->priv->db, dest_dir, entry);
		if (dest == NULL) {
			rb_debug ("could not create destination path for entry");
			continue;
		}

		sane_dest = rb_sanitize_uri_for_filesystem (dest);
		g_free (dest);

		rb_removable_media_manager_queue_transfer (rm_mgr, entry,
							  sane_dest, NULL,
							  callback, source);
	}
	g_boxed_free (RHYTHMDB_TYPE_ENTRY_TYPE, source_entry_type);

	g_object_unref (rm_mgr);
}

static void
impl_paste (RBSource *asource, GList *entries)
{
	char *dest_dir;

	dest_dir = eel_gconf_get_string (CONF_DEFAULT_LIBRARY_LOCATION);
	do_paste (RB_LIBRARY_SOURCE (asource),
			dest_dir,
			entries,
			(RBTransferCompleteCallback) paste_completed_cb);
	g_free (dest_dir);
}

void
rb_library_source_move_files (RBLibrarySource *source, RBLibraryChildSource *dest_source, GList *entries)
{
	char *dest;

	g_assert (RB_IS_LIBRARY_SOURCE (source));
	g_assert (RB_IS_LIBRARY_CHILD_SOURCE (dest_source));

	g_object_get (dest_source, "uri", &dest, NULL);
	g_assert (dest != NULL);

	do_paste (source, dest, entries, (RBTransferCompleteCallback) move_completed_cb);
	g_free (dest);
}

static guint
impl_want_uri (RBSource *source, const char *uri)
{
	/* assume anything local, on smb, or on sftp is a song */
	if (rb_uri_is_local (uri) ||
	    g_str_has_prefix (uri, "smb://") ||
	    g_str_has_prefix (uri, "sftp://") ||
	    g_str_has_prefix (uri, "ssh://"))
		return 50;

	return 0;
}

static void
import_job_status_changed_cb (RhythmDBImportJob *job, int total, int imported, RBLibrarySource *source)
{
	RhythmDBImportJob *head = RHYTHMDB_IMPORT_JOB (source->priv->import_jobs->data);
	if (job == head) {		/* it was inevitable */
		rb_source_notify_status_changed (RB_SOURCE (source));
	}
}

static void
import_job_complete_cb (RhythmDBImportJob *job, int total, RBLibrarySource *source)
{
	rb_debug ("import job complete");

	/* maybe show a notification here? */

	source->priv->import_jobs = g_list_remove (source->priv->import_jobs, job);
	g_object_unref (job);
}

static gboolean
start_import_job (RBLibrarySource *source)
{
	RhythmDBImportJob *job;
	source->priv->start_import_job_id = 0;

	rb_debug ("starting import job");
	job = RHYTHMDB_IMPORT_JOB (source->priv->import_jobs->data);

	rhythmdb_import_job_start (job);

	return FALSE;
}

static RhythmDBImportJob *
maybe_create_import_job (RBLibrarySource *source)
{
	RhythmDBImportJob *job;
	if (source->priv->import_jobs == NULL || source->priv->start_import_job_id == 0) {
		rb_debug ("creating new import job");
		job = rhythmdb_import_job_new (source->priv->db,
					       RHYTHMDB_ENTRY_TYPE_SONG,
					       RHYTHMDB_ENTRY_TYPE_IGNORE,
					       RHYTHMDB_ENTRY_TYPE_IMPORT_ERROR);
		g_signal_connect_object (job,
					 "status-changed",
					 G_CALLBACK (import_job_status_changed_cb),
					 source, 0);
		g_signal_connect_object (job,
					 "complete",
					 G_CALLBACK (import_job_complete_cb),
					 source, 0);
		source->priv->import_jobs = g_list_prepend (source->priv->import_jobs, job);
	} else {
		rb_debug ("using existing unstarted import job");
		job = RHYTHMDB_IMPORT_JOB (source->priv->import_jobs->data);
	}

	/* allow some time for more URIs to be added if we're importing a bunch of things */
	if (source->priv->start_import_job_id != 0) {
		g_source_remove (source->priv->start_import_job_id);
	}
	source->priv->start_import_job_id = g_timeout_add (250, (GSourceFunc) start_import_job, source);

	return job;
}

static gboolean
impl_add_uri (RBSource *asource, const char *uri, const char *title, const char *genre)
{
	RBLibrarySource *source = RB_LIBRARY_SOURCE (asource);
	RhythmDBImportJob *job;

	job = maybe_create_import_job (source);

	rb_debug ("adding uri %s to library", uri);
	rhythmdb_import_job_add_uri (job, uri);
	return TRUE;
}

static void
rb_library_source_add_child_source (RBLibrarySource *library_source, const char *uri)
{
	RBSource *source;
	RBShell *shell;

	source = rb_library_child_source_new (RB_SOURCE (library_source), uri);
	if (source == NULL) {
		/* URI does not exist. Could be e.g. on a network share
		 * currently not reachable or could be gone forever. Do we need
		 * an error message for this? */
		rb_debug ("failed to create child source for '%s'", uri);
		return;
	}

	g_object_get (library_source,
		      "shell", &shell,
		      NULL);

	rb_shell_append_source (shell, source, RB_SOURCE (library_source));
	library_source->priv->child_sources = g_list_prepend (library_source->priv->child_sources, source);

	rb_debug ("added child source [%p] for uri '%s'", source, uri);

	g_object_unref (shell);
}

static void
rb_library_source_remove_child_source (RBLibrarySource *library_source,
		RBLibraryChildSource *child_source)
{
	char *uri;

	g_object_get (G_OBJECT (child_source), "uri", &uri, NULL);
	rb_debug ("removing child source [%p] for uri '%s'", child_source, uri);
	g_free (uri);

	library_source->priv->child_sources = g_list_remove (library_source->priv->child_sources, child_source);
	rb_source_delete_thyself (RB_SOURCE (child_source));
}

void
rb_library_source_sync_child_sources (RBLibrarySource *source)
{
	GSList *locations, *l;
	RBLibraryChildSource *found;
	GList *child_sources, *s;
	char *uri;

	locations = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);
	if (g_slist_length (locations) > 1) {
		locations = g_slist_reverse (locations);
		for (l = locations; l != NULL; l = g_slist_next (l)) {
			found = rb_library_source_get_child_source_for_uri (source,
					(char *)l->data);
			if (found == NULL) {
				rb_library_source_add_child_source (source, (char *)l->data);
			}
		}
	}

	child_sources = g_list_copy (source->priv->child_sources);
	for (s = child_sources; s != NULL; s = g_list_next (s)) {
		g_object_get (G_OBJECT (s->data),
				"uri", &uri,
				NULL);

		g_assert (uri != NULL);

		if (! rb_string_slist_contains (locations, uri)) {
			rb_library_source_remove_child_source (source, RB_LIBRARY_CHILD_SOURCE (s->data));
		}

		g_free (uri);
	}

	if (g_list_length (source->priv->child_sources) < 2) {
		if (source->priv->child_sources != NULL && source->priv->child_sources->data != NULL) {
			rb_debug ("only one child source, removing it");
			rb_library_source_remove_child_source (source,
					RB_LIBRARY_CHILD_SOURCE (source->priv->child_sources->data));
		}
	}
	rb_slist_deep_free (locations);
	g_list_free (child_sources);
}

static void
impl_get_status (RBSource *source, char **text, char **progress_text, float *progress)
{
	RB_SOURCE_CLASS (rb_library_source_parent_class)->impl_get_status (source, text, progress_text, progress);
	RBLibrarySource *lsource = RB_LIBRARY_SOURCE (source);

	if (lsource->priv->import_jobs != NULL) {
		RhythmDBImportJob *job = RHYTHMDB_IMPORT_JOB (lsource->priv->import_jobs->data);
		_rb_source_set_import_status (source, job, progress_text, progress);
	}
}

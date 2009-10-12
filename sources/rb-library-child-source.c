/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *  Copyright (C) 2009 Christian Becke <christianbecke@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  The Rhythmbox authors hereby grants permission for non-GPL compatible
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

#include "config.h"

#include <string.h>

#include <gconf/gconf.h>

#include "rhythmdb.h"
#include "rb-debug.h"
#include "eel-gconf-extensions.h"
#include "rb-preferences.h"
#include "rb-library-file-helpers.h"
#include "rb-browser-source.h"
#include "rb-library-child-source.h"

static void rb_library_child_source_init (RBLibraryChildSource *source);
static void rb_library_child_source_finalize (GObject *object);
static void rb_library_child_source_set_property (GObject *object,
							guint prop_id,
							const GValue *value,
							GParamSpec *pspec);
static void rb_library_child_source_get_property (GObject *object,
							guint prop_id,
							GValue *value,
							GParamSpec *pspec);

static char *get_state_dir (const char *uri);
static gboolean delete_entry (GtkTreeModel *model,
				GtkTreePath *path,
				GtkTreeIter *iter,
				RhythmDB *db);

/* RBSource implementations */
static char *impl_get_browser_key (RBSource *source);
static char *impl_get_paned_key (RBBrowserSource *source);

#define CONF_STATE_LIBRARY_DIR CONF_PREFIX "/state/library" /* Move this one to rb-preferences.h? */
#define CONF_STATE_LIBRARY_CHILD_SORTING "/sorting"
#define CONF_STATE_LIBRARY_CHILD_PANED_POSITION "/paned_position"
#define CONF_STATE_LIBRARY_CHILD_SHOW_BROWSER "/show_browser"

struct _RBLibraryChildSourcePrivate
{
	char *uri;

	char *browser_key;
	char *paned_key;
};

enum
{
	PROP_0,
	PROP_URI
};

#define RB_LIBRARY_CHILD_SOURCE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), RB_TYPE_LIBRARY_CHILD_SOURCE, RBLibraryChildSourcePrivate))
G_DEFINE_TYPE (RBLibraryChildSource, rb_library_child_source, RB_TYPE_BROWSER_SOURCE);

static void
rb_library_child_source_class_init (RBLibraryChildSourceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RBSourceClass *source_class = RB_SOURCE_CLASS (klass);
	RBBrowserSourceClass *browser_source_class = RB_BROWSER_SOURCE_CLASS (klass);

	object_class->finalize = rb_library_child_source_finalize;
	object_class->get_property = rb_library_child_source_get_property;
	object_class->set_property = rb_library_child_source_set_property;

	source_class->impl_get_browser_key = impl_get_browser_key;

	browser_source_class->impl_get_paned_key = impl_get_paned_key;

	g_object_class_install_property (object_class,
					PROP_URI,
					g_param_spec_string ("uri",
							"uri",
							"URI of library directory",
							NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_type_class_add_private (klass, sizeof (RBLibraryChildSourcePrivate));
}

static void
rb_library_child_source_init (RBLibraryChildSource *source)
{
	source->priv = RB_LIBRARY_CHILD_SOURCE_GET_PRIVATE (source);
}

static void
rb_library_child_source_finalize (GObject *object)
{
	RBLibraryChildSource *source;

	g_return_if_fail (object != NULL);
	g_return_if_fail (RB_IS_LIBRARY_CHILD_SOURCE (object));

	source = RB_LIBRARY_CHILD_SOURCE (object);

	g_return_if_fail (source->priv != NULL);

	g_free (source->priv->uri);
	g_free (source->priv->browser_key);
	g_free (source->priv->paned_key);

	G_OBJECT_CLASS (rb_library_child_source_parent_class)->finalize (object);
}

static void
rb_library_child_source_set_property (GObject *object,
					guint prop_id,
					const GValue *value,
					GParamSpec *pspec)
{
	RBLibraryChildSource *source = RB_LIBRARY_CHILD_SOURCE (object);

	switch (prop_id) {
	case PROP_URI:
		source->priv->uri = g_strdup (g_value_get_string (value));
		char *state_dir;
		state_dir = get_state_dir (source->priv->uri);
		source->priv->browser_key = g_strconcat (state_dir,
				CONF_STATE_LIBRARY_CHILD_SHOW_BROWSER, NULL);
		source->priv->paned_key = g_strconcat (state_dir,
				CONF_STATE_LIBRARY_CHILD_PANED_POSITION, NULL);
		g_free (state_dir);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
rb_library_child_source_get_property (GObject *object,
					guint prop_id,
					GValue *value,
					GParamSpec *pspec)
{
	RBLibraryChildSource *source = RB_LIBRARY_CHILD_SOURCE (object);

	switch (prop_id) {
	case PROP_URI:
		g_value_set_string (value, source->priv->uri);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static char *
get_display_name_for_file (GFile *file)
{
	GFileInfo *file_info;
	GError *err = NULL;
	char *name = NULL;

	file_info = g_file_query_info (file,
			G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
			G_FILE_QUERY_INFO_NONE,
			NULL,
			&err);
	if (file_info == NULL) {
		char *uri;

		uri = g_file_get_uri (file);

		if (err != NULL) {
			rb_debug ("g_file_query_info failed for '%s': %s",
					uri, err->message);
			g_error_free (err);
		} else {
			rb_debug ("g_file_query_info failed for '%s'", uri);
		}

		g_free (uri);

		return NULL;
	}

	name = g_file_info_get_attribute_as_string (file_info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
	g_object_unref (file_info);
	if (name == NULL) {
		rb_debug ("g_file_info_get_attribute_as_string failed!");
	}

	return name;
}

RBSource *
rb_library_child_source_new (RBSource *parent_source, const char *uri)
{
	GFile *file;
	RBSource *source;
	RBShell *shell;
	RhythmDB *db;
	RhythmDBEntryType *entry_type;
	GPtrArray *query;
	GdkPixbuf *icon;
	char *name;
	char *state_dir;
	char *sorting_key;

	file = g_file_new_for_uri (uri);
	if (! g_file_query_exists (file, NULL)) {
		rb_debug ("location '%s' not found, refusing to create child source.", uri);
		g_object_unref (file);
		return NULL;
	}

	name = get_display_name_for_file (file);
	g_object_unref (file);

	g_object_get (parent_source,
			"shell", &shell,
			"entry-type", &entry_type,
			"icon", &icon,
			NULL);

	g_object_get (shell, "db", &db, NULL);
	query = rhythmdb_query_parse (db,
			RHYTHMDB_QUERY_PROP_EQUALS, RHYTHMDB_PROP_TYPE, entry_type,
			RHYTHMDB_QUERY_PROP_PREFIX, RHYTHMDB_PROP_LOCATION, uri,
			RHYTHMDB_QUERY_END);
	g_object_unref (db);

	state_dir = get_state_dir (uri);
	sorting_key = g_strconcat (state_dir, CONF_STATE_LIBRARY_CHILD_SORTING, NULL);
	g_free (state_dir);

	source = RB_SOURCE (g_object_new (RB_TYPE_LIBRARY_CHILD_SOURCE,
					"name", name,
					"shell", shell,
					"uri", uri,
					"entry-type", entry_type,
					"sorting-key", sorting_key,
					"source-group", RB_SOURCE_GROUP_LIBRARY,
					"icon", icon,
					"query", query,
					NULL));

	g_free (name);
	g_object_unref (shell);
	rhythmdb_query_free (query);
	g_boxed_free (RHYTHMDB_TYPE_ENTRY_TYPE, entry_type);
	g_free (sorting_key);
	if (icon != NULL) {
		g_object_unref (icon);
	}

	return source;
}

static char *
get_state_dir (const char *uri)
{
	char *state_dir;
	char *uri_escaped;

	uri_escaped = gconf_escape_key (uri, strlen (uri));
	state_dir = g_strconcat (CONF_STATE_LIBRARY_DIR, "/", uri_escaped, NULL);
	g_free (uri_escaped);

	return state_dir;
}

static char *
impl_get_browser_key (RBSource *source)
{
	RBLibraryChildSourcePrivate *priv;
	priv = RB_LIBRARY_CHILD_SOURCE_GET_PRIVATE (RB_LIBRARY_CHILD_SOURCE (source));

	return g_strdup (priv->browser_key);
}

static char *
impl_get_paned_key (RBBrowserSource *source)
{
	RBLibraryChildSourcePrivate *priv;
	priv = RB_LIBRARY_CHILD_SOURCE_GET_PRIVATE (RB_LIBRARY_CHILD_SOURCE (source));

	return g_strdup (priv->paned_key);
}

static gboolean
delete_entry (GtkTreeModel *model,
		GtkTreePath *path,
		GtkTreeIter *iter,
		RhythmDB *db)
{
	RhythmDBEntry *entry;
	int position;

	gtk_tree_model_get (model, iter, 0, &entry, 1, &position, -1);

	rb_debug ("deleting: %2d - '%s' - '%s' - '%s'",
			position,
			rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST),
			rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ALBUM),
			rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_TITLE));

	rhythmdb_entry_delete (db, entry);

	rhythmdb_entry_unref (entry);

	return FALSE;
}

void
rb_library_child_source_remove_songs_and_state (RBLibraryChildSource *source)
{
	RBShell *shell;
	RhythmDBQueryModel *model;
	RhythmDB *db;
	char *uri;
	char *state_dir;

	rb_debug ("deleting songs and state.");

	g_object_get (source,
			"shell", &shell,
			"base-query-model", &model,
			"uri", &uri,
			NULL);
	g_object_get (shell, "db", &db, NULL);
	g_object_unref (shell);

	/* remove songs from db */
	gtk_tree_model_foreach (GTK_TREE_MODEL (model),
			(GtkTreeModelForeachFunc)delete_entry,
			db);
	rhythmdb_commit (db);
	g_object_unref (db);

	state_dir = get_state_dir (uri);
	g_free (uri);

	/* remove state gconf keys */
	eel_gconf_recursive_unset (state_dir);
	eel_gconf_suggest_sync ();
	g_free (state_dir);
}

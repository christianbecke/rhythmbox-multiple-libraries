/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *  Copyright (C) 2002 Jorn Baayen <jorn@nl.linux.org>
 *  Copyright (C) 2003,2004 Colin Walters <walters@verbum.org>
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

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <profiles/gnome-media-profiles.h>

#include "rhythmdb.h"
#include "rb-debug.h"
#include "rb-dialog.h"
#include "rb-builder-helpers.h"
#include "rb-util.h"
#include "rb-library-file-helpers.h"
#include "rb-library-preferences.h"
#include "rb-library-child-source.h"

static void rb_library_prefs_locations_column_cell_data_func (GtkTreeViewColumn *column,
		GtkCellRenderer *cell,
		GtkTreeModel *model,
		GtkTreeIter *iter,
		gpointer data);

/* widget callbacks */
static void rb_library_prefs_edit_profile_clicked_cb (GtkButton *button,
						       RBLibrarySource *source);
static void rb_library_prefs_path_changed_cb (GtkComboBox *box,
						RBLibrarySource *source);
static void rb_library_prefs_filename_changed_cb (GtkComboBox *box,
						   RBLibrarySource *source);
static void rb_library_prefs_format_changed_cb (GtkWidget *widget,
						 RBLibrarySource *source);
static void rb_library_prefs_add_location_button_clicked_cb (GtkButton *button,
		RBLibrarySource *source);
static void rb_library_prefs_remove_location_button_clicked_cb (GtkButton *button,
		RBLibrarySource *source);
static void rb_library_prefs_default_location_toggled_cb (GtkCellRendererToggle *cell,
		const char *path,
		RBLibrarySource *source);
static void rb_library_prefs_monitor_library_locations_toggled_cb (GtkCellRendererToggle *cell,
		const char *path,
		RBLibrarySource *source);

/* dialogs */
static char *run_location_file_chooser (RBLibrarySource *source);
static gint show_remove_library_location_message_dialog (RBLibrarySource *source);

static gboolean location_is_valid (GSList *library_locations,
		const char *uri,
		char **reason);
static void layout_example_label_update (RBLibrarySource *source);
static void rb_library_prefs_sync (RBLibrarySource *source);

typedef struct {
	char *title;
	char *path;
} LibraryPathElement;

const LibraryPathElement library_layout_paths[] = {
	{N_("Artist/Artist - Album"), "%aa/%aa - %at"},
	{N_("Artist/Album"), "%aa/%at"},
	{N_("Artist - Album"), "%aa - %at"},
	{N_("Album"), "%at"},
	{N_("Artist"), "%aa"},
};
const int num_library_layout_paths = G_N_ELEMENTS (library_layout_paths);

const LibraryPathElement library_layout_filenames[] = {
	{N_("Number - Title"), "%tN - %tt"},
	{N_("Artist - Title"), "%ta - %tt"},
	{N_("Artist - Number - Title"), "%ta - %tN - %tt"},
	{N_("Artist (Album) - Number - Title"), "%ta (%at) - %tN - %tt"},
	{N_("Title"), "%tt"},
	{N_("Number. Artist - Title"), "%tN. %ta - %tt"},
};
const int num_library_layout_filenames = G_N_ELEMENTS (library_layout_filenames);

enum {
	LOCATION_COLUMN_PATH,
	LOCATION_COLUMN_DEFAULT_LIBRARY_LOCATION,
	LOCATION_COLUMN_MONITOR_LIBRARY_LOCATIONS,
	LOCATION_N_COLUMNS
};

enum {
	RESPONSE_KEEP_SONGS_AND_STATE,
	RESPONSE_REMOVE_SONGS_AND_STATE
};

static void
rb_library_prefs_locations_column_cell_data_func (GtkTreeViewColumn *column,
		GtkCellRenderer *cell,
		GtkTreeModel *model,
		GtkTreeIter *iter,
		gpointer data)
{
	char *uri, *escaped_uri;

	gtk_tree_model_get (model, iter, LOCATION_COLUMN_PATH, &uri, -1);

	escaped_uri = g_uri_unescape_string (uri, NULL);
	g_free (uri);

	g_object_set (cell, "text", escaped_uri, NULL);
	g_free (escaped_uri);
}

GtkWidget *
rb_library_prefs_get_config_widget (RBSource *asource, RBShellPreferences *prefs)
{
	RBLibrarySource *source = RB_LIBRARY_SOURCE (asource);
	GtkWidget *tmp;
	GtkBuilder *builder;
	GtkWidget *label;
	int i;
	GtkListStore *liststore;
	GtkCellRenderer *cell;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;

	rb_debug ("impl_get_config_widget");

	if (source->priv->config_widget)
		return source->priv->config_widget;


	g_object_ref (G_OBJECT (prefs));
	source->priv->shell_prefs = prefs;

	builder = rb_builder_load ("library-prefs.ui", source);
	source->priv->config_widget =
		GTK_WIDGET (gtk_builder_get_object (builder, "library_vbox"));

	rb_builder_boldify_label (builder, "library_location_label");

	source->priv->locations_tree = GTK_WIDGET (gtk_builder_get_object (builder, "library_locations_treeview"));
	liststore = gtk_list_store_new (LOCATION_N_COLUMNS,
			G_TYPE_STRING,
			G_TYPE_BOOLEAN,
			G_TYPE_BOOLEAN);
	gtk_tree_view_set_model (GTK_TREE_VIEW (source->priv->locations_tree), GTK_TREE_MODEL (liststore));
	g_object_unref (G_OBJECT (liststore));

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (source->priv->locations_tree));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

	cell = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_pack_start (column, cell, TRUE);
	gtk_tree_view_column_set_title (column, _("Locations"));
	gtk_tree_view_column_set_cell_data_func (column,
			cell,
			(GtkTreeCellDataFunc) rb_library_prefs_locations_column_cell_data_func,
			NULL,
			NULL);
	gtk_tree_view_column_set_expand (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (source->priv->locations_tree), column);

	cell = gtk_cell_renderer_toggle_new ();
	gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (cell), TRUE);
	g_signal_connect (G_OBJECT (cell),
		"toggled",
		G_CALLBACK (rb_library_prefs_default_location_toggled_cb),
		asource);
	column = gtk_tree_view_column_new_with_attributes (_("Default Location"),
		cell,
		"active", LOCATION_COLUMN_DEFAULT_LIBRARY_LOCATION,
		NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (source->priv->locations_tree), column);

	cell = gtk_cell_renderer_toggle_new ();
	g_signal_connect (G_OBJECT (cell),
			"toggled",
			G_CALLBACK (rb_library_prefs_monitor_library_locations_toggled_cb),
			asource);
	column = gtk_tree_view_column_new_with_attributes (_("Watch for new files"),
			cell,
			"active", LOCATION_COLUMN_MONITOR_LIBRARY_LOCATIONS,
			NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (source->priv->locations_tree), column);

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "add_library_location_button"));
	g_signal_connect (G_OBJECT (tmp),
			"clicked",
			G_CALLBACK (rb_library_prefs_add_location_button_clicked_cb),
			asource);

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "remove_library_location_button"));
	g_signal_connect (G_OBJECT (tmp),
			"clicked",
			G_CALLBACK (rb_library_prefs_remove_location_button_clicked_cb),
			asource);

	rb_builder_boldify_label (builder, "library_structure_label");

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "layout_path_menu_box"));
	label = GTK_WIDGET (gtk_builder_get_object (builder, "layout_path_menu_label"));
	source->priv->layout_path_menu = gtk_combo_box_new_text ();
	gtk_box_pack_start (GTK_BOX (tmp), source->priv->layout_path_menu, TRUE, TRUE, 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), source->priv->layout_path_menu);
	g_signal_connect (G_OBJECT (source->priv->layout_path_menu),
			  "changed",
			  G_CALLBACK (rb_library_prefs_path_changed_cb),
			  asource);
	for (i = 0; i < num_library_layout_paths; i++) {
		gtk_combo_box_append_text (GTK_COMBO_BOX (source->priv->layout_path_menu),
					   _(library_layout_paths[i].title));
	}

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "layout_filename_menu_box"));
	label = GTK_WIDGET (gtk_builder_get_object (builder, "layout_filename_menu_label"));
	source->priv->layout_filename_menu = gtk_combo_box_new_text ();
	gtk_box_pack_start (GTK_BOX (tmp), source->priv->layout_filename_menu, TRUE, TRUE, 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), source->priv->layout_filename_menu);
	g_signal_connect (G_OBJECT (source->priv->layout_filename_menu),
			  "changed",
			  G_CALLBACK (rb_library_prefs_filename_changed_cb),
			  asource);
	for (i = 0; i < num_library_layout_filenames; i++) {
		gtk_combo_box_append_text (GTK_COMBO_BOX (source->priv->layout_filename_menu),
					   _(library_layout_filenames[i].title));
	}

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "edit_profile_button"));
	g_signal_connect (G_OBJECT (tmp),
			  "clicked",
			  G_CALLBACK (rb_library_prefs_edit_profile_clicked_cb),
			  asource);

	tmp = GTK_WIDGET (gtk_builder_get_object (builder, "preferred_format_menu_box"));
	label = GTK_WIDGET (gtk_builder_get_object (builder, "preferred_format_menu_label"));
	source->priv->preferred_format_menu = gm_audio_profile_choose_new ();
	gtk_box_pack_start (GTK_BOX (tmp), source->priv->preferred_format_menu, TRUE, TRUE, 0);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), source->priv->preferred_format_menu);
	g_signal_connect (G_OBJECT (source->priv->preferred_format_menu),
			  "changed",
			  G_CALLBACK (rb_library_prefs_format_changed_cb),
			  asource);

	source->priv->layout_example_label = GTK_WIDGET (gtk_builder_get_object (builder, "layout_example_label"));

	rb_library_prefs_sync (source);

	return source->priv->config_widget;
}

void
rb_library_prefs_library_location_changed (GConfClient *client,
					    guint cnxn_id,
					    GConfEntry *entry,
					    RBLibrarySource *source)
{
	rb_debug ("library locations changed");

	rb_library_prefs_sync_default_library_location (source, FALSE);
	rb_library_prefs_sync_monitored_locations (source, FALSE);

	if (source->priv->config_widget)
		rb_library_prefs_sync (source);

	rb_library_source_sync_child_sources (source);
}

void
rb_library_prefs_default_library_location_changed (GConfClient *client,
					    guint cnxn_id,
					    GConfEntry *entry,
					    RBLibrarySource *source)
{
	rb_debug ("default library location changed");

	rb_library_prefs_sync_default_library_location (source, TRUE);

	if (source->priv->config_widget)
		rb_library_prefs_sync (source);
}

void
rb_library_prefs_monitor_library_locations_changed (GConfClient *client,
					    guint cnxn_id,
					    GConfEntry *entry,
					    RBLibrarySource *source)
{
	rb_debug ("monitored library locations changed");

	rb_library_prefs_sync_monitored_locations (source, TRUE);

	if (source->priv->config_widget)
		rb_library_prefs_sync (source);
}

void
rb_library_prefs_ui_pref_changed (GConfClient *client,
				   guint cnxn_id,
				   GConfEntry *entry,
				   RBLibrarySource *source)
{
	rb_debug ("ui pref changed");
	rb_library_prefs_ui_prefs_sync (source);
}

void
rb_library_prefs_layout_path_changed (GConfClient *client,
				       guint cnxn_id,
				       GConfEntry *entry,
				       RBLibrarySource *source)
{
	char *value;
	int active;
	int i;

	g_return_if_fail (entry != NULL);
	g_return_if_fail (strcmp (entry->key, CONF_LIBRARY_LAYOUT_PATH) == 0);

	rb_debug ("layout path changed");

	if (entry->value == NULL) {
		value = g_strdup (library_layout_paths[0].path);
	} else if (entry->value->type == GCONF_VALUE_STRING) {
		value = g_strdup (gconf_value_get_string (entry->value));
	} else {
		return;
	}

	active = -1;
	for (i = 0; library_layout_paths[i].path != NULL; i++) {
		if (strcmp (library_layout_paths[i].path, value) == 0) {
			active = 1;
			break;
		}
	}

	g_free (value);
	gtk_combo_box_set_active (GTK_COMBO_BOX (source->priv->layout_path_menu), active);

	layout_example_label_update (source);
}

void
rb_library_prefs_layout_filename_changed (GConfClient *client,
					   guint cnxn_id,
					   GConfEntry *entry,
					   RBLibrarySource *source)
{
	char *value;
	int i = 0;

	g_return_if_fail (entry != NULL);
	g_return_if_fail (strcmp (entry->key, CONF_LIBRARY_LAYOUT_FILENAME) == 0);

	rb_debug ("layout filename changed");

	if (entry->value == NULL) {
		value = g_strdup (library_layout_filenames[0].path);
	} else if (entry->value->type == GCONF_VALUE_STRING) {
		value = g_strdup (gconf_value_get_string (entry->value));
	} else {
		return;
	}

	while (library_layout_filenames[i].path && strcmp (library_layout_filenames[i].path, value) != 0) {
		i++;
	}

	g_free (value);
	gtk_combo_box_set_active (GTK_COMBO_BOX (source->priv->layout_filename_menu), i);

	layout_example_label_update (source);
}

static char *
run_location_file_chooser (RBLibrarySource *source)
{
	GtkWidget *dialog;
	char *uri = NULL;

	rb_debug ("running location file chooser dialog");

	dialog = rb_file_chooser_new (_("Choose New Library Location"), GTK_WINDOW (source->priv->shell_prefs),
			GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, FALSE);
	gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (dialog), FALSE);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
		uri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog));
	}
	gtk_widget_destroy (dialog);
	return uri;
}

static gint
show_remove_library_location_message_dialog (RBLibrarySource *source)
{
	GtkWidget *dialog;
	gint ret;

	dialog = gtk_message_dialog_new (GTK_WINDOW (source->priv->shell_prefs),
			GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_QUESTION,
			GTK_BUTTONS_NONE,
			"%s", _("Remove Library Location"));

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
			"%s", _("Would you like to remove all songs stored at this location from your library? "
				"If you click 'remove songs', the songs will not be deleted from disk, "
				"but will no longer appear in the library. "
				"If you click 'keep songs', the songs will remain in your library."));

	gtk_dialog_add_buttons (GTK_DIALOG (dialog),
			_("Remove songs"), RESPONSE_REMOVE_SONGS_AND_STATE,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			_("Keep songs"), RESPONSE_KEEP_SONGS_AND_STATE,
			NULL);

	gtk_window_set_title (GTK_WINDOW (dialog), "");

	gtk_container_set_border_width (GTK_CONTAINER (dialog), 6);

	ret = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
	return ret;
}

static void
rb_library_prefs_edit_profile_clicked_cb (GtkButton *button, RBLibrarySource *source)
{
	GtkWidget *dialog;

	dialog = gm_audio_profiles_edit_new (eel_gconf_client_get_global (),
					     GTK_WINDOW (source->priv->shell_prefs));
	gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);
	gtk_widget_show_all (dialog);
	gtk_dialog_run (GTK_DIALOG (dialog));
}

static void
rb_library_prefs_add_location_button_clicked_cb (GtkButton *button, RBLibrarySource *source)
{
	GSList *library_locations;
	char *uri;
	char *sanitized_uri;
	char *reason = NULL;

	rb_debug ("add_location button clicked");

	library_locations = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);

	uri = run_location_file_chooser (source);
	sanitized_uri = rb_library_check_append_slash (uri);
	g_free (uri);
	if (sanitized_uri != NULL) {
		if (location_is_valid (library_locations, sanitized_uri, &reason)) {
			library_locations = g_slist_prepend (library_locations, g_strdup (sanitized_uri));
			eel_gconf_set_string_list (CONF_LIBRARY_LOCATION, library_locations);
			rb_debug ("location '%s' added", sanitized_uri);
		} else {
			rb_debug ("invalid location '%s': %s", sanitized_uri, reason);
			rb_error_dialog (GTK_WINDOW (source->priv->shell_prefs),
					_("The location could not be added"),
					"%s", reason);
			g_free (reason);
		}
	}
	g_free (sanitized_uri);
	rb_slist_deep_free (library_locations);
}

static void
rb_library_prefs_monitor_library_locations_toggled_cb (GtkCellRendererToggle *cell,
		const char *path,
		RBLibrarySource *source)
{
	GtkTreeModel *model;
	GtkTreeIter it;
	char *uri;
	gboolean do_monitor;
	GSList *monitor_locations, *found;
	gboolean update = FALSE;

	rb_debug ("monitor library location toggled");

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (source->priv->locations_tree));

	g_assert (gtk_tree_model_get_iter_from_string (model, &it, path));

	gtk_tree_model_get (model, &it,
			LOCATION_COLUMN_PATH, &uri,
			LOCATION_COLUMN_MONITOR_LIBRARY_LOCATIONS, &do_monitor,
			-1);

	g_assert (uri != NULL);

	do_monitor = (!do_monitor); /* toggle! */

	monitor_locations = eel_gconf_get_string_list (CONF_MONITOR_LIBRARY_LOCATIONS);
	found = g_slist_find_custom (monitor_locations, uri, (GCompareFunc)strcmp);
	if (do_monitor) {
		if (found == NULL) {
			monitor_locations = g_slist_prepend (monitor_locations, g_strdup (uri));
			update = TRUE;
		}
	} else {
		if (found != NULL) {
			monitor_locations = g_slist_delete_link (monitor_locations, found);
			update = TRUE;
		}
	}
	g_free (uri);

	if (update) {
		eel_gconf_set_string_list (CONF_MONITOR_LIBRARY_LOCATIONS, monitor_locations);
	}
	rb_slist_deep_free (monitor_locations);
}

static void
rb_library_prefs_default_location_toggled_cb (GtkCellRendererToggle *cell,
		const char *path,
		RBLibrarySource *source)
{
	GtkTreeModel *model;
	GtkTreeIter it;
	char *uri;

	rb_debug ("default dest toggled");

	model = gtk_tree_view_get_model (GTK_TREE_VIEW (source->priv->locations_tree));

	g_assert (gtk_tree_model_get_iter_from_string (model, &it, path));

	gtk_tree_model_get (model, &it, LOCATION_COLUMN_PATH, &uri, -1);

	g_assert (uri != NULL);

	eel_gconf_set_string (CONF_DEFAULT_LIBRARY_LOCATION, uri);
	g_free (uri);
}

static void
rb_library_prefs_remove_location_button_clicked_cb (GtkButton *button, RBLibrarySource *source)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GList *selected_rows, *i;
	gboolean location_removed = FALSE;
	gint response;

	rb_debug ("removing location");

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (source->priv->locations_tree));
	selected_rows = gtk_tree_selection_get_selected_rows (selection, &model);
	response = show_remove_library_location_message_dialog (source);
	if (response == RESPONSE_KEEP_SONGS_AND_STATE || response == RESPONSE_REMOVE_SONGS_AND_STATE) {
		GSList *library_locations, *found;
		GtkTreePath *path;
		GtkTreeIter it;
		char *uri;
		RBLibraryChildSource *child_source;

		library_locations = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);

		for (i = selected_rows; i != NULL; i = g_list_next (i)) {
			path = (GtkTreePath *)i->data;

			g_assert (gtk_tree_model_get_iter (model, &it, path));

			gtk_tree_model_get (model, &it, LOCATION_COLUMN_PATH, &uri, -1);

			g_assert (uri != NULL);

			found = g_slist_find_custom (library_locations, uri, (GCompareFunc)strcmp);
			if (found != NULL) {
				/* remove location */
				child_source = rb_library_source_get_child_source_for_uri (source, uri);
				if (child_source != NULL) {
					if (response == RESPONSE_REMOVE_SONGS_AND_STATE) {
						rb_debug ("removing songs and state for child_source [%p]",
							child_source);
						rb_library_child_source_remove_songs_and_state (child_source);
					}
				}

				library_locations = g_slist_delete_link (library_locations, found);
				location_removed = TRUE;
				rb_debug ("location '%s' removed", uri);
			}

			g_free (uri);
		}

		if (location_removed) {
			eel_gconf_set_string_list (CONF_LIBRARY_LOCATION, library_locations);
		}
		rb_slist_deep_free (library_locations);
	}
	rb_list_destroy_free (selected_rows, (GDestroyNotify)gtk_tree_path_free);
}

static void
rb_library_prefs_path_changed_cb (GtkComboBox *box, RBLibrarySource *source)
{
	const char *path;
	gint index;

	index = gtk_combo_box_get_active (box);
	if (index >= 0) {
		path = library_layout_paths[index].path;
		eel_gconf_set_string (CONF_LIBRARY_LAYOUT_PATH, path);
	}
}

static void
rb_library_prefs_filename_changed_cb (GtkComboBox *box, RBLibrarySource *source)
{
	const char *filename;
	gint index;

	index = gtk_combo_box_get_active (box);
	if (index >= 0) {
		filename = library_layout_filenames[index].path;
		eel_gconf_set_string (CONF_LIBRARY_LAYOUT_FILENAME, filename);
	}
}

static void
rb_library_prefs_format_changed_cb (GtkWidget *widget, RBLibrarySource *source)
{
	GMAudioProfile *profile;

	profile = gm_audio_profile_choose_get_active (widget);
	eel_gconf_set_string (CONF_LIBRARY_PREFERRED_FORMAT, gm_audio_profile_get_id (profile));

	layout_example_label_update (source);
}

static gboolean
location_is_valid (GSList *library_locations, const char *uri, char **reason)
{
	GSList *i;
	gboolean is_valid = TRUE;
	GFile *new, *tmp;
	char *tmp_uri, *tmp_uri_unescaped, *uri_unescaped;

	g_assert (reason != NULL);

	*reason = NULL;

	uri_unescaped = g_uri_unescape_string (uri, NULL);

	new = g_file_new_for_uri (uri);
	for (i = library_locations; i != NULL; i = g_slist_next (i)) {
		tmp = g_file_new_for_uri ((char *)i->data);
		if (g_file_equal (tmp, new)) {
			*reason = g_strdup_printf (_("The location '%s' is already in your library."), uri_unescaped);
			is_valid = FALSE;
		} else if (g_file_has_prefix (tmp, new)) {
			tmp_uri = g_file_get_uri (tmp);
			tmp_uri_unescaped = g_uri_unescape_string (tmp_uri, NULL);
			g_free (tmp_uri);
			*reason = g_strdup_printf (_("Subdirectory '%s' of location '%s' is already in your library. "
						"Nested library locations are not allowed."),
					tmp_uri_unescaped, uri_unescaped);
			g_free (tmp_uri_unescaped);
			is_valid = FALSE;
		} else if (g_file_has_prefix (new, tmp)) {
			tmp_uri = g_file_get_uri (tmp);
			tmp_uri_unescaped = g_uri_unescape_string (tmp_uri, NULL);
			g_free (tmp_uri);
			*reason = g_strdup_printf (_("Location '%s' is a subdirectory of '%s', which is already in your library. "
						"Nested library locations are not allowed."),
					uri_unescaped, tmp_uri_unescaped);
			g_free (tmp_uri_unescaped);
			is_valid = FALSE;
		}
		g_object_unref (G_OBJECT (tmp));
		if (!is_valid) {
			break;
		}
	}
	g_free (uri_unescaped);
	g_object_unref (G_OBJECT (new));

	return is_valid;
}

void
rb_library_prefs_sync_default_library_location (RBLibrarySource *source, gboolean disable_notification)
{
	gboolean removed = FALSE;
	GSList *library_locations;
	char *default_library_location;

	if (disable_notification && source->priv->default_library_location_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->default_library_location_notify_id);
		removed = TRUE;
	}

	library_locations = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);
	default_library_location = eel_gconf_get_string (CONF_DEFAULT_LIBRARY_LOCATION);
	if (default_library_location == NULL || !rb_string_slist_contains (library_locations, default_library_location)) {
		if (library_locations != NULL && library_locations->data != NULL) {
			eel_gconf_set_string (CONF_DEFAULT_LIBRARY_LOCATION, (char *)library_locations->data);
		} else {
			eel_gconf_set_string (CONF_DEFAULT_LIBRARY_LOCATION, "");
		}
	}
	g_free (default_library_location);
	rb_slist_deep_free (library_locations);

	if (removed) {
		source->priv->default_library_location_notify_id =
			eel_gconf_notification_add (CONF_DEFAULT_LIBRARY_LOCATION,
					(GConfClientNotifyFunc) rb_library_prefs_default_library_location_changed,
					source);
	}
}

void
rb_library_prefs_sync_monitored_locations (RBLibrarySource *source, gboolean disable_notification)
{
	gboolean removed = FALSE;
	GSList *monitored_locations;

	if (disable_notification && source->priv->monitor_library_locations_notify_id != 0) {
		eel_gconf_notification_remove (source->priv->monitor_library_locations_notify_id);
		removed = TRUE;
	}

	monitored_locations = eel_gconf_get_string_list (CONF_MONITOR_LIBRARY_LOCATIONS);
	if (monitored_locations != NULL) {
		GSList *library_locations, *i, *remove = NULL;
		gboolean location_removed = FALSE;

		library_locations = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);

		for (i = monitored_locations; i != NULL; i = g_slist_next (i)) {
			if (!rb_string_slist_contains (library_locations, (char *)i->data)) {
				remove = g_slist_prepend (remove, i);
				location_removed = TRUE;
			}
		}
		rb_slist_deep_free (library_locations);

		for (i = remove; i != NULL; i = g_slist_next (i)) {
			monitored_locations = g_slist_remove_link (monitored_locations, (GSList *)i->data);
		}

		if (location_removed) {
			eel_gconf_set_string_list (CONF_MONITOR_LIBRARY_LOCATIONS, monitored_locations);
		}
	}
	rb_slist_deep_free (monitored_locations);

	if (removed) {
		source->priv->monitor_library_locations_notify_id =
			eel_gconf_notification_add (CONF_MONITOR_LIBRARY_LOCATIONS,
					(GConfClientNotifyFunc) rb_library_prefs_monitor_library_locations_changed,
					source);
	}
}

void
rb_library_prefs_ui_prefs_sync (RBLibrarySource *source)
{
	if (source->priv->config_widget)
		rb_library_prefs_sync (source);
}

static void
rb_library_prefs_sync (RBLibrarySource *source)
{
	GSList *list, *i;
	char *str;
	GConfClient *gconf_client;
	GtkTreeModel *model;
	GtkTreeIter it;
	char *uri;
	char *default_dest_uri;
	gboolean is_default_dest;
	GSList *monitor_locations;
	gboolean is_monitored;

	rb_debug ("syncing pref dialog state");

	/* library location */
	list = eel_gconf_get_string_list (CONF_LIBRARY_LOCATION);
	list = g_slist_reverse (list);
	default_dest_uri = eel_gconf_get_string (CONF_DEFAULT_LIBRARY_LOCATION);
	monitor_locations = eel_gconf_get_string_list (CONF_MONITOR_LIBRARY_LOCATIONS);
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (source->priv->locations_tree));
	gtk_list_store_clear (GTK_LIST_STORE (model));
	for (i = list; i != NULL; i = g_slist_next (i)) {
		uri = (char *)i->data;
		is_default_dest = (uri != NULL && default_dest_uri != NULL && strcmp (uri, default_dest_uri) == 0)? TRUE : FALSE;
		is_monitored = rb_string_slist_contains (monitor_locations, uri);
		gtk_list_store_append (GTK_LIST_STORE (model), &it);
		gtk_list_store_set (GTK_LIST_STORE (model), &it,
				LOCATION_COLUMN_PATH, uri,
				LOCATION_COLUMN_DEFAULT_LIBRARY_LOCATION, is_default_dest,
				LOCATION_COLUMN_MONITOR_LIBRARY_LOCATIONS, is_monitored,
				-1);
	}
	g_free (default_dest_uri);
	rb_slist_deep_free (list);
	rb_slist_deep_free (monitor_locations);

	/* preferred format */
	str = eel_gconf_get_string (CONF_LIBRARY_PREFERRED_FORMAT);
	if (str) {
		gm_audio_profile_choose_set_active (source->priv->preferred_format_menu, str);
		g_free (str);
	}

	source->priv->layout_path_notify_id =
		eel_gconf_notification_add (CONF_LIBRARY_LAYOUT_PATH,
				    (GConfClientNotifyFunc) rb_library_prefs_layout_path_changed, source);
	source->priv->layout_filename_notify_id =
		eel_gconf_notification_add (CONF_LIBRARY_LAYOUT_FILENAME,
				    (GConfClientNotifyFunc) rb_library_prefs_layout_filename_changed, source);

	gconf_client = eel_gconf_client_get_global ();
	/* layout path */
	rb_library_prefs_layout_path_changed (gconf_client, -1,
					       gconf_client_get_entry (gconf_client, CONF_LIBRARY_LAYOUT_PATH, NULL, TRUE, NULL),
					       source);
	/* layout filename */
	rb_library_prefs_layout_filename_changed (gconf_client, -1,
						   gconf_client_get_entry (gconf_client, CONF_LIBRARY_LAYOUT_FILENAME, NULL, TRUE, NULL),
						   source);
}

static void
layout_example_label_update (RBLibrarySource *source)
{
	char *file_pattern;
	char *path_pattern;
	char *file_value;
	char *path_value;
	char *example;
	char *format;
	char *tmp;
	GMAudioProfile *profile;
	RhythmDBEntryType entry_type;
	RhythmDBEntry *sample_entry;

	profile = gm_audio_profile_choose_get_active (source->priv->preferred_format_menu);

	/* TODO: sucky. Replace with get-gconf-key-with-default mojo */
	file_pattern = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_FILENAME);
	if (file_pattern == NULL) {
		file_pattern = g_strdup (library_layout_filenames[0].path);
	}
	tmp = rb_library_sanitize_pattern (file_pattern);
	g_free (file_pattern);
	file_pattern = tmp;

	path_pattern = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_PATH);
	if (path_pattern == NULL) {
		path_pattern = g_strdup (library_layout_paths[0].path);
	}

	g_object_get (source, "entry-type", &entry_type, NULL);
	sample_entry = rhythmdb_entry_example_new (source->priv->db, entry_type, NULL);
	g_boxed_free (RHYTHMDB_TYPE_ENTRY_TYPE, entry_type);

	file_value = rb_library_filepath_parse_pattern (source->priv->db, file_pattern, sample_entry);
	path_value = rb_library_filepath_parse_pattern (source->priv->db, path_pattern, sample_entry);
	rhythmdb_entry_unref (sample_entry);

	example = g_build_filename (G_DIR_SEPARATOR_S, path_value, file_value, NULL);
	g_free (file_value);
	g_free (file_pattern);
	g_free (path_value);
	g_free (path_pattern);

	format = g_strconcat ("<small><i><b>",
			      _("Example Path:"),
			      "</b> ",
			      example,
			      ".",
			      profile ? gm_audio_profile_get_extension (profile) : "ogg",
			      "</i></small>", NULL);
	g_free (example);

	gtk_label_set_markup (GTK_LABEL (source->priv->layout_example_label), format);
	g_free (format);
}

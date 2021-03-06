/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *  Implmentation of DAAP (iTunes Music Sharing) sharing
 *
 *  Copyright (C) 2005 Charles Schmidt <cschmidt2@emich.edu>
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

#include <time.h>
#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libsoup/soup.h>
#include <libsoup/soup-address.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-uri.h>
#include <libsoup/soup-server.h>

#include "rb-daap-share.h"
#include "rb-daap-structure.h"
#include "rb-daap-mdns-publisher.h"
#include "rb-daap-dialog.h"

#include "rb-playlist-source.h"
#include "rb-debug.h"
#include "eel-gconf-extensions.h"
#include "rb-file-helpers.h"

static void rb_daap_share_set_property  (GObject *object,
					 guint prop_id,
					 const GValue *value,
					 GParamSpec *pspec);
static void rb_daap_share_get_property  (GObject *object,
					 guint prop_id,
					 GValue *value,
				 	 GParamSpec *pspec);
static void rb_daap_share_dispose	(GObject *object);
static void rb_daap_share_maybe_restart (RBDAAPShare *share);
static gboolean rb_daap_share_publish_start (RBDAAPShare *share);
static gboolean rb_daap_share_publish_stop  (RBDAAPShare *share);
static gboolean rb_daap_share_server_start  (RBDAAPShare *share);
static gboolean rb_daap_share_server_stop   (RBDAAPShare *share);
static void rb_daap_share_playlist_created (RBPlaylistManager *mgr,
					    RBSource *playlist,
					    RBDAAPShare *share);
static void rb_daap_share_process_playlist (RBSource *playlist,
					    RBDAAPShare *share);
static void rb_daap_share_playlist_destroyed (RBDAAPShare *share,
					      RBSource *source);
static void rb_daap_share_forget_playlist (gpointer data,
					   RBDAAPShare *share);

#define STANDARD_DAAP_PORT 3689

/* HTTP chunk size used to send files to clients */
#define DAAP_SHARE_CHUNK_SIZE	16384

typedef enum {
	RB_DAAP_SHARE_AUTH_METHOD_NONE              = 0,
	RB_DAAP_SHARE_AUTH_METHOD_NAME_AND_PASSWORD = 1,
	RB_DAAP_SHARE_AUTH_METHOD_PASSWORD          = 2
} RBDAAPShareAuthMethod;

struct RBDAAPSharePrivate {
	gchar *name;
	guint port;
	char *password;
	RBDAAPShareAuthMethod auth_method;

	/* mdns/dns-sd publishing things */
	gboolean server_active;
	gboolean published;
	RBDaapMdnsPublisher *publisher;

	/* http server things */
	SoupServer *server;
	guint revision_number;

	GHashTable *session_ids;

	/* db things */
	RhythmDB *db;
	RhythmDBEntryType entry_type;
	gulong entry_added_id;
	gulong entry_deleted_id;
	gulong entry_changed_id;

	/* playlist things */
	RBPlaylistManager *playlist_manager;
	guint next_playlist_id;
	GList *playlist_ids;	/* contains RBPlaylistIDs */
};

#define RB_DAAP_SHARE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), RB_TYPE_DAAP_SHARE, RBDAAPSharePrivate))

typedef struct {
	RBSource *source;
	gint32 id;
} RBPlaylistID;

enum {
	PROP_0,
	PROP_NAME,
	PROP_PASSWORD,
	PROP_DB,
	PROP_PLAYLIST_MANAGER,
	PROP_ENTRY_TYPE
};

G_DEFINE_TYPE (RBDAAPShare, rb_daap_share, G_TYPE_OBJECT)

static void
rb_daap_share_class_init (RBDAAPShareClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = rb_daap_share_get_property;
	object_class->set_property = rb_daap_share_set_property;
	object_class->dispose = rb_daap_share_dispose;

	g_object_class_install_property (object_class,
					 PROP_NAME,
					 g_param_spec_string ("name",
						 	      "Name",
							      "Share Name",
							      NULL,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_PASSWORD,
					 g_param_spec_string ("password",
							      "Authentication password",
							      "Authentication password",
							      NULL,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_DB,
					 g_param_spec_object ("db",
							      "RhythmDB",
							      "RhythmDB object",
							      RHYTHMDB_TYPE,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
					 PROP_PLAYLIST_MANAGER,
					 g_param_spec_object ("playlist-manager",
							      "Playlist Manager",
							      "Playlist manager object",
							      RB_TYPE_PLAYLIST_MANAGER,
							       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (object_class,
					 PROP_ENTRY_TYPE,
					 g_param_spec_boxed ("entry-type",
							     "Entry type",
							     "Type of entries to be shared",
							     RHYTHMDB_TYPE_ENTRY_TYPE,
							     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_type_class_add_private (klass, sizeof (RBDAAPSharePrivate));
}

static void
rb_daap_share_set_name (RBDAAPShare *share,
			const char  *name)
{
	GError *error;
	gboolean res;

	g_return_if_fail (share != NULL);

	g_free (share->priv->name);
	share->priv->name = g_strdup (name);

	error = NULL;
	res = rb_daap_mdns_publisher_set_name (share->priv->publisher, name, &error);
	if (error != NULL) {
		g_warning ("Unable to change MDNS service name: %s", error->message);
		g_error_free (error);
	}
}

static void
published_cb (RBDaapMdnsPublisher *publisher,
	      const char          *name,
	      RBDAAPShare         *share)
{
	if (share->priv->name == NULL || name == NULL) {
		return;
	}

	if (strcmp (name, share->priv->name) == 0) {
		rb_debug ("mDNS publish successful");
		share->priv->published = TRUE;
	}
}

static void
name_collision_cb (RBDaapMdnsPublisher *publisher,
		   const char          *name,
		   RBDAAPShare         *share)
{
	char *new_name;

	if (share->priv->name == NULL || name == NULL) {
		return;
	}

	if (strcmp (name, share->priv->name) == 0) {
		rb_debug ("Duplicate share name on mDNS");

		GDK_THREADS_ENTER ();
		new_name = rb_daap_collision_dialog_new_run (NULL, share->priv->name);
		GDK_THREADS_LEAVE ();

		rb_daap_share_set_name (share, new_name);
		g_free (new_name);
	}

	return;
}

static void
rb_daap_share_init (RBDAAPShare *share)
{
	share->priv = RB_DAAP_SHARE_GET_PRIVATE (share);

	share->priv->revision_number = 5;

	share->priv->auth_method = RB_DAAP_SHARE_AUTH_METHOD_NONE;
	share->priv->publisher = rb_daap_mdns_publisher_new ();
	g_signal_connect_object (share->priv->publisher,
				 "published",
				 G_CALLBACK (published_cb),
				 share, 0);
	g_signal_connect_object (share->priv->publisher,
				 "name-collision",
				 G_CALLBACK (name_collision_cb),
				 share, 0);

}

static void
rb_daap_share_set_password (RBDAAPShare *share,
			    const char  *password)
{
	g_return_if_fail (share != NULL);

	if (share->priv->password && password &&
	    strcmp (password, share->priv->password) == 0) {
		return;
	}

	g_free (share->priv->password);
	share->priv->password = g_strdup (password);
	if (password != NULL) {
		share->priv->auth_method = RB_DAAP_SHARE_AUTH_METHOD_PASSWORD;
	} else {
		share->priv->auth_method = RB_DAAP_SHARE_AUTH_METHOD_NONE;
	}

	rb_daap_share_maybe_restart (share);
}

static void
rb_daap_share_set_db (RBDAAPShare *share,
		      RhythmDB    *db)
{
	if (share->priv->db != NULL) {
		g_object_unref (share->priv->db);
	}

	share->priv->db = db;

	if (share->priv->db != NULL) {
		g_object_ref (share->priv->db);
	}
}

static void
rb_daap_share_set_playlist_manager (RBDAAPShare       *share,
				    RBPlaylistManager *playlist_manager)
{
	GList *playlists;

	g_return_if_fail (share != NULL);

	if (share->priv->playlist_manager != NULL) {
		g_object_unref (share->priv->playlist_manager);
		g_signal_handlers_disconnect_by_func (share->priv->playlist_manager,
						      G_CALLBACK (rb_daap_share_playlist_created),
						      share);
	}

	share->priv->playlist_manager = playlist_manager;

	if (share->priv->playlist_manager != NULL) {
		g_object_ref (share->priv->playlist_manager);

		g_signal_connect_object (G_OBJECT (share->priv->playlist_manager),
					 "playlist_added",
					 G_CALLBACK (rb_daap_share_playlist_created),
					 share, 0);

		/* Currently, there are no playlists when this object is created, but in
		 * case it changes..
		 */
		playlists = rb_playlist_manager_get_playlists (share->priv->playlist_manager);
		g_list_foreach (playlists, (GFunc) rb_daap_share_process_playlist, share);
		g_list_free (playlists);
	}
}

static void
rb_daap_share_set_property (GObject *object,
			    guint prop_id,
			    const GValue *value,
			    GParamSpec *pspec)
{
	RBDAAPShare *share = RB_DAAP_SHARE (object);

	switch (prop_id) {
	case PROP_NAME:
		rb_daap_share_set_name (share, g_value_get_string (value));
		break;
	case PROP_PASSWORD:
		rb_daap_share_set_password (share, g_value_get_string (value));
		break;
	case PROP_DB:
		rb_daap_share_set_db (share, g_value_get_object (value));
		break;
	case PROP_PLAYLIST_MANAGER:
		rb_daap_share_set_playlist_manager (share, g_value_get_object (value));
		break;
	case PROP_ENTRY_TYPE:
		share->priv->entry_type = g_value_get_boxed (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
rb_daap_share_get_property (GObject *object,
			    guint prop_id,
			    GValue *value,
			    GParamSpec *pspec)
{
	RBDAAPShare *share = RB_DAAP_SHARE (object);

	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, share->priv->name);
		break;
	case PROP_PASSWORD:
		g_value_set_string (value, share->priv->password);
		break;
	case PROP_DB:
		g_value_set_object (value, share->priv->db);
		break;
	case PROP_PLAYLIST_MANAGER:
		g_value_set_object (value, share->priv->playlist_manager);
		break;
	case PROP_ENTRY_TYPE:
		g_value_set_boxed (value, share->priv->entry_type);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static gint
_find_by_id (gconstpointer a, gconstpointer b)
{
	RBPlaylistID *ai = (RBPlaylistID *)a;
	gint bv = GPOINTER_TO_INT (b);
	return (ai->id - bv);
}

static gint
_find_by_source (gconstpointer a, gconstpointer b)
{
	RBPlaylistID *ai = (RBPlaylistID *)a;
	RBSource *bs = (RBSource *)b;
	return (ai->source - bs);
}

static void
rb_daap_share_playlist_created (RBPlaylistManager *manager,
				RBSource *source,
				RBDAAPShare *share)
{
	rb_daap_share_process_playlist (source, share);
}

static void
rb_daap_share_process_playlist (RBSource *source,
				RBDAAPShare *share)
{
	RBPlaylistID *id;

	/* make sure we're not going insane.. */
	g_assert (g_list_find_custom (share->priv->playlist_ids,
				      source,
				      _find_by_source) == NULL);

	g_object_weak_ref (G_OBJECT (source),
			   (GWeakNotify) rb_daap_share_playlist_destroyed,
			   share);
	id = g_new0 (RBPlaylistID, 1);
	id->source = source;
	id->id = share->priv->next_playlist_id++;
	share->priv->playlist_ids = g_list_append (share->priv->playlist_ids, id);

	/* if we knew how to send updates to clients, we'd probably do something here */
}

static void
rb_daap_share_playlist_destroyed (RBDAAPShare *share,
				  RBSource *source)
{
	GList *id;

	id = g_list_find_custom (share->priv->playlist_ids, source, _find_by_source);
	if (!id)
		return;

	share->priv->playlist_ids = g_list_remove_link (share->priv->playlist_ids, id);
	g_free (id->data);
	g_list_free_1 (id);
}

static void
rb_daap_share_forget_playlist (gpointer data,
			       RBDAAPShare *share)
{
	RBPlaylistID *id = (RBPlaylistID *)data;
	g_object_weak_unref (G_OBJECT (id->source),
			     (GWeakNotify) rb_daap_share_playlist_destroyed,
			     share);
}

static void
rb_daap_share_dispose (GObject *object)
{
	RBDAAPShare *share = RB_DAAP_SHARE (object);

	if (share->priv->published) {
		rb_daap_share_publish_stop (share);
	}

	if (share->priv->server_active) {
		rb_daap_share_server_stop (share);
	}

	g_free (share->priv->name);
	g_object_unref (share->priv->db);
	g_object_unref (share->priv->playlist_manager);

	g_list_foreach (share->priv->playlist_ids, (GFunc) rb_daap_share_forget_playlist, share);
	g_list_foreach (share->priv->playlist_ids, (GFunc) g_free, NULL);
	g_list_free (share->priv->playlist_ids);

	if (share->priv->publisher) {
		g_object_unref (share->priv->publisher);
	}

	G_OBJECT_CLASS (rb_daap_share_parent_class)->dispose (object);
}

RBDAAPShare *
rb_daap_share_new (const char *name,
		   const char *password,
		   RhythmDB *db,
		   RhythmDBEntryType entry_type,
		   RBPlaylistManager *playlist_manager)
{
	RBDAAPShare *share;

	share = RB_DAAP_SHARE (g_object_new (RB_TYPE_DAAP_SHARE,
					     "name", name,
					     "password", password,
					     "db", db,
					     "playlist-manager", playlist_manager,
					     "entry-type", entry_type,
					     NULL));

	rb_daap_share_server_start (share);
	rb_daap_share_publish_start (share);

	return share;
}

static void
message_add_standard_headers (SoupMessage *message)
{
	soup_message_headers_append (message->response_headers, "DAAP-Server", "Rhythmbox " VERSION);

	soup_message_headers_append (message->response_headers, "Content-Type", "application/x-dmap-tagged");
}

static void
message_set_from_rb_daap_structure (SoupMessage *message,
				    GNode *structure)
{
	gchar *resp;
	guint length;

	resp = rb_daap_structure_serialize (structure, &length);

	if (resp == NULL) {
		rb_debug ("serialize gave us null?\n");
		return;
	}

	soup_message_set_response (message, "application/x-dmap-tagged", SOUP_MEMORY_TAKE, resp, length);

	message_add_standard_headers (message);

	soup_message_set_status (message, SOUP_STATUS_OK);
}

#define DMAP_STATUS_OK 200

#define DMAP_VERSION 2.0
#define DAAP_VERSION 3.0
#define DMAP_TIMEOUT 1800

static void
server_info_cb (SoupServer        *server,
		SoupMessage       *message,
		const char        *path,
		GHashTable        *query,
		SoupClientContext *context,
		RBDAAPShare       *share)
{
/* MSRV	server info response
 * 	MSTT status
 * 	MPRO dmap version
 * 	APRO daap version
 * 	MINM name
 * 	MSAU authentication method
 * 	MSLR login required
 * 	MSTM timeout interval
 * 	MSAL supports auto logout
 * 	MSUP supports update
 * 	MSPI supports persistent ids
 * 	MSEX supports extensions
 * 	MSBR supports browse
 * 	MSQY supports query
 * 	MSIX supports index
 * 	MSRS supports resolve
 * 	MSDC databases count
 */
	GNode *msrv;

	msrv = rb_daap_structure_add (NULL, RB_DAAP_CC_MSRV);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MPRO, (gdouble) DMAP_VERSION);
	rb_daap_structure_add (msrv, RB_DAAP_CC_APRO, (gdouble) DAAP_VERSION);
	/* 2/3 is for itunes 4.8 (at least).  its determined by the
	 * Client-DAAP-Version header sent, but if we decide not to support
	 * older versions..? anyway
	 *
	 * 1.0 is 1/1
	 * 2.0 is 1/2
	 * 3.0 is 2/3
	 */
	rb_daap_structure_add (msrv, RB_DAAP_CC_MINM, share->priv->name);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSAU, share->priv->auth_method);
	/* authentication method
	 * 0 is nothing
	 * 1 is name & password
	 * 2 is password only
	 */
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSLR, 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSTM, (gint32) DMAP_TIMEOUT);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSAL, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSUP, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSPI, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSEX, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSBR, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSQY, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSIX, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSRS, (gchar) 0);
	rb_daap_structure_add (msrv, RB_DAAP_CC_MSDC, (gint32) 1);

	message_set_from_rb_daap_structure (message, msrv);
	rb_daap_structure_destroy (msrv);
}

static void
content_codes_cb (SoupServer        *server,
		  SoupMessage       *message,
		  const char        *path,
		  GHashTable        *query,
		  SoupClientContext *context,
		  RBDAAPShare       *share)
{
/* MCCR content codes response
 * 	MSTT status
 * 	MDCL dictionary
 * 		MCNM content codes number
 * 		MCNA content codes name
 * 		MCTY content codes type
 * 	MDCL dictionary
 * 	...
 */
	const RBDAAPContentCodeDefinition *defs;
	guint num_defs = 0;
	guint i;
	GNode *mccr;

	defs = rb_daap_content_codes (&num_defs);

	mccr = rb_daap_structure_add (NULL, RB_DAAP_CC_MCCR);
	rb_daap_structure_add (mccr, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);

	for (i = 0; i < num_defs; i++) {
		GNode *mdcl;

		mdcl = rb_daap_structure_add (mccr, RB_DAAP_CC_MDCL);
		rb_daap_structure_add (mdcl, RB_DAAP_CC_MCNM, rb_daap_content_code_string_as_int32(defs[i].string));
		rb_daap_structure_add (mdcl, RB_DAAP_CC_MCNA, defs[i].name);
		rb_daap_structure_add (mdcl, RB_DAAP_CC_MCTY, (gint32) defs[i].type);
	}

	message_set_from_rb_daap_structure (message, mccr);
	rb_daap_structure_destroy (mccr);
}

static gboolean
get_session_id (GHashTable *query,
		guint32    *id)
{
	char *session_id_str;
	guint32 session_id;

	session_id_str = g_hash_table_lookup (query, "session-id");
	if (session_id_str == NULL) {
		rb_debug ("session id not found");
		return FALSE;
	}

	session_id = (guint32) strtoul (session_id_str, NULL, 10);
	if (id != NULL) {
		*id = session_id;
	}
	return TRUE;
}

static gboolean
get_revision_number (GHashTable *query,
		     guint *number)
{
	char *revision_number_str;
	guint revision_number;

	revision_number_str = g_hash_table_lookup (query, "revision-number");
	if (revision_number_str == NULL) {
		rb_debug ("client asked for an update without a revision number?!?");
		return FALSE;
	}

	revision_number = strtoul (revision_number_str, NULL, 10);
	if (number != NULL) {
		*number = revision_number;
	}
	return TRUE;
}

static gboolean
session_id_validate (RBDAAPShare       *share,
		     SoupClientContext *context,
		     SoupMessage       *message,
		     GHashTable        *query,
		     guint32           *id)
{
	guint32     session_id;
	gboolean    res;
	const char *addr;
	const char *remote_address;

	if (id) {
		*id = 0;
	}

	res = get_session_id (query, &session_id);
	if (! res) {
		rb_debug ("Validation failed: Unable to parse session id from message");
		return FALSE;
	}

	/* check hash for remote address */
	addr = g_hash_table_lookup (share->priv->session_ids, GUINT_TO_POINTER (session_id));
	if (addr == NULL) {
		rb_debug ("Validation failed: Unable to lookup session id %u", session_id);
		return FALSE;
	}

	remote_address = soup_client_context_get_host (context);
	rb_debug ("Validating session id %u from %s matches %s",
		  session_id, remote_address, addr);
	if (remote_address == NULL || strcmp (addr, remote_address) != 0) {
		rb_debug ("Validation failed: Remote address does not match stored address");
		return FALSE;
	}

	if (id) {
		*id = session_id;
	}

	return TRUE;
}

static guint32
session_id_generate (RBDAAPShare       *share,
		     SoupClientContext *context)
{
	guint32 id;

	id = g_random_int ();

	return id;
}

static guint32
session_id_create (RBDAAPShare       *share,
		   SoupClientContext *context)
{
	guint32     id;
	const char *addr;
	char       *remote_address;

	do {
		/* create a unique session id */
		id = session_id_generate (share, context);
		rb_debug ("Generated session id %u", id);

		/* if already used, try again */
		addr = g_hash_table_lookup (share->priv->session_ids, GUINT_TO_POINTER (id));
	} while	(addr != NULL);

	/* store session id and remote address */
	remote_address = g_strdup (soup_client_context_get_host (context));
	g_hash_table_insert (share->priv->session_ids, GUINT_TO_POINTER (id), remote_address);

	return id;
}

static void
session_id_remove (RBDAAPShare       *share,
		   SoupClientContext *context,
		   guint32            id)
{
	g_hash_table_remove (share->priv->session_ids, GUINT_TO_POINTER (id));
}

static void
login_cb (SoupServer        *server,
	  SoupMessage       *message,
	  const char        *path,
	  GHashTable        *query,
	  SoupClientContext *context,
	  RBDAAPShare       *share)
{
/* MLOG login response
 * 	MSTT status
 * 	MLID session id
 */
	GNode *mlog;
	guint32 session_id;

	session_id = session_id_create (share, context);

	rb_debug ("Handling login session id %u", session_id);

	mlog = rb_daap_structure_add (NULL, RB_DAAP_CC_MLOG);
	rb_daap_structure_add (mlog, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
	rb_daap_structure_add (mlog, RB_DAAP_CC_MLID, session_id);

	message_set_from_rb_daap_structure (message, mlog);
	rb_daap_structure_destroy (mlog);
}

static void
logout_cb (SoupServer        *server,
	   SoupMessage       *message,
	   const char        *path,
	   GHashTable        *query,
	   SoupClientContext *context,
	   RBDAAPShare       *share)
{
	int     status;
	guint32 id;

	if (session_id_validate (share, context, message, query, &id)) {
		rb_debug ("Handling logout session id %u", id);
		session_id_remove (share, context, id);

		status = SOUP_STATUS_NO_CONTENT;
	} else {
		status = SOUP_STATUS_FORBIDDEN;
	}

	soup_message_set_status (message, status);
}

static void
update_cb (SoupServer        *server,
	   SoupMessage       *message,
	   const char        *path,
	   GHashTable        *query,
	   SoupClientContext *context,
	   RBDAAPShare       *share)
{
	guint    revision_number;
	gboolean res;

	res = get_revision_number (query, &revision_number);

	if (res && revision_number != share->priv->revision_number) {
		/* MUPD update response
		 * 	MSTT status
		 * 	MUSR server revision
		 */
		GNode *mupd;

		mupd = rb_daap_structure_add (NULL, RB_DAAP_CC_MUPD);
		rb_daap_structure_add (mupd, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
		rb_daap_structure_add (mupd, RB_DAAP_CC_MUSR, (gint32) share->priv->revision_number);

		message_set_from_rb_daap_structure (message, mupd);
		rb_daap_structure_destroy (mupd);
	} else {
		/* FIXME: This seems like a bug. It just leaks the
		 * message (and socket) without ever replying.
		 */
		g_object_ref (message);
		soup_server_pause_message (server, message);
	}
}

typedef enum {
	ITEM_ID = 0,
	ITEM_NAME,
	ITEM_KIND,
	PERSISTENT_ID,
	CONTAINER_ITEM_ID,
	SONG_ALBUM,
	SONG_GROUPING,
	SONG_ARTIST,
	SONG_BITRATE,
	SONG_BPM,
	SONG_COMMENT,
	SONG_COMPILATION,
	SONG_COMPOSER,
	SONG_DATA_KIND,
	SONG_DATA_URL,
	SONG_DATE_ADDED,
	SONG_DATE_MODIFIED,
	SONG_DISC_COUNT,
	SONG_DISC_NUMBER,
	SONG_DISABLED,
	SONG_EQ_PRESET,
	SONG_FORMAT,
	SONG_GENRE,
	SONG_DESCRIPTION,
	SONG_RELATIVE_VOLUME,
	SONG_SAMPLE_RATE,
	SONG_SIZE,
	SONG_START_TIME,
	SONG_STOP_TIME,
	SONG_TIME,
	SONG_TRACK_COUNT,
	SONG_TRACK_NUMBER,
	SONG_USER_RATING,
	SONG_YEAR,
	SONG_SORT_ARTIST,
	SONG_SORT_ALBUM
} DAAPMetaData;

struct DAAPMetaDataMap {
	gchar *tag;
	DAAPMetaData md;
};

struct DAAPMetaDataMap meta_data_map[] = {
	{"dmap.itemid",			ITEM_ID},
	{"dmap.itemname",		ITEM_NAME},
	{"dmap.itemkind",		ITEM_KIND},
	{"dmap.persistentid",		PERSISTENT_ID},
	{"dmap.containeritemid",	CONTAINER_ITEM_ID},
	{"daap.songalbum",		SONG_ALBUM},
	{"daap.songartist",		SONG_ARTIST},
	{"daap.songbitrate",		SONG_BITRATE},
	{"daap.songbeatsperminute",	SONG_BPM},
	{"daap.songcomment",		SONG_COMMENT},
	{"daap.songcompilation",	SONG_COMPILATION},
	{"daap.songcomposer",		SONG_COMPOSER},
	{"daap.songdatakind",		SONG_DATA_KIND},
	{"daap.songdataurl",		SONG_DATA_URL},
	{"daap.songdateadded",		SONG_DATE_ADDED},
	{"daap.songdatemodified",	SONG_DATE_MODIFIED},
	{"daap.songdescription",	SONG_DESCRIPTION},
	{"daap.songdisabled",		SONG_DISABLED},
	{"daap.songdisccount",		SONG_DISC_COUNT},
	{"daap.songdiscnumber",		SONG_DISC_NUMBER},
	{"daap.songeqpreset",		SONG_EQ_PRESET},
	{"daap.songformat",		SONG_FORMAT},
	{"daap.songgenre",		SONG_GENRE},
	{"daap.songgrouping",		SONG_GROUPING},
	{"daap.songrelativevolume",	SONG_RELATIVE_VOLUME},
	{"daap.songsamplerate",		SONG_SAMPLE_RATE},
	{"daap.songsize",		SONG_SIZE},
	{"daap.songstarttime",		SONG_START_TIME},
	{"daap.songstoptime",		SONG_STOP_TIME},
	{"daap.songtime",		SONG_TIME},
	{"daap.songtrackcount",		SONG_TRACK_COUNT},
	{"daap.songtracknumber",	SONG_TRACK_NUMBER},
	{"daap.songuserrating",		SONG_USER_RATING},
	{"daap.songyear",		SONG_YEAR},
	{"daap.sortartist",		SONG_SORT_ARTIST},
	{"daap.sortalbum",		SONG_SORT_ALBUM}};

typedef unsigned long long bitwise;

struct MLCL_Bits {
	GNode *mlcl;
	bitwise bits;
	gpointer pointer;
};

static gboolean
client_requested (bitwise bits,
		  gint field)
{
	return 0 != (bits & (((bitwise) 1) << field));
}

#define DMAP_ITEM_KIND_AUDIO 2
#define DAAP_SONG_DATA_KIND_NONE 0

static void
add_entry_to_mlcl (RhythmDBEntry *entry,
		   struct MLCL_Bits *mb)
{
	GNode *mlit;
	gint id;

	if (rhythmdb_entry_get_boolean (entry, RHYTHMDB_PROP_HIDDEN))
		return;

	id = rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_ENTRY_ID);
	mlit = rb_daap_structure_add (mb->mlcl, RB_DAAP_CC_MLIT);

	if (client_requested (mb->bits, ITEM_KIND))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIKD, (gchar) DMAP_ITEM_KIND_AUDIO);
	if (client_requested (mb->bits, ITEM_ID))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIID, (gint32) id);
	if (client_requested (mb->bits, ITEM_NAME))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MINM, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_TITLE));
	if (client_requested (mb->bits, PERSISTENT_ID))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MPER, (gint64) id);
	if (client_requested (mb->bits, CONTAINER_ITEM_ID))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MCTI, (gint32) id);
	if (client_requested (mb->bits, SONG_DATA_KIND))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDK, (gchar) DAAP_SONG_DATA_KIND_NONE);
	if (client_requested (mb->bits, SONG_DATA_URL))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASUL, "");
	if (client_requested (mb->bits, SONG_ALBUM))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASAL, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ALBUM));
	if (client_requested (mb->bits, SONG_GROUPING))
		rb_daap_structure_add (mlit, RB_DAAP_CC_AGRP, "");
	if (client_requested (mb->bits, SONG_ARTIST))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASAR, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST));
	if (client_requested (mb->bits, SONG_BITRATE)) {
		gulong bitrate = rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_BITRATE);
		if (bitrate != 0)
			rb_daap_structure_add (mlit, RB_DAAP_CC_ASBR, (gint32) bitrate);
	}
	if (client_requested (mb->bits, SONG_BPM))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASBT, (gint32) 0);
	if (client_requested (mb->bits, SONG_COMMENT))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASCM, "");
	if (client_requested (mb->bits, SONG_COMPILATION))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASCO, (gchar) FALSE);
	if (client_requested (mb->bits, SONG_COMPOSER))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASCP, "");
	if (client_requested (mb->bits, SONG_DATE_ADDED))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDA, (gint32) rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_FIRST_SEEN));
	if (client_requested (mb->bits, SONG_DATE_MODIFIED))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDM, (gint32) rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_MTIME));
	if (client_requested (mb->bits, SONG_DISC_COUNT))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDC, (gint32) 0);
	if (client_requested (mb->bits, SONG_DISC_NUMBER))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDN, (gint32) rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_DISC_NUMBER));
	if (client_requested (mb->bits, SONG_DISABLED))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDB, (gchar) FALSE);
	if (client_requested (mb->bits, SONG_EQ_PRESET))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASEQ, "");
	if (client_requested (mb->bits, SONG_FORMAT)) {
		const gchar *filename;
		gchar *ext;

		filename = rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION);
		ext = strrchr (filename, '.');
		if (ext == NULL) {
			/* FIXME we should use RHYTHMDB_PROP_MIMETYPE instead */
			ext = "mp3";
			rb_daap_structure_add (mlit, RB_DAAP_CC_ASFM, ext);
		} else {
			ext++;
			rb_daap_structure_add (mlit, RB_DAAP_CC_ASFM, ext);
		}
	}
	if (client_requested (mb->bits, SONG_GENRE))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASGN, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_GENRE));
	if (client_requested (mb->bits, SONG_DESCRIPTION))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASDT, "");
	if (client_requested (mb->bits, SONG_RELATIVE_VOLUME))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASRV, 0);
	if (client_requested (mb->bits, SONG_SAMPLE_RATE))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASSR, 0);
	if (client_requested (mb->bits, SONG_SIZE))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASSZ, (gint32) rhythmdb_entry_get_uint64 (entry, RHYTHMDB_PROP_FILE_SIZE));
	if (client_requested (mb->bits, SONG_START_TIME))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASST, 0);
	if (client_requested (mb->bits, SONG_STOP_TIME))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASSP, 0);
	if (client_requested (mb->bits, SONG_TIME))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASTM, (gint32) (1000 * rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_DURATION)));
	if (client_requested (mb->bits, SONG_TRACK_COUNT))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASTC, 0);
	if (client_requested (mb->bits, SONG_TRACK_NUMBER))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASTN, (gint32) rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_TRACK_NUMBER));
	if (client_requested (mb->bits, SONG_USER_RATING))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASUR, 0); /* FIXME */
	if (client_requested (mb->bits, SONG_YEAR))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASYR, (gint32) rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_YEAR));
	if (client_requested (mb->bits, SONG_SORT_ARTIST))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASSA, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_SORTNAME));
	if (client_requested (mb->bits, SONG_SORT_ALBUM))
		rb_daap_structure_add (mlit, RB_DAAP_CC_ASSU, rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ALBUM_SORTNAME));

	return;
}

static void
add_playlist_to_mlcl (RBPlaylistID *playlist_id,
		      GNode *mlcl)
{
 /* 	MLIT listing item
 * 		MIID item id
 * 		MPER persistent item id
 * 		MINM item name
 * 		MIMC item count
 */
	GNode *mlit;
	gchar *name;
	guint num_songs;
	RhythmDBQueryModel *model;

	g_object_get (playlist_id->source,
		      "name", &name,
		      "query-model", &model,
		      NULL);

	num_songs = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (model), NULL);
	g_object_unref (model);

	mlit = rb_daap_structure_add (mlcl, RB_DAAP_CC_MLIT);
	rb_daap_structure_add (mlit, RB_DAAP_CC_MIID, playlist_id->id);
	/* we don't have a persistant ID for playlists, unfortunately */
	rb_daap_structure_add (mlit, RB_DAAP_CC_MPER, (gint64) playlist_id->id);
	rb_daap_structure_add (mlit, RB_DAAP_CC_MINM, name);
	rb_daap_structure_add (mlit, RB_DAAP_CC_MIMC, (gint32) num_songs);

	g_free (name);

	return;
}

static gboolean
add_playlist_entry_to_mlcl (GtkTreeModel *model,
			    GtkTreePath *path,
			    GtkTreeIter *iter,
			    struct MLCL_Bits *mb)
{
	GNode *mlit;
	RhythmDBEntry *entry;
	gint id;

	mlit = rb_daap_structure_add (mb->mlcl, RB_DAAP_CC_MLIT);

	gtk_tree_model_get (model, iter, 0, &entry, -1);

	id = rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_ENTRY_ID);

	if (client_requested (mb->bits, ITEM_KIND))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIKD, (gchar) DMAP_ITEM_KIND_AUDIO);
	if (client_requested (mb->bits, ITEM_ID))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIID, (gint32) id);
	if (client_requested (mb->bits, CONTAINER_ITEM_ID))
		rb_daap_structure_add (mlit, RB_DAAP_CC_MCTI, (gint32) id);

	rhythmdb_entry_unref (entry);

	return FALSE;
}

static bitwise
parse_meta_str (const char *attrs)
{
	gchar **attrsv;
	guint i;
	bitwise bits = 0;
	
	attrsv = g_strsplit (attrs, ",", -1);

	for (i = 0; attrsv[i]; i++) {
		guint j;

		for (j = 0; j < G_N_ELEMENTS (meta_data_map); j++) {
			if (strcmp (meta_data_map[j].tag, attrsv[i]) == 0) {
				bits |= (((bitwise) 1) << meta_data_map[j].md);
			}
		}
	}
	
	g_strfreev (attrsv);

	return bits;
}

static bitwise
parse_meta (GHashTable *query)
{
	const gchar *attrs;

	attrs = g_hash_table_lookup (query, "meta");
	if (attrs == NULL) {
		return 0;
	}
	return parse_meta_str (attrs);
}

static void
write_next_chunk (SoupMessage *message, GInputStream *instream)
{
	gssize read_size;
	GError *error = NULL;
	gchar *chunk = g_malloc (DAAP_SHARE_CHUNK_SIZE);

	read_size = g_input_stream_read (instream, chunk, DAAP_SHARE_CHUNK_SIZE, NULL, &error);
	if (read_size > 0) {
		soup_message_body_append (message->response_body, SOUP_MEMORY_TAKE, chunk, read_size);
	} else {
		if (error != NULL) {
			rb_debug ("error reading from input stream: %s", error->message);
			g_error_free (error);
		}
		g_free (chunk);
		soup_message_body_complete (message->response_body);
	}
}

static void
chunked_message_finished (SoupMessage *message, GInputStream *instream)
{
	rb_debug ("finished sending chunked file");
	g_input_stream_close (instream, NULL, NULL);
}

static void
send_chunked_file (SoupMessage *message, RhythmDBEntry *entry, guint64 file_size, guint64 offset)
{
	GFile *file;
	GInputStream *stream;
	const char *location;
	GError *error = NULL;

	location = rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION);

	rb_debug ("sending %s chunked from offset %" G_GUINT64_FORMAT, location, offset);
	file = g_file_new_for_uri (location);
	stream = G_INPUT_STREAM (g_file_read (file, NULL, &error));
	if (error != NULL) {
		rb_debug ("couldn't open %s: %s", location, error->message);
		g_error_free (error);
		soup_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
		return;
	}

	if (offset != 0) {
		if (g_seekable_seek (G_SEEKABLE (stream), offset, G_SEEK_SET, NULL, &error) == FALSE) {
			g_warning ("error seeking: %s", error->message);
			g_input_stream_close (stream, NULL, NULL);
			soup_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
			return;
		}
		file_size -= offset;
	}

	soup_message_headers_set_encoding (message->response_headers, SOUP_ENCODING_CHUNKED);

	g_signal_connect (message, "wrote_chunk", G_CALLBACK (write_next_chunk), stream);
	g_signal_connect (message, "finished", G_CALLBACK (chunked_message_finished), stream);
	write_next_chunk (message, stream);
}

static void
mapped_file_message_finished (SoupMessage *message, GMappedFile *file)
{
	rb_debug ("finished sending mmapped file");
#if GLIB_CHECK_VERSION (2,22,0)
	g_mapped_file_unref (file);
#else
	g_mapped_file_free (file);
#endif
}

static void
send_mapped_file (SoupMessage *message, RhythmDBEntry *entry, guint64 file_size, guint64 offset)
{
	GFile *file;
	GMappedFile *mapped_file;
	char *path;
	GError *error = NULL;

	file = g_file_new_for_uri (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION));
	path = g_file_get_path (file);
	if (path == NULL) {
		rb_debug ("couldn't send %s mmapped: couldn't get path", path);
		soup_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
		g_object_unref (file);
		return;
	}
	g_object_unref (file);
	rb_debug ("sending file %s mmapped, from offset %" G_GUINT64_FORMAT, path, offset);

	mapped_file = g_mapped_file_new (path, FALSE, &error);
	if (mapped_file == NULL) {
		g_warning ("Unable to map file %s: %s", path, error->message);
		soup_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
	} else {
		soup_message_set_response (message, "application/x-dmap-tagged",
					   SOUP_MEMORY_TEMPORARY,
					   g_mapped_file_get_contents (mapped_file) + offset,
					   file_size);

		g_signal_connect (message,
				  "finished",
				  G_CALLBACK (mapped_file_message_finished),
				  mapped_file);
	}
	g_free (path);
}

static void
databases_cb (SoupServer        *server,
	      SoupMessage       *message,
	      const char        *path,
	      GHashTable        *query,
	      SoupClientContext *context,
	      RBDAAPShare       *share)

{
	const char *rest_of_path;
	/*guint revision_number;*/

	if (! session_id_validate (share, context, message, query, NULL)) {
		soup_message_set_status (message, SOUP_STATUS_FORBIDDEN);
		return;
	}

	rest_of_path = strchr (path + 1, '/');

	if (rest_of_path == NULL) {
	/* AVDB server databases
	 * 	MSTT status
	 * 	MUTY update type
	 * 	MTCO specified total count
	 * 	MRCO returned count
	 * 	MLCL listing
	 * 		MLIT listing item
	 * 			MIID item id
	 * 			MPER persistent id
	 * 			MINM item name
	 * 			MIMC item count
	 * 			MCTC container count
	 */
		GNode *avdb;
		GNode *mlcl;
		GNode *mlit;

		avdb = rb_daap_structure_add (NULL, RB_DAAP_CC_AVDB);
		rb_daap_structure_add (avdb, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
		rb_daap_structure_add (avdb, RB_DAAP_CC_MUTY, 0);
		rb_daap_structure_add (avdb, RB_DAAP_CC_MTCO, (gint32) 1);
		rb_daap_structure_add (avdb, RB_DAAP_CC_MRCO, (gint32) 1);
		mlcl = rb_daap_structure_add (avdb, RB_DAAP_CC_MLCL);
		mlit = rb_daap_structure_add (mlcl, RB_DAAP_CC_MLIT);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIID, (gint32) 1);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MPER, (gint64) 1);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MINM, share->priv->name);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIMC, (gint32) rhythmdb_entry_count_by_type (share->priv->db, share->priv->entry_type));
		rb_daap_structure_add (mlit, RB_DAAP_CC_MCTC, (gint32) 1);

		message_set_from_rb_daap_structure (message, avdb);
		rb_daap_structure_destroy (avdb);
	} else if (g_ascii_strcasecmp ("/1/items", rest_of_path) == 0) {
	/* ADBS database songs
	 * 	MSTT status
	 * 	MUTY update type
	 * 	MTCO specified total count
	 * 	MRCO returned count
	 * 	MLCL listing
	 * 		MLIT
	 * 			attrs
	 * 		MLIT
	 * 		...
	 */
		GNode *adbs;
		gint32 num_songs = rhythmdb_entry_count_by_type (share->priv->db, share->priv->entry_type);
		struct MLCL_Bits mb = {NULL,0};

		mb.bits = parse_meta (query);

		adbs = rb_daap_structure_add (NULL, RB_DAAP_CC_ADBS);
		rb_daap_structure_add (adbs, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
		rb_daap_structure_add (adbs, RB_DAAP_CC_MUTY, 0);
		rb_daap_structure_add (adbs, RB_DAAP_CC_MTCO, (gint32) num_songs);
		rb_daap_structure_add (adbs, RB_DAAP_CC_MRCO, (gint32) num_songs);
		mb.mlcl = rb_daap_structure_add (adbs, RB_DAAP_CC_MLCL);

		rhythmdb_entry_foreach_by_type (share->priv->db, share->priv->entry_type, (GFunc) add_entry_to_mlcl, &mb);

		message_set_from_rb_daap_structure (message, adbs);
		rb_daap_structure_destroy (adbs);
		adbs = NULL;
	} else if (g_ascii_strcasecmp ("/1/containers", rest_of_path) == 0) {
	/* APLY database playlists
	 * 	MSTT status
	 * 	MUTY update type
	 * 	MTCO specified total count
	 * 	MRCO returned count
	 * 	MLCL listing
	 * 		MLIT listing item
	 * 			MIID item id
	 * 			MPER persistent item id
	 * 			MINM item name
	 * 			MIMC item count
	 * 			ABPL baseplaylist (only for base)
	 * 		MLIT
	 * 		...
	 */
		GNode *aply;
		GNode *mlcl;
		GNode *mlit;

		aply = rb_daap_structure_add (NULL, RB_DAAP_CC_APLY);
		rb_daap_structure_add (aply, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
		rb_daap_structure_add (aply, RB_DAAP_CC_MUTY, 0);
		rb_daap_structure_add (aply, RB_DAAP_CC_MTCO, (gint32) 1);
		rb_daap_structure_add (aply, RB_DAAP_CC_MRCO, (gint32) 1);
		mlcl = rb_daap_structure_add (aply, RB_DAAP_CC_MLCL);
		mlit = rb_daap_structure_add (mlcl, RB_DAAP_CC_MLIT);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIID, (gint32) 1);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MPER, (gint64) 1);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MINM, share->priv->name);
		rb_daap_structure_add (mlit, RB_DAAP_CC_MIMC, (gint32) rhythmdb_entry_count_by_type (share->priv->db, share->priv->entry_type));
		rb_daap_structure_add (mlit, RB_DAAP_CC_ABPL, (gchar) 1); /* base playlist */

		g_list_foreach (share->priv->playlist_ids, (GFunc) add_playlist_to_mlcl, mlcl);

		message_set_from_rb_daap_structure (message, aply);
		rb_daap_structure_destroy (aply);

	} else if (g_ascii_strncasecmp ("/1/containers/", rest_of_path, 14) == 0) {
	/* APSO playlist songs
	 * 	MSTT status
	 * 	MUTY update type
	 * 	MTCO specified total count
	 * 	MRCO returned count
	 * 	MLCL listing
	 * 		MLIT listing item
	 * 			MIKD item kind
	 * 			MIID item id
	 * 			MCTI container item id
	 * 		MLIT
	 * 		...
	 */
		GNode *apso;
		struct MLCL_Bits mb = {NULL,0};
		gint pl_id = atoi (rest_of_path + 14);

		mb.bits = parse_meta (query);

		apso = rb_daap_structure_add (NULL, RB_DAAP_CC_APSO);
		rb_daap_structure_add (apso, RB_DAAP_CC_MSTT, (gint32) DMAP_STATUS_OK);
		rb_daap_structure_add (apso, RB_DAAP_CC_MUTY, 0);

		if (pl_id == 1) {
			gint32 num_songs = (gint32) rhythmdb_entry_count_by_type (share->priv->db, share->priv->entry_type);
			rb_daap_structure_add (apso, RB_DAAP_CC_MTCO, (gint32) num_songs);
			rb_daap_structure_add (apso, RB_DAAP_CC_MRCO, (gint32) num_songs);
			mb.mlcl = rb_daap_structure_add (apso, RB_DAAP_CC_MLCL);

			rhythmdb_entry_foreach_by_type (share->priv->db,
							share->priv->entry_type,
							(GFunc) add_entry_to_mlcl,
							&mb);
		} else {
			RBPlaylistID *id;
			GList *idl;
			guint num_songs;
			RhythmDBQueryModel *model;

			idl = g_list_find_custom (share->priv->playlist_ids,
						  GINT_TO_POINTER (pl_id),
						  _find_by_id);
			if (idl == NULL) {
				soup_message_set_status (message, SOUP_STATUS_NOT_FOUND);
				return;
			}
			id = (RBPlaylistID *)idl->data;

			mb.mlcl = rb_daap_structure_add (apso, RB_DAAP_CC_MLCL);

			g_object_get (id->source, "base-query-model", &model, NULL);
			num_songs = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (model), NULL);

			rb_daap_structure_add (apso, RB_DAAP_CC_MTCO, (gint32) num_songs);
			rb_daap_structure_add (apso, RB_DAAP_CC_MRCO, (gint32) num_songs);

			gtk_tree_model_foreach (GTK_TREE_MODEL (model), (GtkTreeModelForeachFunc) add_playlist_entry_to_mlcl, &mb);
			g_object_unref (model);
		}

		message_set_from_rb_daap_structure (message, apso);
		rb_daap_structure_destroy (apso);
	} else if (g_ascii_strncasecmp ("/1/items/", rest_of_path, 9) == 0) {
	/* just the file :) */
		const gchar *id_str;
		gint id;
		RhythmDBEntry *entry;
		const gchar *location;
		const gchar *range_header;
		guint64 file_size;
		guint64 offset = 0;

		id_str = rest_of_path + 9;
		id = atoi (id_str);

		entry = rhythmdb_entry_lookup_by_id (share->priv->db, id);
		location = rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION);
		file_size = rhythmdb_entry_get_uint64 (entry, RHYTHMDB_PROP_FILE_SIZE);

		message_add_standard_headers (message);
		soup_message_headers_append (message->response_headers, "Accept-Ranges", "bytes");

		range_header = soup_message_headers_get (message->request_headers, "Range");
		if (range_header) {
			const gchar *s;
			gchar *content_range;

			s = range_header + 6; /* bytes= */
			offset = atoll (s);

			content_range = g_strdup_printf ("bytes %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, offset, file_size, file_size);
			soup_message_headers_append (message->response_headers, "Content-Range", content_range);
			g_free (content_range);

			soup_message_set_status (message, SOUP_STATUS_PARTIAL_CONTENT);
			file_size -= offset;
		} else {
			soup_message_set_status (message, SOUP_STATUS_OK);
		}

		/* don't use chunked transfers if we can send the file mmapped,
		 * as itunes clients can't seek properly when we do.
		 */
		if (rb_uri_is_local (location)) {
			send_mapped_file (message, entry, file_size, offset);
		} else {
			send_chunked_file (message, entry, file_size, offset);
		}
	} else {
		rb_debug ("unhandled: %s\n", path);
	}
}

static void
db_entry_added_cb (RhythmDB *db,
		   RhythmDBEntry *entry,
		   RBDAAPShare *share)
{
	/* TODO: update our db version number? */
}

static void
add_db_entry (RhythmDBEntry *entry,
	      RBDAAPShare *share)
{
	db_entry_added_cb (share->priv->db, entry, share);
}

static void
db_entry_deleted_cb (RhythmDB *db,
		     RhythmDBEntry *entry,
		     RBDAAPShare *share)
{
	/* TODO: update our db version number? */
}

static void
db_entry_changed_cb (RhythmDB *db,
		     RhythmDBEntry *entry,
		     GValueArray *changes,
		     RBDAAPShare *share)
{
	if (rhythmdb_entry_get_boolean (entry, RHYTHMDB_PROP_HIDDEN)) {
		db_entry_deleted_cb (db, entry, share);
	} else {
		db_entry_added_cb (db, entry, share);
	}
}

static gboolean
soup_auth_filter (SoupAuthDomain *auth_domain,
		  SoupMessage    *msg,
		  gpointer        user_data)
{
	const char *path;

	path = soup_message_get_uri (msg)->path;
	if (g_str_has_prefix (path, "/databases/")) {
		/* Subdirectories of /databases don't actually require
		 * authentication
		 */
		return FALSE;
	} else {
		/* Everything else in auth_domain's paths, including
		 * /databases itself, does require auth.
		 */
		return TRUE;
	}
}

static gboolean
soup_auth_callback (SoupAuthDomain *auth_domain,
		    SoupMessage    *msg,
		    const char     *username,
		    gpointer        password,
		    RBDAAPShare    *share)
{
	gboolean    allowed;
	const char *path;

	path = soup_message_get_uri (msg)->path;
	rb_debug ("Auth request for %s, user %s", path, username);

	allowed = !strcmp (password, share->priv->password);
	rb_debug ("Auth request: %s", allowed ? "ALLOWED" : "DENIED");

	return allowed;
}

static gboolean
rb_daap_share_server_start (RBDAAPShare *share)
{
	int                   port = STANDARD_DAAP_PORT;
	gboolean              password_required;

	share->priv->server = soup_server_new (SOUP_SERVER_PORT, port, NULL);
	if (share->priv->server == NULL) {
		rb_debug ("Unable to start music sharing server on port %d, trying any open port", port);
		share->priv->server = soup_server_new (SOUP_SERVER_PORT, SOUP_ADDRESS_ANY_PORT, NULL);

		if (share->priv->server == NULL) {
			g_warning ("Unable to start music sharing server");
			return FALSE;
		}
	}

	share->priv->port = (guint)soup_server_get_port (share->priv->server);
	rb_debug ("Started DAAP server on port %u", share->priv->port);

	password_required = (share->priv->auth_method != RB_DAAP_SHARE_AUTH_METHOD_NONE);

	if (password_required) {
		SoupAuthDomain *auth_domain;

		auth_domain = soup_auth_domain_basic_new (SOUP_AUTH_DOMAIN_REALM, "Music Sharing",
							  SOUP_AUTH_DOMAIN_ADD_PATH, "/login",
							  SOUP_AUTH_DOMAIN_ADD_PATH, "/update",
							  SOUP_AUTH_DOMAIN_ADD_PATH, "/database",
							  SOUP_AUTH_DOMAIN_FILTER, soup_auth_filter,
							  NULL);
		soup_auth_domain_basic_set_auth_callback (auth_domain,
							  (SoupAuthDomainBasicAuthCallback)soup_auth_callback,
							  g_object_ref (share),
							  g_object_unref);
		soup_server_add_auth_domain (share->priv->server, auth_domain);
	}

	soup_server_add_handler (share->priv->server, "/server-info",
				 (SoupServerCallback) server_info_cb,
				 share, NULL);
	soup_server_add_handler (share->priv->server, "/content-codes",
				 (SoupServerCallback) content_codes_cb,
				 share, NULL);
	soup_server_add_handler (share->priv->server, "/login",
				 (SoupServerCallback) login_cb,
				 share, NULL);
	soup_server_add_handler (share->priv->server, "/logout",
				 (SoupServerCallback) logout_cb,
				 share, NULL);
	soup_server_add_handler (share->priv->server, "/update",
				 (SoupServerCallback) update_cb,
				 share, NULL);
	soup_server_add_handler (share->priv->server, "/databases",
				 (SoupServerCallback) databases_cb,
				 share, NULL);
	soup_server_run_async (share->priv->server);

	/* using direct since there is no g_uint_hash or g_uint_equal */
	share->priv->session_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

	share->priv->next_playlist_id = 2;		/* 1 already used */

	rhythmdb_entry_foreach (share->priv->db, (GFunc)add_db_entry, share);

	share->priv->entry_added_id = g_signal_connect (G_OBJECT (share->priv->db),
							"entry-added",
							G_CALLBACK (db_entry_added_cb),
							share);
	share->priv->entry_deleted_id = g_signal_connect (G_OBJECT (share->priv->db),
							  "entry-deleted",
							  G_CALLBACK (db_entry_deleted_cb),
							  share);
	share->priv->entry_changed_id = g_signal_connect (G_OBJECT (share->priv->db),
							  "entry-changed",
							  G_CALLBACK (db_entry_changed_cb),
							  share);

	share->priv->server_active = TRUE;

	return TRUE;
}

static gboolean
rb_daap_share_server_stop (RBDAAPShare *share)
{
	rb_debug ("Stopping music sharing server on port %d", share->priv->port);

	if (share->priv->server) {
		soup_server_quit (share->priv->server);
		g_object_unref (share->priv->server);
		share->priv->server = NULL;
	}

	if (share->priv->session_ids) {
		g_hash_table_destroy (share->priv->session_ids);
		share->priv->session_ids = NULL;
	}

	if (share->priv->entry_added_id != 0) {
		g_signal_handler_disconnect (share->priv->db, share->priv->entry_added_id);
		share->priv->entry_added_id = 0;
	}

	if (share->priv->entry_deleted_id != 0) {
		g_signal_handler_disconnect (share->priv->db, share->priv->entry_deleted_id);
		share->priv->entry_deleted_id = 0;
	}

	if (share->priv->entry_changed_id != 0) {
		g_signal_handler_disconnect (share->priv->db, share->priv->entry_changed_id);
		share->priv->entry_changed_id = 0;
	}

	share->priv->server_active = FALSE;

	return TRUE;
}

static gboolean
rb_daap_share_publish_start (RBDAAPShare *share)
{
	GError  *error;
	gboolean res;
	gboolean password_required;

	password_required = (share->priv->auth_method != RB_DAAP_SHARE_AUTH_METHOD_NONE);

	error = NULL;
	res = rb_daap_mdns_publisher_publish (share->priv->publisher,
					      share->priv->name,
					      share->priv->port,
					      password_required,
					      &error);

	if (res == FALSE) {
		if (error != NULL) {
			g_warning ("Unable to notify network of music sharing: %s", error->message);
			g_error_free (error);
		} else {
			g_warning ("Unable to notify network of music sharing");
		}
		return FALSE;
	} else {
		rb_debug ("Published DAAP server information to mdns");
	}

	return TRUE;
}

static gboolean
rb_daap_share_publish_stop (RBDAAPShare *share)
{
	if (share->priv->publisher) {
		gboolean res;
		GError  *error;
		error = NULL;
		res = rb_daap_mdns_publisher_withdraw (share->priv->publisher, &error);
		if (error != NULL) {
			g_warning ("Unable to withdraw music sharing service: %s", error->message);
			g_error_free (error);
		}
		return res;
	}

	share->priv->published = FALSE;
	return TRUE;
}

static void
rb_daap_share_restart (RBDAAPShare *share)
{
	gboolean res;

	rb_daap_share_server_stop (share);
	res = rb_daap_share_server_start (share);
	if (res) {
		/* To update information just publish again */
		rb_daap_share_publish_start (share);
	} else {
		rb_daap_share_publish_stop (share);
	}
}

static void
rb_daap_share_maybe_restart (RBDAAPShare *share)
{
	if (share->priv->published) {
		rb_daap_share_restart (share);
	}
}

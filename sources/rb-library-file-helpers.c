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

#include "config.h"

#include <profiles/gnome-media-profiles.h>

#include "rb-debug.h"
#include "eel-gconf-extensions.h"
#include "rb-library-file-helpers.h"

/**
 * Make sure URI ends with a slash.
 *
 * This will check if the last character of URI is a slash ('/')
 * and append one if it is not.
 * Free the returned string with g_free.
 */
char *
rb_library_check_append_slash (const char *uri)
{
	if (uri == NULL)
		return NULL;

	if (g_str_has_suffix (uri, "/"))
		return g_strdup (uri);
	else
		return g_strconcat (uri, "/", NULL);
}

/**
 * Perform magic on a path to make it safe.
 *
 * This will always replace '/' with '-', and optionally make the file name
 * shell-friendly. This involves removing replacing shell metacharacters and all
 * whitespace with '_'. Also any leading periods are removed so that the files
 * don't end up being hidden.
 */
static char *
sanitize_path (const char *str)
{
	gchar *s;

	/* Skip leading periods, otherwise files disappear... */
	while (*str == '.')
		str++;

	s = g_strdup(str);
	/* Replace path seperators with a hyphen */
	g_strdelimit (s, "/", '-');
	if (eel_gconf_get_boolean (CONF_LIBRARY_STRIP_CHARS)) {
		/* Replace separators with a hyphen */
		g_strdelimit (s, "\\:|", '-');
		/* Replace all other weird characters to whitespace */
		g_strdelimit (s, "*?&!\'\"$()`>{}", ' ');
		/* Replace all whitespace with underscores */
		/* TODO: I'd like this to compress whitespace aswell */
		g_strdelimit (s, "\t ", '_');
	}

	return s;
}

char *
rb_library_sanitize_pattern (const char *pat)
{
	if (eel_gconf_get_boolean (CONF_LIBRARY_STRIP_CHARS)) {
		gchar *s;

		s = g_strdup (pat);
		g_strdelimit (s, "\t ", '_');
		return s;
	} else {
		return g_strdup (pat);
	}
}

/*
 * Parse a filename pattern and replace markers with values from a RhythmDBEntry
 *
 * Valid markers so far are:
 * %at -- album title
 * %aa -- album artist
 * %aA -- album artist (lowercase)
 * %as -- album artist sortname
 * %aS -- album artist sortname (lowercase)
 * %aY -- album release year
 * %an -- album disc number
 * %aN -- album disc number, zero padded
 * %ag -- album genre
 * %aG -- album genre (lowercase)
 * %tn -- track number (i.e 8)
 * %tN -- track number, zero padded (i.e 08)
 * %tt -- track title
 * %ta -- track artist
 * %tA -- track artist (lowercase)
 * %ts -- track artist sortname
 * %tS -- track artist sortname (lowercase)
 */
char *
rb_library_filepath_parse_pattern (RhythmDB *db,
		const char *pattern,
		RhythmDBEntry *entry)
{
	/* p is the pattern iterator, i is a general purpose iterator */
	const char *p;
	char *temp;
	GString *s;
	GValue *value;
	RBRefString *albumartist;

	if (pattern == NULL || pattern[0] == 0)
		return g_strdup (" ");

	/* request album artist (this is sort of temporary) */
	value = rhythmdb_entry_request_extra_metadata (db, entry, RHYTHMDB_PROP_ALBUM_ARTIST);
	if (value != NULL) {
		albumartist = rb_refstring_new (g_value_get_string (value));
		g_value_unset (value);
		g_free (value);
	} else {
		albumartist = rhythmdb_entry_get_refstring (entry, RHYTHMDB_PROP_ARTIST);
	}

	s = g_string_new (NULL);

	p = pattern;
	while (*p) {
		char *string = NULL;
		char *t;

		/* If not a % marker, copy and continue */
		if (*p != '%') {
			g_string_append_c (s, *p++);
			/* Explicit increment as we continue past the increment */
			continue;
		}

		/* Is a % marker, go to next and see what to do */
		switch (*++p) {
		case '%':
			/*
			 * Literal %
			 */
			g_string_append_c (s, '%');
			break;
		case 'a':
			/*
			 * Album tag
			 */
			switch (*++p) {
			case 't':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ALBUM));
				break;
			case 'T':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ALBUM_FOLDED));
				break;
			case 'a':
				string = sanitize_path (rb_refstring_get (albumartist));
				break;
			case 'A':
				string = sanitize_path (rb_refstring_get (albumartist));
				break;
			case 's':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_SORTNAME));
				break;
			case 'S':
				t = g_utf8_strdown (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_SORTNAME), -1);
				string = sanitize_path (t);
				g_free (t);
				break;
			case 'y':
				string = g_strdup_printf ("%u", (guint)rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_YEAR));
				break;
			case 'n':
				string = g_strdup_printf ("%u", (guint)rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_DISC_NUMBER));
				break;
			case 'N':
				string = g_strdup_printf ("%02u", (guint)rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_DISC_NUMBER));
				break;
			case 'g':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_GENRE));
				break;
			case 'G':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_GENRE_FOLDED));
				break;
			default:
				string = g_strdup_printf ("%%a%c", *p);
			}

			break;

		case 't':
			/*
			 * Track tag
			 */
			switch (*++p) {
			case 't':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_TITLE));
				break;
			case 'T':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_TITLE_FOLDED));
				break;
			case 'a':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST));
				break;
			case 'A':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_FOLDED));
				break;
			case 's':
				string = sanitize_path (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_SORTNAME));
				break;
			case 'S':
				t = g_utf8_strdown (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_ARTIST_SORTNAME), -1);
				string = sanitize_path (t);
				g_free (t);
				break;
			case 'n':
				/* Track number */
				string = g_strdup_printf ("%u", (guint)rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_TRACK_NUMBER));
				break;
			case 'N':
				/* Track number, zero-padded */
				string = g_strdup_printf ("%02u", (guint)rhythmdb_entry_get_ulong (entry, RHYTHMDB_PROP_TRACK_NUMBER));
				break;
			default:
				string = g_strdup_printf ("%%t%c", *p);
			}

			break;

		default:
			string = g_strdup_printf ("%%%c", *p);
		}

		if (string)
			g_string_append (s, string);
		g_free (string);

		++p;
	}

	temp = s->str;
	g_string_free (s, FALSE);
	rb_refstring_unref (albumartist);
	return temp;
}

/**
 * Build the absolute filename for the specified track.
 *
 * The base path is the extern variable 'base_path', the format to use
 * is the extern variable 'file_pattern'. Free the result when you
 * have finished with it.
 *
 * Stolen from Sound-Juicer
 */
char*
rb_library_build_filename (RhythmDB *db, const char *dest_dir, RhythmDBEntry *entry)
{
	GFile *library_location;
	GFile *dir;
	GFile *dest;
	char *realfile;
	char *realpath;
	char *filename;
	char *string = NULL;
	char *extension = NULL;
	char *tmp;
	char *layout_path;
	char *layout_filename;
	char *preferred_format;

	g_return_val_if_fail (dest_dir != NULL, NULL);

	layout_path = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_PATH);
	layout_filename = eel_gconf_get_string (CONF_LIBRARY_LAYOUT_FILENAME);
	preferred_format = eel_gconf_get_string (CONF_LIBRARY_PREFERRED_FORMAT);

	if (layout_path == NULL || layout_filename == NULL || preferred_format == NULL) {
		/* emit warning */
		rb_debug ("Could not retrieve settings from GConf");
		goto out;
	}

	tmp = rb_library_sanitize_pattern (layout_filename);
	g_free (layout_filename);
	layout_filename = tmp;

	realpath = rb_library_filepath_parse_pattern (db, layout_path, entry);

	library_location = g_file_new_for_uri (dest_dir);
	dir = g_file_resolve_relative_path (library_location, realpath);
	g_object_unref (library_location);
	g_free (realpath);

	if (g_str_has_prefix (rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_MIMETYPE), "audio/x-raw")) {
		GMAudioProfile *profile;
		profile = gm_audio_profile_lookup (preferred_format);
		if (profile)
			extension = g_strdup (gm_audio_profile_get_extension (profile));
	}

	if (extension == NULL) {
		const char *uri;
		const char *loc;
		char *tmp;

		/* use the old extension. strip anything after a '?' for http/daap/etc */
		uri = rhythmdb_entry_get_string (entry, RHYTHMDB_PROP_LOCATION);
		loc = g_utf8_strrchr (uri, -1, '.');
		if (loc == NULL)
			loc = g_utf8_strrchr (uri, -1, '/');
		if (loc == NULL)
			loc = uri;

		extension = g_strdup (loc + 1);

		tmp = g_utf8_strchr (extension, -1, '?');
		if (tmp)
			*tmp = '\0';
	}

	realfile = rb_library_filepath_parse_pattern (db, layout_filename, entry);
	if (extension) {
		filename = g_strdup_printf ("%s.%s", realfile, extension);
		g_free (realfile);
	} else {
		filename = realfile;
	}

	dest = g_file_resolve_relative_path (dir, filename);
	g_object_unref (dir);
	g_free (extension);
	g_free (filename);

	string = g_file_get_uri (dest);
	g_object_unref (dest);
 out:
	g_free (layout_path);
	g_free (layout_filename);
	g_free (preferred_format);

	return string;
}


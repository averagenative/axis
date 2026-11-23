/*
 * axis-compat-path.h - the server's address, and mapping its paths to ours
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The address: the only place the daemon makes requests to.  It is an
 * http or https URL with a host, an optional port, no credentials, no
 * query and no fragment, and no path beyond "/" (the daemon adds the
 * endpoints itself).  Anything else is refused when it is written.
 *
 * The path map: the media server sees its libraries under its own paths
 * ("/data/movies"), Axis sees the same directories under others
 * ("/media/disk3/movies").  An entry maps a prefix of the first kind to a
 * prefix of the second; the longest matching prefix, on a path-segment
 * boundary, wins.  A path that is not absolute or has an empty, "." or ".."
 * segment is never mapped (and so never matched to a file).  Nothing is
 * guessed: a library whose folders no entry reaches is *unmapped*, and a
 * suggestion is only offered, never applied.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-compat-path.c).
 */

#ifndef AXIS_COMPAT_PATH_H
#define AXIS_COMPAT_PATH_H

#include <glib.h>

#include "core/axis-http-origin.h"

G_BEGIN_DECLS

#define AXIS_COMPAT_ADDRESS_MAX_BYTES AXIS_HTTP_ORIGIN_MAX_BYTES

/* The longest path, in bytes, that is a path here: as typed, and as a
 * mapping gives it. */
#define AXIS_COMPAT_PATH_MAX_BYTES 1024

/* A media server's address is an origin of the outbound client
 * (core/axis-http-origin.h). */
typedef AxisHttpOrigin AxisCompatAddress;

void
axis_compat_address_clear(AxisCompatAddress *address);

/**
 * axis_compat_address_parse:
 * @text: the setting
 * @address: (out): the parts
 * @problem: (out) (optional) (transfer full): why not, a sentence
 *   fragment without any of the text in it
 *
 * axis_http_origin_parse() under the media server's name.
 *
 * Returns: %TRUE if @text is an acceptable address
 */
gboolean
axis_compat_address_parse(
	const gchar       *text,
	AxisCompatAddress *address,
	gchar            **problem
);

/* axis_compat_address_parse() as a setting validator (axis-setting.h). */
gboolean
axis_compat_address_validate(
	const gchar  *text,
	gchar       **reason
);

/**
 * axis_compat_path_normalize:
 * @text: a path as typed or as the server states it
 * @problem: (out) (optional) (transfer full): why not
 *
 * Returns: (transfer full) (nullable): the path with no trailing slash
 *   ("/" stays "/"), or %NULL unless it is absolute, valid UTF-8 without
 *   control characters, at most 1024 bytes, with no empty, "." or ".."
 *   segment
 */
gchar *
axis_compat_path_normalize(
	const gchar  *text,
	gchar       **problem
);

typedef struct
{
	gint64  id;		/* the entry, or 0 */
	gchar  *server;		/* normalised */
	gchar  *axis;		/* normalised */
	gchar  *kind;		/* the kind of server whose paths these are ("plex"), or %NULL */
} AxisCompatPathEntry;

void
axis_compat_path_entry_free(AxisCompatPathEntry *entry);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatPathEntry, axis_compat_path_entry_free)

/**
 * axis_compat_path_map_apply:
 * @entries: (element-type AxisCompatPathEntry): the map
 * @server_path: a path as the server states it
 *
 * Returns: (transfer full) (nullable): the path under Axis's names, or
 *   %NULL when @server_path is not a clean absolute path or no entry
 *   reaches it
 */
gchar *
axis_compat_path_map_apply(
	GPtrArray   *entries,
	const gchar *server_path
);

/* Whether @path is @folder or lies below it, on a segment boundary (both
 * clean absolute paths; %FALSE if either is not). */
gboolean
axis_compat_path_holds(
	const gchar *folder,
	const gchar *path
);

/**
 * axis_compat_path_map_reverse:
 * @entries: (element-type AxisCompatPathEntry): the map
 * @axis_path: a path as Axis sees it
 *
 * The other way round: the longest entry whose Axis prefix holds @axis_path
 * (on a segment boundary) gives it under the server's names.
 *
 * Returns: (transfer full) (nullable): the path as the server sees it, or
 *   %NULL when @axis_path is not a clean absolute path, no entry reaches it
 *   or the result would pass %AXIS_COMPAT_PATH_MAX_BYTES (such a path is
 *   never sent)
 */
gchar *
axis_compat_path_map_reverse(
	GPtrArray   *entries,
	const gchar *axis_path
);

typedef struct
{
	gint64  id;
	gchar  *path;		/* the root's absolute path on its volume, normalised */
	gchar  *media_type;
} AxisCompatRootRef;

void
axis_compat_root_ref_free(AxisCompatRootRef *root);

/**
 * axis_compat_root_for_path:
 * @roots: (element-type AxisCompatRootRef)
 * @axis_path: a clean absolute path
 * @rel_path: (out) (optional) (transfer full): the path below the root
 *
 * Returns: the root @axis_path is inside (the longest such root), or %NULL
 */
const AxisCompatRootRef *
axis_compat_root_for_path(
	GPtrArray    *roots,
	const gchar  *axis_path,
	gchar       **rel_path
);

typedef enum
{
	AXIS_MAP_UNMAPPED,
	AXIS_MAP_PARTLY,
	AXIS_MAP_MAPPED
} AxisCompatMapState;

typedef struct
{
	gchar *folder;		/* the server's folder */
	gint64 root_id;
	gchar *root_path;
} AxisCompatSuggestion;

void
axis_compat_suggestion_free(AxisCompatSuggestion *suggestion);

typedef struct
{
	AxisCompatMapState state;
	GArray            *root_ids;		/* gint64, ascending, no duplicates */
	GPtrArray         *unmapped_folders;	/* utf8 */
	GPtrArray         *suggestions;		/* AxisCompatSuggestion */
} AxisCompatMapping;

void
axis_compat_mapping_clear(AxisCompatMapping *mapping);

/**
 * axis_compat_map_library:
 * @folders: (element-type utf8): the library's folders
 * @entries: (element-type AxisCompatPathEntry): the path map
 * @roots: (element-type AxisCompatRootRef): Axis's roots
 * @mapping: (out): the result
 *
 * A folder reaches a root when, under the path map, it is the root, lies
 * inside it, or contains it.  The library is *mapped* only if every folder
 * reaches at least one root; with some folders reaching none it is
 * *partly* mapped, and with none *unmapped*; verdicts use only mapped
 * libraries.  For each folder that reaches nothing, a suggestion is made
 * when exactly one root has the longest run of trailing path segments in
 * common with it (at least one).
 */
void
axis_compat_map_library(
	GPtrArray         *folders,
	GPtrArray         *entries,
	GPtrArray         *roots,
	AxisCompatMapping *mapping
);

G_END_DECLS

#endif /* AXIS_COMPAT_PATH_H */

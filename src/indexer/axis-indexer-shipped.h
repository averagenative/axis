/*
 * axis-indexer-shipped.h - the site definitions Axis ships, and how an
 * administrator's override of one is told apart from it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A built-in definition (add-builtin-torrent-indexers) is a file of
 * data/definitions compiled into the daemon (a GResource), in the format the
 * engine runs (indexer/def/axis-def.h), written and tested with Axis.  Its id
 * begins with %AXIS_INDEXER_SHIPPED_PREFIX, which no definition an
 * administrator installs may use unless it overrides a built-in one, and it
 * states its version.
 *
 * Each is a row of indexer_definition under its own id, written at every
 * start (axis_indexers_sync_shipped(), indexer/axis-indexer.h): so an
 * indexer runs it as it runs any installed definition, an upgrade's newer
 * copy replaces the stored one, and a database restored from an older backup
 * has the current copies again.  An administrator's override of one is a
 * row of its own, "<id>%AXIS_INDEXER_OVERRIDE_SUFFIX", which the start never
 * touches; the one an indexer runs is decided whenever it is read
 * (axis_indexer_shipped_choose()): the override when it is usable and newer
 * than the built-in copy, else the built-in copy.
 *
 * GLib only, no I/O beyond the compiled-in resource.
 */

#ifndef AXIS_INDEXER_SHIPPED_H
#define AXIS_INDEXER_SHIPPED_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_INDEXER_SHIPPED_PREFIX  "axis-"
#define AXIS_INDEXER_OVERRIDE_SUFFIX ".override"

/**
 * AxisIndexerShipped:
 * @def_id: its id ("axis-limetorrents")
 * @name: its name
 * @description: (nullable): its one sentence
 * @version: its version (1 and up)
 * @body: the file, NUL-terminated
 * @length: its length in bytes
 * @sha256: of the file, in hex
 *
 * One built-in definition, as compiled in.
 */
typedef struct
{
	gchar *def_id;
	gchar *name;
	gchar *description;
	gint   version;
	gchar *body;
	gsize  length;
	gchar *sha256;
} AxisIndexerShipped;

/* How many there are, and each (sorted by id); static, valid for the
 * process.  One that does not read as a usable definition with a version is
 * left out, with a warning: the tests make sure none is. */
guint
axis_indexer_shipped_count(void);

const AxisIndexerShipped *
axis_indexer_shipped_at(guint index);

/* The built-in definition of @def_id, or %NULL. */
const AxisIndexerShipped *
axis_indexer_shipped_find(const gchar *def_id);

/* Whether @def_id is of the form Axis keeps for itself (begins with "axis-"). */
gboolean
axis_indexer_shipped_reserved(const gchar *def_id);

/* "<id>.override": the row an override of @def_id is kept in. */
gchar *
axis_indexer_shipped_override_id(const gchar *def_id);

/**
 * AxisIndexerShippedChoice:
 * @AXIS_INDEXER_SHIPPED_BUILT_IN: the built-in copy is used; there is no
 *   override
 * @AXIS_INDEXER_SHIPPED_OVERRIDDEN: the override is used
 * @AXIS_INDEXER_SHIPPED_IGNORED: there is an override, but it is not used:
 *   not newer than the built-in copy (after an upgrade), or not usable by
 *   this engine
 */
typedef enum
{
	AXIS_INDEXER_SHIPPED_BUILT_IN,
	AXIS_INDEXER_SHIPPED_OVERRIDDEN,
	AXIS_INDEXER_SHIPPED_IGNORED
} AxisIndexerShippedChoice;

/**
 * axis_indexer_shipped_choose:
 * @shipped: the built-in definition
 * @override: (nullable): the stored override's file
 * @override_version: (out) (optional): its version, 0 when there is none or
 *   it does not say
 * @why: (out) (optional) (transfer full): when it is ignored, why, in a
 *   sentence
 *
 * Which of the two an indexer runs.
 *
 * Returns: what was chosen
 */
AxisIndexerShippedChoice
axis_indexer_shipped_choose(
	const AxisIndexerShipped  *shipped,
	const gchar               *override,
	gint                      *override_version,
	gchar                    **why
);

/**
 * axis_indexer_shipped_check_override:
 * @shipped: the built-in definition it would override
 * @data: the file
 * @length: its length
 * @problem: (out) (optional) (transfer full): why it is refused, a sentence
 *
 * Whether @data may be stored as an override of @shipped: it reads as a
 * definition, has @shipped's id, says a version higher than @shipped's, is
 * usable by this engine, and builds the requests of a movie, an episode and
 * a test against its first address (asking nothing).
 */
gboolean
axis_indexer_shipped_check_override(
	const AxisIndexerShipped  *shipped,
	const gchar               *data,
	gsize                      length,
	gchar                    **problem
);

/**
 * axis_indexer_read_file_at_most:
 * @path: a file (a definition, a saved page)
 * @max_bytes: the most that is read
 * @length: (out): its length
 *
 * Reads @path when it is a regular file of at most @max_bytes, looking at
 * its size before reading, and reading no more than @max_bytes and one byte
 * whatever the size said (a file that grows while it is read is refused).
 * Fails with %G_FILE_ERROR.
 *
 * Returns: (transfer full): its bytes, NUL-terminated, or %NULL
 */
gchar *
axis_indexer_read_file_at_most(
	const gchar  *path,
	gsize         max_bytes,
	gsize        *length,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_INDEXER_SHIPPED_H */

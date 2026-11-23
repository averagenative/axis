/*
 * axis-library-filter.h - the file list's filters, sort and paging, parsed
 * strictly from a query string
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The flat file list (/library/files) is described by a query string of a
 * fixed set of typed parameters:
 *
 *   root        a root's id (digits)
 *   kind        video, audio, subtitle, image, text, other or symlink
 *   container   a container name as ffprobe gives it ("matroska,webm")
 *   vcodec      the main video stream's codec ("hevc")
 *   res         sd, 720p, 1080p, 2160p or other
 *   hdr         sdr, hlg, hdr10, hdr10plus, dolby_vision or uncertain
 *   acodec      the main audio stream's codec ("eac3")
 *   pinned      yes or no
 *   probe       ok, failed, timeout or none (not probed yet)
 *   state       present (the default), suspected or missing
 *   min_mib     smallest size, in MiB (digits)
 *   max_mib     largest size, in MiB
 *   name        text the file's name contains, any case (3 to 64 characters)
 *   changed     yes or no: the scanner saw new content since the file was
 *               last probed (its probe, if any, may describe older content)
 *   volume      a volume's id (digits)
 *   type        what the root holds: movies, tv, music, audiobooks, books,
 *               downloads or other
 *   under       a folder's id (library_dir): the files in it and below it
 *   ids        up to 24 file ids, comma-separated (for a group of copies)
 *   min_bpp     video bits per pixel and frame, in thousandths, at least
 *   max_bpp     likewise, below (so the two give a half-open range)
 *   leftover    sample, extras, subtitle, junk or any: the files a leftover
 *               rule of the Analysis pages matches (axis-analysis-rules.h)
 *   target      a conversion profile's slug (lower-case letters, digits and
 *               '_', axis-profile.h): the files it is for
 *   rev         with a target: the number of the profile revision to
 *               decide with (digits); without, the profile's current one
 *   outcome     with a target: eligible (would be converted, its volume
 *               there or not), skipped (any reason), or one reason's id
 *               (axis-estimate.h): what the estimator decides for the file
 *               now.  These two are not SQL: the list evaluates them with
 *               the estimator (axis-estimator.h); axis_library_list_files()
 *               refuses a filter that has them
 *   sort        name (the default), size, modified, duration or bitrate
 *   dir         asc or desc (asc for name, desc for the others)
 *   after       the id of the last row of the previous page (keyset paging)
 *   since       a scan id (for the "list changed" notice only)
 *
 * Anything else is refused: an unknown parameter, a repeated one, a value
 * that is not of the parameter's type or not one the daemon defines, a
 * malformed percent-escape, a query over AXIS_LIBRARY_FILTER_MAX_QUERY
 * bytes.  An empty value means "not set" (a form posts every field).  A
 * file's name never appears in a query: a page's cursor is a row id.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-library-filter.c).
 */

#ifndef AXIS_LIBRARY_FILTER_H
#define AXIS_LIBRARY_FILTER_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_LIBRARY_FILTER_MAX_QUERY  2048
#define AXIS_LIBRARY_FILTER_MAX_PAIRS  32
#define AXIS_LIBRARY_FILTER_NAME_CHARS 64
#define AXIS_LIBRARY_FILTER_NAME_MIN_CHARS 3
#define AXIS_LIBRARY_FILTER_TOKEN_MAX  64
#define AXIS_LIBRARY_FILTER_MAX_IDS    24
#define AXIS_LIBRARY_FILTER_MAX_BPP    1000000
#define AXIS_LIBRARY_FILTER_MAX_REV    9999999

#define AXIS_LIBRARY_FILTER_ERROR (axis_library_filter_error_quark())

typedef enum
{
	AXIS_LIBRARY_FILTER_ERROR_INVALID
} AxisLibraryFilterError;

GQuark
axis_library_filter_error_quark(void);

typedef enum
{
	AXIS_LIBRARY_SORT_NAME,
	AXIS_LIBRARY_SORT_SIZE,
	AXIS_LIBRARY_SORT_MODIFIED,
	AXIS_LIBRARY_SORT_DURATION,
	AXIS_LIBRARY_SORT_BITRATE
} AxisLibrarySort;

#define AXIS_LIBRARY_SORT_COUNT 5

/**
 * AxisLibraryFilter:
 * @root_id: 0 for any root
 * @kind: (nullable): one of the static kind names
 * @container: (nullable) (owned): a container name
 * @video_codec: (nullable) (owned): a codec name
 * @resolution: (nullable): one of the static resolution classes
 * @hdr: (nullable): one of the static HDR classes, or "uncertain"
 * @audio_codec: (nullable) (owned): a codec name
 * @pinned: -1 for either, 1 for pinned, 0 for not pinned
 * @probe: (nullable): "ok", "failed", "timeout" or "none"
 * @state: always set: "present", "suspected" or "missing"
 * @min_mib: -1 for none
 * @max_mib: -1 for none
 * @name: (nullable) (owned): text the name contains
 * @changed: -1 for either, 1 for changed, 0 for not changed
 * @volume_id: 0 for any volume
 * @type: (nullable): one of the static media type names
 * @dir_id: 0 for any folder
 * @ids: (nullable) (owned) (array length=n_ids): file ids, ascending
 * @n_ids: how many
 * @min_bpp: -1 for none
 * @max_bpp: -1 for none
 * @leftover: (nullable): one of the static leftover rule names, or "any"
 * @target: (nullable) (owned): a profile slug (well-formed; whether there is
 *   such a profile is the estimator's to say)
 * @revision: with @target, a revision number; 0 for the current one
 * @outcome: (nullable): "eligible", "skipped" or a static reason id; only
 *   with @target
 * @sort: the sort
 * @descending: the direction (after the default for the sort is applied)
 * @after: the row after which the page starts, 0 for the first page
 * @since: a scan id, 0 for none
 */
typedef struct
{
	gint64           root_id;
	const gchar     *kind;
	gchar           *container;
	gchar           *video_codec;
	const gchar     *resolution;
	const gchar     *hdr;
	gchar           *audio_codec;
	gint             pinned;
	const gchar     *probe;
	const gchar     *state;
	gint64           min_mib;
	gint64           max_mib;
	gchar           *name;
	gint             changed;
	gint64           volume_id;
	const gchar     *type;
	gint64           dir_id;
	gint64          *ids;
	guint            n_ids;
	gint64           min_bpp;
	gint64           max_bpp;
	const gchar     *leftover;
	gchar           *target;
	gint             revision;
	const gchar     *outcome;
	AxisLibrarySort  sort;
	gboolean         descending;
	gint64           after;
	gint64           since;
} AxisLibraryFilter;

/* The defaults: no filter, present files, by name. */
void
axis_library_filter_init(AxisLibraryFilter *filter);

void
axis_library_filter_clear(AxisLibraryFilter *filter);

/* A copy, with its own strings. */
void
axis_library_filter_copy(
	AxisLibraryFilter       *to,
	const AxisLibraryFilter *from
);

/**
 * axis_library_filter_parse:
 * @query: (nullable): the raw query string, without the '?'
 * @filter: (out caller-allocates): the result; initialised first, and left
 *   initialised (empty) when the query is refused
 * @error: (nullable): return location for an error; the message is fixed
 *   text, never the query's
 *
 * Returns: %TRUE if the whole query is valid
 */
gboolean
axis_library_filter_parse(
	const gchar        *query,
	AxisLibraryFilter  *filter,
	GError            **error
);

/**
 * axis_library_filter_to_query:
 * @filter: a filter
 * @with_cursor: whether to include "after" and "since"
 *
 * The canonical query string of @filter: parsing it gives an equal filter.
 * Values are percent-encoded; "" for the defaults.
 *
 * Returns: (transfer full): the query string, without the '?'
 */
gchar *
axis_library_filter_to_query(
	const AxisLibraryFilter *filter,
	gboolean                 with_cursor
);

/* Whether any filter other than the sort, direction and cursor is set. */
gboolean
axis_library_filter_is_narrowed(const AxisLibraryFilter *filter);

/* "name", "size", ... */
const gchar *
axis_library_sort_to_string(AxisLibrarySort sort);

/* "Name", "Size", ... */
const gchar *
axis_library_sort_label(AxisLibrarySort sort);

/* Whether the sort's natural direction is descending (everything but name). */
gboolean
axis_library_sort_default_descending(AxisLibrarySort sort);

/* Option lists, for the filter form: NULL-terminated. */
const gchar *const *
axis_library_filter_kinds(void);

const gchar *const *
axis_library_filter_resolutions(void);

const gchar *const *
axis_library_filter_hdr_classes(void);

const gchar *const *
axis_library_filter_probe_states(void);

const gchar *const *
axis_library_filter_states(void);

const gchar *const *
axis_library_filter_types(void);

/* The values "outcome" accepts: "eligible", "skipped", then every reason. */
const gchar *const *
axis_library_filter_outcomes(void);

/* Whether the filter has conditions only the estimator can evaluate
 * (target, outcome). */
gboolean
axis_library_filter_needs_estimator(const AxisLibraryFilter *filter);

G_END_DECLS

#endif /* AXIS_LIBRARY_FILTER_H */

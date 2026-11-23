/*
 * axis-estimator-load.h - the loader's statements and row reader, shared by
 * the estimator's files
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_ESTIMATOR_LOAD_H
#define AXIS_ESTIMATOR_LOAD_H

#include "transcode/axis-estimator-private.h"

G_BEGIN_DECLS

/* A loaded file: the facts the estimator reads, and the record it owns. */
typedef struct
{
	AxisEstimateFile  file;
	AxisMediaInfo    *info;
} AxisEstimateLoaded;

AxisEstimateRows *
axis_estimate_rows_new(void);

/*
 * The columns of one file, over library_file f, library_root r, volume v
 * and media_info m (AXIS_ESTIMATE_SQL_FROM).  "counted" is the Analysis'
 * hard-link rule (axis-analysis-refresh.c): of the present paths of one
 * volume and inode with more than one link, the lowest id outside a
 * downloads root counts the bytes, else the lowest.  The last column is how
 * many present audio files the file's album unit has (AXIS_ESTIMATE_SQL_UNIT).
 */
#define AXIS_ESTIMATE_SQL_COLUMNS AXIS_ESTIMATE_SQL_COLUMNS_BASE ", " AXIS_ESTIMATE_SQL_UNIT_FILES

#define AXIS_ESTIMATE_SQL_COLUMNS_BASE \
	"f.id, f.root_id, f.volume_id, f.kind, r.media_type, f.state, f.size, f.nlink, f.pinned, f.changed," \
	" f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode," \
	" v.state IN ('online', 'read_only')," \
	" (f.nlink <= 1 OR f.id = (SELECT h1.id FROM library_file h1 JOIN library_root h2 ON h2.id = h1.root_id" \
	"   WHERE h1.volume_id = f.volume_id AND h1.inode = f.inode AND h1.state = 'present' AND h1.nlink > 1" \
	"   ORDER BY (h2.media_type = 'downloads'), h1.id LIMIT 1))," \
	" f.rel_path, " AXIS_ESTIMATE_SQL_UNIT "," \
	" m.file_id IS NOT NULL, m.status, m.probe_version, m.file_size, m.mtime_sec, m.mtime_nsec, m.ctime_sec," \
	" m.ctime_nsec, m.inode, m.container, m.duration_ms, m.bitrate, m.bitrate_estimated, m.streams_omitted," \
	" m.video_streams, m.audio_streams, m.subtitle_streams, m.video_codec, m.width, m.height, m.resolution_class," \
	" m.bit_depth, m.frame_rate, m.hdr_format, m.hdr_uncertain, m.bits_per_pixel, m.interlaced, m.audio_codec," \
	" m.audio_channels, m.audio_sample_rate, m.audio_bit_depth, m.audio_lossless, m.has_embedded_art," \
	" m.art_width, m.art_height"

/*
 * How many present audio files the file's album unit has (0 for a file
 * that is no album's).  The files under "artist/album/" are a range of the
 * prefix index with both ends in the index condition: from left(album/,
 * 600) to left(album0, 600) ('0' follows '/'; rel_path is COLLATE "C"),
 * which for an album of 599 characters or more is the one 600-character
 * prefix; starts_with() then checks the whole path.  (A bound outside the
 * index condition would read every path of the root after the album's.)
 */
#define AXIS_ESTIMATE_SQL_UNIT_FILES \
	"(CASE WHEN f.kind = 'audio' AND r.media_type IN ('music', 'audiobooks') THEN" \
	"   CASE WHEN f.rel_path ~ '^[^/]+/[^/]+/' THEN" \
	"    (SELECT count(*) FROM library_file u WHERE u.root_id = f.root_id AND u.state = 'present'" \
	"     AND u.kind = 'audio'" \
	"     AND left(u.rel_path, 600) >= left(substring(f.rel_path FROM '^[^/]+/[^/]+') || '/', 600)" \
	"     AND left(u.rel_path, 600) <= left(substring(f.rel_path FROM '^[^/]+/[^/]+') || '0', 600)" \
	"     AND starts_with(u.rel_path, substring(f.rel_path FROM '^[^/]+/[^/]+') || '/'))" \
	"   ELSE (SELECT count(*) FROM library_file u WHERE u.root_id = f.root_id AND u.dir_hash = f.dir_hash" \
	"     AND u.state = 'present' AND u.kind = 'audio' AND u.rel_path !~ '^[^/]+/[^/]+/') END" \
	"  ELSE 0 END)"

#define AXIS_ESTIMATE_SQL_FROM \
	"FROM library_file f JOIN library_root r ON r.id = f.root_id JOIN volume v ON v.id = f.volume_id" \
	" LEFT JOIN media_info m ON m.file_id = f.id"

/*
 * For the short statements of lists and file pages, in their transaction.
 * The correlated subqueries above give the loader a planned cost far above
 * jit_above_cost, so PostgreSQL would compile it with LLVM each time: about
 * 380 ms (measured: tests/scale/test-library-scale.c) for a statement that
 * then runs in 1.5 ms, paid by every batch of a list.
 */
#define AXIS_ESTIMATE_SQL_NO_JIT "SET LOCAL jit = off"

/* The files of $1 (an id array). */
#define AXIS_ESTIMATE_SQL_FILES \
	"SELECT " AXIS_ESTIMATE_SQL_COLUMNS " " AXIS_ESTIMATE_SQL_FROM " WHERE f.id = ANY($1::bigint[])"

/*
 * Every present audio file of the roots of the files of $1 (an id array)
 * whose unit is one of theirs.  The members are found first, from each
 * file's unit alone (its root, its "artist/album" prefix or its folder) by
 * an index for each of the unit's two forms: the prefix range of
 * AXIS_ESTIMATE_SQL_UNIT_FILES, or the folder for a shallower file.  The
 * LATERAL subquery (kept a subquery by OFFSET 0) has those as its only outer
 * values, so that the range is the index condition; joined directly, the
 * planner may read the whole root and filter it.
 */
#define AXIS_ESTIMATE_SQL_UNIT_MEMBERS \
	"SELECT " AXIS_ESTIMATE_SQL_COLUMNS " " AXIS_ESTIMATE_SQL_FROM \
	" WHERE f.id IN (SELECT w.id FROM" \
	"  (SELECT o.root_id, o.dir_hash, o.rel_path ~ '^[^/]+/[^/]+/' AS deep," \
	"    substring(o.rel_path FROM '^[^/]+/[^/]+') AS album FROM library_file o" \
	"    WHERE o.id = ANY($1::bigint[])) t" \
	"  CROSS JOIN LATERAL (" \
	"   SELECT u.id FROM library_file u WHERE t.deep AND u.root_id = t.root_id" \
	"    AND left(u.rel_path, 600) >= left(t.album || '/', 600)" \
	"    AND left(u.rel_path, 600) <= left(t.album || '0', 600)" \
	"    AND starts_with(u.rel_path, t.album || '/') AND u.state = 'present' AND u.kind = 'audio'" \
	"   UNION ALL" \
	"   SELECT u.id FROM library_file u WHERE NOT t.deep AND u.root_id = t.root_id AND u.dir_hash = t.dir_hash" \
	"    AND u.rel_path !~ '^[^/]+/[^/]+/' AND u.state = 'present' AND u.kind = 'audio'" \
	"   OFFSET 0) w)" \
	" ORDER BY f.id"

/* The streams of the files of $1 (an id array). */
#define AXIS_ESTIMATE_SQL_STREAMS \
	"SELECT file_id, idx, kind, codec, profile, duration_ms, bitrate, bitrate_estimated, language, is_default," \
	" is_forced, attached_pic, width, height, bit_depth, frame_rate, real_frame_rate, field_order, hdr_format," \
	" hdr_uncertain, channels, sample_rate, bit_depth_audio, lossless, subtitle_kind" \
	" FROM media_stream WHERE file_id = ANY($1::bigint[]) ORDER BY file_id, idx"

/* Reads row @row of a result of AXIS_ESTIMATE_SQL_COLUMNS (strings go to
 * @rows' chunk); the caller adds it to @rows->owned. */
AxisEstimateLoaded *
axis_estimator_read_row(
	AxisEstimateRows *rows,
	AxisDbResult     *result,
	guint             row
);

/* Reads the streams of the probed files of @rows->owned from @from on. */
gboolean
axis_estimator_attach_streams(
	AxisDbConn       *conn,
	AxisEstimateRows *rows,
	guint             from,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_ESTIMATOR_LOAD_H */

/*
 * axis-probe-hdr.c - which HDR format a video stream is, and when that is
 * not certain
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The result decides which files may later be converted, and converting a
 * Dolby Vision file as if it were SDR ruins it (the picture comes out
 * green and purple) with the original deleted afterwards.  So the rule is
 * asymmetric: a stream is classified only on positive, consistent
 * evidence, and every other case is "uncertain", which later work treats
 * as not eligible.  A wrong "uncertain" costs a conversion that did not
 * happen; a wrong "sdr" costs a file.
 *
 * Evidence comes from the stream's colour fields (primaries, transfer,
 * matrix), its bit depth, and side data, both the stream's own and that of
 * its first frames (static HDR10 metadata is often only in the bitstream).
 * In order:
 *
 *  1. Dolby Vision: a "DOVI configuration record", or Dolby Vision frame
 *     metadata.  Classified dolby_vision whatever the colour fields say.
 *     The record must have exactly the expected shape (all eight integer
 *     fields in range, version 1 or 2, profile 4, 5, 7, 8, 9 or 10, RPU and
 *     base layer present, and for profiles 5, 7 and 8 the enhancement-layer
 *     and compatibility values those profiles use); otherwise the stream is
 *     still dolby_vision, but uncertain.  Frame metadata without a record,
 *     or together with HDR10+, is uncertain too.
 *  2. Side data whose name looks like dynamic HDR or Dolby Vision but is
 *     none of the recognised ones: not classified, uncertain.
 *  3. HDR10+: the exact "HDR Dynamic Metadata SMPTE2094-40 (HDR10+)" side
 *     data.  hdr10plus; uncertain unless the transfer is PQ with
 *     BT.2020 primaries, at least 10 bits and static mastering metadata.
 *  4. PQ (smpte2084): HDR10 when there is mastering display metadata, the
 *     primaries are BT.2020 and the depth is at least 10 bits; PQ without
 *     mastering metadata is not classified; the other mismatches are HDR10
 *     but uncertain.
 *  5. HLG (arib-std-b67): hlg, uncertain unless BT.2020 primaries and at
 *     least 10 bits.
 *  6. An SDR transfer (bt709, smpte170m, ...): sdr, uncertain if there is
 *     HDR metadata beside it, or BT.2020 primaries.
 *  7. No transfer signalled: sdr only for 8-bit video with no HDR metadata
 *     and no BT.2020 primaries; anything deeper is not classified, because
 *     a Dolby Vision profile 5 stream looks exactly like that when the
 *     container carries no record.
 *  8. Any other transfer: not classified, uncertain.
 *
 * And a stream whose frame side data was wanted but could not be read is
 * uncertain, unless it was settled by Dolby Vision above.
 *
 * HDR10 static metadata values (version 2 of the probe).  A mastering
 * display entry that claims a luminance, or a content light entry, must
 * have every value, each in range (axis-probe-model.h), and every entry of
 * the stream and its frames must give the same values; otherwise an HDR10,
 * HDR10+ or HLG stream is uncertain (an SDR one already is, for carrying
 * such data at all).  The values are stored with the stream only when that
 * holds, so a stored value is one an encoder can be given back exactly.
 */

#include "probe/axis-probe-model.h"

#include <string.h>

/* ── Names ──────────────────────────────────────────────────────── */

static const struct
{
	AxisHdrFormat  format;
	const gchar   *name;
	const gchar   *label;
} formats[] = {
	{ AXIS_HDR_SDR,          "sdr",          "SDR" },
	{ AXIS_HDR_HLG,          "hlg",          "HLG" },
	{ AXIS_HDR_HDR10,        "hdr10",        "HDR10" },
	{ AXIS_HDR_HDR10PLUS,    "hdr10plus",    "HDR10+" },
	{ AXIS_HDR_DOLBY_VISION, "dolby_vision", "Dolby Vision" }
};

const gchar *
axis_hdr_format_to_string(AxisHdrFormat format)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(formats); i++) {
		if (formats[i].format == format)
			return formats[i].name;
	}
	return NULL;
}

gboolean
axis_hdr_format_from_string(
	const gchar   *text,
	AxisHdrFormat *format
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(formats); i++) {
		if (strcmp(text, formats[i].name) == 0) {
			*format = formats[i].format;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_hdr_format_label(AxisHdrFormat format)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(formats); i++) {
		if (formats[i].format == format)
			return formats[i].label;
	}
	return "Not classified";
}

/* ── Notes: every reason for doubt, as a fixed sentence ─────────── */

#define NOTE_FRAMES        "A frame could not be read to look for HDR or Dolby Vision data."
#define NOTE_DV_BAD        "A Dolby Vision record is present but not in the expected form."
#define NOTE_DV_RPU        "Dolby Vision frame data is present without a configuration record."
#define NOTE_DV_AND_PLUS   "Both Dolby Vision and HDR10+ metadata are present."
#define NOTE_DYNAMIC_OTHER "Dynamic HDR metadata of an unrecognised kind is present."
#define NOTE_PLUS_NOT_PQ   "HDR10+ metadata is present but the transfer function is not PQ."
#define NOTE_PLUS_PARTIAL  "HDR10+ metadata is present without the static HDR10 metadata that goes with it."
#define NOTE_PQ_NO_MASTER  "PQ transfer function without mastering display metadata."
#define NOTE_PQ_PARTIAL    "PQ transfer function with primaries other than BT.2020, or under 10 bits."
#define NOTE_HLG_PARTIAL   "HLG transfer function with primaries other than BT.2020, or under 10 bits."
#define NOTE_SDR_HDR_DATA  "HDR metadata is present but the transfer function says SDR."
#define NOTE_SDR_WIDE      "BT.2020 primaries with an SDR transfer function."
#define NOTE_NONE_HDR_DATA "HDR metadata is present but no transfer function is signalled."
#define NOTE_NONE_WIDE     "BT.2020 primaries but no transfer function is signalled."
#define NOTE_NONE_DEEP     "More than 8 bits per sample and no colour signalling."
#define NOTE_OTHER_TC      "An unusual transfer function is signalled."
#define NOTE_COLOR_CONFLICT "The stream and its frames disagree about the colour signalling."
#define NOTE_HDR10_VALUES  "The HDR10 mastering display or content light values are incomplete, out of range, " \
	"or not the same everywhere they are given."

/* ── Reading side data ──────────────────────────────────────────── */

void
axis_hdr_signals_init(AxisHdrSignals *signals)
{
	memset(signals, 0, sizeof(*signals));
	signals->dv_version_major = AXIS_PROBE_UNKNOWN;
	signals->dv_version_minor = AXIS_PROBE_UNKNOWN;
	signals->dv_profile = AXIS_PROBE_UNKNOWN;
	signals->dv_level = AXIS_PROBE_UNKNOWN;
	signals->dv_rpu_present = AXIS_PROBE_UNKNOWN;
	signals->dv_el_present = AXIS_PROBE_UNKNOWN;
	signals->dv_bl_present = AXIS_PROBE_UNKNOWN;
	signals->dv_bl_compat_id = AXIS_PROBE_UNKNOWN;
}

/* A rational as ffprobe prints it ("1/10000"), or a plain number. */
static gboolean
read_ratio(
	const AxisJsonNode *node,
	gdouble            *value
){
	const gchar *text = axis_json_scalar_text(node);
	const gchar *slash;
	gint64 numerator;
	gint64 denominator;
	g_autofree gchar *head = NULL;
	g_autofree gchar *tail = NULL;

	if (text == NULL)
		return FALSE;
	slash = strchr(text, '/');
	if (slash == NULL)
		return axis_json_get_double(node, -1e18, 1e18, value);
	head = g_strndup(text, (gsize)(slash - text));
	tail = g_strdup(slash + 1);
	{
		gchar *end = NULL;

		numerator = g_ascii_strtoll(head, &end, 10);
		if (head[0] == '\0' || *end != '\0')
			return FALSE;
		denominator = g_ascii_strtoll(tail, &end, 10);
		if (tail[0] == '\0' || *end != '\0' || denominator <= 0)
			return FALSE;
	}
	*value = (gdouble)numerator / (gdouble)denominator;
	return TRUE;
}

/*
 * A rational ("34000/50000", "17/25") or a plain number, as a whole number
 * of 1/@units, rounded: integer arithmetic for rationals, so that a value
 * an encoder wrote comes back exactly.  Negative values, a zero or negative
 * denominator, and anything above 10^13 units are refused.
 */
static gboolean
read_units(
	const AxisJsonNode *node,
	gint64              units,
	gint64             *value
){
	const gchar *text = axis_json_scalar_text(node);
	const gchar *slash;
	gchar *end = NULL;
	gint64 numerator;
	gint64 denominator;
	gdouble plain;

	if (text == NULL || text[0] == '\0')
		return FALSE;
	slash = strchr(text, '/');
	if (slash == NULL) {
		if (!axis_json_get_double(node, 0.0, 1e13 / (gdouble)units, &plain) || !(plain >= 0.0))
			return FALSE;
		*value = (gint64)(plain * (gdouble)units + 0.5);
		return TRUE;
	}
	if (!g_ascii_isdigit(text[0]) || !g_ascii_isdigit(slash[1]))
		return FALSE;
	numerator = g_ascii_strtoll(text, &end, 10);
	if (end != slash || numerator < 0 || numerator > G_GINT64_CONSTANT(10000000000000))
		return FALSE;
	denominator = g_ascii_strtoll(slash + 1, &end, 10);
	if (*end != '\0' || denominator <= 0 || denominator > G_GINT64_CONSTANT(10000000000000))
		return FALSE;
	/* numerator * units stays below 2^63 for units up to 10^5. */
	if (units <= 0 || units > 100000)
		return FALSE;
	*value = (numerator * units + denominator / 2) / denominator;
	return *value <= G_GINT64_CONSTANT(10000000000000);
}

/* A coordinate in 0.00002 (it must fit an int to be compared at all). */
static gboolean
read_chroma(
	const AxisJsonNode *entry,
	const gchar        *name,
	gint               *out
){
	gint64 value;

	if (!read_units(axis_json_member(entry, name), AXIS_HDR10_CHROMA_MAX, &value) || value > G_MAXINT)
		return FALSE;
	*out = (gint)value;
	return TRUE;
}

/* Keeps the first valid set of values; any other that differs is a
 * conflict. */
static void
remember_hdr10(
	AxisHdrSignals          *signals,
	const AxisHdr10Metadata *seen
){
	AxisHdr10Metadata *kept = &signals->hdr10;

	if (seen->mastering) {
		if (!kept->mastering) {
			kept->mastering = TRUE;
			kept->red_x = seen->red_x;
			kept->red_y = seen->red_y;
			kept->green_x = seen->green_x;
			kept->green_y = seen->green_y;
			kept->blue_x = seen->blue_x;
			kept->blue_y = seen->blue_y;
			kept->white_x = seen->white_x;
			kept->white_y = seen->white_y;
			kept->min_luminance = seen->min_luminance;
			kept->max_luminance = seen->max_luminance;
		} else if (kept->red_x != seen->red_x || kept->red_y != seen->red_y || kept->green_x != seen->green_x ||
		           kept->green_y != seen->green_y || kept->blue_x != seen->blue_x || kept->blue_y != seen->blue_y ||
		           kept->white_x != seen->white_x || kept->white_y != seen->white_y ||
		           kept->min_luminance != seen->min_luminance || kept->max_luminance != seen->max_luminance) {
			signals->hdr10_conflict = TRUE;
		}
	}
	if (seen->light) {
		if (!kept->light) {
			kept->light = TRUE;
			kept->max_content = seen->max_content;
			kept->max_average = seen->max_average;
		} else if (kept->max_content != seen->max_content || kept->max_average != seen->max_average) {
			signals->hdr10_conflict = TRUE;
		}
	}
}

/* A mastering display entry that claims a luminance: all ten values, in
 * range. */
static void
read_mastering(
	const AxisJsonNode *entry,
	AxisHdrSignals     *signals,
	gboolean            from_frame
){
	AxisHdr10Metadata seen;
	gboolean complete;

	memset(&seen, 0, sizeof(seen));
	seen.mastering = TRUE;
	complete = read_chroma(entry, "red_x", &seen.red_x) && read_chroma(entry, "red_y", &seen.red_y) &&
		read_chroma(entry, "green_x", &seen.green_x) && read_chroma(entry, "green_y", &seen.green_y) &&
		read_chroma(entry, "blue_x", &seen.blue_x) && read_chroma(entry, "blue_y", &seen.blue_y) &&
		read_chroma(entry, "white_point_x", &seen.white_x) && read_chroma(entry, "white_point_y", &seen.white_y) &&
		read_units(axis_json_member(entry, "min_luminance"), 10000, &seen.min_luminance) &&
		read_units(axis_json_member(entry, "max_luminance"), 10000, &seen.max_luminance);
	if (!complete || !axis_hdr10_metadata_valid(&seen)) {
		signals->hdr10_bad = TRUE;
		return;
	}
	if (from_frame)
		signals->mastering_in_frames = TRUE;
	remember_hdr10(signals, &seen);
}

/* A content light entry: both levels, whole numbers in range. */
static void
read_content_light(
	const AxisJsonNode *entry,
	AxisHdrSignals     *signals,
	gboolean            from_frame
){
	AxisHdr10Metadata seen;
	gint64 content;
	gint64 average;

	memset(&seen, 0, sizeof(seen));
	if (!axis_json_get_integer(axis_json_member(entry, "max_content"), 0, AXIS_HDR10_LIGHT_MAX, &content) ||
	    !axis_json_get_integer(axis_json_member(entry, "max_average"), 0, AXIS_HDR10_LIGHT_MAX, &average)) {
		signals->hdr10_bad = TRUE;
		return;
	}
	seen.light = TRUE;
	seen.max_content = (gint)content;
	seen.max_average = (gint)average;
	if (!axis_hdr10_metadata_valid(&seen)) {
		signals->hdr10_bad = TRUE;
		return;
	}
	if (from_frame)
		signals->light_in_frames = TRUE;
	remember_hdr10(signals, &seen);
}

/* The eight fields of a DOVI configuration record. */
static void
read_dovi_record(
	const AxisJsonNode *entry,
	AxisHdrSignals     *signals
){
	gint64 value;
	gboolean bad = FALSE;
	gint fields[8];
	static const gchar *const names[8] = {
		"dv_version_major", "dv_version_minor", "dv_profile", "dv_level", "rpu_present_flag",
		"el_present_flag", "bl_present_flag", "dv_bl_signal_compatibility_id"
	};
	guint i;

	for (i = 0; i < 8; i++) {
		fields[i] = AXIS_PROBE_UNKNOWN;
		if (axis_json_get_integer(axis_json_member(entry, names[i]), 0, 255, &value))
			fields[i] = (gint)value;
		else
			bad = TRUE;
	}

	/* A second record, or one that differs from the first, is not a shape
	 * to trust. */
	if (signals->dovi_record) {
		gint before[8];

		before[0] = signals->dv_version_major;
		before[1] = signals->dv_version_minor;
		before[2] = signals->dv_profile;
		before[3] = signals->dv_level;
		before[4] = signals->dv_rpu_present;
		before[5] = signals->dv_el_present;
		before[6] = signals->dv_bl_present;
		before[7] = signals->dv_bl_compat_id;
		for (i = 0; i < 8; i++) {
			if (before[i] != fields[i])
				bad = TRUE;
		}
	}

	signals->dovi_record = TRUE;
	signals->dv_version_major = fields[0];
	signals->dv_version_minor = fields[1];
	signals->dv_profile = fields[2];
	signals->dv_level = fields[3];
	signals->dv_rpu_present = fields[4];
	signals->dv_el_present = fields[5];
	signals->dv_bl_present = fields[6];
	signals->dv_bl_compat_id = fields[7];

	if (!bad) {
		if (fields[0] != 1 && fields[0] != 2)
			bad = TRUE;
		if (fields[2] != 4 && fields[2] != 5 && fields[2] != 7 && fields[2] != 8 &&
		    fields[2] != 9 && fields[2] != 10)
			bad = TRUE;
		if (fields[3] > 13)
			bad = TRUE;
		if (fields[4] != 1 || fields[6] != 1 || fields[5] > 1)
			bad = TRUE;
		if (fields[2] == 5 && (fields[5] != 0 || fields[7] != 0))
			bad = TRUE;
		if (fields[2] == 8 && (fields[5] != 0 || (fields[7] != 1 && fields[7] != 2 && fields[7] != 4)))
			bad = TRUE;
		if (fields[2] == 7 && fields[5] != 1)
			bad = TRUE;
	}
	if (bad)
		signals->dovi_shape_bad = TRUE;
}

/* Whether a side data name looks like dynamic HDR or Dolby Vision. */
static gboolean
looks_dynamic(const gchar *type)
{
	g_autofree gchar *lower = g_ascii_strdown(type, -1);
	static const gchar *const words[] = {
		"dolby", "dovi", "dynamic", "hdr", "2094", "vivid", "cuva", "rpu", NULL
	};
	guint i;

	for (i = 0; words[i] != NULL; i++) {
		if (strstr(lower, words[i]) != NULL)
			return TRUE;
	}
	return FALSE;
}

void
axis_probe_hdr_read_side_data(
	const AxisJsonNode *side_data_list,
	AxisHdrSignals     *signals
){
	axis_probe_hdr_read_side_data_from(side_data_list, signals, FALSE);
}

void
axis_probe_hdr_read_side_data_from(
	const AxisJsonNode *side_data_list,
	AxisHdrSignals     *signals,
	gboolean            from_frame
){
	guint length = MIN(axis_json_length(side_data_list), 64u);
	guint i;

	for (i = 0; i < length; i++) {
		const AxisJsonNode *entry = axis_json_element(side_data_list, i);
		const gchar *type = axis_json_string(axis_json_member(entry, "side_data_type"));
		gdouble value;

		if (type == NULL)
			continue;
		if (strcmp(type, "Mastering display metadata") == 0) {
			/* All zeros is what a writer that had nothing puts there. */
			if (read_ratio(axis_json_member(entry, "max_luminance"), &value) && value > 0) {
				signals->has_mastering = TRUE;
				read_mastering(entry, signals, from_frame);
			}
		} else if (strcmp(type, "Content light level metadata") == 0) {
			if (axis_json_member(entry, "max_content") != NULL ||
			    axis_json_member(entry, "max_average") != NULL) {
				signals->has_content_light = TRUE;
				read_content_light(entry, signals, from_frame);
			}
		} else if (strcmp(type, "DOVI configuration record") == 0) {
			read_dovi_record(entry, signals);
		} else if (strcmp(type, "Dolby Vision Metadata") == 0 ||
		           strcmp(type, "Dolby Vision RPU Data") == 0) {
			signals->dovi_metadata = TRUE;
		} else if (strcmp(type, "HDR Dynamic Metadata SMPTE2094-40 (HDR10+)") == 0) {
			signals->hdr10plus = TRUE;
		} else if (looks_dynamic(type)) {
			signals->dynamic_other = TRUE;
		}
	}
}

/* ── Colour fields ──────────────────────────────────────────────── */

typedef enum
{
	TRANSFER_UNSPECIFIED,
	TRANSFER_SDR,
	TRANSFER_PQ,
	TRANSFER_HLG,
	TRANSFER_OTHER
} Transfer;

static Transfer
transfer_class(const gchar *name)
{
	static const gchar *const sdr[] = {
		"bt709", "smpte170m", "bt470m", "bt470bg", "smpte240m", "iec61966-2-1", "iec61966-2-4",
		"bt1361e", "bt2020-10", "bt2020-12", "gamma22", "gamma28", NULL
	};
	guint i;

	if (name == NULL || name[0] == '\0' || strcmp(name, "unknown") == 0 ||
	    strcmp(name, "unspecified") == 0 || strcmp(name, "reserved") == 0)
		return TRANSFER_UNSPECIFIED;
	if (strcmp(name, "smpte2084") == 0)
		return TRANSFER_PQ;
	if (strcmp(name, "arib-std-b67") == 0)
		return TRANSFER_HLG;
	for (i = 0; sdr[i] != NULL; i++) {
		if (strcmp(name, sdr[i]) == 0)
			return TRANSFER_SDR;
	}
	return TRANSFER_OTHER;
}

static gboolean
has_hdr_signals(const AxisHdrSignals *signals)
{
	return signals->has_mastering || signals->has_content_light || signals->dovi_record ||
		signals->dovi_metadata || signals->hdr10plus || signals->dynamic_other;
}

gboolean
axis_probe_hdr_wants_frames(const AxisMediaStream *stream)
{
	Transfer transfer;

	if (stream->kind != AXIS_STREAM_VIDEO || stream->attached_pic)
		return FALSE;
	transfer = transfer_class(stream->color_transfer);
	/* The one case that cannot be anything but SDR: 8 bits, not wide
	 * gamut, no HDR data on the stream. */
	if (stream->bit_depth == 8 && (transfer == TRANSFER_SDR || transfer == TRANSFER_UNSPECIFIED) &&
	    g_strcmp0(stream->color_primaries, "bt2020") != 0 && !has_hdr_signals(&stream->signals))
		return FALSE;
	return TRUE;
}

/* ── Classification ─────────────────────────────────────────────── */

static void
verdict(
	AxisMediaStream *stream,
	AxisHdrFormat    format,
	gboolean         uncertain,
	const gchar     *note
){
	stream->hdr_format = format;
	stream->hdr_uncertain = uncertain;
	stream->hdr_note = uncertain ? note : NULL;
}

static void
classify_dolby_vision(AxisMediaStream *stream)
{
	const AxisHdrSignals *signals = &stream->signals;
	const gchar *note = NULL;

	if (signals->dovi_record) {
		stream->dv_profile = signals->dv_profile;
		stream->dv_level = signals->dv_level;
		stream->dv_bl_compat_id = signals->dv_bl_compat_id;
		/* Only the values Dolby defines for the base layer's
		 * compatibility say anything; the others stay unknown. */
		if (signals->dv_bl_compat_id == 1)
			stream->dv_hdr10_compatible = AXIS_TRI_YES;
		else if (signals->dv_bl_compat_id == 0 || signals->dv_bl_compat_id == 2 ||
		         signals->dv_bl_compat_id == 4)
			stream->dv_hdr10_compatible = AXIS_TRI_NO;
		if (signals->dovi_shape_bad)
			note = NOTE_DV_BAD;
	} else {
		note = NOTE_DV_RPU;
	}
	if (note == NULL && (signals->hdr10plus || signals->dynamic_other))
		note = signals->hdr10plus ? NOTE_DV_AND_PLUS : NOTE_DYNAMIC_OTHER;

	verdict(stream, AXIS_HDR_DOLBY_VISION, note != NULL, note);
}

void
axis_probe_hdr_classify(
	AxisMediaStream *stream,
	gboolean         frames_read
){
	const AxisHdrSignals *signals = &stream->signals;
	Transfer transfer = transfer_class(stream->color_transfer);
	gboolean wide = g_strcmp0(stream->color_primaries, "bt2020") == 0;
	gboolean deep = stream->bit_depth >= 10;
	AxisHdrFormat format = AXIS_HDR_NONE;
	gboolean uncertain = FALSE;
	const gchar *note = NULL;

	gboolean values_bad = signals->hdr10_bad || signals->hdr10_conflict;

	stream->hdr_format = AXIS_HDR_NONE;
	stream->hdr_uncertain = FALSE;
	stream->hdr_note = NULL;
	stream->dv_profile = AXIS_PROBE_UNKNOWN;
	stream->dv_level = AXIS_PROBE_UNKNOWN;
	stream->dv_bl_compat_id = AXIS_PROBE_UNKNOWN;
	stream->dv_hdr10_compatible = AXIS_TRI_UNKNOWN;
	memset(&stream->hdr10, 0, sizeof(stream->hdr10));

	if (stream->kind != AXIS_STREAM_VIDEO || stream->attached_pic)
		return;

	/* The values are stored only when every entry agreed and was valid. */
	if (!values_bad && axis_hdr10_metadata_valid(&signals->hdr10))
		stream->hdr10 = signals->hdr10;

	/* 1. Dolby Vision settles it, whatever else is said. */
	if (signals->dovi_record || signals->dovi_metadata) {
		classify_dolby_vision(stream);
		return;
	}

	/* 2. Dynamic metadata that is not HDR10+. */
	if (signals->dynamic_other && !signals->hdr10plus) {
		verdict(stream, AXIS_HDR_NONE, TRUE, NOTE_DYNAMIC_OTHER);
		return;
	}

	/* 3. HDR10+. */
	if (signals->hdr10plus) {
		if (transfer != TRANSFER_PQ)
			note = NOTE_PLUS_NOT_PQ;
		else if (!signals->has_mastering || !wide || !deep)
			note = NOTE_PLUS_PARTIAL;
		else if (values_bad)
			note = NOTE_HDR10_VALUES;
		if (signals->dynamic_other && note == NULL)
			note = NOTE_DYNAMIC_OTHER;
		verdict(stream, AXIS_HDR_HDR10PLUS, note != NULL, note);
		return;
	}

	switch (transfer) {
	case TRANSFER_PQ:
		/* 4. */
		if (!signals->has_mastering) {
			format = AXIS_HDR_NONE;
			uncertain = TRUE;
			note = NOTE_PQ_NO_MASTER;
		} else {
			format = AXIS_HDR_HDR10;
			if (!wide || !deep) {
				uncertain = TRUE;
				note = NOTE_PQ_PARTIAL;
			} else if (values_bad) {
				uncertain = TRUE;
				note = NOTE_HDR10_VALUES;
			}
		}
		break;
	case TRANSFER_HLG:
		/* 5. */
		format = AXIS_HDR_HLG;
		if (!wide || !deep) {
			uncertain = TRUE;
			note = NOTE_HLG_PARTIAL;
		} else if ((signals->has_mastering || signals->has_content_light) && values_bad) {
			uncertain = TRUE;
			note = NOTE_HDR10_VALUES;
		}
		break;
	case TRANSFER_SDR:
		/* 6. */
		format = AXIS_HDR_SDR;
		if (signals->has_mastering || signals->has_content_light) {
			uncertain = TRUE;
			note = NOTE_SDR_HDR_DATA;
		} else if (wide) {
			uncertain = TRUE;
			note = NOTE_SDR_WIDE;
		}
		break;
	case TRANSFER_UNSPECIFIED:
		/* 7. */
		if (signals->has_mastering || signals->has_content_light) {
			uncertain = TRUE;
			note = NOTE_NONE_HDR_DATA;
		} else if (wide) {
			uncertain = TRUE;
			note = NOTE_NONE_WIDE;
		} else if (stream->bit_depth == 8) {
			format = AXIS_HDR_SDR;
		} else {
			uncertain = TRUE;
			note = NOTE_NONE_DEEP;
		}
		break;
	case TRANSFER_OTHER:
	default:
		/* 8. */
		uncertain = TRUE;
		note = NOTE_OTHER_TC;
		break;
	}

	if (!uncertain && stream->frame_color_conflict) {
		uncertain = TRUE;
		note = NOTE_COLOR_CONFLICT;
	}
	if (!uncertain && !frames_read && axis_probe_hdr_wants_frames(stream)) {
		uncertain = TRUE;
		note = NOTE_FRAMES;
	}

	verdict(stream, format, uncertain, note);
}

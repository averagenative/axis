/*
 * axis-convert-check.c - the argument vector's grammar, read apart from the
 * code that writes it, and the check of an output against its plan
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * axis_convert_args_valid() knows the vector only as a grammar: which
 * options there are, which values each may take, which arguments come in
 * which part.  It shares nothing with axis-convert-plan.c but the colour
 * name table and the format whitelist, so a mistake in the builder that
 * puts anything else into the vector (a library string above all) fails
 * here.  The builder refuses a plan whose vector fails; the fuzz target
 * checks every vector with it too.
 */

#include "transcode/axis-convert.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ── The alphabet ───────────────────────────────────────────────── */

/* Every character an argument may have.  No space, quote, $, ;, `, |, &,
 * <, >, newline or anything beyond ASCII: none of it is needed, and text
 * of a file would bring it. */
static gboolean
alphabet_ok(const gchar *text)
{
	const gchar *p;

	if (text == NULL || text[0] == '\0')
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && strchr(":=+-_.,/()", *p) == NULL)
			return FALSE;
	}
	return TRUE;
}

/* ── Values ─────────────────────────────────────────────────────── */

static gboolean
one_of(
	const gchar        *value,
	const gchar *const *list
){
	guint i;

	for (i = 0; value != NULL && list[i] != NULL; i++) {
		if (strcmp(value, list[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* A decimal number of at most 10 digits in [@low, @high], no sign, no
 * leading zero. */
static gboolean
number_in(
	const gchar *text,
	gint64       low,
	gint64       high
){
	gint64 value = 0;
	gsize n = 0;
	const gchar *p;

	if (text == NULL || text[0] == '\0' || (text[0] == '0' && text[1] != '\0'))
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p) || ++n > 10)
			return FALSE;
		value = value * 10 + (*p - '0');
	}
	return value >= low && value <= high;
}

static const gchar *const video_encoders[] = {
	"libx265", "libx264", "hevc_nvenc", "h264_nvenc", "hevc_vaapi", "h264_vaapi", NULL
};
static const gchar *const other_codecs[] = { "copy", "aac", "mov_text", "srt", NULL };
static const gchar *const x26x_presets[] = {
	"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL
};
static const gchar *const nvenc_preset_names[] = { "p1", "p2", "p3", "p4", "p5", "p6", "p7", NULL };
static const gchar *const picture_formats[] = { "yuv420p", "yuv420p10le", "p010le", NULL };
static const gchar *const dispositions[] = {
	"+default+forced", "+default-forced", "-default+forced", "-default-forced", NULL
};
static const gchar *const blanked_tags[] = {
	"BPS=", "BPS-eng=", "NUMBER_OF_FRAMES=", "NUMBER_OF_FRAMES-eng=", "NUMBER_OF_BYTES=", "NUMBER_OF_BYTES-eng=",
	"_STATISTICS_WRITING_APP=", "_STATISTICS_WRITING_APP-eng=", "_STATISTICS_WRITING_DATE_UTC=",
	"_STATISTICS_WRITING_DATE_UTC-eng=", "_STATISTICS_TAGS=", "_STATISTICS_TAGS-eng=", "title=", NULL
};

/* "setparams=k=v:k=v", "format=nv12|p010le", "hwupload", comma-separated,
 * in that order, each at most once. */
static gboolean
filter_ok(const gchar *text)
{
	g_auto(GStrv) parts = g_strsplit(text, ",", -1);
	guint stage = 0;
	guint i;

	for (i = 0; parts[i] != NULL; i++) {
		const gchar *part = parts[i];

		if (g_str_has_prefix(part, "setparams=") && stage == 0) {
			g_auto(GStrv) pairs = g_strsplit(part + strlen("setparams="), ":", -1);
			guint seen = 0;
			guint j;

			if (pairs[0] == NULL)
				return FALSE;
			for (j = 0; pairs[j] != NULL; j++) {
				const gchar *eq = strchr(pairs[j], '=');
				g_autofree gchar *key = NULL;
				const gchar *value;
				guint bit;

				if (eq == NULL)
					return FALSE;
				key = g_strndup(pairs[j], (gsize)(eq - pairs[j]));
				value = eq + 1;
				if (strcmp(key, "color_primaries") == 0 && one_of(value, axis_convert_colour_names("primaries")))
					bit = 1;
				else if (strcmp(key, "color_trc") == 0 && one_of(value, axis_convert_colour_names("transfer")))
					bit = 2;
				else if (strcmp(key, "colorspace") == 0 && one_of(value, axis_convert_colour_names("matrix")))
					bit = 4;
				else if (strcmp(key, "range") == 0 && one_of(value, axis_convert_colour_names("range")))
					bit = 8;
				else
					return FALSE;
				if ((seen & bit) != 0)
					return FALSE;
				seen |= bit;
			}
			stage = 1;
		} else if ((strcmp(part, "format=nv12") == 0 || strcmp(part, "format=p010le") == 0) && stage < 2) {
			stage = 2;
		} else if (strcmp(part, "hwupload") == 0 && stage == 2) {
			stage = 3;
		} else {
			return FALSE;
		}
	}
	/* A format is only there to be uploaded. */
	return i > 0 && stage != 2;
}

/* x265's master-display: G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min). */
static gboolean
master_display_ok(const gchar *text)
{
	static const gchar *const heads[] = { "G(", "B(", "R(", "WP(", "L(" };
	const gchar *p = text;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(heads); i++) {
		gchar *end = NULL;
		gint64 a;
		gint64 b;

		if (!g_str_has_prefix(p, heads[i]))
			return FALSE;
		p += strlen(heads[i]);
		if (!g_ascii_isdigit(*p))
			return FALSE;
		a = g_ascii_strtoll(p, &end, 10);
		if (end == p || *end != ',' || end - p > 10)
			return FALSE;
		p = end + 1;
		if (!g_ascii_isdigit(*p))
			return FALSE;
		b = g_ascii_strtoll(p, &end, 10);
		if (end == p || *end != ')' || end - p > 10)
			return FALSE;
		p = end + 1;
		if (i < 4 && (a < 0 || a > AXIS_HDR10_CHROMA_MAX || b < 0 || b > AXIS_HDR10_CHROMA_MAX))
			return FALSE;
		if (i == 4 && (a < AXIS_HDR10_MAX_LUMINANCE_LOWEST || a > AXIS_HDR10_MAX_LUMINANCE_HIGHEST || b < 0 ||
		    b > AXIS_HDR10_MIN_LUMINANCE_HIGHEST || b >= a))
			return FALSE;
	}
	return *p == '\0';
}

static gboolean
pair_ok(
	const gchar *text,
	gint64       high
){
	g_auto(GStrv) two = g_strsplit(text, ",", -1);

	return g_strv_length(two) == 2 && (strcmp(two[0], "0") == 0 || number_in(two[0], 1, high)) &&
		(strcmp(two[1], "0") == 0 || number_in(two[1], 1, high));
}

/* x265-params or x264-params: key=value, colon-separated, each key once. */
static gboolean
encoder_params_ok(
	const gchar *text,
	gboolean     x265
){
	g_auto(GStrv) pairs = g_strsplit(text, ":", -1);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;

	if (pairs[0] == NULL)
		return FALSE;
	for (i = 0; pairs[i] != NULL; i++) {
		const gchar *eq = strchr(pairs[i], '=');
		g_autofree gchar *key = NULL;
		const gchar *value;
		gboolean ok;

		if (eq == NULL)
			return FALSE;
		key = g_strndup(pairs[i], (gsize)(eq - pairs[i]));
		value = eq + 1;
		if (g_hash_table_contains(seen, key))
			return FALSE;
		if (strcmp(key, "colorprim") == 0)
			ok = one_of(value, axis_convert_colour_names("primaries"));
		else if (strcmp(key, "transfer") == 0)
			ok = one_of(value, axis_convert_colour_names("transfer"));
		else if (strcmp(key, "colormatrix") == 0)
			ok = one_of(value, axis_convert_colour_names("matrix"));
		else if (strcmp(key, "range") == 0)
			ok = x265 ? (strcmp(value, "limited") == 0 || strcmp(value, "full") == 0) :
				one_of(value, axis_convert_colour_names("range"));
		else if (x265 && strcmp(key, "hdr10") == 0)
			ok = strcmp(value, "1") == 0;
		else if (x265 && strcmp(key, "master-display") == 0)
			ok = master_display_ok(value);
		else if (x265 && strcmp(key, "max-cll") == 0)
			ok = pair_ok(value, AXIS_HDR10_LIGHT_MAX);
		else if (x265 && strcmp(key, "cll") == 0)
			ok = strcmp(value, "0") == 0;
		else if (x265 && strcmp(key, "pools") == 0)
			ok = number_in(value, 1, 64);
		else if (x265 && strcmp(key, "log-level") == 0)
			ok = strcmp(value, "error") == 0;
		else
			ok = FALSE;
		if (!ok)
			return FALSE;
		g_hash_table_add(seen, g_steal_pointer(&key));
	}
	/* x265's own logging is not ffmpeg's: it must be told. */
	return !x265 || g_hash_table_contains(seen, "log-level");
}

/* A stream index that a stream of @info has. */
static gboolean
source_exists(
	const AxisMediaInfo *info,
	gint64               index
){
	guint i;

	for (i = 0; info != NULL && info->streams != NULL && i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->index == index)
			return TRUE;
	}
	return FALSE;
}

/* ── The vector ─────────────────────────────────────────────────── */

static gboolean
fail(
	gchar      **problem,
	guint        at,
	const gchar *what
){
	if (problem != NULL)
		*problem = g_strdup_printf("argument %u: %s", at, what);
	return FALSE;
}

/* Expects the fixed sequence @want at @args[*at]. */
static gboolean
expect(
	const gchar *const *args,
	guint              *at,
	const gchar *const *want
){
	guint i;

	for (i = 0; want[i] != NULL; i++) {
		if (args[*at + i] == NULL || strcmp(args[*at + i], want[i]) != 0)
			return FALSE;
	}
	*at += i;
	return TRUE;
}

/* "-name:K" with K < @maps: the name and K. */
static gboolean
per_stream(
	const gchar  *text,
	guint         maps,
	gchar       **name,
	guint        *k
){
	const gchar *colon;
	g_autofree gchar *index = NULL;

	if (text[0] != '-' || (colon = strrchr(text, ':')) == NULL || colon == text + 1)
		return FALSE;
	index = g_strdup(colon + 1);
	if (!(strcmp(index, "0") == 0 || number_in(index, 1, 100000)))
		return FALSE;
	*k = (guint)g_ascii_strtoull(index, NULL, 10);
	if (*k >= maps)
		return FALSE;
	*name = g_strndup(text + 1, (gsize)(colon - text - 1));
	return TRUE;
}

gboolean
axis_convert_args_valid(
	const gchar *const  *args,
	const AxisMediaInfo *info,
	gchar              **problem
){
	static const gchar *const head[] = {
		"-nostdin", "-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", NULL
	};
	static const gchar *const input_head[] = { "-protocol_whitelist", "fd", "-format_whitelist", NULL };
	static const gchar *const input_tail[] = { "-fd", "3", "-i", "fd:", NULL };
	static const gchar *const metadata[] = { "-map_chapters", "0", "-map_metadata", "0", NULL };
	static const gchar *const matroska[] = { "-default_mode", "passthrough", "-f", "matroska", NULL };
	static const gchar *const mp4[] = { "-f", "mp4", NULL };
	static const gchar *const output[] = { "-protocol_whitelist", "fd", "-fd", "4", "fd:", NULL };
	g_autofree guint *codecs = NULL;
	g_autofree guint *disps = NULL;
	guint at = 0;
	guint maps = 0;
	guint n;
	guint i;

	if (problem != NULL)
		*problem = NULL;
	if (args == NULL)
		return fail(problem, 0, "no vector");
	for (n = 0; args[n] != NULL; n++) {
		if (n > 100000)
			return fail(problem, n, "too long");
		if (!alphabet_ok(args[n]))
			return fail(problem, n, "empty, or a character outside the alphabet");
	}

	if (!expect(args, &at, head))
		return fail(problem, at, "not the global options");
	if (args[at] != NULL && strcmp(args[at], "-vaapi_device") == 0) {
		const gchar *device = args[at + 1];

		if (device == NULL || strlen(device) != 19 || !g_str_has_prefix(device, "/dev/dri/renderD") ||
		    !g_ascii_isdigit(device[16]) || !g_ascii_isdigit(device[17]) || !g_ascii_isdigit(device[18]))
			return fail(problem, at + 1, "not a render device");
		at += 2;
	}
	if (!expect(args, &at, input_head) || args[at] == NULL ||
	    strcmp(args[at], axis_probe_format_whitelist()) != 0)
		return fail(problem, at, "not the input's protocol and format whitelists");
	at++;
	if (!expect(args, &at, input_tail))
		return fail(problem, at, "not descriptor 3 as the input");

	while (args[at] != NULL && strcmp(args[at], "-map") == 0) {
		const gchar *value = args[at + 1];

		if (value == NULL || !g_str_has_prefix(value, "0:") ||
		    !(strcmp(value + 2, "0") == 0 || number_in(value + 2, 1, 100000)) ||
		    !source_exists(info, g_ascii_strtoll(value + 2, NULL, 10)))
			return fail(problem, at + 1, "a map that names no stream of the file");
		maps++;
		at += 2;
	}
	if (maps == 0 || maps > AXIS_PROBE_MAX_STREAMS + 1)
		return fail(problem, at, "no stream mapped, or too many");
	if (!expect(args, &at, metadata))
		return fail(problem, at, "not the chapters and tags of input 0");

	codecs = g_new0(guint, maps);
	disps = g_new0(guint, maps);
	while (args[at] != NULL && strcmp(args[at], "-f") != 0 && strcmp(args[at], "-default_mode") != 0) {
		g_autofree gchar *name = NULL;
		const gchar *value = args[at + 1];
		guint k = 0;
		gboolean ok;

		if (value == NULL)
			return fail(problem, at, "an option without its value");
		if (g_str_has_prefix(args[at], "-metadata:s:")) {
			const gchar *index = args[at] + strlen("-metadata:s:");

			if (!(strcmp(index, "0") == 0 || number_in(index, 1, 100000)) ||
			    g_ascii_strtoull(index, NULL, 10) >= maps || !one_of(value, blanked_tags))
				return fail(problem, at, "a tag that is not one Axis blanks");
			at += 2;
			continue;
		}
		if (!per_stream(args[at], maps, &name, &k))
			return fail(problem, at, "not an option of an output stream that exists");
		if (strcmp(name, "c") == 0) {
			ok = one_of(value, video_encoders) || one_of(value, other_codecs);
			codecs[k]++;
		} else if (strcmp(name, "disposition") == 0) {
			ok = one_of(value, dispositions);
			disps[k]++;
		} else if (strcmp(name, "filter") == 0) {
			ok = filter_ok(value);
		} else if (strcmp(name, "pix_fmt") == 0) {
			ok = one_of(value, picture_formats);
		} else if (strcmp(name, "preset") == 0) {
			ok = one_of(value, x26x_presets) || one_of(value, nvenc_preset_names);
		} else if (strcmp(name, "crf") == 0) {
			ok = strcmp(value, "0") == 0 || number_in(value, 1, 51);
		} else if (strcmp(name, "cq") == 0) {
			ok = number_in(value, 1, 51);
		} else if (strcmp(name, "qp") == 0) {
			ok = number_in(value, 1, 52);
		} else if (strcmp(name, "rc") == 0) {
			ok = strcmp(value, "vbr") == 0;
		} else if (strcmp(name, "rc_mode") == 0) {
			ok = strcmp(value, "CQP") == 0;
		} else if (strcmp(name, "b") == 0) {
			ok = strcmp(value, "0") == 0 || number_in(value, 96000, 320000);
		} else if (strcmp(name, "ac") == 0) {
			ok = strcmp(value, "2") == 0;
		} else if (strcmp(name, "x265-params") == 0) {
			ok = encoder_params_ok(value, TRUE);
		} else if (strcmp(name, "x264-params") == 0) {
			ok = encoder_params_ok(value, FALSE);
		} else if (strcmp(name, "a53cc") == 0) {
			ok = strcmp(value, "1") == 0;
		} else if (strcmp(name, "threads") == 0) {
			ok = number_in(value, 1, 64);
		} else {
			ok = FALSE;
		}
		if (!ok)
			return fail(problem, at, "an option or value the builder does not use");
		at += 2;
	}
	/* Every output stream has its codec and its flags, once. */
	for (i = 0; i < maps; i++) {
		if (codecs[i] != 1 || disps[i] != 1)
			return fail(problem, at, "an output stream without exactly one codec and one disposition");
	}
	if (!expect(args, &at, matroska) && !expect(args, &at, mp4))
		return fail(problem, at, "not Matroska or MP4");
	if (!expect(args, &at, output) || args[at] != NULL)
		return fail(problem, at, "not descriptor 4 as the output, last");
	return TRUE;
}

/* ── The output against the plan ────────────────────────────────── */

/* The ten mastering display values (the light levels apart). */
static gboolean
mastering_equal(
	const AxisHdr10Metadata *a,
	const AxisHdr10Metadata *b
){
	AxisHdr10Metadata x = *a;
	AxisHdr10Metadata y = *b;

	x.light = y.light = FALSE;
	x.max_content = y.max_content = 0;
	x.max_average = y.max_average = 0;
	return axis_hdr10_metadata_equal(&x, &y);
}

static gboolean
unstated(const gchar *value)
{
	return value == NULL || value[0] == '\0' || strcmp(value, "unknown") == 0 || strcmp(value, "unspecified") == 0 ||
		strcmp(value, "reserved") == 0;
}

/* "und" and no language are the same. */
static const gchar *
language_of(const gchar *language)
{
	return language == NULL || language[0] == '\0' || strcmp(language, "und") == 0 ? NULL : language;
}

static void
differ(
	GPtrArray   *mismatches,
	gboolean    *ok,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
differ(
	GPtrArray   *mismatches,
	gboolean    *ok,
	const gchar *format,
	...
){
	va_list args;

	*ok = FALSE;
	if (mismatches == NULL)
		return;
	va_start(args, format);
	g_ptr_array_add(mismatches, g_strdup_vprintf(format, args));
	va_end(args);
}

/* ── Colour and the HDR class, by what they mean ────────────────── */

gboolean
axis_convert_colour_same(
	const gchar *property,
	const gchar *planned,
	const gchar *got
){
	const gchar *a = unstated(planned) ? NULL : planned;
	const gchar *b = unstated(got) ? NULL : got;

	/* A range that is not stated is limited ("tv") by the definition of
	 * every format here: H.264 and H.265 infer video_full_range_flag 0
	 * when the bitstream leaves it out (E.2.1 of both), and Matroska's
	 * Range and MP4's colour box have no other reading.  ffmpeg 7.1 and
	 * 8.1 hand the encoders a limited range for frames that state none,
	 * so x265 and NVENC write the flag out (measured: the source's VUI
	 * without a signal type, the output's with video_full_range_flag 0
	 * and no colour description); and some formats cannot state limited
	 * on its own (MP4 writes no colour box with only the range known,
	 * x264 no signal type for it).  Limited and unstated are one picture;
	 * full ("pc") is another, and must be stated exactly. */
	if (g_strcmp0(property, "range") == 0) {
		if (a == NULL)
			a = "tv";
		if (b == NULL)
			b = "tv";
	}
	return g_strcmp0(a, b) == 0;
}

const gchar *
axis_convert_colour_said(
	const gchar *property,
	const gchar *value
){
	if (g_strcmp0(property, "range") != 0)
		return unstated(value) ? "unstated" : value;
	if (unstated(value))
		return "unstated (limited)";
	if (strcmp(value, "tv") == 0)
		return "limited (tv)";
	if (strcmp(value, "pc") == 0)
		return "full (pc)";
	return value;
}

/* The four properties: the name axis_convert_colour_same() takes, the
 * words for them, and where each is in a plan's stream and a probe's. */
typedef struct
{
	const gchar *property;
	const gchar *words;
	gboolean     plural;
} ColourProperty;

static const ColourProperty colour_properties[] = {
	{ "range", "colour range", FALSE },
	{ "matrix", "colour matrix", FALSE },
	{ "transfer", "transfer function", FALSE },
	{ "primaries", "colour primaries", TRUE }
};

static const gchar *
planned_colour(
	const AxisConvertStream *e,
	guint                    k
){
	switch (k) {
	case 0:
		return e->color_range;
	case 1:
		return e->color_space;
	case 2:
		return e->color_transfer;
	default:
		return e->color_primaries;
	}
}

static const gchar *
output_colour(
	const AxisMediaStream *o,
	guint                  k
){
	switch (k) {
	case 0:
		return o->color_range;
	case 1:
		return o->color_space;
	case 2:
		return o->color_transfer;
	default:
		return o->color_primaries;
	}
}

/* Any HDR or Dolby Vision signal at all, in the stream or its frames (the
 * probe's own list). */
static gboolean
hdr_signals(const AxisHdrSignals *s)
{
	return s->has_mastering || s->has_content_light || s->dovi_record || s->dovi_metadata || s->hdr10plus ||
		s->dynamic_other;
}

/*
 * Whether the output's picture @o (stream @position of @output) is of the
 * planned HDR class.  HDR10 and HLG: the same, and not uncertain.  SDR:
 * SDR, or "not classified" where that is all the probe can say of an SDR
 * picture (axis-probe-hdr.c, rule 7: a picture deeper than 8 bits stating
 * no transfer function cannot be told from Dolby Vision profile 5 without
 * its record).  Space saver makes exactly that of a source that states no
 * transfer function, so it is accepted, but only when nothing else could
 * have made it: the plan states no transfer function, nor does the
 * output; it is deeper than 8 bits and not BT.2020; its frames were read;
 * and it carries no HDR or Dolby Vision signal of any kind.
 */
static gboolean
hdr_class_same(
	const AxisConvertStream *e,
	const AxisMediaStream   *o,
	const AxisMediaInfo     *output,
	guint                    position
){
	switch (e->hdr_format) {
	case AXIS_HDR_HDR10:
	case AXIS_HDR_HLG:
		return o->hdr_format == e->hdr_format && !o->hdr_uncertain;
	case AXIS_HDR_SDR:
		if (o->hdr_format == AXIS_HDR_SDR)
			return TRUE;
		return o->hdr_format == AXIS_HDR_NONE && e->color_transfer == NULL && unstated(o->color_transfer) &&
			o->bit_depth > 8 && g_strcmp0(o->color_primaries, "bt2020") != 0 && !hdr_signals(&o->signals) &&
			output->main_video == (gint)position && !output->frames_unavailable;
	default:
		return FALSE;
	}
}

/* "HDR10", "not classified, uncertain (the probe says: <its note>)" and
 * the like. */
static gchar *
hdr_class_said(const AxisMediaStream *o)
{
	const gchar *label = o->hdr_format == AXIS_HDR_NONE ? "not classified" : axis_hdr_format_label(o->hdr_format);

	if (!o->hdr_uncertain)
		return g_strdup(label);
	if (o->hdr_note != NULL)
		return g_strdup_printf("%s, uncertain (the probe says: %s)", label, o->hdr_note);
	return g_strdup_printf("%s, uncertain", label);
}

/* HDR10's values in words: "mastering display G(x,y) B(x,y) R(x,y)
 * WP(x,y) L(max,min), content light 1000,400", "none" without either. */
static gchar *
hdr10_said(const AxisHdr10Metadata *m)
{
	GString *out = g_string_new(NULL);

	if (m->mastering)
		g_string_append_printf(out, "mastering display G(%d,%d) B(%d,%d) R(%d,%d) WP(%d,%d) L(%" G_GINT64_FORMAT ",%"
			G_GINT64_FORMAT ")", m->green_x, m->green_y, m->blue_x, m->blue_y, m->red_x, m->red_y, m->white_x,
			m->white_y, m->max_luminance, m->min_luminance);
	if (m->light)
		g_string_append_printf(out, "%scontent light %d,%d", out->len > 0 ? ", " : "", m->max_content,
			m->max_average);
	if (out->len == 0)
		g_string_append(out, "none");
	return g_string_free(out, FALSE);
}

gboolean
axis_convert_plan_check_picture(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *output,
	gchar                **why
){
	const AxisConvertStream *e;
	const AxisMediaStream *o;
	GString *said;
	gboolean unkept;
	guint k;

	g_return_val_if_fail(plan != NULL, FALSE);

	if (why != NULL)
		*why = NULL;
	if (plan->streams->len == 0 || ((const AxisConvertStream *)g_ptr_array_index(plan->streams, 0))->action !=
	    AXIS_CONVERT_ENCODE) {
		if (why != NULL)
			*why = g_strdup("The plan has no encoded picture to compare with.");
		return FALSE;
	}
	e = g_ptr_array_index(plan->streams, 0);
	if (output == NULL || output->streams == NULL || e->position >= output->streams->len) {
		if (why != NULL)
			*why = g_strdup_printf("The output has no stream %u, where the plan has its encoded picture.", e->position);
		return FALSE;
	}
	o = g_ptr_array_index(output->streams, e->position);
	if (o->kind != AXIS_STREAM_VIDEO || o->attached_pic) {
		if (why != NULL && o->kind == AXIS_STREAM_VIDEO)
			*why = g_strdup_printf("Stream %u of the output is a cover picture, where the plan has its encoded "
				"picture.", e->position);
		else if (why != NULL)
			*why = g_strdup_printf("Stream %u of the output is not a picture (its kind is %s), where the plan has its "
				"encoded picture.", e->position, axis_stream_kind_to_string(o->kind));
		return FALSE;
	}

	said = g_string_new(NULL);
	if (o->width != e->width || o->height != e->height || o->bit_depth != e->bit_depth)
		g_string_append_printf(said, "%sThe picture is %dx%d at %d bits; the plan says %dx%d at %d bits.",
			said->len > 0 ? " " : "", o->width, o->height, o->bit_depth, e->width, e->height, e->bit_depth);
	for (k = 0; k < G_N_ELEMENTS(colour_properties); k++) {
		const ColourProperty *p = &colour_properties[k];
		const gchar *planned = planned_colour(e, k);
		const gchar *got = output_colour(o, k);

		if (!axis_convert_colour_same(p->property, planned, got))
			g_string_append_printf(said, "%sThe picture's %s %s not the planned %s: the plan says %s, the output "
				"has %s.", said->len > 0 ? " " : "", p->words, p->plural ? "are" : "is", p->plural ? "ones" : "one",
				axis_convert_colour_said(p->property, planned), axis_convert_colour_said(p->property, got));
	}
	if (!hdr_class_same(e, o, output, e->position)) {
		g_autofree gchar *got = hdr_class_said(o);

		g_string_append_printf(said, "%sThe picture's HDR class is not the planned one: the plan says %s, the output "
			"has %s.", said->len > 0 ? " " : "", axis_hdr_format_label(e->hdr_format), got);
	}
	/* Values the probe does not keep (out of range, or not the same in
	 * every entry) are not in @o->hdr10, but they were there. */
	unkept = (o->signals.has_mastering && !o->hdr10.mastering) || (o->signals.has_content_light && !o->hdr10.light);
	if (!axis_hdr10_metadata_equal(&o->hdr10, &e->hdr10) || unkept) {
		g_autofree gchar *want = hdr10_said(&e->hdr10);
		g_autofree gchar *got = hdr10_said(&o->hdr10);

		g_string_append_printf(said, "%sThe picture's HDR10 values are not the planned ones: the plan says %s, the "
			"output has %s%s.", said->len > 0 ? " " : "", want, got, unkept ? " (and values the probe does not "
			"keep: out of range, or not the same in every entry)" : "");
	}
	if (said->len == 0) {
		g_string_free(said, TRUE);
		return TRUE;
	}
	if (why != NULL)
		*why = g_string_free(said, FALSE);
	else
		g_string_free(said, TRUE);
	return FALSE;
}

static void
check_colour(
	GPtrArray               *mismatches,
	gboolean                *ok,
	guint                    position,
	const AxisConvertStream *e,
	const AxisMediaStream   *o
){
	guint k;

	for (k = 0; k < G_N_ELEMENTS(colour_properties); k++) {
		const ColourProperty *p = &colour_properties[k];
		const gchar *planned = planned_colour(e, k);
		const gchar *got = output_colour(o, k);

		if (!axis_convert_colour_same(p->property, planned, got))
			differ(mismatches, ok, "stream %u: the %s %s %s, the plan says %s", position, p->words,
				p->plural ? "are" : "is", axis_convert_colour_said(p->property, got),
				axis_convert_colour_said(p->property, planned));
	}
}

gboolean
axis_convert_plan_check_output(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *output,
	GPtrArray             *mismatches
){
	gboolean ok = TRUE;
	guint i;

	g_return_val_if_fail(plan != NULL, FALSE);

	if (output == NULL || output->streams == NULL) {
		differ(mismatches, &ok, "there is no probe of the output");
		return FALSE;
	}
	if (g_strcmp0(output->container, plan->container == AXIS_CONTAINER_MP4 ? "mov,mp4,m4a,3gp,3g2,mj2" :
	    "matroska,webm") != 0)
		differ(mismatches, &ok, "the container is %s", output->container != NULL ? output->container : "unknown");
	if (output->streams_omitted != 0 || output->streams->len != plan->streams->len)
		differ(mismatches, &ok, "the output has %d streams, the plan %u", output->stream_count, plan->streams->len);
	if (plan->chapters >= 0 && output->chapter_count != plan->chapters)
		differ(mismatches, &ok, "the output has %d chapters, the source %d", output->chapter_count, plan->chapters);

	for (i = 0; i < plan->streams->len && i < output->streams->len; i++) {
		const AxisConvertStream *e = g_ptr_array_index(plan->streams, i);
		const AxisMediaStream *o = g_ptr_array_index(output->streams, i);

		if (o->kind != e->kind) {
			differ(mismatches, &ok, "stream %u is %s, the plan says %s", i, axis_stream_kind_to_string(o->kind),
				axis_stream_kind_to_string(e->kind));
			continue;
		}
		if (e->action == AXIS_CONVERT_MUXER) {
			if (g_strcmp0(o->codec, e->codec) != 0)
				differ(mismatches, &ok, "stream %u (the muxer's) is %s", i, o->codec != NULL ? o->codec : "unknown");
			continue;
		}
		if (g_strcmp0(o->codec, e->codec) != 0)
			differ(mismatches, &ok, "stream %u is %s, the plan says %s", i, o->codec != NULL ? o->codec : "unknown",
				e->codec != NULL ? e->codec : "none");
		if ((o->is_default != FALSE) != (e->is_default != FALSE))
			differ(mismatches, &ok, "stream %u: default is %s", i, o->is_default ? "set" : "not set");
		if ((o->is_forced != FALSE) != (e->is_forced != FALSE))
			differ(mismatches, &ok, "stream %u: forced is %s", i, o->is_forced ? "set" : "not set");
		if ((o->attached_pic != FALSE) != (e->attached_pic != FALSE))
			differ(mismatches, &ok, "stream %u: cover picture is %s", i, o->attached_pic ? "set" : "not set");
		if (e->channels > 0 && o->channels != e->channels)
			differ(mismatches, &ok, "stream %u has %d channels, the plan %d", i, o->channels, e->channels);
		if (e->kind != AXIS_STREAM_ATTACHMENT &&
		    g_strcmp0(language_of(o->language), language_of(e->language)) != 0)
			differ(mismatches, &ok, "stream %u: the language is %s, the plan says %s", i,
				language_of(o->language) != NULL ? o->language : "none",
				language_of(e->language) != NULL ? e->language : "none");
		if (e->check_title && g_strcmp0(o->title, e->title) != 0)
			differ(mismatches, &ok, "stream %u: the title differs", i);
		if (e->action != AXIS_CONVERT_ENCODE)
			continue;

		/* The picture. */
		if (o->width != e->width || o->height != e->height)
			differ(mismatches, &ok, "stream %u is %dx%d, the source %dx%d", i, o->width, o->height, e->width,
				e->height);
		if (o->bit_depth != e->bit_depth)
			differ(mismatches, &ok, "stream %u has %d bits, the plan %d", i, o->bit_depth, e->bit_depth);
		check_colour(mismatches, &ok, i, e, o);
		/* SDR stays SDR (an unsignalled deep picture "not classified",
		 * which the plan says it may be); HDR10 and HLG stay themselves. */
		if (!hdr_class_same(e, o, output, i)) {
			g_autofree gchar *got = hdr_class_said(o);

			differ(mismatches, &ok, "stream %u is %s, the plan says %s", i, got, axis_hdr_format_label(e->hdr_format));
		}
		/* HDR10's values: exactly, and in the bitstream itself. */
		if (e->hdr10.mastering) {
			if (!o->hdr10.mastering || !mastering_equal(&o->hdr10, &e->hdr10))
				differ(mismatches, &ok, "stream %u: the mastering display values differ or are missing", i);
			else if (!o->signals.mastering_in_frames)
				differ(mismatches, &ok, "stream %u: the mastering display values are not in the bitstream", i);
		} else if (o->hdr10.mastering || o->signals.has_mastering) {
			differ(mismatches, &ok, "stream %u carries mastering display values the source does not", i);
		}
		if (e->hdr10.light) {
			if (!o->hdr10.light || o->hdr10.max_content != e->hdr10.max_content ||
			    o->hdr10.max_average != e->hdr10.max_average)
				differ(mismatches, &ok, "stream %u: the content light levels differ or are missing", i);
			else if (!o->signals.light_in_frames)
				differ(mismatches, &ok, "stream %u: the content light levels are not in the bitstream", i);
		} else if (o->hdr10.light || o->signals.has_content_light) {
			differ(mismatches, &ok, "stream %u carries content light levels the source does not", i);
		}
	}
	return ok;
}

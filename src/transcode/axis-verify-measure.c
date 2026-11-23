/*
 * axis-verify-measure.c - the measurements of checks 5 to 8, the same on the
 * daemon and on a worker
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The commands are the ones the daemon's checks always ran (design.md of
 * add-conversion-execution, of fix-first-conversion and of
 * verify-on-worker), with a thread count for each input and the filters
 * when one is given.  What the tools say is kept as figures and as how each
 * run ended; nothing here decides a check (axis-verify-judge.c does, for the
 * daemon only).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-verify.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-model.h"

#define ERROR_CAP        (16 * 1024)
#define PACKETS_CAP      (G_GUINT64_CONSTANT(64) * 1024 * 1024)
#define SIMILARITY_CAP   (1024 * 1024)
#define HASH_CAP         (1024 * 1024)
#define MEMORY_LIMIT     (G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024)

/* ── Commands ───────────────────────────────────────────────────── */

static GPtrArray *
command(const gchar *const *prefix)
{
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; prefix != NULL && prefix[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(prefix[i]));
	return argv;
}

static gchar **
finish_argv(GPtrArray *argv)
{
	g_ptr_array_add(argv, NULL);
	/* The array's free function is dropped with it: the strings are the
	 * vector's now. */
	g_ptr_array_set_free_func(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* The common head of every input: its decoder's threads when given, quiet,
 * the descriptor only. */
static void
add_input(
	GPtrArray             *argv,
	const AxisVerifyTools *tools,
	const gchar           *fd,
	const gchar           *seek,
	const gchar           *length
){
	if (tools->threads > 0) {
		g_ptr_array_add(argv, g_strdup("-threads"));
		g_ptr_array_add(argv, g_strdup_printf("%u", MIN(tools->threads, (guint)AXIS_VERIFY_MAX_THREADS)));
	}
	g_ptr_array_add(argv, g_strdup("-protocol_whitelist"));
	g_ptr_array_add(argv, g_strdup("fd"));
	g_ptr_array_add(argv, g_strdup("-format_whitelist"));
	g_ptr_array_add(argv, g_strdup(axis_probe_format_whitelist()));
	if (seek != NULL) {
		g_ptr_array_add(argv, g_strdup("-ss"));
		g_ptr_array_add(argv, g_strdup(seek));
	}
	if (length != NULL) {
		g_ptr_array_add(argv, g_strdup("-t"));
		g_ptr_array_add(argv, g_strdup(length));
	}
	g_ptr_array_add(argv, g_strdup("-fd"));
	g_ptr_array_add(argv, g_strdup(fd));
	g_ptr_array_add(argv, g_strdup("-i"));
	g_ptr_array_add(argv, g_strdup("fd:"));
}

/* The filters' threads: a global option, before the inputs. */
static void
add_filter_threads(
	GPtrArray             *argv,
	const AxisVerifyTools *tools,
	const gchar           *option
){
	if (tools->threads == 0)
		return;
	g_ptr_array_add(argv, g_strdup(option));
	g_ptr_array_add(argv, g_strdup_printf("%u", MIN(tools->threads, (guint)AXIS_VERIFY_MAX_THREADS)));
}

static void
add_quiet(GPtrArray *argv)
{
	g_ptr_array_add(argv, g_strdup("-nostdin"));
	g_ptr_array_add(argv, g_strdup("-hide_banner"));
	g_ptr_array_add(argv, g_strdup("-v"));
	g_ptr_array_add(argv, g_strdup("error"));
	g_ptr_array_add(argv, g_strdup("-nostats"));
}

/*
 * An output of ffmpeg's streamhash muxer for the plan's copied tracks,
 * mapped by their index in the original (@source) or in the converted file:
 * copied, every packet (the first ones too), and the SHA-256 of each track's
 * packet payloads, in order, written to standard output as
 * "k,type,SHA256=hex".  Timestamps and the container's framing are not
 * hashed, only the payload, so the same track hashes the same in Matroska
 * and MP4.  AAC is brought to one framing first: a muxer that copies ADTS
 * AAC (from MPEG-TS) takes the ADTS headers off, and aac_adtstoasc does the
 * same here and leaves raw AAC as it is.
 */
static void
add_hash_output(
	GPtrArray            *argv,
	const AxisVerifyPlan *plan,
	gboolean              source
){
	guint k;

	for (k = 0; k < plan->n_tracks; k++) {
		g_ptr_array_add(argv, g_strdup("-map"));
		g_ptr_array_add(argv, g_strdup_printf("0:%d", source ? plan->tracks[k].source : (gint)plan->tracks[k].output));
	}
	g_ptr_array_add(argv, g_strdup("-c"));
	g_ptr_array_add(argv, g_strdup("copy"));
	g_ptr_array_add(argv, g_strdup("-copyinkf"));
	for (k = 0; k < plan->n_tracks; k++) {
		if (plan->tracks[k].aac) {
			g_ptr_array_add(argv, g_strdup_printf("-bsf:%u", k));
			g_ptr_array_add(argv, g_strdup("aac_adtstoasc"));
		}
	}
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("streamhash"));
	g_ptr_array_add(argv, g_strdup("-hash"));
	g_ptr_array_add(argv, g_strdup("sha256"));
	g_ptr_array_add(argv, g_strdup("-"));
}

gchar **
axis_verify_decode_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan
){
	GPtrArray *argv = command(tools->ffmpeg);

	add_quiet(argv);
	g_ptr_array_add(argv, g_strdup("-xerror"));
	add_filter_threads(argv, tools, "-filter_threads");
	add_input(argv, tools, "3", NULL, NULL);
	g_ptr_array_add(argv, g_strdup("-map"));
	g_ptr_array_add(argv, g_strdup("0:v?"));
	g_ptr_array_add(argv, g_strdup("-map"));
	g_ptr_array_add(argv, g_strdup("0:a?"));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("null"));
	g_ptr_array_add(argv, g_strdup("-"));
	/* The same reading of the file hashes its copied tracks for check 7:
	 * the converted file is read once for both. */
	if (plan->n_tracks > 0)
		add_hash_output(argv, plan, FALSE);
	return finish_argv(argv);
}

gchar **
axis_verify_packets_argv(
	const AxisVerifyTools *tools,
	gint                   index
){
	GPtrArray *argv = command(tools->ffprobe);

	g_ptr_array_add(argv, g_strdup("-v"));
	g_ptr_array_add(argv, g_strdup("error"));
	g_ptr_array_add(argv, g_strdup("-hide_banner"));
	g_ptr_array_add(argv, g_strdup("-protocol_whitelist"));
	g_ptr_array_add(argv, g_strdup("fd"));
	g_ptr_array_add(argv, g_strdup("-format_whitelist"));
	g_ptr_array_add(argv, g_strdup(axis_probe_format_whitelist()));
	g_ptr_array_add(argv, g_strdup("-fd"));
	g_ptr_array_add(argv, g_strdup("3"));
	g_ptr_array_add(argv, g_strdup("-select_streams"));
	g_ptr_array_add(argv, g_strdup_printf("%d", index));
	g_ptr_array_add(argv, g_strdup("-show_entries"));
	g_ptr_array_add(argv, g_strdup("packet=size"));
	g_ptr_array_add(argv, g_strdup("-of"));
	g_ptr_array_add(argv, g_strdup("csv=p=0"));
	g_ptr_array_add(argv, g_strdup("-i"));
	g_ptr_array_add(argv, g_strdup("fd:"));
	return finish_argv(argv);
}

gchar **
axis_verify_tracks_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan
){
	GPtrArray *argv = command(tools->ffmpeg);

	add_quiet(argv);
	add_input(argv, tools, "3", NULL, NULL);
	add_hash_output(argv, plan, TRUE);
	return finish_argv(argv);
}

gchar **
axis_verify_point_argv(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	guint                  k
){
	GPtrArray *argv;
	gchar at[G_ASCII_DTOSTR_BUF_SIZE];
	gchar span[G_ASCII_DTOSTR_BUF_SIZE];

	g_return_val_if_fail(k < plan->n_points, NULL);

	argv = command(tools->ffmpeg);
	g_ascii_formatd(at, sizeof(at), "%.3f", (gdouble)plan->points[k].start_ms / 1000.0);
	g_ascii_formatd(span, sizeof(span), "%.3f", (gdouble)plan->points[k].length_ms / 1000.0);
	add_quiet(argv);
	add_filter_threads(argv, tools, "-filter_complex_threads");
	add_input(argv, tools, "3", at, span);
	add_input(argv, tools, "4", at, span);
	/* Frames paired by the nearest timestamp: the two containers round the
	 * same frame's time differently (measured on ffmpeg 8.1: an MKV source
	 * and its MP4 output paired by the default rule are one frame apart,
	 * SSIM 0.89 instead of 0.99). */
	g_ptr_array_add(argv, g_strdup("-lavfi"));
	g_ptr_array_add(argv, g_strdup_printf("[0:%d]format=yuv420p10le,setsar=1[a];[1:%u]format=yuv420p10le,setsar=1[b];"
		"[a][b]ssim=stats_file=-:ts_sync_mode=nearest", plan->picture_source, plan->picture_output));
	g_ptr_array_add(argv, g_strdup("-an"));
	g_ptr_array_add(argv, g_strdup("-sn"));
	g_ptr_array_add(argv, g_strdup("-dn"));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("null"));
	g_ptr_array_add(argv, g_strdup("-"));
	return finish_argv(argv);
}

/* ── Running ────────────────────────────────────────────────────── */

/* Runs @argv with @in as descriptor 3 and @second (or -1) as 4, which is
 * read only (the child may write no file). */
static void
run_tool(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gchar                **argv,
	gint                   in,
	gint                   second,
	guint64                output_cap,
	AxisProbeRunResult    *result
){
	AxisProbeRunSpec spec;

	if (in >= 0)
		(void)lseek(in, 0, SEEK_SET);
	if (second >= 0)
		(void)lseek(second, 0, SEEK_SET);
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = in;
	spec.output_fd = second >= 0 ? second : 0;
	spec.file_size_limit = 0;
	spec.timeout_ms = plan->timeout_ms;
	spec.output_cap = (gsize)output_cap;
	spec.error_cap = ERROR_CAP;
	spec.cancellable = tools->cancellable;
	spec.memory_limit = MEMORY_LIMIT;
	spec.working_directory = tools->scratch;
	spec.cpu_seconds = plan->timeout_ms / 1000 * 16 + 60;
	axis_probe_run(&spec, result);
}

/* How the run ended, as evidence. */
static void
keep_run(
	const AxisProbeRunResult *result,
	AxisVerifyRun            *run
){
	axis_verify_run_clear(run);
	run->status = result->status;
	run->code = result->exit_status;
	run->error_bytes = result->errors != NULL ? result->errors->len : 0;
	if (result->errors != NULL && result->errors->len > 0) {
		g_autofree gchar *raw = g_strndup((const gchar *)result->errors->data, result->errors->len);

		run->said = axis_library_text_clean(raw, AXIS_VERIFY_SAID_MAX_CHARS);
		if (run->said != NULL && run->said[0] == '\0')
			g_clear_pointer(&run->said, g_free);
	}
}

/* What a tool wrote on standard output, as text ("" for nothing). */
static gchar *
output_text(const GByteArray *output)
{
	if (output == NULL || output->len == 0 || output->data == NULL)
		return g_strdup("");
	return g_strndup((const gchar *)output->data, output->len);
}

/* The hashes a streamhash output wrote, by output index; %NULL unless there
 * is exactly one well-formed line for each of @n. */
static gchar **
parse_hashes(
	const GByteArray *output,
	guint             n
){
	g_autofree gchar *text = output_text(output);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_auto(GStrv) hashes = g_new0(gchar *, n + 1);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *at;
		gchar *end = NULL;
		guint64 k;
		guint j;

		g_strstrip(lines[i]);
		if (lines[i][0] == '\0')
			continue;
		k = g_ascii_strtoull(lines[i], &end, 10);
		at = strstr(lines[i], ",SHA256=");
		if (end == lines[i] || *end != ',' || at == NULL || k >= n || hashes[k] != NULL)
			return NULL;
		at += strlen(",SHA256=");
		for (j = 0; at[j] != '\0'; j++) {
			if (!g_ascii_isxdigit(at[j]))
				return NULL;
		}
		if (j != 64)
			return NULL;
		hashes[k] = g_ascii_strdown(at, -1);
	}
	for (i = 0; i < n; i++) {
		if (hashes[i] == NULL)
			return NULL;
	}
	return g_steal_pointer(&hashes);
}

/* ── 5: the decode ──────────────────────────────────────────────── */

void
axis_verify_measure_decode(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
){
	g_auto(GStrv) argv = axis_verify_decode_argv(tools, plan);
	AxisProbeRunResult result;

	run_tool(tools, plan, argv, out_fd, -1, HASH_CAP, &result);
	keep_run(&result, &evidence->decode);
	g_clear_pointer(&evidence->output_hashes, g_strfreev);
	if (plan->n_tracks > 0 && result.status == AXIS_PROBE_RUN_OK && result.errors->len == 0)
		evidence->output_hashes = parse_hashes(result.output, plan->n_tracks);
	evidence->decoded = TRUE;
	axis_probe_run_result_clear(&result);
}

/* ── 6: the packets ─────────────────────────────────────────────── */

static void
count_packets(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   fd,
	gint                   index,
	AxisVerifyCount       *count
){
	g_auto(GStrv) argv = axis_verify_packets_argv(tools, index);
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *text = NULL;
	AxisProbeRunResult result;
	guint i;

	run_tool(tools, plan, argv, fd, -1, PACKETS_CAP, &result);
	keep_run(&result, &count->run);
	count->read = FALSE;
	count->count = 0;
	count->bytes = 0;
	if (result.status != AXIS_PROBE_RUN_OK) {
		axis_probe_run_result_clear(&result);
		return;
	}
	text = output_text(result.output);
	axis_probe_run_result_clear(&result);
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *end = NULL;
		guint64 n;

		g_strstrip(lines[i]);
		if (lines[i][0] == '\0')
			continue;
		n = g_ascii_strtoull(lines[i], &end, 10);
		if (end == lines[i] || (*end != '\0' && *end != ',')) {
			count->count = 0;
			count->bytes = 0;
			return;
		}
		count->count++;
		count->bytes += n;
	}
	count->read = TRUE;
}

void
axis_verify_measure_packets(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   src_fd,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
){
	count_packets(tools, plan, src_fd, plan->picture_source, &evidence->source_packets);
	count_packets(tools, plan, out_fd, (gint)plan->picture_output, &evidence->output_packets);
	evidence->counted = TRUE;
}

/* ── 7: the original's tracks ───────────────────────────────────── */

void
axis_verify_measure_tracks(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   src_fd,
	AxisVerifyEvidence    *evidence
){
	g_auto(GStrv) argv = NULL;
	AxisProbeRunResult result;

	g_clear_pointer(&evidence->source_hashes, g_strfreev);
	axis_verify_run_clear(&evidence->tracks);
	evidence->hashed = TRUE;
	if (plan->n_tracks == 0)
		return;	/* nothing copied: a run that ended well, with nothing said */
	argv = axis_verify_tracks_argv(tools, plan);
	run_tool(tools, plan, argv, src_fd, -1, HASH_CAP, &result);
	keep_run(&result, &evidence->tracks);
	if (result.status == AXIS_PROBE_RUN_OK && result.errors->len == 0)
		evidence->source_hashes = parse_hashes(result.output, plan->n_tracks);
	axis_probe_run_result_clear(&result);
}

/* ── 8: the pictures ────────────────────────────────────────────── */

/* The lowest of Y, U and V of SSIM's per-frame lines; frames counted.  The
 * first and the last frame of a window are not counted when there are more
 * than four: where the two files' timestamps are rounded differently (a
 * Matroska source's milliseconds, an MP4's exact frame times), the cut at
 * the window's edges can keep a frame in one and not the other, and that
 * one pair is two different frames. */
static gboolean
lowest_similarity(
	const gchar *text,
	gdouble     *lowest,
	guint       *frames
){
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GArray) values = g_array_new(FALSE, FALSE, sizeof(gdouble));
	guint first;
	guint last;
	guint i;

	*lowest = 1.0;
	*frames = 0;
	for (i = 0; lines[i] != NULL; i++) {
		static const gchar *const keys[] = { " Y:", " U:", " V:", NULL };
		gdouble frame = 1.0;
		guint k;

		if (!g_str_has_prefix(lines[i], "n:"))
			continue;
		for (k = 0; keys[k] != NULL; k++) {
			const gchar *at = strstr(lines[i], keys[k]);
			gchar *end = NULL;
			gdouble value;

			if (at == NULL)
				return FALSE;
			value = g_ascii_strtod(at + 3, &end);
			if (end == at + 3 || !isfinite(value))
				return FALSE;
			frame = MIN(frame, value);
		}
		g_array_append_val(values, frame);
	}
	first = values->len > 4 ? 1 : 0;
	last = values->len > 4 ? values->len - 1 : values->len;
	for (i = first; i < last; i++) {
		*lowest = MIN(*lowest, g_array_index(values, gdouble, i));
		(*frames)++;
	}
	return *frames > 0;
}

void
axis_verify_measure_point(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	guint                  k,
	gint                   src_fd,
	gint                   out_fd,
	AxisVerifyEvidence    *evidence
){
	g_auto(GStrv) argv = NULL;
	AxisVerifyPicture *p;
	AxisProbeRunResult result;

	g_return_if_fail(k < plan->n_points && k == evidence->n_points);

	argv = axis_verify_point_argv(tools, plan, k);
	p = &evidence->points[k];
	run_tool(tools, plan, argv, src_fd, out_fd, SIMILARITY_CAP, &result);
	keep_run(&result, &p->run);
	p->read = FALSE;
	p->lowest = 0.0;
	p->frames = 0;
	if (result.status == AXIS_PROBE_RUN_OK) {
		g_autofree gchar *text = output_text(result.output);
		gdouble lowest = 0.0;
		guint frames = 0;

		if (lowest_similarity(text, &lowest, &frames)) {
			p->read = TRUE;
			p->lowest = lowest;
			p->frames = frames;
		}
	}
	axis_probe_run_result_clear(&result);
	evidence->n_points = k + 1;
}

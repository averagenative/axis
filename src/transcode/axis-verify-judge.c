/*
 * axis-verify-judge.c - the plan and the evidence of checks 5 to 8, and the
 * checks decided from them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: the figures in, a verdict and the check's sentence out.  Only the
 * daemon calls the judges (a worker reports figures, never a verdict), on
 * figures it measured itself or a worker's that it bound to the files first.
 * The sentences are the ones the checks always wrote.
 */

#include "transcode/axis-verify.h"

#include <math.h>
#include <signal.h>
#include <string.h>

/* ── The plan ───────────────────────────────────────────────────── */

AxisVerifyPlan *
axis_verify_plan_new(void)
{
	return g_new0(AxisVerifyPlan, 1);
}

void
axis_verify_plan_free(AxisVerifyPlan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->tracks);
	g_free(plan);
}

AxisVerifyPlan *
axis_verify_plan_copy(const AxisVerifyPlan *plan)
{
	AxisVerifyPlan *copy;

	g_return_val_if_fail(plan != NULL, NULL);

	copy = g_new0(AxisVerifyPlan, 1);
	*copy = *plan;
	copy->tracks = plan->n_tracks > 0 ? g_memdup2(plan->tracks, sizeof(AxisVerifyTrack) * plan->n_tracks) : NULL;
	return copy;
}

void
axis_verify_plan_add_track(
	AxisVerifyPlan *plan,
	gint            source,
	guint           output,
	gboolean        aac
){
	g_return_if_fail(plan != NULL);

	plan->tracks = g_renew(AxisVerifyTrack, plan->tracks, plan->n_tracks + 1);
	plan->tracks[plan->n_tracks].source = source;
	plan->tracks[plan->n_tracks].output = output;
	plan->tracks[plan->n_tracks].aac = aac;
	plan->n_tracks++;
}

AxisVerifyPlan *
axis_verify_plan_make(
	const AxisConvertPlan *plan,
	gint64                 duration_ms,
	guint                  points,
	guint                  timeout_ms
){
	g_autoptr(AxisVerifyPlan) out = NULL;
	const AxisConvertStream *picture;
	guint i;

	g_return_val_if_fail(plan != NULL, NULL);

	if (plan->streams == NULL || plan->streams->len == 0)
		return NULL;
	out = axis_verify_plan_new();
	picture = g_ptr_array_index(plan->streams, 0);
	out->picture_source = picture->source;
	out->picture_output = picture->position;
	out->timeout_ms = timeout_ms;
	for (i = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

		/* Copied as packets: an attachment is held in the container's
		 * header and compared by the daemon from it. */
		if (s->action != AXIS_CONVERT_COPY || s->kind == AXIS_STREAM_ATTACHMENT)
			continue;
		axis_verify_plan_add_track(out, s->source, s->position, g_strcmp0(s->codec, "aac") == 0);
	}
	if (points == 0)
		points = 5;
	points = MIN(points, (guint)AXIS_VERIFY_MAX_POINTS);
	if (duration_ms > 0) {
		/* At the instants check 8 always used: a window of at most a
		 * second, centred on k + 0.5 parts of the movie. */
		gdouble seconds = (gdouble)duration_ms / 1000.0;
		gdouble length = MIN(1.0, seconds / (points * 2.0));
		guint k;

		for (k = 0; k < points; k++) {
			gdouble at = MAX(seconds * (k + 0.5) / points - length / 2.0, 0.0);

			out->points[k].start_ms = (gint64)llround(at * 1000.0);
			out->points[k].length_ms = MAX((gint64)llround(length * 1000.0), 1);
		}
		out->n_points = points;
	}
	return g_steal_pointer(&out);
}

static gboolean
refuse(
	gchar       **problem,
	const gchar  *text
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup(text);
	return FALSE;
}

gboolean
axis_verify_plan_check(
	const AxisVerifyPlan  *plan,
	gchar                **problem
){
	guint i;

	if (plan == NULL)
		return refuse(problem, "there is no plan of the checks");
	if (plan->picture_source < 0 || plan->picture_source >= AXIS_VERIFY_MAX_TRACKS ||
	    plan->picture_output >= AXIS_VERIFY_MAX_TRACKS)
		return refuse(problem, "the picture's indices are out of range");
	if (plan->n_tracks > AXIS_VERIFY_MAX_TRACKS || (plan->n_tracks > 0 && plan->tracks == NULL))
		return refuse(problem, "there are too many copied tracks");
	for (i = 0; i < plan->n_tracks; i++) {
		guint j;

		if (plan->tracks[i].source < 0 || plan->tracks[i].source >= AXIS_VERIFY_MAX_TRACKS ||
		    plan->tracks[i].output >= AXIS_VERIFY_MAX_TRACKS)
			return refuse(problem, "a copied track's indices are out of range");
		for (j = 0; j < i; j++) {
			if (plan->tracks[j].output == plan->tracks[i].output)
				return refuse(problem, "two copied tracks have the same place in the converted file");
		}
	}
	if (plan->n_points > AXIS_VERIFY_MAX_POINTS)
		return refuse(problem, "there are too many points");
	for (i = 0; i < plan->n_points; i++) {
		if (plan->points[i].start_ms < 0 || plan->points[i].start_ms > AXIS_VERIFY_MAX_START_MS ||
		    plan->points[i].length_ms < 1 || plan->points[i].length_ms > 60000)
			return refuse(problem, "a point is out of range");
	}
	if (plan->timeout_ms < AXIS_VERIFY_MIN_TIMEOUT_MS || plan->timeout_ms > AXIS_VERIFY_MAX_TIMEOUT_MS)
		return refuse(problem, "the time limit is out of range");
	return TRUE;
}

/* ── The evidence ───────────────────────────────────────────────── */

AxisVerifyEvidence *
axis_verify_evidence_new(void)
{
	return g_new0(AxisVerifyEvidence, 1);
}

void
axis_verify_run_clear(AxisVerifyRun *run)
{
	if (run == NULL)
		return;
	g_clear_pointer(&run->said, g_free);
	memset(run, 0, sizeof(*run));
}

void
axis_verify_evidence_free(AxisVerifyEvidence *evidence)
{
	guint k;

	if (evidence == NULL)
		return;
	g_free(evidence->output_digest);
	g_free(evidence->output_sample);
	g_free(evidence->source_sample);
	axis_verify_run_clear(&evidence->decode);
	g_strfreev(evidence->output_hashes);
	axis_verify_run_clear(&evidence->source_packets.run);
	axis_verify_run_clear(&evidence->output_packets.run);
	axis_verify_run_clear(&evidence->tracks);
	g_strfreev(evidence->source_hashes);
	for (k = 0; k < AXIS_VERIFY_MAX_POINTS; k++)
		axis_verify_run_clear(&evidence->points[k].run);
	g_free(evidence);
}

/* As axis_probe_run_crashed() says of the runner's result: a signal other
 * than the two the runner's own limits send. */
gboolean
axis_verify_run_crashed(const AxisVerifyRun *run)
{
	g_return_val_if_fail(run != NULL, FALSE);

	return run->status == AXIS_PROBE_RUN_SIGNALLED && run->code != SIGXFSZ && run->code != SIGXCPU;
}

gboolean
axis_verify_run_reportable(const AxisVerifyRun *run)
{
	g_return_val_if_fail(run != NULL, FALSE);

	switch (run->status) {
	case AXIS_PROBE_RUN_OK:
	case AXIS_PROBE_RUN_EXITED:
	case AXIS_PROBE_RUN_TIMEOUT:
	case AXIS_PROBE_RUN_TOO_MUCH:
		return TRUE;
	case AXIS_PROBE_RUN_SIGNALLED:
		return !axis_verify_run_crashed(run);
	default:
		return FALSE;
	}
}

gboolean
axis_verify_evidence_complete(
	const AxisVerifyEvidence *evidence,
	const AxisVerifyPlan     *plan
){
	guint k;

	g_return_val_if_fail(evidence != NULL && plan != NULL, FALSE);

	if (!evidence->decoded || !evidence->counted || !evidence->hashed || evidence->n_points != plan->n_points)
		return FALSE;
	if (!axis_verify_run_reportable(&evidence->decode) || !axis_verify_run_reportable(&evidence->tracks) ||
	    !axis_verify_run_reportable(&evidence->source_packets.run) ||
	    !axis_verify_run_reportable(&evidence->output_packets.run))
		return FALSE;
	for (k = 0; k < evidence->n_points; k++) {
		if (!axis_verify_run_reportable(&evidence->points[k].run))
			return FALSE;
	}
	return TRUE;
}

static gboolean
hashes_fit(
	gchar      **hashes,
	guint        n,
	gchar      **problem
){
	guint i;

	if (hashes == NULL)
		return TRUE;
	if (g_strv_length(hashes) != n)
		return refuse(problem, "the number of track hashes is not the number of copied tracks");
	for (i = 0; i < n; i++) {
		const gchar *h = hashes[i];
		guint j;

		for (j = 0; h[j] != '\0'; j++) {
			if (!g_ascii_isxdigit(h[j]) || g_ascii_isupper(h[j]))
				return refuse(problem, "a track hash is not a SHA-256 in hexadecimal");
		}
		if (j != 64)
			return refuse(problem, "a track hash is not a SHA-256 in hexadecimal");
	}
	return TRUE;
}

gboolean
axis_verify_evidence_fits(
	const AxisVerifyEvidence  *evidence,
	const AxisVerifyPlan      *plan,
	gint64                     output_size,
	gint64                     source_size,
	gchar                    **problem
){
	guint k;

	if (evidence == NULL || plan == NULL)
		return refuse(problem, "there are no measurements");
	if (!axis_verify_evidence_complete(evidence, plan))
		return refuse(problem, "the measurements are not all there, or one did not end in a way a measurement may");
	if (!hashes_fit(evidence->output_hashes, plan->n_tracks, problem) ||
	    !hashes_fit(evidence->source_hashes, plan->n_tracks, problem))
		return FALSE;
	/* Hashes come only from a run that ended cleanly (one that did not gives
	 * none), and only for a plan that copies tracks. */
	if (plan->n_tracks == 0 && (evidence->output_hashes != NULL || evidence->source_hashes != NULL))
		return refuse(problem, "there are track hashes where the plan copies no track");
	if (evidence->output_hashes != NULL &&
	    (evidence->decode.status != AXIS_PROBE_RUN_OK || evidence->decode.error_bytes != 0))
		return refuse(problem, "the decode's hashes are there without a clean decode");
	if (evidence->source_hashes != NULL &&
	    (evidence->tracks.status != AXIS_PROBE_RUN_OK || evidence->tracks.error_bytes != 0))
		return refuse(problem, "the original's hashes are there without a clean run");
	/* A packet's data is in the file: no more bytes than the file has. */
	if (evidence->source_packets.read && (evidence->source_packets.run.status != AXIS_PROBE_RUN_OK ||
	                                      evidence->source_packets.bytes > (guint64)MAX(source_size, 0)))
		return refuse(problem, "the original's packet count is not plausible");
	if (evidence->output_packets.read && (evidence->output_packets.run.status != AXIS_PROBE_RUN_OK ||
	                                      evidence->output_packets.bytes > (guint64)MAX(output_size, 0)))
		return refuse(problem, "the converted file's packet count is not plausible");
	for (k = 0; k < evidence->n_points; k++) {
		const AxisVerifyPicture *p = &evidence->points[k];

		if (p->read && (p->run.status != AXIS_PROBE_RUN_OK || p->frames == 0 || !isfinite(p->lowest) ||
		                p->lowest < -1.0 || p->lowest > 1.0))
			return refuse(problem, "a point's similarity is not plausible");
	}
	return TRUE;
}

/* ── Words ──────────────────────────────────────────────────────── */

static const struct
{
	AxisProbeRunStatus  status;
	const gchar        *word;
} status_words[] = {
	{ AXIS_PROBE_RUN_OK, "ok" },
	{ AXIS_PROBE_RUN_EXITED, "exited" },
	{ AXIS_PROBE_RUN_SIGNALLED, "signalled" },
	{ AXIS_PROBE_RUN_TIMEOUT, "timeout" },
	{ AXIS_PROBE_RUN_TOO_MUCH, "too-much" },
	{ AXIS_PROBE_RUN_CANCELLED, "cancelled" },
	{ AXIS_PROBE_RUN_SPAWN_FAILED, "spawn-failed" },
	{ AXIS_PROBE_RUN_UNKILLABLE, "unkillable" }
};

const gchar *
axis_verify_status_to_string(AxisProbeRunStatus status)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(status_words); i++) {
		if (status_words[i].status == status)
			return status_words[i].word;
	}
	return "spawn-failed";
}

gboolean
axis_verify_status_from_string(
	const gchar        *text,
	AxisProbeRunStatus *status
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(status_words); i++) {
		if (strcmp(status_words[i].word, text) == 0) {
			*status = status_words[i].status;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── Judging ────────────────────────────────────────────────────── */

/* What a run said: its standard error, cleaned, or how it ended. */
static const gchar *
said(const AxisVerifyRun *run)
{
	return run->said != NULL ? run->said : axis_probe_run_status_describe(run->status);
}

static gboolean
clean(const AxisVerifyRun *run)
{
	return run->status == AXIS_PROBE_RUN_OK && run->error_bytes == 0;
}

/* 5 */
gboolean
axis_verify_judge_whole(
	const AxisVerifyEvidence  *evidence,
	gchar                    **detail
){
	g_return_val_if_fail(evidence != NULL && detail != NULL, FALSE);

	if (!evidence->decoded) {
		*detail = g_strdup("A complete decode of the picture and the sound did not end cleanly: it was not made.");
		return FALSE;
	}
	if (!clean(&evidence->decode)) {
		*detail = g_strdup_printf("A complete decode of the picture and the sound did not end cleanly: %s",
			said(&evidence->decode));
		return FALSE;
	}
	*detail = g_strdup("The picture and every sound track decode from start to end without an error.");
	return TRUE;
}

/* 6: why a count could not be made, or %NULL. */
static gchar *
not_counted(const AxisVerifyCount *count)
{
	if (count->run.status != AXIS_PROBE_RUN_OK)
		return g_strdup(said(&count->run));
	if (!count->read)
		return g_strdup("ffprobe's packet list is not one it writes");
	return NULL;
}

gboolean
axis_verify_judge_packets(
	const AxisVerifyEvidence  *evidence,
	guint64                   *packets,
	gchar                    **detail
){
	g_autofree gchar *why = NULL;

	g_return_val_if_fail(evidence != NULL && packets != NULL && detail != NULL, FALSE);

	*packets = 0;
	if (!evidence->counted) {
		*detail = g_strdup("The picture's packets cannot be counted: they were not counted.");
		return FALSE;
	}
	/* The original first, as it was always counted first. */
	why = not_counted(&evidence->source_packets);
	if (why == NULL)
		why = not_counted(&evidence->output_packets);
	if (why != NULL) {
		*detail = g_strdup_printf("The picture's packets cannot be counted: %s", why);
		return FALSE;
	}
	if (evidence->source_packets.count == 0 || evidence->source_packets.count != evidence->output_packets.count) {
		*detail = g_strdup_printf("The picture has %" G_GUINT64_FORMAT " packets; the original's has %" G_GUINT64_FORMAT
			".", evidence->output_packets.count, evidence->source_packets.count);
		return FALSE;
	}
	*packets = evidence->output_packets.count;
	return TRUE;
}

/* 7 */
static const gchar *
kind_word(AxisStreamKind kind)
{
	switch (kind) {
	case AXIS_STREAM_AUDIO:
		return "sound";
	case AXIS_STREAM_SUBTITLE:
		return "subtitle";
	case AXIS_STREAM_VIDEO:
		return "picture";
	case AXIS_STREAM_ATTACHMENT:
		return "attachment";
	default:
		return "data";
	}
}

gboolean
axis_verify_judge_tracks(
	const AxisVerifyEvidence  *evidence,
	const AxisConvertPlan     *plan,
	gchar                    **detail
){
	guint copied = 0;
	guint i;
	guint k;

	g_return_val_if_fail(evidence != NULL && plan != NULL && detail != NULL, FALSE);

	for (i = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

		if (s->action == AXIS_CONVERT_COPY && s->kind != AXIS_STREAM_ATTACHMENT)
			copied++;
	}
	if (copied == 0)
		return TRUE;
	if (evidence->output_hashes == NULL || g_strv_length(evidence->output_hashes) != copied) {
		*detail = g_strdup("The converted file's copied tracks could not be read for the comparison.");
		return FALSE;
	}
	if (!evidence->hashed || !clean(&evidence->tracks) || evidence->source_hashes == NULL ||
	    g_strv_length(evidence->source_hashes) != copied) {
		*detail = g_strdup_printf("The original's tracks could not be read for the comparison: %s",
			evidence->hashed ? said(&evidence->tracks) : "they were not read");
		return FALSE;
	}
	for (i = 0, k = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

		if (s->action != AXIS_CONVERT_COPY || s->kind == AXIS_STREAM_ATTACHMENT)
			continue;
		if (g_ascii_strcasecmp(evidence->source_hashes[k], evidence->output_hashes[k]) != 0) {
			*detail = g_strdup_printf("The copied %s track (stream %u of the converted file, stream %d of the "
				"original) is not the original's: its data differs (SHA-256 of its packets %.12s... in the "
				"converted file, %.12s... in the original).", kind_word(s->kind), s->position, s->source,
				evidence->output_hashes[k], evidence->source_hashes[k]);
			return FALSE;
		}
		k++;
	}
	return TRUE;
}

/* 8 */
gboolean
axis_verify_judge_picture(
	const AxisVerifyEvidence  *evidence,
	guint                      n_points,
	gdouble                    floor_value,
	gchar                    **detail
){
	GString *figures;
	gdouble overall = 1.0;
	guint k;

	g_return_val_if_fail(evidence != NULL && detail != NULL, FALSE);

	if (n_points == 0) {
		*detail = g_strdup("The movie's length is not known: no points to compare at.");
		return FALSE;
	}
	figures = g_string_new(NULL);
	for (k = 0; k < n_points && k < AXIS_VERIFY_MAX_POINTS; k++) {
		const AxisVerifyPicture *p = &evidence->points[k];

		if (k >= evidence->n_points) {
			*detail = g_strdup_printf("The pictures at point %u could not be compared: they were not compared.", k + 1);
			g_string_free(figures, TRUE);
			return FALSE;
		}
		if (p->run.status != AXIS_PROBE_RUN_OK) {
			*detail = g_strdup_printf("The pictures at point %u could not be compared: %s", k + 1, said(&p->run));
			g_string_free(figures, TRUE);
			return FALSE;
		}
		if (!p->read || p->frames == 0 || !isfinite(p->lowest)) {
			*detail = g_strdup_printf("The comparison at point %u found no frames to compare.", k + 1);
			g_string_free(figures, TRUE);
			return FALSE;
		}
		g_string_append_printf(figures, "%s%.3f", k > 0 ? ", " : "", p->lowest);
		overall = MIN(overall, p->lowest);
		if (p->lowest < floor_value) {
			*detail = g_strdup_printf("At point %u of %u the lowest similarity (SSIM) is %.3f, below %.3f: it is not the "
				"same picture (%s).", k + 1, n_points, p->lowest, floor_value, figures->str);
			g_string_free(figures, TRUE);
			return FALSE;
		}
	}
	if (n_points > AXIS_VERIFY_MAX_POINTS) {
		*detail = g_strdup("There are more points than can be compared.");
		g_string_free(figures, TRUE);
		return FALSE;
	}
	*detail = g_strdup_printf("The lowest SSIM at each of %u points (picture and colour): %s; at least %.3f needed.",
		n_points, figures->str, floor_value);
	g_string_free(figures, TRUE);
	return overall >= floor_value;
}

gboolean
axis_verify_point_fails(
	const AxisVerifyEvidence *evidence,
	guint                     k,
	gdouble                   floor_value
){
	const AxisVerifyPicture *p;

	g_return_val_if_fail(evidence != NULL, TRUE);

	if (k >= evidence->n_points || k >= AXIS_VERIFY_MAX_POINTS)
		return TRUE;
	p = &evidence->points[k];
	return p->run.status != AXIS_PROBE_RUN_OK || !p->read || p->frames == 0 || !isfinite(p->lowest) ||
		p->lowest < floor_value;
}

/*
 * fuzz-verify-evidence.c - libFuzzer target for a worker's measurements of
 * a converted file's checks (convert.verify, worker protocol 3) and the
 * plan of them, as the daemon reads, keeps and judges them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses ('0' a result, '1' a plan); the rest is the JSON.
 * Whatever it is, the reader must return without crashing or allocating
 * without bound, and what it accepts must keep the promises of
 * api/axis-worker-proto.h and transcode/axis-verify.h:
 *
 *  - every digest and hash is 64 lower-case hexadecimal digits, every text
 *    valid UTF-8 without control characters and within its length, every
 *    number in range, every run one a measurement may end in (never a
 *    crash, a cancellation, a tool that could not start);
 *  - a plan passes axis_verify_plan_check();
 *  - what is accepted, written out again, is accepted again, the same;
 *  - the daemon's checks of it (its fit to a plan, the four judges) return
 *    without crashing, with a sentence for every refusal and every verdict.
 */

#include <glib.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api/axis-worker-proto.h"
#include "transcode/axis-verify.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_text(
	const gchar *text,
	guint        max_chars
){
	const guchar *p;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > (glong)max_chars)
		abort();
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			abort();
	}
}

static void
check_run(const AxisVerifyRun *run)
{
	if (!axis_verify_run_reportable(run) || axis_verify_run_crashed(run))
		abort();
	if (run->said != NULL && run->error_bytes == 0)
		abort();
	check_text(run->said, AXIS_VERIFY_SAID_MAX_CHARS);
}

static void
check_hashes(gchar **hashes)
{
	guint i;

	for (i = 0; hashes != NULL && hashes[i] != NULL; i++) {
		if (!axis_worker_digest_is_valid(hashes[i]))
			abort();
	}
	if (hashes != NULL && i > AXIS_VERIFY_MAX_TRACKS)
		abort();
}

/* A plan the judges can run against: the picture, one copied AAC track and
 * one copied subtitle, two points. */
static AxisVerifyPlan *
plan_for(AxisConvertPlan *convert)
{
	return axis_verify_plan_make(convert, 4000, 2, 60000);
}

static void
judge(const AxisVerifyEvidence *e)
{
	static AxisConvertStream streams[3];
	AxisConvertPlan convert;
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_autoptr(GPtrArray) list = g_ptr_array_new();
	g_autofree gchar *problem = NULL;
	gchar *detail = NULL;
	guint64 packets = 0;
	guint i;

	memset(&convert, 0, sizeof(convert));
	memset(streams, 0, sizeof(streams));
	streams[0].position = 0;
	streams[0].source = 0;
	streams[0].kind = AXIS_STREAM_VIDEO;
	streams[0].action = AXIS_CONVERT_ENCODE;
	streams[1].position = 1;
	streams[1].source = 1;
	streams[1].kind = AXIS_STREAM_AUDIO;
	streams[1].action = AXIS_CONVERT_COPY;
	streams[1].codec = (gchar *)"aac";
	streams[2].position = 2;
	streams[2].source = 2;
	streams[2].kind = AXIS_STREAM_SUBTITLE;
	streams[2].action = AXIS_CONVERT_COPY;
	for (i = 0; i < G_N_ELEMENTS(streams); i++)
		g_ptr_array_add(list, &streams[i]);
	convert.streams = list;
	plan = plan_for(&convert);
	if (plan == NULL || !axis_verify_plan_check(plan, NULL))
		abort();
	(void)axis_verify_evidence_complete(e, plan);
	if (!axis_verify_evidence_fits(e, plan, 1 << 30, 1 << 30, &problem) && problem == NULL)
		abort();
	(void)axis_verify_judge_whole(e, &detail);
	if (detail == NULL)
		abort();
	g_clear_pointer(&detail, g_free);
	if (!axis_verify_judge_packets(e, &packets, &detail) && detail == NULL)
		abort();
	g_clear_pointer(&detail, g_free);
	if (!axis_verify_judge_tracks(e, &convert, &detail) && detail == NULL)
		abort();
	g_clear_pointer(&detail, g_free);
	(void)axis_verify_judge_picture(e, e->n_points, 0.85, &detail);
	if (detail == NULL)
		abort();
	g_clear_pointer(&detail, g_free);
}

static void
fuzz_result(
	const gchar *text,
	gsize        length
){
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisVerifyResult) result = axis_verify_result_parse(text, length, &problem);
	g_autoptr(AxisVerifyResult) again = NULL;
	g_autofree gchar *written = NULL;
	g_autofree gchar *rewritten = NULL;
	const AxisVerifyEvidence *e;
	guint k;

	if (result == NULL) {
		if (problem == NULL)
			abort();
		check_text(problem, 300);
		return;
	}
	e = result->evidence;
	if (!axis_convert_output_name_valid(result->output) || result->output_size < 1 || e == NULL ||
	    !axis_worker_digest_is_valid(e->output_digest) || !axis_worker_digest_is_valid(e->output_sample) ||
	    !axis_worker_digest_is_valid(e->source_sample) || !e->decoded || !e->counted || !e->hashed ||
	    e->n_points > AXIS_VERIFY_MAX_POINTS)
		abort();
	check_run(&e->decode);
	check_run(&e->tracks);
	check_run(&e->source_packets.run);
	check_run(&e->output_packets.run);
	check_hashes(e->output_hashes);
	check_hashes(e->source_hashes);
	for (k = 0; k < e->n_points; k++) {
		check_run(&e->points[k].run);
		if (e->points[k].read && (!isfinite(e->points[k].lowest) || e->points[k].lowest < -1.0 ||
		                          e->points[k].lowest > 1.0 || e->points[k].frames == 0))
			abort();
	}
	judge(e);
	/* Written again, read again, the same. */
	written = axis_verify_result_to_json(result);
	if (written == NULL)
		abort();
	again = axis_verify_result_parse(written, strlen(written), NULL);
	if (again == NULL)
		abort();
	rewritten = axis_verify_result_to_json(again);
	if (rewritten == NULL || strcmp(written, rewritten) != 0)
		abort();
}

static void
fuzz_plan(
	const gchar *text,
	gsize        length
){
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisVerifyPlan) plan = axis_verify_plan_parse(text, length, &problem);
	g_autoptr(AxisVerifyPlan) again = NULL;
	g_autofree gchar *written = NULL;
	g_autofree gchar *rewritten = NULL;

	if (plan == NULL) {
		if (problem == NULL)
			abort();
		check_text(problem, 300);
		return;
	}
	if (!axis_verify_plan_check(plan, NULL))
		abort();
	written = axis_verify_plan_to_json(plan);
	again = axis_verify_plan_parse(written, strlen(written), NULL);
	if (again == NULL)
		abort();
	rewritten = axis_verify_plan_to_json(again);
	if (strcmp(written, rewritten) != 0)
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text;
	gsize length;

	if (size < 1)
		return 0;
	text = (const gchar *)data + 1;
	length = size - 1;
	if (data[0] == '0')
		fuzz_result(text, length);
	else if (data[0] == '1')
		fuzz_plan(text, length);
	return 0;
}

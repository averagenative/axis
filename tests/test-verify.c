/*
 * test-verify.c - the measurements of a converted file's checks 5 to 8, the
 * same on the daemon and on a worker, and the daemon's judges of them; the
 * check job's description and evidence in the protocol; the whole-file
 * digest
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No database.  The measurements run ffmpeg on clips generated here and are
 * skipped, before anything is made, when ffmpeg is not installed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "api/axis-worker-proto.h"
#include "storage/axis-file-check.h"
#include "transcode/axis-verify.h"

/* 64 of @c: a SHA-256 in hexadecimal, made up. */
static gchar *
hex_of(gchar c)
{
	return g_strnfill(64, c);
}

static const gchar *const ffmpeg[] = { "ffmpeg", NULL };
static const gchar *const ffprobe[] = { "ffprobe", NULL };

/* ── Plans made by hand ─────────────────────────────────────────── */

typedef struct
{
	AxisConvertPlan   plan;
	AxisConvertStream streams[6];
} Plan;

/* A picture encoded anew (0 -> 0), an AAC track copied (1 -> 1), a
 * subtitle copied (3 -> 2), a stereo track made anew (1 -> 3), an
 * attachment copied (4 -> 4). */
static void
plan_init(Plan *p)
{
	static const struct
	{
		guint             position;
		gint              source;
		AxisStreamKind    kind;
		AxisConvertAction action;
		const gchar      *codec;
	} rows[] = {
		{ 0, 0, AXIS_STREAM_VIDEO, AXIS_CONVERT_ENCODE, "hevc" },
		{ 1, 1, AXIS_STREAM_AUDIO, AXIS_CONVERT_COPY, "aac" },
		{ 2, 3, AXIS_STREAM_SUBTITLE, AXIS_CONVERT_COPY, "subrip" },
		{ 3, 1, AXIS_STREAM_AUDIO, AXIS_CONVERT_STEREO, "aac" },
		{ 4, 4, AXIS_STREAM_ATTACHMENT, AXIS_CONVERT_COPY, NULL }
	};
	guint i;

	memset(p, 0, sizeof(*p));
	p->plan.streams = g_ptr_array_new();
	for (i = 0; i < G_N_ELEMENTS(rows); i++) {
		p->streams[i].position = rows[i].position;
		p->streams[i].source = rows[i].source;
		p->streams[i].kind = rows[i].kind;
		p->streams[i].action = rows[i].action;
		p->streams[i].codec = (gchar *)rows[i].codec;
		g_ptr_array_add(p->plan.streams, &p->streams[i]);
	}
}

static void
plan_clear(Plan *p)
{
	g_ptr_array_unref(p->plan.streams);
}

/* Spec (conversion-execution): the measurements are of the tracks the plan
 * copies as packets, at the instants the checks always used. */
static void
test_plan(void)
{
	g_autoptr(AxisVerifyPlan) made = NULL;
	g_autoptr(AxisVerifyPlan) unknown = NULL;
	g_autoptr(AxisVerifyPlan) copy = NULL;
	g_autofree gchar *problem = NULL;
	Plan p;

	plan_init(&p);
	made = axis_verify_plan_make(&p.plan, 4000, 5, 60000);
	g_assert_nonnull(made);
	g_assert_cmpint(made->picture_source, ==, 0);
	g_assert_cmpuint(made->picture_output, ==, 0);
	/* The AAC and the subtitle; not the stereo track, not the attachment. */
	g_assert_cmpuint(made->n_tracks, ==, 2);
	g_assert_cmpint(made->tracks[0].source, ==, 1);
	g_assert_cmpuint(made->tracks[0].output, ==, 1);
	g_assert_true(made->tracks[0].aac);
	g_assert_cmpint(made->tracks[1].source, ==, 3);
	g_assert_cmpuint(made->tracks[1].output, ==, 2);
	g_assert_false(made->tracks[1].aac);
	/* Four seconds, five points: windows of 0.4 s centred at 0.4, 1.2, ... */
	g_assert_cmpuint(made->n_points, ==, 5);
	g_assert_cmpint(made->points[0].start_ms, ==, 200);
	g_assert_cmpint(made->points[0].length_ms, ==, 400);
	g_assert_cmpint(made->points[4].start_ms, ==, 3400);
	g_assert_cmpuint(made->timeout_ms, ==, 60000);
	g_assert_true(axis_verify_plan_check(made, NULL));
	copy = axis_verify_plan_copy(made);
	g_assert_cmpuint(copy->n_tracks, ==, 2);
	g_assert_cmpint(copy->tracks[1].source, ==, 3);

	/* Its length not known: no points (check 8 then fails). */
	unknown = axis_verify_plan_make(&p.plan, 0, 5, 60000);
	g_assert_cmpuint(unknown->n_points, ==, 0);

	/* Out of bounds. */
	copy->timeout_ms = 10;
	g_assert_false(axis_verify_plan_check(copy, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	copy->timeout_ms = 60000;
	copy->tracks[1].output = 1;
	g_assert_false(axis_verify_plan_check(copy, &problem));
	g_assert_nonnull(strstr(problem, "same place"));
	plan_clear(&p);
}

/* ── The commands ───────────────────────────────────────────────── */

static gint
index_of(
	gchar       **argv,
	const gchar  *word,
	gint          from
){
	gint i;

	for (i = from; argv[i] != NULL; i++) {
		if (strcmp(argv[i], word) == 0)
			return i;
	}
	return -1;
}

static guint
count_of(
	gchar       **argv,
	const gchar  *word
){
	guint n = 0;
	guint i;

	for (i = 0; argv[i] != NULL; i++)
		n += strcmp(argv[i], word) == 0 ? 1 : 0;
	return n;
}

/* Spec (conversion-execution): on the server every ffmpeg command of the
 * checks asks for a bounded number of threads for each input and for its
 * filters; a worker's leaves it to ffmpeg. */
static void
test_commands(void)
{
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_auto(GStrv) decode = NULL;
	g_auto(GStrv) point = NULL;
	g_auto(GStrv) tracks = NULL;
	g_auto(GStrv) packets = NULL;
	g_auto(GStrv) free_decode = NULL;
	g_auto(GStrv) free_point = NULL;
	AxisVerifyTools tools;
	Plan p;
	gint at;

	plan_init(&p);
	plan = axis_verify_plan_make(&p.plan, 4000, 2, 60000);
	memset(&tools, 0, sizeof(tools));
	tools.ffmpeg = ffmpeg;
	tools.ffprobe = ffprobe;
	tools.threads = 2;
	decode = axis_verify_decode_argv(&tools, plan);
	point = axis_verify_point_argv(&tools, plan, 1);
	tracks = axis_verify_tracks_argv(&tools, plan);
	packets = axis_verify_packets_argv(&tools, 0);

	/* The decode: two threads for its input, before the input; two for its
	 * filters; its copied tracks hashed in the same reading. */
	at = index_of(decode, "-threads", 0);
	g_assert_cmpint(at, >, 0);
	g_assert_cmpstr(decode[at + 1], ==, "2");
	g_assert_cmpint(at, <, index_of(decode, "-i", 0));
	at = index_of(decode, "-filter_threads", 0);
	g_assert_cmpint(at, >, 0);
	g_assert_cmpstr(decode[at + 1], ==, "2");
	g_assert_cmpint(index_of(decode, "-xerror", 0), >, 0);
	g_assert_cmpint(index_of(decode, "streamhash", 0), >, 0);
	g_assert_cmpint(index_of(decode, "aac_adtstoasc", 0), >, 0);
	/* The comparison: two inputs of two threads each, and the graph's. */
	g_assert_cmpuint(count_of(point, "-threads"), ==, 2);
	at = index_of(point, "-filter_complex_threads", 0);
	g_assert_cmpint(at, >, 0);
	g_assert_cmpstr(point[at + 1], ==, "2");
	g_assert_cmpint(at, <, index_of(point, "-i", 0));
	at = index_of(point, "-ss", 0);
	g_assert_cmpstr(point[at + 1], ==, "2.500");
	at = index_of(point, "-t", 0);
	g_assert_cmpstr(point[at + 1], ==, "1.000");
	/* The original's tracks: its input too; the packet count is ffprobe's
	 * demuxing only. */
	g_assert_cmpuint(count_of(tracks, "-threads"), ==, 1);
	g_assert_cmpuint(count_of(packets, "-threads"), ==, 0);
	/* Only descriptors and the fd protocol. */
	g_assert_cmpint(index_of(decode, "fd:", 0), >, 0);
	g_assert_cmpint(index_of(point, "4", 0), >, 0);

	/* A worker's: ffmpeg's own threads. */
	tools.threads = 0;
	free_decode = axis_verify_decode_argv(&tools, plan);
	free_point = axis_verify_point_argv(&tools, plan, 0);
	g_assert_cmpuint(count_of(free_decode, "-threads"), ==, 0);
	g_assert_cmpuint(count_of(free_decode, "-filter_threads"), ==, 0);
	g_assert_cmpuint(count_of(free_point, "-threads"), ==, 0);
	g_assert_cmpuint(count_of(free_point, "-filter_complex_threads"), ==, 0);
	plan_clear(&p);
}

/* ── The judges ─────────────────────────────────────────────────── */

static void
set_run(
	AxisVerifyRun      *run,
	AxisProbeRunStatus  status,
	gint                code,
	const gchar        *said
){
	axis_verify_run_clear(run);
	run->status = status;
	run->code = code;
	run->said = g_strdup(said);
	run->error_bytes = said != NULL ? strlen(said) : 0;
}

/* Evidence of a good output for plan_init()'s plan, two points. */
static AxisVerifyEvidence *
good_evidence(void)
{
	AxisVerifyEvidence *e = axis_verify_evidence_new();
	guint k;

	e->decoded = TRUE;
	e->output_hashes = g_new0(gchar *, 3);
	e->output_hashes[0] = hex_of('a');
	e->output_hashes[1] = hex_of('b');
	e->counted = TRUE;
	e->source_packets.read = TRUE;
	e->source_packets.count = 96;
	e->source_packets.bytes = 1000;
	e->output_packets.read = TRUE;
	e->output_packets.count = 96;
	e->output_packets.bytes = 500;
	e->hashed = TRUE;
	e->source_hashes = g_strdupv(e->output_hashes);
	e->n_points = 2;
	for (k = 0; k < 2; k++) {
		e->points[k].read = TRUE;
		e->points[k].lowest = 0.97 + 0.01 * k;
		e->points[k].frames = 22;
	}
	e->output_digest = hex_of('c');
	e->output_sample = hex_of('d');
	e->source_sample = hex_of('e');
	return e;
}

/* Spec (conversion-execution): the daemon decides checks 5 to 8 from the
 * figures, with the sentences it always wrote. */
static void
test_judges(void)
{
	g_autoptr(AxisVerifyEvidence) e = good_evidence();
	g_autofree gchar *detail = NULL;
	guint64 packets = 0;
	Plan p;

	plan_init(&p);

	/* 5 */
	g_assert_true(axis_verify_judge_whole(e, &detail));
	g_assert_cmpstr(detail, ==, "The picture and every sound track decode from start to end without an error.");
	g_clear_pointer(&detail, g_free);
	set_run(&e->decode, AXIS_PROBE_RUN_OK, 0, "[hevc] error while decoding MB 3 4");
	g_assert_false(axis_verify_judge_whole(e, &detail));
	g_assert_cmpstr(detail, ==, "A complete decode of the picture and the sound did not end cleanly: [hevc] error "
		"while decoding MB 3 4");
	g_clear_pointer(&detail, g_free);
	/* Bytes on standard error that cleaned away to nothing still fail. */
	set_run(&e->decode, AXIS_PROBE_RUN_OK, 0, NULL);
	e->decode.error_bytes = 3;
	g_assert_false(axis_verify_judge_whole(e, &detail));
	g_clear_pointer(&detail, g_free);
	set_run(&e->decode, AXIS_PROBE_RUN_EXITED, 1, NULL);
	g_assert_false(axis_verify_judge_whole(e, &detail));
	g_assert_true(g_str_has_prefix(detail, "A complete decode of the picture and the sound did not end cleanly: "));
	g_clear_pointer(&detail, g_free);
	set_run(&e->decode, AXIS_PROBE_RUN_OK, 0, NULL);

	/* 6 */
	g_assert_true(axis_verify_judge_packets(e, &packets, &detail));
	g_assert_cmpuint(packets, ==, 96);
	g_assert_null(detail);
	e->output_packets.count = 95;
	g_assert_false(axis_verify_judge_packets(e, &packets, &detail));
	g_assert_cmpstr(detail, ==, "The picture has 95 packets; the original's has 96.");
	g_clear_pointer(&detail, g_free);
	e->output_packets.count = 96;
	e->source_packets.read = FALSE;
	g_assert_false(axis_verify_judge_packets(e, &packets, &detail));
	g_assert_cmpstr(detail, ==, "The picture's packets cannot be counted: ffprobe's packet list is not one it writes");
	g_clear_pointer(&detail, g_free);
	e->source_packets.read = TRUE;
	set_run(&e->output_packets.run, AXIS_PROBE_RUN_TIMEOUT, 0, NULL);
	g_assert_false(axis_verify_judge_packets(e, &packets, &detail));
	g_assert_true(g_str_has_prefix(detail, "The picture's packets cannot be counted: "));
	g_clear_pointer(&detail, g_free);
	set_run(&e->output_packets.run, AXIS_PROBE_RUN_OK, 0, NULL);
	e->source_packets.count = 0;
	e->output_packets.count = 0;
	g_assert_false(axis_verify_judge_packets(e, &packets, &detail));
	g_clear_pointer(&detail, g_free);
	e->source_packets.count = 96;
	e->output_packets.count = 96;

	/* 7 */
	g_assert_true(axis_verify_judge_tracks(e, &p.plan, &detail));
	g_assert_null(detail);
	g_free(e->source_hashes[1]);
	e->source_hashes[1] = hex_of('f');
	g_assert_false(axis_verify_judge_tracks(e, &p.plan, &detail));
	g_assert_cmpstr(detail, ==, "The copied subtitle track (stream 2 of the converted file, stream 3 of the original) "
		"is not the original's: its data differs (SHA-256 of its packets bbbbbbbbbbbb... in the converted file, "
		"ffffffffffff... in the original).");
	g_clear_pointer(&detail, g_free);
	g_clear_pointer(&e->output_hashes, g_strfreev);
	g_assert_false(axis_verify_judge_tracks(e, &p.plan, &detail));
	g_assert_cmpstr(detail, ==, "The converted file's copied tracks could not be read for the comparison.");
	g_clear_pointer(&detail, g_free);
	e->output_hashes = g_strdupv(e->source_hashes);
	set_run(&e->tracks, AXIS_PROBE_RUN_EXITED, 1, "moov atom not found");
	g_assert_false(axis_verify_judge_tracks(e, &p.plan, &detail));
	g_assert_cmpstr(detail, ==, "The original's tracks could not be read for the comparison: moov atom not found");
	g_clear_pointer(&detail, g_free);
	set_run(&e->tracks, AXIS_PROBE_RUN_OK, 0, NULL);

	/* 8 */
	g_assert_true(axis_verify_judge_picture(e, 2, 0.85, &detail));
	g_assert_cmpstr(detail, ==, "The lowest SSIM at each of 2 points (picture and colour): 0.970, 0.980; at least "
		"0.850 needed.");
	g_clear_pointer(&detail, g_free);
	e->points[1].lowest = 0.42;
	g_assert_false(axis_verify_judge_picture(e, 2, 0.85, &detail));
	g_assert_cmpstr(detail, ==, "At point 2 of 2 the lowest similarity (SSIM) is 0.420, below 0.850: it is not the "
		"same picture (0.970, 0.420).");
	g_clear_pointer(&detail, g_free);
	g_assert_true(axis_verify_point_fails(e, 1, 0.85));
	g_assert_false(axis_verify_point_fails(e, 0, 0.85));
	e->points[1].read = FALSE;
	g_assert_false(axis_verify_judge_picture(e, 2, 0.85, &detail));
	g_assert_cmpstr(detail, ==, "The comparison at point 2 found no frames to compare.");
	g_clear_pointer(&detail, g_free);
	set_run(&e->points[1].run, AXIS_PROBE_RUN_EXITED, 1, "Invalid argument");
	g_assert_false(axis_verify_judge_picture(e, 2, 0.85, &detail));
	g_assert_cmpstr(detail, ==, "The pictures at point 2 could not be compared: Invalid argument");
	g_clear_pointer(&detail, g_free);
	/* A point not measured is not a pass. */
	g_assert_false(axis_verify_judge_picture(e, 3, 0.85, &detail));
	g_clear_pointer(&detail, g_free);
	g_assert_false(axis_verify_judge_picture(e, 0, 0.85, &detail));
	g_assert_cmpstr(detail, ==, "The movie's length is not known: no points to compare at.");
	g_clear_pointer(&detail, g_free);
	plan_clear(&p);
}

/* Spec (worker-protocol): a worker's measurements must fit the plan it was
 * given, and be ones a measurement may be. */
static void
test_fits(void)
{
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_autoptr(AxisVerifyEvidence) e = NULL;
	g_autofree gchar *problem = NULL;
	Plan p;

	plan_init(&p);
	plan = axis_verify_plan_make(&p.plan, 4000, 2, 60000);
	e = good_evidence();
	g_assert_true(axis_verify_evidence_complete(e, plan));
	g_assert_true(axis_verify_evidence_fits(e, plan, 600, 1200, &problem));
	g_assert_null(problem);

#define REFUSED() do { \
		g_assert_false(axis_verify_evidence_fits(e, plan, 600, 1200, &problem)); \
		g_assert_nonnull(problem); \
		g_clear_pointer(&problem, g_free); \
	} while (0)
	/* Another number of points or hashes. */
	e->n_points = 1;
	REFUSED();
	e->n_points = 2;
	g_free(e->output_hashes[1]);
	e->output_hashes[1] = NULL;
	REFUSED();
	e->output_hashes[1] = g_strnfill(64, 'A');
	REFUSED();
	g_free(e->output_hashes[1]);
	e->output_hashes[1] = hex_of('b');
	/* Hashes from a decode that did not end cleanly. */
	set_run(&e->decode, AXIS_PROBE_RUN_EXITED, 1, "x");
	REFUSED();
	set_run(&e->decode, AXIS_PROBE_RUN_OK, 0, NULL);
	/* More packet bytes than the file has. */
	e->output_packets.bytes = 601;
	REFUSED();
	e->output_packets.bytes = 500;
	/* A similarity out of its range. */
	e->points[0].lowest = 1.5;
	REFUSED();
	e->points[0].lowest = 0.97;
	/* A crash, a cancellation: not measurements. */
	set_run(&e->points[1].run, AXIS_PROBE_RUN_SIGNALLED, SIGSEGV, NULL);
	g_assert_true(axis_verify_run_crashed(&e->points[1].run));
	REFUSED();
	set_run(&e->points[1].run, AXIS_PROBE_RUN_SIGNALLED, SIGXCPU, NULL);
	g_assert_false(axis_verify_run_crashed(&e->points[1].run));
	g_assert_true(axis_verify_run_reportable(&e->points[1].run));
	set_run(&e->tracks, AXIS_PROBE_RUN_CANCELLED, 0, NULL);
	REFUSED();
#undef REFUSED
	plan_clear(&p);
}

/* ── The protocol ───────────────────────────────────────────────── */

/* Spec (worker-protocol): the check job's description and result are read
 * strictly and written exactly. */
static void
test_protocol(void)
{
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_autoptr(AxisVerifyPlan) back = NULL;
	g_autoptr(AxisVerifyResult) result = g_new0(AxisVerifyResult, 1);
	g_autoptr(AxisVerifyResult) read = NULL;
	g_autofree gchar *plan_json = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *json_again = NULL;
	g_autofree gchar *problem = NULL;
	Plan p;
	guint i;

	plan_init(&p);
	plan = axis_verify_plan_make(&p.plan, 4000, 2, 60000);
	plan_json = axis_verify_plan_to_json(plan);
	back = axis_verify_plan_parse(plan_json, strlen(plan_json), &problem);
	g_assert_nonnull(back);
	again = axis_verify_plan_to_json(back);
	g_assert_cmpstr(again, ==, plan_json);

	result->output = g_strdup("41-1.tmp");
	result->output_size = 600;
	result->evidence = good_evidence();
	set_run(&result->evidence->points[0].run, AXIS_PROBE_RUN_OK, 0, NULL);
	json = axis_verify_result_to_json(result);
	g_assert_nonnull(json);
	read = axis_verify_result_parse(json, strlen(json), &problem);
	if (read == NULL)
		g_error("%s: %s", json, problem);
	json_again = axis_verify_result_to_json(read);
	g_assert_cmpstr(json_again, ==, json);
	g_assert_true(axis_verify_evidence_fits(read->evidence, plan, 600, 1200, NULL));

	/* What a worker may not say: a run that is not a measurement, words
	 * without bytes, a hash not in lower case, a similarity out of range, a
	 * count without bytes, a member the protocol does not have. */
	{
		static const struct
		{
			const gchar *from;
			const gchar *to;
		} changes[] = {
			{ "\"status\":\"ok\"", "\"status\":\"cancelled\"" },
			{ "\"status\":\"ok\"", "\"status\":\"spawn-failed\"" },
			{ "\"status\":\"ok\",\"code\":0", "\"status\":\"signalled\",\"code\":11" },
			{ "\"error_bytes\":0,\"said\":null", "\"error_bytes\":0,\"said\":\"x\"" },
			{ "\"hashes\":[\"a", "\"hashes\":[\"A" },
			{ "\"lowest\":0.970000", "\"lowest\":1.500000" },
			{ "\"count\":96,\"bytes\":1000", "\"count\":96,\"bytes\":null" },
			{ "\"output\":\"41-1.tmp\"", "\"output\":\"../41-1.tmp\"" },
			{ "\"output_size\":600", "\"output_size\":600,\"passed\":true" },
			{ "\"output_digest\":\"c", "\"output_digest\":\"z" }
		};

		for (i = 0; i < G_N_ELEMENTS(changes); i++) {
			g_autofree gchar *bad = NULL;
			const gchar *at = strstr(json, changes[i].from);
			g_autoptr(AxisVerifyResult) refused = NULL;
			g_autofree gchar *why = NULL;

			g_assert_nonnull(at);
			bad = g_strdup_printf("%.*s%s%s", (gint)(at - json), json, changes[i].to, at + strlen(changes[i].from));
			refused = axis_verify_result_parse(bad, strlen(bad), &why);
			if (refused != NULL)
				g_error("accepted: %s", bad);
			g_assert_nonnull(why);
		}
	}
	/* A result that is not a measurement is not written. */
	set_run(&result->evidence->decode, AXIS_PROBE_RUN_SIGNALLED, SIGSEGV, NULL);
	g_assert_null(axis_verify_result_to_json(result));

	/* The description: written and read back; one with another kind of name
	 * or a plan out of bounds is refused. */
	{
		g_autoptr(AxisVerifyWork) work = g_new0(AxisVerifyWork, 1);
		AxisWorkerJob job;
		g_autofree gchar *claimed = NULL;
		g_autoptr(AxisWorkerJob) got = NULL;
		g_autofree gchar *bad = NULL;
		g_autofree gchar *why = NULL;
		AxisWorkerJob *refused = NULL;

		work->volume = g_strdup("3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b");
		work->root = g_strdup("Movies");
		work->path = g_strdup("Example (2001)/Example (2001).mkv");
		work->size = 1200;
		work->mtime_sec = 1700000000;
		work->mtime_nsec = 5;
		work->inode = 77;
		work->source_sample = hex_of('e');
		work->output = g_strdup("41-1.tmp");
		work->output_size = 600;
		work->plan = axis_verify_plan_copy(plan);
		g_assert_true(axis_verify_work_check(work, NULL));
		memset(&job, 0, sizeof(job));
		job.id = 9;
		job.kind = (gchar *)AXIS_WORKER_KIND_VERIFY;
		job.lease_token = (gchar *)"0b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c";
		job.lease_ms = 60000;
		job.heartbeat_ms = 15000;
		job.attempt = 1;
		job.verify = work;
		claimed = axis_worker_claimed_to_json(&job, NULL);
		g_assert_true(axis_worker_claimed_parse(claimed, strlen(claimed), &got, NULL, NULL));
		g_assert_nonnull(got->verify);
		g_assert_cmpstr(got->verify->output, ==, "41-1.tmp");
		g_assert_cmpint(got->verify->output_size, ==, 600);
		g_assert_cmpuint(got->verify->plan->n_tracks, ==, 2);
		g_assert_cmpuint(got->verify->plan->n_points, ==, 2);
		bad = g_strdup(claimed);
		*strstr(bad, "41-1.tmp") = 'x';
		g_assert_false(axis_worker_claimed_parse(bad, strlen(bad), &refused, NULL, &why));
		g_assert_null(refused);
		g_assert_nonnull(why);
	}
	plan_clear(&p);
}

/* ── The whole file's digest ────────────────────────────────────── */

static gchar *
scratch_file(
	const gchar *dir,
	gsize        size
){
	g_autofree gchar *data = g_malloc(size);
	gchar *path = g_build_filename(dir, "whole.bin", NULL);
	gsize i;

	for (i = 0; i < size; i++)
		data[i] = (gchar)(i * 7 + 3);
	g_assert_true(g_file_set_contents(path, data, (gssize)size, NULL));
	return path;
}

/* Spec (conversion-execution): the SHA-256 of a whole file binds a worker's
 * measurements to the file; it is plain (what sha256sum says), read without
 * moving the descriptor's offset, and refuses a file of another length. */
static void
test_whole_digest(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("axis-verify-XXXXXX", NULL);
	g_autofree gchar *path = scratch_file(dir, 9 * 1024 * 1024 + 5);
	g_autofree gchar *content = NULL;
	g_autofree gchar *hex = NULL;
	g_autofree gchar *expected = NULL;
	gsize length = 0;
	gint stop = 0;
	gint err = 0;
	gint fd;

	g_assert_true(g_file_get_contents(path, &content, &length, NULL));
	expected = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)content, length);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(lseek(fd, 1234, SEEK_SET), ==, 1234);
	g_assert_true(axis_file_whole_digest(fd, (gint64)length, NULL, &hex, &err));
	g_assert_cmpstr(hex, ==, expected);
	g_assert_cmpint(lseek(fd, 0, SEEK_CUR), ==, 1234);
	g_clear_pointer(&hex, g_free);
	/* Shorter or longer than it must be. */
	g_assert_false(axis_file_whole_digest(fd, (gint64)length + 1, NULL, &hex, &err));
	g_assert_cmpint(err, ==, EIO);
	g_assert_false(axis_file_whole_digest(fd, (gint64)length - 1, NULL, &hex, &err));
	g_assert_cmpint(err, ==, EIO);
	/* Stopped. */
	stop = 1;
	g_assert_false(axis_file_whole_digest(fd, (gint64)length, &stop, &hex, &err));
	g_assert_cmpint(err, ==, ECANCELED);
	g_assert_null(hex);
	close(fd);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* ── The measurements, for real ─────────────────────────────────── */

static gboolean
run(const gchar *const *argv)
{
	gint status = 0;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
	                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL))
		return FALSE;
	return g_spawn_check_wait_status(status, NULL);
}

static void
measure_all(
	const AxisVerifyTools *tools,
	const AxisVerifyPlan  *plan,
	gint                   src,
	gint                   out,
	AxisVerifyEvidence    *e
){
	guint k;

	axis_verify_measure_decode(tools, plan, out, e);
	axis_verify_measure_packets(tools, plan, src, out, e);
	axis_verify_measure_tracks(tools, plan, src, e);
	for (k = 0; k < plan->n_points; k++)
		axis_verify_measure_point(tools, plan, k, src, out, e);
}

/* Spec (conversion-execution): the measurements of a good output pass every
 * judge; those of a cut output fail one. */
static void
test_measure(void)
{
	g_autofree gchar *found = g_find_program_in_path("ffmpeg");
	g_autofree gchar *found_probe = g_find_program_in_path("ffprobe");
	g_autofree gchar *dir = NULL;
	g_autofree gchar *clip = NULL;
	g_autofree gchar *cut = NULL;
	g_autofree gchar *detail = NULL;
	g_autofree gchar *content = NULL;
	g_autoptr(AxisVerifyPlan) plan = NULL;
	g_autoptr(AxisVerifyEvidence) good = NULL;
	g_autoptr(AxisVerifyEvidence) bad = NULL;
	const gchar *programs[2];
	const gchar *probe_programs[2];
	AxisVerifyTools tools;
	gsize length = 0;
	guint64 packets = 0;
	gint src;
	gint out;
	guint k;

	/* Nothing is made before this. */
	if (found == NULL || found_probe == NULL) {
		g_test_skip("ffmpeg is not installed");
		return;
	}
	dir = g_dir_make_tmp("axis-verify-XXXXXX", NULL);
	clip = g_build_filename(dir, "clip.mkv", NULL);
	cut = g_build_filename(dir, "cut.mkv", NULL);
	{
		const gchar *argv[] = {
			"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=2",
			"-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a",
			"aac", "-b:a", "96k", clip, NULL
		};

		g_assert_true(run(argv));
	}
	g_assert_true(g_file_get_contents(clip, &content, &length, NULL));
	g_assert_true(g_file_set_contents(cut, content, (gssize)(length / 2), NULL));

	/* The picture 0 -> 0 and the AAC copied 1 -> 1, two points: the clip
	 * itself as its own "converted" file. */
	plan = axis_verify_plan_new();
	plan->picture_source = 0;
	plan->picture_output = 0;
	axis_verify_plan_add_track(plan, 1, 1, TRUE);
	plan->n_points = 2;
	plan->points[0].start_ms = 300;
	plan->points[0].length_ms = 500;
	plan->points[1].start_ms = 1200;
	plan->points[1].length_ms = 500;
	plan->timeout_ms = 120000;
	/* The runner runs a program by its path: it searches nothing. */
	programs[0] = found;
	programs[1] = NULL;
	probe_programs[0] = found_probe;
	probe_programs[1] = NULL;
	memset(&tools, 0, sizeof(tools));
	tools.ffmpeg = programs;
	tools.ffprobe = probe_programs;
	tools.scratch = dir;
	tools.threads = 1;

	src = open(clip, O_RDONLY | O_CLOEXEC);
	out = open(clip, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(src, >=, 0);
	g_assert_cmpint(out, >=, 0);
	good = axis_verify_evidence_new();
	measure_all(&tools, plan, src, out, good);
	g_assert_true(axis_verify_evidence_complete(good, plan));
	g_assert_nonnull(good->output_hashes);
	g_assert_nonnull(good->source_hashes);
	g_assert_true(axis_verify_judge_whole(good, &detail));
	g_clear_pointer(&detail, g_free);
	g_assert_true(axis_verify_judge_packets(good, &packets, &detail));
	g_assert_cmpuint(packets, ==, 48);
	g_assert_cmpstr(good->output_hashes[0], ==, good->source_hashes[0]);
	for (k = 0; k < 2; k++) {
		g_assert_true(good->points[k].read);
		g_assert_cmpfloat(good->points[k].lowest, >, 0.999);
	}
	g_assert_true(axis_verify_judge_picture(good, 2, 0.85, &detail));
	g_clear_pointer(&detail, g_free);
	close(out);

	/* Half of it: whatever the decode says, the packets are not all there. */
	out = open(cut, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(out, >=, 0);
	bad = axis_verify_evidence_new();
	measure_all(&tools, plan, src, out, bad);
	g_assert_false(axis_verify_judge_whole(bad, &detail) && axis_verify_judge_packets(bad, &packets, &detail));
	close(out);
	close(src);

	g_assert_cmpint(g_unlink(clip), ==, 0);
	g_assert_cmpint(g_unlink(cut), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/verify/plan", test_plan);
	g_test_add_func("/verify/commands", test_commands);
	g_test_add_func("/verify/judges", test_judges);
	g_test_add_func("/verify/fits", test_fits);
	g_test_add_func("/verify/protocol", test_protocol);
	g_test_add_func("/verify/whole-digest", test_whole_digest);
	g_test_add_func("/verify/measure", test_measure);
	return g_test_run();
}

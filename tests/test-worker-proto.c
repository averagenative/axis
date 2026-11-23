/*
 * test-worker-proto.c - the worker API's messages read strictly and
 * written exactly; the proof of a volume at a path; opening a file beneath
 * a proven directory, never through a link
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-storage.h"
#include "axis-version.h"
#include "api/axis-worker-proto.h"
#include "storage/axis-file-check.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-volume-marker.h"
#include "storage/axis-volume-proof.h"

#define UUID_A  "3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b"
#define UUID_B  "11111111-2222-4333-8444-555555555555"
#define TOKEN_A "0b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c"
#define DIGEST  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define SAMPLE  "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisEncoderCapabilities *
some_caps(void)
{
	AxisEncoderCapabilities *caps = axis_encoder_capabilities_new();

	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup(DIGEST);
	caps->measured_at_us = G_GINT64_CONSTANT(1731500000) * G_USEC_PER_SEC;
	caps->aac = TRUE;
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), TRUE, TRUE, TRUE, NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, FALSE, FALSE, NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("hevc_nvenc"), FALSE, FALSE, FALSE,
		"No NVENC capable devices found");
	return caps;
}

static AxisSampleDescription *
some_description(void)
{
	AxisSampleDescription *desc = axis_sample_description_new();
	AxisSampleFilePlan f;

	desc->media = AXIS_TARGET_VIDEO;
	desc->codec = AXIS_TCODEC_HEVC;
	desc->family = AXIS_ENCODER_SOFTWARE;
	desc->target_quality = 4;
	desc->excerpt_timeout_ms = 60000;
	memset(&f, 0, sizeof(f));
	f.volume = (gchar *)UUID_A;
	f.root = (gchar *)"Films";
	f.path = (gchar *)"Film (2001)/Film (2001).mkv";
	f.size = 123456;
	f.mtime_sec = 1700000000;
	f.mtime_nsec = 5;
	f.stream = 0;
	f.quality = 4;
	f.bit_depth = 10;
	f.n_excerpts = 3;
	f.start_ms[0] = 1000;
	f.start_ms[1] = 2000;
	f.start_ms[2] = 3000;
	f.length_ms = 2000;
	axis_sample_description_add_file(desc, &f);
	return desc;
}

static gchar *
claimed_with(const gchar *description)
{
	return g_strdup_printf("{\"protocol\":3,\"job\":{\"id\":7,\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"lease_token\":\"" TOKEN_A "\",\"lease_ms\":60000,\"heartbeat_ms\":15000,\"attempt\":1,"
		"\"description\":%s},\"paused\":null}", description);
}

/* A whole description with one file whose members are @file_members. */
static gchar *
description_with(const gchar *file)
{
	return g_strdup_printf("{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,"
		"\"audio_bitrate\":0,\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,"
		"\"excerpt_timeout_ms\":60000,\"files\":[%s]}", file);
}

static gchar *
file_with(
	const gchar *root,
	const gchar *path
){
	g_autoptr(GString) out = g_string_new(NULL);

	g_string_append(out, "{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":");
	g_string_append_printf(out, "\"%s\",\"path\":\"%s\"", root, path);
	g_string_append(out, ",\"size\":1,\"mtime_sec\":1,\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":8,"
		"\"channels\":0,\"sample_rate\":0,\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}");
	return g_strdup(out->str);
}

static gboolean
claimed_ok(const gchar *json)
{
	AxisWorkerJob *job = NULL;
	gchar *problem = NULL;
	gboolean ok = axis_worker_claimed_parse(json, strlen(json), &job, NULL, &problem);

	if (ok)
		g_assert_null(problem);
	else
		g_assert_nonnull(problem);
	axis_worker_job_free(job);
	g_free(problem);
	return ok;
}

/* ── Round trips ────────────────────────────────────────────────── */

static void
test_register_round_trip(void)
{
	g_autoptr(AxisWorkerRegister) request = axis_worker_register_new();
	g_autoptr(AxisWorkerRegister) back = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerVolumeSeen *seen = g_new0(AxisWorkerVolumeSeen, 1);
	const AxisEncoderCapability *cap;

	request->worker_version = g_strdup("0.1.0");
	request->max_jobs = 2;
	request->caps = some_caps();
	seen->uuid = g_strdup(UUID_A);
	seen->path = g_strdup("/media/disk1");
	seen->keeps_identity = TRUE;
	g_ptr_array_add(request->volumes, seen);
	seen = g_new0(AxisWorkerVolumeSeen, 1);
	seen->uuid = g_strdup(UUID_B);
	seen->path = g_strdup("/mnt/server/disk2");
	seen->keeps_identity = FALSE;
	g_ptr_array_add(request->volumes, seen);
	json = axis_worker_register_to_json(request);
	g_assert_nonnull(strstr(json, "\"protocol\":3"));
	back = axis_worker_register_parse(json, strlen(json), &problem);
	g_assert_null(problem);
	g_assert_nonnull(back);
	g_assert_cmpstr(back->worker_version, ==, "0.1.0");
	g_assert_cmpuint(back->max_jobs, ==, 2);
	g_assert_cmpstr(back->caps->tool_version, ==, "8.1.3");
	g_assert_cmpstr(back->caps->tool_digest, ==, DIGEST);
	cap = axis_encoder_capabilities_get(back->caps, axis_convert_encoder_named("libx265"));
	g_assert_true(cap->measured && cap->usable && cap->ten_bit && cap->hdr10);
	cap = axis_encoder_capabilities_get(back->caps, axis_convert_encoder_named("hevc_nvenc"));
	g_assert_true(cap->measured && !cap->usable);
	g_assert_cmpstr(cap->reason, ==, "No NVENC capable devices found");
	cap = axis_encoder_capabilities_get(back->caps, axis_convert_encoder_named("h264_vaapi"));
	g_assert_false(cap->measured);
	g_assert_cmpuint(back->volumes->len, ==, 2);
	g_assert_cmpstr(((AxisWorkerVolumeSeen *)g_ptr_array_index(back->volumes, 0))->path, ==, "/media/disk1");
	/* Whether each mount keeps file identities, as the worker said. */
	g_assert_true(((AxisWorkerVolumeSeen *)g_ptr_array_index(back->volumes, 0))->keeps_identity);
	g_assert_false(((AxisWorkerVolumeSeen *)g_ptr_array_index(back->volumes, 1))->keeps_identity);
}

static void
test_claim_round_trip(void)
{
	g_autoptr(AxisSampleDescription) desc = some_description();
	g_autoptr(AxisWorkerJob) back = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *nothing = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerJob job;
	const AxisSampleFilePlan *f;
	AxisWorkerJob *none = (AxisWorkerJob *)0x1;

	memset(&job, 0, sizeof(job));
	job.id = 42;
	job.kind = (gchar *)AXIS_WORKER_KIND_SAMPLE;
	job.lease_token = (gchar *)TOKEN_A;
	job.lease_ms = 60000;
	job.heartbeat_ms = 15000;
	job.attempt = 1;
	job.sample = desc;
	json = axis_worker_claimed_to_json(&job, NULL);
	g_assert_true(axis_worker_claimed_parse(json, strlen(json), &back, NULL, &problem));
	g_assert_null(problem);
	g_assert_cmpint(back->id, ==, 42);
	f = g_ptr_array_index(back->sample->files, 0);
	g_assert_cmpstr(f->root, ==, "Films");
	g_assert_cmpstr(f->path, ==, "Film (2001)/Film (2001).mkv");
	g_assert_cmpint(f->start_ms[2], ==, 3000);
	g_assert_cmpint(f->bit_depth, ==, 10);
	/* No absolute path in what a worker is told. */
	g_assert_null(strstr(json, "\"/"));

	nothing = axis_worker_claimed_to_json(NULL, NULL);
	g_assert_true(axis_worker_claimed_parse(nothing, strlen(nothing), &none, NULL, NULL));
	g_assert_null(none);
}

/* A conversion's description, as the daemon gives it. */
static AxisConvertWork *
some_work(void)
{
	static const gchar *const args[] = {
		"-nostdin", "-protocol_whitelist", "fd", "-fd", "3", "-i", "fd:", "-map", "0:0", "-c:0", "libx265", "-f",
		"matroska", "-protocol_whitelist", "fd", "-fd", "4", "fd:", NULL
	};
	AxisConvertWork *work = g_new0(AxisConvertWork, 1);

	work->volume = g_strdup(UUID_A);
	work->root = g_strdup("Movies");
	work->path = g_strdup("Movie (2001)/Movie (2001).mkv");
	work->size = 1000000;
	work->mtime_sec = 1700000000;
	work->mtime_nsec = 123456789;
	work->inode = 98765;
	work->source_sample = g_strdup(SAMPLE);
	work->output = g_strdup("7-2.tmp");
	work->args = g_strdupv((gchar **)args);
	work->source_streams = 2;
	work->duration_ms = 4000;
	work->timeout_ms = 600000;
	return work;
}

/* @json with the first @from replaced by @to. */
static gchar *
replaced(
	const gchar *json,
	const gchar *from,
	const gchar *to
){
	const gchar *at = strstr(json, from);

	g_assert_nonnull(at);
	return g_strdup_printf("%.*s%s%s", (gint)(at - json), json, to, at + strlen(from));
}

/* Spec (worker-protocol): a conversion's description and result, read
 * strictly: the work area's names only, paths relative to the volume, no
 * argument a shell or a terminal could take for something else. */
static void
test_convert_messages(void)
{
	static const gchar *const bad_outputs[] = {
		"\"../x.tmp\"", "\"01-1.tmp\"", "\"1-01.tmp\"", "\"1-1.tmp/x\"", "\"trial-1.mkv\"", "\"/etc/passwd\"",
		"\"1-1.mkv\"", "\"\"", NULL
	};
	g_autoptr(AxisConvertWork) work = some_work();
	g_autoptr(AxisWorkerJob) back = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerJob job;
	guint i;

	g_assert_true(axis_convert_work_check(work, NULL));
	memset(&job, 0, sizeof(job));
	job.id = 7;
	job.kind = (gchar *)AXIS_WORKER_KIND_CONVERT;
	job.lease_token = (gchar *)TOKEN_A;
	job.lease_ms = 60000;
	job.heartbeat_ms = 15000;
	job.attempt = 2;
	job.convert = work;
	json = axis_worker_claimed_to_json(&job, NULL);
	g_assert_true(axis_worker_claimed_parse(json, strlen(json), &back, NULL, &problem));
	g_assert_null(problem);
	g_assert_cmpstr(back->kind, ==, AXIS_WORKER_KIND_CONVERT);
	g_assert_cmpstr(back->convert->root, ==, "Movies");
	g_assert_cmpstr(back->convert->path, ==, "Movie (2001)/Movie (2001).mkv");
	g_assert_cmpint(back->convert->mtime_nsec, ==, 123456789);
	g_assert_cmpint(back->convert->inode, ==, 98765);
	g_assert_cmpstr(back->convert->source_sample, ==, SAMPLE);
	g_assert_cmpstr(back->convert->output, ==, "7-2.tmp");
	g_assert_cmpuint(g_strv_length(back->convert->args), ==, 18);
	g_assert_cmpstr(back->convert->args[10], ==, "libx265");
	g_assert_null(strstr(json, "\"/"));

	/* The output's name: the work area's form, nothing else. */
	for (i = 0; bad_outputs[i] != NULL; i++) {
		g_autofree gchar *bad = replaced(json, "\"7-2.tmp\"", bad_outputs[i]);

		g_assert_false(claimed_ok(bad));
	}
	/* Paths that leave the root, an absolute one, a control character in
	 * an argument, no arguments, an unknown member, a size of nothing. */
	{
		const gchar *const pairs[][2] = {
			{ "\"Movie (2001)/Movie (2001).mkv\"", "\"../Movie.mkv\"" },
			{ "\"Movie (2001)/Movie (2001).mkv\"", "\"/etc/passwd\"" },
			{ "\"Movies\"", "\"/srv/Movies\"" },
			{ "\"Movies\"", "\"Movies/../..\"" },
			{ "\"libx265\"", "\"libx265\\u0007\"" },
			{ "\"libx265\"", "\"lib\\nx265\"" },
			{ "\"source_streams\":2", "\"source_streams\":0" },
			{ "\"size\":1000000", "\"size\":0" },
			{ "\"timeout_ms\":600000", "\"timeout_ms\":1" },
			{ "\"duration_ms\":4000", "\"duration_ms\":4000,\"shell\":\"sh\"" },
			/* The digest of the source's samples: required, a SHA-256 in
			 * lower-case hexadecimal (protocol 1's description, without it, is
			 * not one this worker runs). */
			{ "\"source_sample\":\"" SAMPLE "\",", "" },
			{ "\"" SAMPLE "\"", "\"FEDCBA9876543210fedcba9876543210fedcba9876543210fedcba9876543210\"" },
			{ "\"" SAMPLE "\"", "\"fedcba\"" },
			{ "\"" SAMPLE "\"", "null" },
			{ "\"" SAMPLE "\"", "\"" SAMPLE "00\"" }
		};

		for (i = 0; i < G_N_ELEMENTS(pairs); i++) {
			g_autofree gchar *bad = replaced(json, pairs[i][0], pairs[i][1]);

			if (claimed_ok(bad))
				g_error("accepted: %s", bad);
		}
	}
	{
		const gchar *start = strstr(json, "\"args\":[");
		const gchar *end = start != NULL ? strchr(start, ']') : NULL;
		g_autofree gchar *none = NULL;

		g_assert_nonnull(end);
		none = g_strdup_printf("%.*s\"args\":[%s", (gint)(start - json), json, end);
		g_assert_false(claimed_ok(none));
	}

	/* The result: the name and a size. */
	{
		AxisConvertResult result;
		AxisWorkerComplete complete;
		g_autofree gchar *done = NULL;
		g_autoptr(AxisWorkerComplete) parsed = NULL;

		result.output = (gchar *)"7-2.tmp";
		result.size = 4242;
		memset(&complete, 0, sizeof(complete));
		complete.job = 7;
		complete.lease_token = (gchar *)TOKEN_A;
		complete.kind = (gchar *)AXIS_WORKER_KIND_CONVERT;
		complete.convert = &result;
		done = axis_worker_complete_to_json(&complete);
		parsed = axis_worker_complete_parse(done, strlen(done), NULL);
		g_assert_nonnull(parsed);
		g_assert_cmpstr(parsed->convert->output, ==, "7-2.tmp");
		g_assert_cmpint(parsed->convert->size, ==, 4242);
		for (i = 0; bad_outputs[i] != NULL; i++) {
			g_autofree gchar *bad = replaced(done, "\"7-2.tmp\"", bad_outputs[i]);
			AxisWorkerComplete *refused = axis_worker_complete_parse(bad, strlen(bad), NULL);

			g_assert_null(refused);
		}
		{
			g_autofree gchar *empty = replaced(done, "4242", "0");

			g_assert_null(axis_worker_complete_parse(empty, strlen(empty), NULL));
		}
	}
}

static void
test_heartbeat_and_results(void)
{
	AxisWorkerHeartbeat beat;
	AxisWorkerBeat answer;
	AxisWorkerComplete complete;
	AxisWorkerFail fail;
	AxisWorkerCompleted completed;
	g_autoptr(AxisWorkerHeartbeat) beat_back = NULL;
	g_autoptr(AxisWorkerBeat) answer_back = NULL;
	g_autoptr(AxisWorkerComplete) complete_back = NULL;
	g_autoptr(AxisWorkerFail) fail_back = NULL;
	g_autoptr(AxisSampleResult) result = axis_sample_result_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *e = NULL;
	g_autofree gchar *state = NULL;
	guint64 bytes[3] = { 1000, 2000, 3000 };

	memset(&beat, 0, sizeof(beat));
	beat.job = 5;
	beat.lease_token = (gchar *)TOKEN_A;
	beat.has_progress = TRUE;
	beat.progress = 0.25;
	beat.message = (gchar *)"file 1 of 2\nexcerpt 1";
	a = axis_worker_heartbeat_to_json(&beat);
	beat_back = axis_worker_heartbeat_parse(a, strlen(a), NULL);
	g_assert_nonnull(beat_back);
	g_assert_true(beat_back->has_progress);
	g_assert_cmpfloat_with_epsilon(beat_back->progress, 0.25, 1e-9);
	/* Free text comes back cleaned: no control character. */
	g_assert_null(strchr(beat_back->message, '\n'));

	memset(&answer, 0, sizeof(answer));
	answer.lease_ms = 60000;
	b = axis_worker_beat_to_json(&answer);
	answer_back = axis_worker_beat_parse(b, strlen(b), NULL);
	g_assert_nonnull(answer_back);
	g_assert_false(answer_back->stop);
	g_assert_cmpuint(answer_back->lease_ms, ==, 60000);

	axis_sample_result_add(result, 0, bytes, 3, NULL);
	axis_sample_result_add(result, 1, NULL, 0, AXIS_SAMPLE_SKIP_CHANGED);
	memset(&complete, 0, sizeof(complete));
	complete.job = 5;
	complete.lease_token = (gchar *)TOKEN_A;
	complete.kind = (gchar *)AXIS_WORKER_KIND_SAMPLE;
	complete.sample = result;
	c = axis_worker_complete_to_json(&complete);
	complete_back = axis_worker_complete_parse(c, strlen(c), NULL);
	g_assert_nonnull(complete_back);
	g_assert_cmpuint(complete_back->sample->files->len, ==, 2);
	g_assert_cmpuint(((AxisSampleFileResult *)g_ptr_array_index(complete_back->sample->files, 0))->bytes[2], ==, 3000);
	g_assert_cmpstr(((AxisSampleFileResult *)g_ptr_array_index(complete_back->sample->files, 1))->reason, ==, "changed");

	memset(&fail, 0, sizeof(fail));
	fail.job = 5;
	fail.lease_token = (gchar *)TOKEN_A;
	fail.error = (gchar *)"the worker is stopping";
	fail.stopping = TRUE;
	d = axis_worker_fail_to_json(&fail);
	fail_back = axis_worker_fail_parse(d, strlen(d), NULL);
	g_assert_nonnull(fail_back);
	g_assert_true(fail_back->stopping && fail_back->retry);

	completed.recorded = 2;
	completed.refused = 1;
	e = axis_worker_completed_to_json(&completed);
	memset(&completed, 0, sizeof(completed));
	g_assert_true(axis_worker_completed_parse(e, strlen(e), &completed, NULL));
	g_assert_cmpuint(completed.recorded, ==, 2);
	g_assert_cmpuint(completed.refused, ==, 1);
	g_free(e);
	e = axis_worker_failed_to_json("queued");
	g_assert_true(axis_worker_failed_parse(e, strlen(e), &state, NULL));
	g_assert_cmpstr(state, ==, "queued");
}

/* ── Strictness ─────────────────────────────────────────────────── */

static void
test_unknown_and_repeated_members(void)
{
	static const gchar *const refused[] = {
		/* A member it does not know. */
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\",\"extra\":1}",
		/* A member twice. */
		"{\"protocol\":3,\"job\":5,\"job\":6,\"lease_token\":\"" TOKEN_A "\"}",
		/* Another protocol: an older and a newer one. */
		"{\"protocol\":1,\"job\":5,\"lease_token\":\"" TOKEN_A "\"}",
		"{\"protocol\":" "2,\"job\":5,\"lease_token\":\"" TOKEN_A "\"}",
		"{\"protocol\":4,\"job\":5,\"lease_token\":\"" TOKEN_A "\"}",
		/* No protocol. */
		"{\"job\":5,\"lease_token\":\"" TOKEN_A "\"}",
		/* A number in a string is not a number. */
		"{\"protocol\":3,\"job\":\"5\",\"lease_token\":\"" TOKEN_A "\"}",
		/* Out of range, fractional, negative. */
		"{\"protocol\":3,\"job\":0,\"lease_token\":\"" TOKEN_A "\"}",
		"{\"protocol\":3,\"job\":5.5,\"lease_token\":\"" TOKEN_A "\"}",
		"{\"protocol\":3,\"job\":9007199254740992,\"lease_token\":\"" TOKEN_A "\"}",
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\",\"progress\":1.5}",
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\",\"progress\":\"0.5\"}",
		/* A token that is not one. */
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"3F2A5C1E-7B9D-4E2F-8A6B-1C3D5E7F9A0B\"}",
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"00000000-0000-0000-0000-000000000000\"}",
		"{\"protocol\":3,\"job\":5,\"lease_token\":5}",
		/* Not an object, not JSON, trailing text, empty. */
		"[1,2]",
		"{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\"} x",
		"{\"protocol\":3,",
		"",
		NULL
	};
	guint i;

	for (i = 0; refused[i] != NULL; i++) {
		gchar *problem = NULL;
		AxisWorkerHeartbeat *beat = axis_worker_heartbeat_parse(refused[i], strlen(refused[i]), &problem);

		if (beat != NULL)
			g_error("accepted: %s", refused[i]);
		g_assert_nonnull(problem);
		/* A refusal never quotes the input. */
		g_assert_null(strstr(problem, TOKEN_A));
		g_free(problem);
	}
}

static void
test_text_bounds(void)
{
	g_autofree gchar *long_message = g_strnfill(201, 'x');
	g_autofree gchar *fits = g_strnfill(200, 'x');
	g_autofree gchar *too_long = g_strdup_printf("{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\","
		"\"message\":\"%s\"}", long_message);
	g_autofree gchar *ok = g_strdup_printf("{\"protocol\":3,\"job\":5,\"lease_token\":\"" TOKEN_A "\","
		"\"message\":\"%s\"}", fits);
	g_autofree gchar *huge = g_strnfill(AXIS_WORKER_MAX_REQUEST_BYTES + 1, ' ');
	g_autoptr(AxisWorkerHeartbeat) accepted = NULL;
	AxisWorkerHeartbeat *refused;
	gchar *problem = NULL;

	refused = axis_worker_heartbeat_parse(too_long, strlen(too_long), NULL);
	g_assert_null(refused);
	accepted = axis_worker_heartbeat_parse(ok, strlen(ok), NULL);
	g_assert_nonnull(accepted);
	refused = axis_worker_heartbeat_parse(huge, strlen(huge), &problem);
	g_assert_null(refused);
	g_assert_nonnull(strstr(problem, "larger"));
	g_free(problem);
}

static void
test_register_refusals(void)
{
	static const gchar *const head = "{\"protocol\":3,\"worker_version\":\"0.1.0\",\"max_jobs\":1,"
		"\"ffmpeg_version\":\"8.1.3\",\"ffmpeg_digest\":\"" DIGEST "\",";
	static const gchar *const caps = "\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,"
		"\"subrip\":false,\"encoders\":[]},";
#define KEEPS ",\"keeps_file_identity\":true"
	static const gchar *const refused[] = {
		/* A relative, unnormalised or dotted path. */
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"media/disk1\"" KEEPS "}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/media//disk1\"" KEEPS "}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/media/../etc\"" KEEPS "}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/media/disk1/\"" KEEPS "}]}",
		/* A volume twice, a UUID that is not one. */
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\"" KEEPS "},{\"uuid\":\"" UUID_A "\",\"path\":\"/b\""
		KEEPS "}]}",
		"\"volumes\":[{\"uuid\":\"not-a-uuid\",\"path\":\"/a\"" KEEPS "}]}",
		/* Whether the mount keeps file identities: required (a protocol 1
		 * register has no such member), true or false and nothing else. */
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\"}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\",\"keeps_file_identity\":\"no\"}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\",\"keeps_file_identity\":0}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\",\"keeps_file_identity\":null}]}",
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\"" KEEPS KEEPS "}]}",
		NULL
	};
	static const gchar *const bad_caps[] = {
		/* 10 bits without usable; HDR10 without 10 bits; 10 bits of H.264. */
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libx265\",\"measured\":true,\"usable\":false,\"ten_bit\":true,\"hdr10\":false,\"reason\":null}]},",
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libx265\",\"measured\":true,\"usable\":true,\"ten_bit\":false,\"hdr10\":true,\"reason\":null}]},",
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libx264\",\"measured\":true,\"usable\":true,\"ten_bit\":true,\"hdr10\":false,\"reason\":null}]},",
		/* Usable without a measurement; an encoder not in the table; one twice. */
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libx265\",\"measured\":false,\"usable\":true,\"ten_bit\":false,\"hdr10\":false,\"reason\":null}]},",
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libaom\",\"measured\":true,\"usable\":true,\"ten_bit\":false,\"hdr10\":false,\"reason\":null}]},",
		"\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[{"
		"\"name\":\"libx264\",\"measured\":true,\"usable\":true,\"ten_bit\":false,\"hdr10\":false,\"reason\":null},{"
		"\"name\":\"libx264\",\"measured\":true,\"usable\":true,\"ten_bit\":false,\"hdr10\":false,\"reason\":null}]},",
		NULL
	};
	g_autofree gchar *good = g_strconcat(head, caps, "\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\"" KEEPS
		"}]}", NULL);
	g_autofree gchar *loose = g_strconcat(head, caps, "\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/a\","
		"\"keeps_file_identity\":false}]}", NULL);
	g_autoptr(AxisWorkerRegister) accepted = axis_worker_register_parse(good, strlen(good), NULL);
	g_autoptr(AxisWorkerRegister) without = axis_worker_register_parse(loose, strlen(loose), NULL);
	guint i;
#undef KEEPS

	g_assert_nonnull(accepted);
	g_assert_true(((AxisWorkerVolumeSeen *)g_ptr_array_index(accepted->volumes, 0))->keeps_identity);
	g_assert_nonnull(without);
	g_assert_false(((AxisWorkerVolumeSeen *)g_ptr_array_index(without->volumes, 0))->keeps_identity);
	for (i = 0; refused[i] != NULL; i++) {
		g_autofree gchar *json = g_strconcat(head, caps, refused[i], NULL);
		AxisWorkerRegister *r = axis_worker_register_parse(json, strlen(json), NULL);

		if (r != NULL)
			g_error("accepted volumes: %s", refused[i]);
	}
	for (i = 0; bad_caps[i] != NULL; i++) {
		g_autofree gchar *json = g_strconcat(head, bad_caps[i], "\"volumes\":[]}", NULL);
		AxisWorkerRegister *r = axis_worker_register_parse(json, strlen(json), NULL);

		if (r != NULL)
			g_error("accepted capabilities: %s", bad_caps[i]);
	}
}

/* Spec: Escaping path: a parent reference, an absolute or over-long path
 * in a job description is refused by the worker's reader. */
static void
test_description_paths(void)
{
	g_autofree gchar *long_part = g_strnfill(256, 'a');
	g_autofree gchar *deep = NULL;
	const gchar *const refused[][2] = {
		{ "Films", "../outside.mkv" },
		{ "Films", "a/../../outside.mkv" },
		{ "Films", "./film.mkv" },
		{ "Films", "/etc/passwd" },
		{ "Films", "a//film.mkv" },
		{ "Films", "film.mkv/" },
		{ "Films", "" },
		{ "/media/disk1/Films", "film.mkv" },
		{ "..", "film.mkv" },
		{ "Films/..", "film.mkv" },
		{ "Films/", "film.mkv" },
		{ "Films", long_part },
		{ NULL, NULL }
	};
	guint i;

	{
		g_autofree gchar *file = file_with("Films", "Film (2001)/Film (2001).mkv");
		g_autofree gchar *description = description_with(file);
		g_autofree gchar *json = claimed_with(description);

		g_assert_true(claimed_ok(json));
	}
	{
		/* The volume's top as the root. */
		g_autofree gchar *file = file_with("", "film.mkv");
		g_autofree gchar *description = description_with(file);
		g_autofree gchar *json = claimed_with(description);

		g_assert_true(claimed_ok(json));
	}
	for (i = 0; refused[i][0] != NULL; i++) {
		g_autofree gchar *file = file_with(refused[i][0], refused[i][1]);
		g_autofree gchar *description = description_with(file);
		g_autofree gchar *json = claimed_with(description);

		if (claimed_ok(json))
			g_error("accepted root '%s' path '%s'", refused[i][0], refused[i][1]);
	}
	/* Deeper than a stored path can be. */
	{
		GString *path = g_string_new(NULL);
		g_autofree gchar *file = NULL;
		g_autofree gchar *description = NULL;
		g_autofree gchar *json = NULL;

		for (i = 0; i < 130; i++)
			g_string_append(path, "d/");
		g_string_append(path, "f.mkv");
		deep = g_string_free(path, FALSE);
		file = file_with("Films", deep);
		description = description_with(file);
		json = claimed_with(description);
		g_assert_false(claimed_ok(json));
	}
}

static void
test_description_consistency(void)
{
	static const gchar *const refused[] = {
		/* A picture's depth that HEVC is not encoded to; audio with a depth. */
		"{\"media\":\"video\",\"codec\":\"h264\",\"family\":\"software\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,\"files\":["
		"{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"F\",\"path\":\"f.mkv\",\"size\":1,\"mtime_sec\":1,"
		"\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":10,\"channels\":0,\"sample_rate\":0,"
		"\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}]}",
		/* A codec of the other media. */
		"{\"media\":\"audio\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,\"files\":["
		"{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"F\",\"path\":\"f.flac\",\"size\":1,\"mtime_sec\":1,"
		"\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":0,\"channels\":2,\"sample_rate\":44100,"
		"\"sample_bits\":16,\"length_ms\":2000,\"starts_ms\":[0]}]}",
		/* An index that is not its place; four excerpts; no file. */
		"{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,\"files\":["
		"{\"index\":3,\"volume\":\"" UUID_A "\",\"root\":\"F\",\"path\":\"f.mkv\",\"size\":1,\"mtime_sec\":1,"
		"\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,"
		"\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}]}",
		"{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,\"files\":["
		"{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"F\",\"path\":\"f.mkv\",\"size\":1,\"mtime_sec\":1,"
		"\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,"
		"\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0,1,2,3]}]}",
		"{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,"
		"\"files\":[]}",
		/* An encoder family that does not exist. */
		"{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"quantum\",\"target_quality\":4,\"audio_bitrate\":0,"
		"\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,\"excerpt_timeout_ms\":60000,\"files\":["
		"{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"F\",\"path\":\"f.mkv\",\"size\":1,\"mtime_sec\":1,"
		"\"mtime_nsec\":0,\"stream\":0,\"quality\":4,\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,"
		"\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}]}",
		NULL
	};
	guint i;

	for (i = 0; refused[i] != NULL; i++) {
		g_autofree gchar *json = claimed_with(refused[i]);

		if (claimed_ok(json))
			g_error("accepted: %s", refused[i]);
	}
}

static void
test_answers_refused(void)
{
	static const gchar *const beats[] = {
		/* Stop with a lease; continue with a reason; another action. */
		"{\"protocol\":3,\"action\":\"stop\",\"reason\":\"cancelled\",\"lease_ms\":5000}",
		"{\"protocol\":3,\"action\":\"continue\",\"lease_ms\":5000,\"reason\":\"lease\"}",
		"{\"protocol\":3,\"action\":\"pause\"}",
		"{\"protocol\":3,\"action\":\"continue\",\"lease_ms\":10}",
		/* An answer of protocol 1 (an older daemon), right otherwise. */
		"{\"protocol\":1,\"action\":\"continue\",\"lease_ms\":5000}",
		NULL
	};
	static const gchar *const registered[] = {
		/* A heartbeat longer than a third of the lease, a name that is not one. */
		"{\"protocol\":3,\"worker\":\"w\",\"lease_ms\":3000,\"heartbeat_ms\":1500,\"poll_ms\":1000,\"volumes\":[]}",
		"{\"protocol\":3,\"worker\":\"W 1\",\"lease_ms\":3000,\"heartbeat_ms\":750,\"poll_ms\":1000,\"volumes\":[]}",
		/* Protocol 1's answer, right otherwise. */
		"{\"protocol\":1,\"worker\":\"w\",\"lease_ms\":3000,\"heartbeat_ms\":750,\"poll_ms\":1000,\"volumes\":[]}",
		NULL
	};
	guint i;

	for (i = 0; beats[i] != NULL; i++) {
		AxisWorkerBeat *beat = axis_worker_beat_parse(beats[i], strlen(beats[i]), NULL);

		if (beat != NULL)
			g_error("accepted: %s", beats[i]);
	}
	for (i = 0; registered[i] != NULL; i++) {
		AxisWorkerRegistered *r = axis_worker_registered_parse(registered[i], strlen(registered[i]), NULL);

		if (r != NULL)
			g_error("accepted: %s", registered[i]);
	}
}

static void
test_results_refused(void)
{
	static const gchar *const refused[] = {
		/* A file twice, a skip reason that is not one, both bytes and skipped. */
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[1]},{\"index\":0,\"bytes\":[2]}]}}",
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"skipped\":\"bored\"}]}}",
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[1],\"skipped\":\"changed\"}]}}",
		/* A count past the cap; a negative one; four excerpts. */
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[68719476737]}]}}",
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[-1]}]}}",
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[1,2,3,4]}]}}",
		/* A kind the protocol does not have. */
		"{\"protocol\":3,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"transcode.file\",\"result\":{}}",
		/* Protocol 1's report of a good result (an older worker). */
		"{\"protocol\":1,\"job\":1,\"lease_token\":\"" TOKEN_A "\",\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\","
		"\"result\":{\"files\":[{\"index\":0,\"bytes\":[1]}]}}",
		NULL
	};
	guint i;

	for (i = 0; refused[i] != NULL; i++) {
		AxisWorkerComplete *c = axis_worker_complete_parse(refused[i], strlen(refused[i]), NULL);

		if (c != NULL)
			g_error("accepted: %s", refused[i]);
	}
}

/* ── Protocol 2: the pause, the versions ────────────────────────── */

/* Spec (worker-protocol): a worker whose encoder crashed says so in its
 * claim; the daemon's answer says whether it holds the worker paused, and
 * never gives work with that. */
static void
test_claim_pause(void)
{
	static const gchar *const kinds[] = { AXIS_WORKER_KIND_CONVERT, NULL };
	static const gchar *const volumes[] = { UUID_A, NULL };
	static const gchar *const refused[] = {
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":{\"job\":0,\"reason\":\"x\"}}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":{\"job\":7,\"reason\":\"\"}}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":{\"job\":7}}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":{\"job\":7,\"reason\":5}}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":{\"job\":7,\"reason\":\"x\","
		"\"more\":1}}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":true}",
		"{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],\"paused\":null}",
		NULL
	};
	g_autofree gchar *long_reason = g_strnfill(AXIS_WORKER_PAUSE_MAX_CHARS + 1, 'r');
	g_autofree gchar *too_long = g_strdup_printf("{\"protocol\":3,\"kinds\":[\"convert.encode\"],\"volumes\":[],"
		"\"paused\":{\"job\":7,\"reason\":\"%s\"}}", long_reason);
	AxisWorkerClaim request;
	guint i;

	/* Not paused: no member at all, as before. */
	memset(&request, 0, sizeof(request));
	request.kinds = (gchar **)kinds;
	request.volumes = (gchar **)volumes;
	{
		g_autofree gchar *json = axis_worker_claim_to_json(&request);
		g_autoptr(AxisWorkerClaim) back = axis_worker_claim_parse(json, strlen(json), NULL);

		g_assert_null(strstr(json, "paused"));
		g_assert_nonnull(back);
		g_assert_cmpint(back->paused_job, ==, 0);
		g_assert_null(back->paused_reason);
	}
	/* Paused: the job and what happened, cleaned. */
	request.paused_job = 77;
	request.paused_reason = (gchar *)"signal 11,\nSIGSEGV";
	{
		g_autofree gchar *json = axis_worker_claim_to_json(&request);
		g_autoptr(AxisWorkerClaim) back = axis_worker_claim_parse(json, strlen(json), NULL);

		g_assert_nonnull(back);
		g_assert_cmpint(back->paused_job, ==, 77);
		g_assert_nonnull(back->paused_reason);
		g_assert_null(strchr(back->paused_reason, '\n'));
		g_assert_nonnull(strstr(back->paused_reason, "SIGSEGV"));
	}
	for (i = 0; refused[i] != NULL; i++) {
		AxisWorkerClaim *c = axis_worker_claim_parse(refused[i], strlen(refused[i]), NULL);

		if (c != NULL)
			g_error("accepted: %s", refused[i]);
	}
	g_assert_null(axis_worker_claim_parse(too_long, strlen(too_long), NULL));

	/* The answer: "paused" is required, null or a sentence; with a sentence,
	 * no job, whatever the writer was given. */
	{
		g_autoptr(AxisConvertWork) work = some_work();
		g_autofree gchar *held = NULL;
		g_autofree gchar *free_answer = NULL;
		g_autofree gchar *paused = NULL;
		g_autofree gchar *none_paused = NULL;
		AxisWorkerJob job;
		AxisWorkerJob *back = NULL;

		memset(&job, 0, sizeof(job));
		job.id = 7;
		job.kind = (gchar *)AXIS_WORKER_KIND_CONVERT;
		job.lease_token = (gchar *)TOKEN_A;
		job.lease_ms = 60000;
		job.heartbeat_ms = 15000;
		job.attempt = 1;
		job.convert = work;
		held = axis_worker_claimed_to_json(&job, "Its encoder crashed on job 77 (signal 11, SIGSEGV).");
		g_assert_true(axis_worker_claimed_parse(held, strlen(held), &back, &paused, NULL));
		g_assert_null(back);
		g_assert_nonnull(strstr(paused, "crashed on job 77"));
		free_answer = axis_worker_claimed_to_json(NULL, NULL);
		g_assert_nonnull(strstr(free_answer, "\"paused\":null"));
		g_assert_true(axis_worker_claimed_parse(free_answer, strlen(free_answer), &back, &none_paused, NULL));
		g_assert_null(back);
		g_assert_null(none_paused);
	}
	{
		const gchar *const answers[] = {
			/* Work given to a worker it says is paused. */
			"{\"protocol\":3,\"job\":{\"id\":7,\"kind\":\"worker.capabilities\",\"lease_token\":\"" TOKEN_A "\","
			"\"lease_ms\":60000,\"heartbeat_ms\":15000,\"attempt\":1,\"description\":{}},\"paused\":\"held\"}",
			/* No "paused" (protocol 1's answer, under protocol 2). */
			"{\"protocol\":3,\"job\":null}",
			/* Protocol 1's answer as it was. */
			"{\"protocol\":1,\"job\":null}",
			/* Not a sentence. */
			"{\"protocol\":3,\"job\":null,\"paused\":true}",
			"{\"protocol\":3,\"job\":null,\"paused\":\"\"}",
			NULL
		};

		for (i = 0; answers[i] != NULL; i++) {
			if (claimed_ok(answers[i]))
				g_error("accepted: %s", answers[i]);
		}
	}
}

/* Spec (worker-protocol): a message of another version is refused, and the
 * refusal is one the other side can read. */
static void
test_versions(void)
{
	g_autofree gchar *mine = axis_worker_error_to_json("lease", "the job is not held");
	g_autofree gchar *for_old = axis_worker_error_to_json_version(1, "malformed", "old");
	g_autofree gchar *for_two = axis_worker_error_to_json_version(2, "malformed", "two");
	g_autofree gchar *for_new = axis_worker_error_to_json_version(4, "malformed", "new");
	g_autofree gchar *older = axis_worker_protocol_refusal(1);
	g_autofree gchar *two = axis_worker_protocol_refusal(2);
	g_autofree gchar *newer = axis_worker_protocol_refusal(4);
	gchar *word = NULL;
	gchar *message = NULL;
	guint version = 0;

	/* An error answer of this version, and of protocols 1 and 2 (an older
	 * daemon's, whose message is shown); a newer one is not read. */
	g_assert_nonnull(strstr(mine, "\"protocol\":3"));
	g_assert_true(axis_worker_error_parse(mine, strlen(mine), &word, &message, &version, NULL));
	g_assert_cmpstr(word, ==, "lease");
	g_assert_cmpuint(version, ==, 3);
	g_clear_pointer(&word, g_free);
	g_clear_pointer(&message, g_free);
	g_assert_nonnull(strstr(for_old, "\"protocol\":1"));
	g_assert_true(axis_worker_error_parse(for_old, strlen(for_old), &word, &message, &version, NULL));
	g_assert_cmpuint(version, ==, 1);
	g_assert_cmpstr(message, ==, "old");
	g_clear_pointer(&word, g_free);
	g_clear_pointer(&message, g_free);
	g_assert_nonnull(strstr(for_two, "\"protocol\":" "2"));
	g_assert_true(axis_worker_error_parse(for_two, strlen(for_two), &word, &message, &version, NULL));
	g_assert_cmpuint(version, ==, 2);
	g_assert_cmpstr(message, ==, "two");
	g_clear_pointer(&word, g_free);
	g_clear_pointer(&message, g_free);
	{
		const gchar *old_daemon = "{\"protocol\":" "2,\"error\":\"malformed\",\"message\":\"the message is of another "
			"protocol version\"}";
		const gchar *newer_daemon = "{\"protocol\":4,\"error\":\"malformed\",\"message\":\"x\"}";

		g_assert_true(axis_worker_error_parse(old_daemon, strlen(old_daemon), &word, &message, &version, NULL));
		g_assert_cmpuint(version, ==, 2);
		g_clear_pointer(&word, g_free);
		g_clear_pointer(&message, g_free);
		g_assert_false(axis_worker_error_parse(newer_daemon, strlen(newer_daemon), &word, &message, &version, NULL));
	}
	/* A version this side cannot write is written in its own. */
	g_assert_nonnull(strstr(for_new, "\"protocol\":3"));

	/* The version a request says it is in, read before anything else. */
#define PEEK(text) axis_worker_message_protocol(text, strlen(text), &version)
	g_assert_true(PEEK("{\"protocol\":1,\"anything\":[]}"));
	g_assert_cmpuint(version, ==, 1);
	g_assert_true(PEEK("{\"kinds\":[],\"protocol\":" "2}"));
	g_assert_cmpuint(version, ==, 2);
	g_assert_false(PEEK("{\"kinds\":[]}"));
	g_assert_false(PEEK("{\"protocol\":\"1\"}"));
	g_assert_false(PEEK("{\"protocol\":1001}"));
	g_assert_false(PEEK("[1]"));
	g_assert_false(PEEK("{\"protocol\":1"));
	g_assert_false(PEEK(""));
#undef PEEK

	/* The refusals name both versions and what to do. */
	g_assert_nonnull(strstr(older, "protocol 3 and this axis-worker 1"));
	g_assert_nonnull(strstr(older, "axis-worker of the daemon's version"));
	g_assert_nonnull(strstr(two, "this daemon speaks worker protocol 3 and this axis-worker 2"));
	g_assert_nonnull(strstr(two, "axis-worker of the daemon's version"));
	g_assert_nonnull(strstr(newer, "protocol 3 and this axis-worker 4"));
	g_assert_nonnull(strstr(newer, "daemon is older"));
	g_assert_cmpint(g_utf8_strlen(older, -1), <=, AXIS_WORKER_ANSWER_MAX_CHARS);
	g_assert_cmpint(g_utf8_strlen(newer, -1), <=, AXIS_WORKER_ANSWER_MAX_CHARS);

	/* The kept measurement keeps its own format number: a worker of
	 * protocol 3 reads what one of protocol 1 kept, and nothing else. */
	{
		g_autoptr(AxisEncoderCapabilities) caps = some_caps();
		g_autofree gchar *kept = axis_worker_tool_to_json(caps);
		g_autoptr(AxisEncoderCapabilities) back = NULL;
		g_autofree gchar *other = NULL;

		g_assert_nonnull(strstr(kept, "\"protocol\":1"));
		back = axis_worker_tool_parse(kept, strlen(kept), NULL);
		g_assert_nonnull(back);
		other = replaced(kept, "\"protocol\":1", "\"protocol\":" "2");
		g_assert_null(axis_worker_tool_parse(other, strlen(other), NULL));
	}
}

/* ── A file's stamp and samples ─────────────────────────────────── */

static struct stat
stat_of(
	gint64 size,
	gint64 sec,
	glong  nsec,
	guint64 ino,
	guint  links
){
	struct stat st;

	memset(&st, 0, sizeof(st));
	st.st_size = (off_t)size;
	st.st_mtim.tv_sec = (time_t)sec;
	st.st_mtim.tv_nsec = nsec;
	st.st_ino = (ino_t)ino;
	st.st_nlink = (nlink_t)links;
	return st;
}

/* Spec (conversion-execution): with identities kept everything is compared;
 * through a mount that does not keep them, the size, the time to the second
 * and the links; what sshfs shows passes only the latter. */
static void
test_file_stamp(void)
{
	AxisFileStamp want;
	struct stat st;

	memset(&want, 0, sizeof(want));
	want.size = 4000000000;
	want.mtime_sec = 1700000000;
	want.mtime_nsec = 123456789;
	want.inode = 987654;
	want.one_link = TRUE;

	st = stat_of(4000000000, 1700000000, 123456789, 987654, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SAME);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SAME);
	/* What sshfs shows: a made-up inode, whole seconds, one link. */
	st = stat_of(4000000000, 1700000000, 0, 7, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SUBSECOND);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SAME);
	/* SMB: tenths of a microsecond, the server's file index. */
	st = stat_of(4000000000, 1700000000, 123456700, 281474976710656 + 987654, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SUBSECOND);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SAME);
	/* Only the inode differs. */
	st = stat_of(4000000000, 1700000000, 123456789, 7, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_INODE);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SAME);
	/* What no mount excuses: the size, the seconds, another link. */
	st = stat_of(4000000001, 1700000000, 0, 7, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SIZE);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SIZE);
	st = stat_of(4000000000, 1700000001, 0, 7, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_TIME);
	st = stat_of(4000000000, 1699999999, 123456789, 987654, 1);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_TIME);
	st = stat_of(4000000000, 1700000000, 0, 7, 2);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_LINKS);
	st = stat_of(4000000000, 1700000000, 123456789, 987654, 2);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_LINKS);
	/* Links and the inode only when asked (the sample measurement). */
	want.one_link = FALSE;
	want.inode = -1;
	st = stat_of(4000000000, 1700000000, 123456789, 7, 3);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SAME);
	st = stat_of(4000000000, 1700000000, 5, 7, 3);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, TRUE), ==, AXIS_FILE_STAMP_SUBSECOND);
	g_assert_cmpint(axis_file_stamp_compare(&st, &want, FALSE), ==, AXIS_FILE_STAMP_SAME);
	g_assert_cmpstr(axis_file_stamp_describe(AXIS_FILE_STAMP_SUBSECOND), ==,
		"its modification time differs below a second");
}

/* The digest worked out apart from the code under test: the label, the size,
 * then the bytes at the offsets the scheme names. */
static gchar *
digest_by_hand(
	const guchar *data,
	gint64        size
){
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_autofree gchar *head = g_strdup_printf("axis-file-sample-1\n%" G_GINT64_FORMAT "\n", size);
	const gint64 block = 1024 * 1024;
	guint k;

	g_checksum_update(sum, (const guchar *)head, (gssize)strlen(head));
	if (size <= 8 * block) {
		g_checksum_update(sum, data, (gssize)size);
	} else {
		for (k = 0; k < 8; k++)
			g_checksum_update(sum, data + (size - block) * k / 7, block);
	}
	return g_strdup(g_checksum_get_string(sum));
}

/* Spec (conversion-execution): the samples are the first and last blocks
 * and six between them (the whole file when small); a change in a sampled
 * block changes the digest; a file shorter than its stamp is not read. */
static void
test_file_samples(void)
{
	gchar *dir = axis_test_storage_dir_new();
	g_autofree gchar *path = g_build_filename(dir, "film.mkv", NULL);
	const gint64 block = AXIS_FILE_SAMPLE_BLOCK;
	const gint64 big = 20 * block + 12345;
	g_autofree guchar *data = g_malloc((gsize)big);
	g_autofree gchar *expected = NULL;
	gchar *digest = NULL;
	gint64 length = 0;
	gint64 i;
	gint err = 0;
	gint fd;

	/* Where the blocks are. */
	g_assert_cmpint(axis_file_sample_offset(100, 0, &length), ==, 0);
	g_assert_cmpint(length, ==, 100);
	g_assert_cmpint(axis_file_sample_offset(100, 1, &length), ==, 0);
	g_assert_cmpint(length, ==, 0);
	g_assert_cmpint(axis_file_sample_offset(8 * block, 0, &length), ==, 0);
	g_assert_cmpint(length, ==, 8 * block);
	g_assert_cmpint(axis_file_sample_offset(big, 0, &length), ==, 0);
	g_assert_cmpint(length, ==, block);
	g_assert_cmpint(axis_file_sample_offset(big, 7, &length), ==, big - block);
	g_assert_cmpint(axis_file_sample_offset(big, 3, &length), ==, (big - block) * 3 / 7);
	for (i = 1; i < 8; i++) {
		gint64 previous = axis_file_sample_offset(big, (guint)i - 1, &length);

		g_assert_cmpint(axis_file_sample_offset(big, (guint)i, &length), >=, previous + block);
	}
	/* A file of four petabytes: no overflow, the last block at its end. */
	g_assert_cmpint(axis_file_sample_offset(G_GINT64_CONSTANT(4503599627370496), 7, &length), ==,
		G_GINT64_CONSTANT(4503599627370496) - block);

	for (i = 0; i < big; i++)
		data[i] = (guchar)((i * 131 + i / 977) & 0xff);
	g_assert_true(g_file_set_contents(path, (const gchar *)data, (gssize)big, NULL));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(axis_file_sample_digest(fd, big, &digest, &err));
	expected = digest_by_hand(data, big);
	g_assert_cmpstr(digest, ==, expected);
	/* Read without moving the descriptor's offset (ffmpeg reads it next). */
	g_assert_cmpint(lseek(fd, 0, SEEK_CUR), ==, 0);
	g_clear_pointer(&digest, g_free);
	close(fd);

	/* A byte changed inside the fourth block: another digest.  One changed
	 * between the blocks is not seen by the samples (the daemon's checks of
	 * the converted file and of the original are what see it). */
	{
		gint64 inside = (big - block) * 3 / 7 + 10;
		gint64 between = block + 5;
		gchar *changed = NULL;

		data[inside] ^= 0xff;
		g_assert_true(g_file_set_contents(path, (const gchar *)data, (gssize)big, NULL));
		fd = open(path, O_RDONLY | O_CLOEXEC);
		g_assert_true(axis_file_sample_digest(fd, big, &changed, &err));
		close(fd);
		g_assert_cmpstr(changed, !=, expected);
		g_free(changed);
		data[inside] ^= 0xff;
		data[between] ^= 0xff;
		g_assert_true(g_file_set_contents(path, (const gchar *)data, (gssize)big, NULL));
		fd = open(path, O_RDONLY | O_CLOEXEC);
		g_assert_true(axis_file_sample_digest(fd, big, &changed, &err));
		close(fd);
		g_assert_cmpstr(changed, ==, expected);
		g_free(changed);
		data[between] ^= 0xff;
	}

	/* Shorter than its stamp says: not read, EIO. */
	g_assert_true(g_file_set_contents(path, (const gchar *)data, (gssize)(big - 1), NULL));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_false(axis_file_sample_digest(fd, big, &digest, &err));
	g_assert_null(digest);
	g_assert_cmpint(err, ==, EIO);
	close(fd);

	/* A small file: all of it, the size in the digest. */
	g_assert_true(g_file_set_contents(path, "a small file", -1, NULL));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_true(axis_file_sample_digest(fd, 12, &digest, &err));
	close(fd);
	g_clear_pointer(&expected, g_free);
	expected = digest_by_hand((const guchar *)"a small file", 12);
	g_assert_cmpstr(digest, ==, expected);
	g_free(digest);
	axis_test_storage_dir_free(dir);
}

/* ── The proof of a volume ──────────────────────────────────────── */

typedef struct
{
	gchar *dir;
	gchar *disk;
} ProofFixture;

static void
proof_setup(ProofFixture *fx, gconstpointer data)
{
	fx->dir = axis_test_storage_dir_new();
	fx->disk = g_build_filename(fx->dir, "disk1", NULL);
	axis_test_storage_mkdir(fx->disk);
}

static void
proof_teardown(ProofFixture *fx, gconstpointer data)
{
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->disk);
}

/* Spec: Wrong disk: right marker accepted, another volume's or none
 * refused; an empty directory where the disk should be refused. */
static void
test_proof_markers(ProofFixture *fx, gconstpointer data)
{
	g_autofree gchar *link = g_build_filename(fx->dir, "link", NULL);
	g_autofree gchar *file = g_build_filename(fx->dir, "plain", NULL);
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_OK;
	AxisVolumeProof *proof;

	/* An empty directory: an unmounted disk. */
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_null(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_NO_MARKER);

	axis_test_storage_write_marker(fx->disk, UUID_A);
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_nonnull(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_OK);
	g_assert_cmpstr(axis_volume_proof_get_uuid(proof), ==, UUID_A);
	g_assert_cmpint(axis_volume_proof_check(proof), ==, AXIS_VOLUME_PROOF_OK);
	axis_volume_proof_free(proof);

	/* A marker that is not one. */
	{
		g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);

		g_assert_true(g_file_set_contents(marker, "axis-volume 1\nuuid nope\n", -1, NULL));
	}
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_null(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_BAD_MARKER);

	/* Through a symbolic link, or not a directory, or not there. */
	g_assert_cmpint(symlink(fx->disk, link), ==, 0);
	proof = axis_volume_proof_make(link, &status);
	g_assert_null(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_LINK);
	axis_test_storage_write(file, "x");
	proof = axis_volume_proof_make(file, &status);
	g_assert_null(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_LINK);
	{
		g_autofree gchar *missing = g_build_filename(fx->dir, "missing", NULL);

		proof = axis_volume_proof_make(missing, &status);
		g_assert_null(proof);
		g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_MISSING);
	}
	/* Not in the plain form. */
	proof = axis_volume_proof_make("relative/path", &status);
	g_assert_null(proof);
	g_assert_cmpint(status, ==, AXIS_VOLUME_PROOF_BAD_PATH);
}

/* Spec: Disk replaced during a job: the check fails when the directory is
 * replaced, when an empty one is left, and when another volume's marker is
 * there; it stays failed. */
static void
test_proof_replaced(ProofFixture *fx, gconstpointer data)
{
	g_autofree gchar *away = g_build_filename(fx->dir, "away", NULL);
	g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_OK;
	AxisVolumeProof *proof;
	AxisVolumeProof *copy;

	axis_test_storage_write_marker(fx->disk, UUID_A);
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_nonnull(proof);
	copy = axis_volume_proof_dup(proof);
	g_assert_nonnull(copy);

	/* The disk goes away and an empty directory is left in its place. */
	g_assert_cmpint(g_rename(fx->disk, away), ==, 0);
	axis_test_storage_mkdir(fx->disk);
	g_assert_cmpint(axis_volume_proof_check(proof), ==, AXIS_VOLUME_PROOF_REPLACED);
	g_assert_cmpint(axis_volume_proof_check(copy), ==, AXIS_VOLUME_PROOF_REPLACED);
	/* It comes back: the proof stays failed (a new one must be made). */
	g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
	g_assert_cmpint(g_rename(away, fx->disk), ==, 0);
	g_assert_cmpint(axis_volume_proof_check(proof), ==, AXIS_VOLUME_PROOF_REPLACED);
	axis_volume_proof_free(proof);
	axis_volume_proof_free(copy);

	/* Another volume's marker in the same directory. */
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_nonnull(proof);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	axis_test_storage_write_marker(fx->disk, UUID_B);
	g_assert_cmpint(axis_volume_proof_check(proof), ==, AXIS_VOLUME_PROOF_OTHER_VOLUME);
	axis_volume_proof_free(proof);

	/* The marker removed. */
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_nonnull(proof);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	g_assert_cmpint(axis_volume_proof_check(proof), ==, AXIS_VOLUME_PROOF_NO_MARKER);
	axis_volume_proof_free(proof);
}

/* Spec: Escaping path: a parent reference or a symbolic link on the way is
 * refused, and nothing outside the directory is opened. */
static void
test_open_beneath(ProofFixture *fx, gconstpointer data)
{
	g_autofree gchar *films = g_build_filename(fx->disk, "Films", NULL);
	g_autofree gchar *folder = g_build_filename(films, "Film", NULL);
	g_autofree gchar *outside = g_build_filename(fx->dir, "outside", NULL);
	g_autofree gchar *film = g_build_filename(folder, "film.mkv", NULL);
	g_autofree gchar *secret = g_build_filename(outside, "secret.mkv", NULL);
	g_autofree gchar *link_dir = g_build_filename(films, "Linked", NULL);
	g_autofree gchar *link_file = g_build_filename(folder, "link.mkv", NULL);
	g_autofree gchar *link_root = g_build_filename(fx->disk, "LinkRoot", NULL);
	g_autofree gchar *fifo = g_build_filename(folder, "fifo.mkv", NULL);
	AxisVolumeProofStatus status = AXIS_VOLUME_PROOF_OK;
	g_autoptr(AxisVolumeProof) proof = NULL;
	struct stat st;
	gint saved = 0;
	gint fd = -1;
	gint dirfd;

	axis_test_storage_write_marker(fx->disk, UUID_A);
	axis_test_storage_mkdir(folder);
	axis_test_storage_mkdir(outside);
	axis_test_storage_write(film, "picture");
	axis_test_storage_write(secret, "not yours");
	g_assert_cmpint(symlink(outside, link_dir), ==, 0);
	g_assert_cmpint(symlink(secret, link_file), ==, 0);
	g_assert_cmpint(symlink(films, link_root), ==, 0);
	g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
	proof = axis_volume_proof_make(fx->disk, &status);
	g_assert_nonnull(proof);
	dirfd = axis_volume_proof_get_fd(proof);

	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Film/film.mkv", &fd, &st, &saved), ==,
		AXIS_STORAGE_OPEN_OK);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(st.st_size, ==, 7);
	close(fd);

	/* Links: a directory on the way, the file itself, the root. */
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Linked/secret.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_NOT_CONTAINED);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Film/link.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_NOT_CONTAINED);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "LinkRoot", "Film/film.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_NOT_CONTAINED);
	/* Not a regular file at the end (and no blocking on a FIFO). */
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Film/fifo.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_NOT_CONTAINED);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Film", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_NOT_CONTAINED);
	/* Parent references and absolute paths are refused before any open. */
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "../../outside/secret.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_INVALID);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "..", "outside/secret.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_INVALID);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", secret, &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_INVALID);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, fx->disk, "Films/Film/film.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_INVALID);
	g_assert_cmpint(axis_storage_open_file_beneath(dirfd, "Films", "Film/missing.mkv", &fd, NULL, &saved), ==,
		AXIS_STORAGE_OPEN_MISSING);
	g_assert_cmpint(fd, ==, -1);
}

/* A worker sends its own version when it registers: this build's, and a
 * pre-release's (config.mk's VERSION_SUFFIX, "0.1.0-rc1"), are versions the
 * daemon reads; the longest suffix the build accepts (20 characters) still
 * fits. */
static void
test_release_versions(void)
{
	g_assert_true(axis_worker_version_is_valid(AXIS_VERSION, AXIS_WORKER_VERSION_MAX));
	g_assert_true(axis_worker_version_is_valid("0.1.0-rc1", AXIS_WORKER_VERSION_MAX));
	g_assert_true(axis_worker_version_is_valid("0.1.0-rc.1", AXIS_WORKER_VERSION_MAX));
	g_assert_true(axis_worker_version_is_valid("100.100.100-abcdefghij123456789", AXIS_WORKER_VERSION_MAX));
	g_assert_false(axis_worker_version_is_valid("0.1.0-rc1 ", AXIS_WORKER_VERSION_MAX));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/worker/proto/register", test_register_round_trip);
	g_test_add_func("/worker/proto/claim", test_claim_round_trip);
	g_test_add_func("/worker/proto/convert", test_convert_messages);
	g_test_add_func("/worker/proto/heartbeat-and-results", test_heartbeat_and_results);
	g_test_add_func("/worker/proto/unknown-and-repeated", test_unknown_and_repeated_members);
	g_test_add_func("/worker/proto/text-bounds", test_text_bounds);
	g_test_add_func("/worker/proto/register-refusals", test_register_refusals);
	g_test_add_func("/worker/proto/description-paths", test_description_paths);
	g_test_add_func("/worker/proto/description-consistency", test_description_consistency);
	g_test_add_func("/worker/proto/answers-refused", test_answers_refused);
	g_test_add_func("/worker/proto/results-refused", test_results_refused);
	g_test_add_func("/worker/proto/claim-pause", test_claim_pause);
	g_test_add_func("/worker/proto/versions", test_versions);
	g_test_add_func("/worker/proto/release-versions", test_release_versions);
	g_test_add_func("/worker/file-check/stamp", test_file_stamp);
	g_test_add_func("/worker/file-check/samples", test_file_samples);
	g_test_add("/worker/proof/markers", ProofFixture, NULL, proof_setup, test_proof_markers, proof_teardown);
	g_test_add("/worker/proof/replaced", ProofFixture, NULL, proof_setup, test_proof_replaced, proof_teardown);
	g_test_add("/worker/proof/open-beneath", ProofFixture, NULL, proof_setup, test_open_beneath, proof_teardown);

	return g_test_run();
}

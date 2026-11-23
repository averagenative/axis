/*
 * fuzz-worker-response.c - libFuzzer target for axis-worker's readers of
 * the daemon's answers, of a job's description, and of its own kept
 * capability measurement
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses ('0' register's answer, '1' claim's, '2'
 * heartbeat's, '3' complete's, '4' fail's, '5' an error answer, '6' a kept
 * measurement, '7' a description); the rest is the answer.  A worker must
 * not be crashable by a daemon it was pointed at by mistake: the reader
 * returns, and what it accepts keeps the promises of
 * api/axis-worker-proto.h: the lease and heartbeat agree, every file of a
 * description is a UUID and two relative paths that pass the check the
 * worker opens them by, every number is in range, every text clean, and
 * what is accepted, written out again, is accepted again.  A claim's answer
 * that says the worker is held paused gives no work; a conversion always
 * carries the digest of its source's samples; an error answer is of a
 * version this side reads.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api/axis-worker-proto.h"
#include "storage/axis-storage-open.h"
#include "transcode/axis-sample-args.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_text(const gchar *text)
{
	const guchar *p;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL))
		abort();
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			abort();
	}
}

static void
check_description(const AxisSampleDescription *desc)
{
	guint i;

	if (!axis_sample_description_check(desc, NULL))
		abort();
	for (i = 0; i < desc->files->len; i++) {
		const AxisSampleFilePlan *f = g_ptr_array_index(desc->files, i);
		AxisEstimateTarget target;
		AxisMediaStream stream;
		const gchar *const prefix[] = { "/usr/bin/ffmpeg", NULL };
		gchar **argv;
		guint k;

		if (!axis_worker_uuid_is_valid(f->volume) || !axis_storage_file_path_valid(f->root, f->path, NULL))
			abort();
		/* The command is built from the enumerations and numbers alone:
		 * every word is printable ASCII, and the input is descriptor 3. */
		axis_sample_description_to_target(desc, f, &target, &stream);
		argv = axis_estimate_sample_argv(prefix, &target, &stream, f->quality, f->bit_depth,
			(gdouble)f->start_ms[0] / 1000.0, (gdouble)f->length_ms / 1000.0);
		for (k = 0; argv[k] != NULL; k++) {
			const gchar *p;

			for (p = argv[k]; *p != '\0'; p++) {
				if ((guchar)*p < 0x21 || (guchar)*p > 0x7e)
					abort();
			}
		}
		if (k < 2 || strcmp(argv[k - 1], "pipe:1") != 0)
			abort();
		g_strfreev(argv);
	}
	{
		gchar *json = axis_sample_description_to_json(desc);
		AxisSampleDescription *again = axis_sample_description_parse(json, strlen(json), NULL);

		if (again == NULL || again->files->len != desc->files->len)
			abort();
		axis_sample_description_free(again);
		g_free(json);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *body;
	gchar *problem = NULL;
	gsize length;

	if (size < 1)
		return 0;
	body = (const gchar *)data + 1;
	length = size - 1;
	switch ((guint)(guint8)(data[0] - '0') % 8) {
	case 0: {
		AxisWorkerRegistered *r = axis_worker_registered_parse(body, length, &problem);
		guint i;

		if (r != NULL) {
			if (!axis_worker_name_is_valid(r->worker) || r->heartbeat_ms > r->lease_ms / 3 ||
			    r->lease_ms < AXIS_WORKER_MIN_LEASE_MS || r->lease_ms > AXIS_WORKER_MAX_LEASE_MS)
				abort();
			for (i = 0; i < r->volumes->len; i++) {
				const AxisWorkerVolumeAnswer *v = g_ptr_array_index(r->volumes, i);

				if (!axis_worker_uuid_is_valid(v->uuid))
					abort();
				check_text(v->reason);
			}
			axis_worker_registered_free(r);
		}
		break;
	}
	case 1: {
		AxisWorkerJob *job = NULL;
		gchar *paused = NULL;

		if (axis_worker_claimed_parse(body, length, &job, &paused, &problem)) {
			/* Held paused: never with work, and said in a clean sentence. */
			if (paused != NULL && (job != NULL || paused[0] == '\0' ||
			    g_utf8_strlen(paused, -1) > AXIS_WORKER_PAUSED_MAX_CHARS))
				abort();
			check_text(paused);
		}
		if (job != NULL) {
			if (job->id < 1 || !axis_worker_uuid_is_valid(job->lease_token) || job->heartbeat_ms > job->lease_ms / 3)
				abort();
			if ((strcmp(job->kind, AXIS_WORKER_KIND_SAMPLE) == 0) != (job->sample != NULL))
				abort();
			if (job->sample != NULL)
				check_description(job->sample);
			/* A conversion always carries the digest of its source's samples. */
			if (job->convert != NULL && !axis_worker_digest_is_valid(job->convert->source_sample))
				abort();
			/* So does a check job, with the output's size and a plan that
			 * passes its own check. */
			if ((strcmp(job->kind, AXIS_WORKER_KIND_VERIFY) == 0) != (job->verify != NULL))
				abort();
			if (job->verify != NULL &&
			    (!axis_worker_digest_is_valid(job->verify->source_sample) || job->verify->output_size < 1 ||
			     job->verify->plan == NULL || !axis_verify_plan_check(job->verify->plan, NULL)))
				abort();
			{
				gchar *json = axis_worker_claimed_to_json(job, NULL);
				AxisWorkerJob *again = NULL;

				if (!axis_worker_claimed_parse(json, strlen(json), &again, NULL, NULL) || again == NULL ||
				    again->id != job->id)
					abort();
				axis_worker_job_free(again);
				g_free(json);
			}
		} else if (paused != NULL) {
			gchar *json = axis_worker_claimed_to_json(NULL, paused);
			gchar *back = NULL;
			AxisWorkerJob *again = NULL;

			if (!axis_worker_claimed_parse(json, strlen(json), &again, &back, NULL) || again != NULL || back == NULL)
				abort();
			g_free(back);
			g_free(json);
		}
		axis_worker_job_free(job);
		g_free(paused);
		break;
	}
	case 2: {
		AxisWorkerBeat *beat = axis_worker_beat_parse(body, length, &problem);

		if (beat != NULL) {
			if (beat->stop ? (beat->reason == NULL || beat->lease_ms != 0) : beat->reason != NULL)
				abort();
			axis_worker_beat_free(beat);
		}
		break;
	}
	case 3: {
		AxisWorkerCompleted completed;

		if (axis_worker_completed_parse(body, length, &completed, &problem) &&
		    (completed.recorded > AXIS_SAMPLE_MAX_FILES || completed.refused > AXIS_SAMPLE_MAX_FILES))
			abort();
		break;
	}
	case 4: {
		gchar *state = NULL;

		if (axis_worker_failed_parse(body, length, &state, &problem) && strcmp(state, "queued") != 0 &&
		    strcmp(state, "failed") != 0 && strcmp(state, "cancelled") != 0)
			abort();
		g_free(state);
		break;
	}
	case 5: {
		gchar *word = NULL;
		gchar *message = NULL;
		guint version = 0;

		if (axis_worker_error_parse(body, length, &word, &message, &version, &problem)) {
			check_text(word);
			check_text(message);
			/* Of a version this side reads (an older daemon's refusal too). */
			if (version < AXIS_WORKER_PROTOCOL_OLDEST || version > AXIS_WORKER_PROTOCOL)
				abort();
		}
		g_free(word);
		g_free(message);
		break;
	}
	case 6: {
		AxisEncoderCapabilities *caps = axis_worker_tool_parse(body, length, &problem);

		if (caps != NULL) {
			gchar *json = axis_worker_tool_to_json(caps);
			AxisEncoderCapabilities *again = axis_worker_tool_parse(json, strlen(json), NULL);

			if (again == NULL || strcmp(again->tool_digest, caps->tool_digest) != 0)
				abort();
			axis_encoder_capabilities_free(again);
			axis_encoder_capabilities_free(caps);
			g_free(json);
		}
		break;
	}
	default: {
		AxisSampleDescription *desc = axis_sample_description_parse(body, length, &problem);

		if (desc != NULL) {
			check_description(desc);
			axis_sample_description_free(desc);
		}
		break;
	}
	}
	if (problem != NULL) {
		check_text(problem);
		if (strlen(problem) > 300)
			abort();
	}
	g_free(problem);
	return 0;
}

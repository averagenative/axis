/*
 * fuzz-worker-request.c - libFuzzer target for the daemon's readers of a
 * worker's requests (register, claim, heartbeat, complete, fail) and for
 * the check of the paths a job description carries
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses ('0' register, '1' claim, '2' heartbeat,
 * '3' complete, '4' fail, '5' a root and a stored path, separated by a
 * line feed, '6' the protocol version a request says it is in, which the
 * daemon reads first); the rest is the request's body.  Whatever it is, the reader
 * must return without crashing or allocating without bound, and what it
 * accepts must keep the promises of api/axis-worker-proto.h:
 *
 *  - every text is valid UTF-8 without control characters, every name,
 *    UUID, digest and version in its alphabet, every number in range;
 *  - every path is absolute and normalised (volumes), or relative without
 *    "." or ".." (job files);
 *  - what is accepted, written out again, is accepted again, the same;
 *  - a refusal is a short sentence.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api/axis-worker-proto.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-open.h"
#include "storage/axis-storage-path.h"

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
check_caps(const AxisEncoderCapabilities *caps)
{
	const AxisConvertEncoder *table = axis_convert_encoders();
	guint i;

	if (!axis_worker_version_is_valid(caps->tool_version, AXIS_WORKER_TOOL_VERSION_MAX) ||
	    !axis_worker_digest_is_valid(caps->tool_digest))
		abort();
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisEncoderCapability *cap = &caps->encoders[i];

		if ((cap->usable && !cap->measured) || (cap->ten_bit && (!cap->usable || !table[i].ten_bit)) ||
		    (cap->hdr10 && !cap->ten_bit))
			abort();
		check_text(cap->reason);
		if (cap->reason != NULL && g_utf8_strlen(cap->reason, -1) > AXIS_WORKER_REASON_MAX_CHARS)
			abort();
	}
}

static void
check_problem(gchar *problem)
{
	if (problem == NULL)
		return;
	check_text(problem);
	if (strlen(problem) > 300)
		abort();
	g_free(problem);
}

static void
fuzz_register(
	const gchar *body,
	gsize        length
){
	gchar *problem = NULL;
	AxisWorkerRegister *r = axis_worker_register_parse(body, length, &problem);
	guint i;

	check_problem(problem);
	if (r == NULL)
		return;
	if (!axis_worker_version_is_valid(r->worker_version, AXIS_WORKER_VERSION_MAX) || r->max_jobs < 1 ||
	    r->max_jobs > AXIS_WORKER_MAX_JOBS || r->volumes->len > AXIS_WORKER_MAX_VOLUMES)
		abort();
	check_caps(r->caps);
	for (i = 0; i < r->volumes->len; i++) {
		const AxisWorkerVolumeSeen *v = g_ptr_array_index(r->volumes, i);
		gchar *normalised = NULL;

		if (!axis_worker_uuid_is_valid(v->uuid) || !axis_storage_path_normalize(v->path, TRUE, &normalised, NULL) ||
		    strcmp(normalised, v->path) != 0)
			abort();
		g_free(normalised);
	}
	{
		gchar *json = axis_worker_register_to_json(r);
		AxisWorkerRegister *again = axis_worker_register_parse(json, strlen(json), NULL);

		if (again == NULL || again->max_jobs != r->max_jobs || again->volumes->len != r->volumes->len ||
		    strcmp(again->worker_version, r->worker_version) != 0)
			abort();
		axis_worker_register_free(again);
		g_free(json);
	}
	axis_worker_register_free(r);
}

static void
fuzz_claim(
	const gchar *body,
	gsize        length
){
	gchar *problem = NULL;
	AxisWorkerClaim *c = axis_worker_claim_parse(body, length, &problem);
	guint i;

	check_problem(problem);
	if (c == NULL)
		return;
	if (c->kinds == NULL || c->kinds[0] == NULL || g_strv_length(c->kinds) > AXIS_WORKER_MAX_KINDS ||
	    g_strv_length(c->volumes) > AXIS_WORKER_MAX_VOLUMES)
		abort();
	for (i = 0; c->kinds[i] != NULL; i++) {
		if (!axis_worker_kind_is_valid(c->kinds[i]))
			abort();
	}
	for (i = 0; c->volumes[i] != NULL; i++) {
		if (!axis_worker_uuid_is_valid(c->volumes[i]))
			abort();
	}
	/* A worker's report of its crash: a job's number and a clean sentence,
	 * both or neither. */
	if ((c->paused_job != 0) != (c->paused_reason != NULL) || c->paused_job < 0 || c->paused_job > AXIS_WORKER_MAX_ID)
		abort();
	if (c->paused_reason != NULL && (c->paused_reason[0] == '\0' ||
	    g_utf8_strlen(c->paused_reason, -1) > AXIS_WORKER_PAUSE_MAX_CHARS))
		abort();
	check_text(c->paused_reason);
	{
		gchar *json = axis_worker_claim_to_json(c);
		AxisWorkerClaim *again = axis_worker_claim_parse(json, strlen(json), NULL);

		if (again == NULL || g_strv_length(again->kinds) != g_strv_length(c->kinds) ||
		    again->paused_job != c->paused_job)
			abort();
		axis_worker_claim_free(again);
		g_free(json);
	}
	axis_worker_claim_free(c);
}

static void
fuzz_heartbeat(
	const gchar *body,
	gsize        length
){
	gchar *problem = NULL;
	AxisWorkerHeartbeat *h = axis_worker_heartbeat_parse(body, length, &problem);

	check_problem(problem);
	if (h == NULL)
		return;
	if (h->job < 1 || h->job > AXIS_WORKER_MAX_ID || !axis_worker_uuid_is_valid(h->lease_token) ||
	    (h->has_progress && !(h->progress >= 0.0 && h->progress <= 1.0)))
		abort();
	check_text(h->message);
	{
		gchar *json = axis_worker_heartbeat_to_json(h);
		AxisWorkerHeartbeat *again = axis_worker_heartbeat_parse(json, strlen(json), NULL);

		if (again == NULL || again->job != h->job)
			abort();
		axis_worker_heartbeat_free(again);
		g_free(json);
	}
	axis_worker_heartbeat_free(h);
}

static void
fuzz_complete(
	const gchar *body,
	gsize        length
){
	gchar *problem = NULL;
	AxisWorkerComplete *c = axis_worker_complete_parse(body, length, &problem);
	guint i;

	check_problem(problem);
	if (c == NULL)
		return;
	/* Exactly one result, of the kind's (a conversion's and a check's too). */
	if ((c->sample != NULL) + (c->caps != NULL) + (c->convert != NULL) + (c->verify != NULL) != 1)
		abort();
	if (c->convert != NULL && (!axis_convert_output_name_valid(c->convert->output) || c->convert->size < 1))
		abort();
	if (c->verify != NULL &&
	    (strcmp(c->kind, AXIS_WORKER_KIND_VERIFY) != 0 || !axis_convert_output_name_valid(c->verify->output) ||
	     c->verify->output_size < 1 || c->verify->evidence == NULL ||
	     !axis_worker_digest_is_valid(c->verify->evidence->output_digest) ||
	     !axis_worker_digest_is_valid(c->verify->evidence->source_sample)))
		abort();
	if (c->caps != NULL)
		check_caps(c->caps);
	for (i = 0; c->sample != NULL && i < c->sample->files->len; i++) {
		const AxisSampleFileResult *f = g_ptr_array_index(c->sample->files, i);

		if (f->index >= AXIS_SAMPLE_MAX_FILES)
			abort();
		if (f->measured && (f->n_bytes < 1 || f->n_bytes > AXIS_SAMPLE_MAX_EXCERPTS))
			abort();
		if (!f->measured && !axis_sample_skip_reason_is_valid(f->reason))
			abort();
	}
	{
		gchar *json = axis_worker_complete_to_json(c);
		AxisWorkerComplete *again = axis_worker_complete_parse(json, strlen(json), NULL);

		if (again == NULL || again->job != c->job || strcmp(again->kind, c->kind) != 0)
			abort();
		axis_worker_complete_free(again);
		g_free(json);
	}
	axis_worker_complete_free(c);
}

static void
fuzz_fail(
	const gchar *body,
	gsize        length
){
	gchar *problem = NULL;
	AxisWorkerFail *f = axis_worker_fail_parse(body, length, &problem);

	check_problem(problem);
	if (f == NULL)
		return;
	if (f->error == NULL || f->error[0] == '\0' || (f->stopping && !f->retry))
		abort();
	check_text(f->error);
	{
		gchar *json = axis_worker_fail_to_json(f);
		AxisWorkerFail *again = axis_worker_fail_parse(json, strlen(json), NULL);

		if (again == NULL || again->job != f->job || again->retry != f->retry || again->stopping != f->stopping)
			abort();
		axis_worker_fail_free(again);
		g_free(json);
	}
	axis_worker_fail_free(f);
}

/* The version a request says it is in, read before anything else of it:
 * found only in a strict JSON object, a whole number of the range, the same
 * as the full readers see; and the refusal of another version is a short
 * sentence, written in a version an older worker reads when it can be. */
static void
fuzz_protocol(
	const gchar *body,
	gsize        length
){
	guint version = 1001;

	if (!axis_worker_message_protocol(body, length, &version)) {
		if (version != 0)
			abort();
		return;
	}
	if (version > 1000)
		abort();
	if (version != AXIS_WORKER_PROTOCOL) {
		gchar *why = axis_worker_protocol_refusal(version);
		gchar *json = axis_worker_error_to_json_version(version, "malformed", why);
		gchar *word = NULL;
		gchar *message = NULL;
		guint seen = 0;

		check_text(why);
		if (!axis_worker_error_parse(json, strlen(json), &word, &message, &seen, NULL) ||
		    strcmp(word, "malformed") != 0 ||
		    seen != (version >= AXIS_WORKER_PROTOCOL_OLDEST && version <= AXIS_WORKER_PROTOCOL ? version :
		             AXIS_WORKER_PROTOCOL))
			abort();
		g_free(word);
		g_free(message);
		g_free(json);
		g_free(why);
	}
}

static void
fuzz_paths(
	const gchar *body,
	gsize        length
){
	gchar *copy = g_strndup(body, length);
	gchar *newline = strchr(copy, '\n');
	const gchar *problem = NULL;
	const gchar *root = copy;
	const gchar *path = newline != NULL ? newline + 1 : "";

	if (newline != NULL)
		*newline = '\0';
	if (axis_storage_file_path_valid(root, path, &problem)) {
		GString *bytes = NULL;
		gchar **parts;
		guint i;

		/* Accepted: relative, and no part steps out. */
		if (root[0] == '/' || path[0] == '/' || !axis_library_name_decode(path, &bytes) || bytes->str[0] == '/')
			abort();
		parts = g_strsplit(bytes->str, "/", -1);
		for (i = 0; parts[i] != NULL; i++) {
			if (parts[i][0] == '\0' || strcmp(parts[i], ".") == 0 || strcmp(parts[i], "..") == 0)
				abort();
		}
		g_strfreev(parts);
		parts = g_strsplit(root, "/", -1);
		for (i = 0; root[0] != '\0' && parts[i] != NULL; i++) {
			if (parts[i][0] == '\0' || strcmp(parts[i], ".") == 0 || strcmp(parts[i], "..") == 0)
				abort();
		}
		g_strfreev(parts);
		g_string_free(bytes, TRUE);
	} else if (problem == NULL) {
		abort();
	}
	g_free(copy);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *body;
	gsize length;

	if (size < 1)
		return 0;
	body = (const gchar *)data + 1;
	length = size - 1;
	switch ((guint)(guint8)(data[0] - '0') % 7) {
	case 0:
		fuzz_register(body, length);
		break;
	case 1:
		fuzz_claim(body, length);
		break;
	case 2:
		fuzz_heartbeat(body, length);
		break;
	case 3:
		fuzz_complete(body, length);
		break;
	case 4:
		fuzz_fail(body, length);
		break;
	case 5:
		fuzz_paths(body, length);
		break;
	default:
		fuzz_protocol(body, length);
		break;
	}
	return 0;
}

/*
 * fuzz-compat-plex.c - libFuzzer target for the parsers of the media
 * server's answers, the classification of a session into evidence, and the
 * verdict function
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the parser ('0' identity, '1' libraries, '2'
 * devices, '3' accounts, '4' sessions, '5' history, '6' item, '7' the root
 * document); the rest is the body of an answer.  Whatever it is, the
 * parser must return without crashing or allocating without bound, and
 * what it returns must keep the promises of axis-compat-model.h:
 *
 *  - every string is valid UTF-8 without control characters, and ids are
 *    made of the characters an id may have;
 *  - every number is unknown or within its limits, and every list is within
 *    its maximum length;
 *  - the evidence made of a session never claims more than the session
 *    says: a container only with a direct play, a codec in one spelling,
 *    nothing for a session that lists several media;
 *  - the verdict function never says "safe" without a play that covers the
 *    combination asked about, and never for a session or a device without
 *    evidence.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compat/axis-compat-evidence.h"
#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-verdict.h"

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
check_id(const gchar *id)
{
	if (id != NULL && !axis_compat_plex_id_valid(id))
		abort();
}

static void
check_stream(const AxisCompatStream *s)
{
	if (s->type < 0 || s->type > 3)
		abort();
	check_text(s->codec);
	check_text(s->profile);
	if (s->bit_depth != -1 && (s->bit_depth < 1 || s->bit_depth > 64))
		abort();
	if (s->width != -1 && (s->width < 1 || s->width > 32768))
		abort();
	if (s->height != -1 && (s->height < 1 || s->height > 32768))
		abort();
	if (s->channels != -1 && (s->channels < 1 || s->channels > 256))
		abort();
	if (s->bitrate_kbps < -1 || s->bitrate_kbps > 1000000000)
		abort();
}

static void
check_evidence(const AxisCompatEvidence *e)
{
	if (e->kind < 1 || e->kind > 3 || e->codec == NULL)
		abort();
	check_text(e->codec);
	check_text(e->profile);
	check_text(e->res_class);
	check_text(e->container);
	check_text(e->out_codec);
	/* The codec is in one spelling, lower case. */
	if (strpbrk(e->codec, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != NULL)
		abort();
	/* A container is evidence only of a play with no conversion. */
	if (e->container != NULL && e->effect != AXIS_EFFECT_DIRECT)
		abort();
	if (e->out_codec != NULL && e->effect != AXIS_EFFECT_CONVERTED_CODEC && e->effect != AXIS_EFFECT_CONVERTED_LIMIT &&
	    e->effect != AXIS_EFFECT_CONVERTED_UNCLEAR)
		abort();
	if (e->hdr < -1 || e->hdr > 1 || e->bit_depth < 0 || e->bit_depth > 64 || e->channels < 0 || e->channels > 256)
		abort();
}

/* A reference for the verdict: safe only if a direct play (or, when no
 * container is asked, a passed-through stream) covers the combination. */
static void
check_verdicts(GPtrArray *evidence)
{
	AxisCompatObservation obs[8];
	guint n = MIN(evidence->len, 8u);
	AxisCompatCombo want;
	guint i;

	for (i = 0; i < n; i++) {
		const AxisCompatEvidence *e = g_ptr_array_index(evidence, i);

		memset(&obs[i], 0, sizeof(obs[i]));
		axis_compat_evidence_combo(e, &obs[i].combo);
		obs[i].effect = e->effect;
		obs[i].plays = 1;
	}
	memset(&want, 0, sizeof(want));
	want.kind = AXIS_COMPAT_STREAM_VIDEO;
	want.codec = "hevc";
	want.profile = "main 10";
	want.bit_depth = 10;
	want.res_class = "1080p";
	want.hdr = 0;
	want.container = "mkv";
	if (axis_compat_device_verdict(&want, obs, n, NULL, NULL) == AXIS_VERDICT_SAFE) {
		gboolean covered = FALSE;

		for (i = 0; i < n; i++)
			covered = covered || (obs[i].effect == AXIS_EFFECT_DIRECT && axis_compat_covers(&obs[i].combo, &want));
		if (!covered)
			abort();
	}
	if (axis_compat_device_verdict(&want, NULL, 0, NULL, NULL) != AXIS_VERDICT_UNKNOWN)
		abort();
}

static void
check_sessions(GPtrArray *sessions)
{
	guint i;
	guint j;

	if (sessions->len > AXIS_COMPAT_MAX_SESSIONS)
		abort();
	for (i = 0; i < sessions->len; i++) {
		const AxisCompatSession *s = g_ptr_array_index(sessions, i);
		AxisCompatFileFacts facts;
		GPtrArray *evidence;

		check_id(s->session_key);
		check_id(s->rating_key);
		check_id(s->library_key);
		check_id(s->device_client_id);
		check_id(s->account_id);
		check_text(s->device_name);
		check_text(s->platform);
		check_text(s->product);
		check_text(s->location);
		check_text(s->account_name);
		check_text(s->file);
		check_text(s->container);
		if (s->bandwidth_kbps < -1 || s->bandwidth_kbps > 1000000000 || s->media_bitrate_kbps < -1 ||
		    s->media_bitrate_kbps > 1000000000)
			abort();
		if (s->streams->len > AXIS_COMPAT_MAX_STREAMS)
			abort();
		for (j = 0; j < s->streams->len; j++)
			check_stream(g_ptr_array_index(s->streams, j));

		evidence = axis_compat_classify_session(s, NULL);
		if (s->ambiguous && evidence->len != 0)
			abort();
		for (j = 0; j < evidence->len; j++)
			check_evidence(g_ptr_array_index(evidence, j));
		check_verdicts(evidence);
		g_ptr_array_unref(evidence);

		memset(&facts, 0, sizeof(facts));
		facts.video_codec = "hevc";
		facts.video_profile = "Main 10";
		facts.bit_depth = 10;
		facts.res_class = "2160p";
		facts.hdr = 1;
		facts.audio_codec = "eac3";
		facts.audio_channels = 6;
		facts.container = "matroska,webm";
		facts.bitrate_kbps = 20000;
		evidence = axis_compat_classify_session(s, &facts);
		for (j = 0; j < evidence->len; j++)
			check_evidence(g_ptr_array_index(evidence, j));
		check_verdicts(evidence);
		g_ptr_array_unref(evidence);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *problem = NULL;
	guint choice;
	const gchar *body;
	gsize length;

	if (size < 1)
		return 0;
	choice = (guint)(data[0] % 8);
	body = (const gchar *)data + 1;
	length = size - 1;

	switch (choice) {
	case 0:
	case 7: {
		AxisCompatIdentity identity;

		memset(&identity, 0, sizeof(identity));
		if (choice == 0)
			axis_compat_plex_parse_identity(body, length, &identity, &problem);
		else
			axis_compat_plex_parse_root(body, length, &identity, &problem);
		check_id(identity.machine_id);
		check_text(identity.version);
		check_text(identity.name);
		axis_compat_identity_clear(&identity);
		break;
	}
	case 1: {
		GPtrArray *libraries = axis_compat_plex_parse_libraries(body, length, &problem);
		guint i;
		guint j;

		if (libraries != NULL) {
			if (libraries->len > AXIS_COMPAT_MAX_LIBRARIES)
				abort();
			for (i = 0; i < libraries->len; i++) {
				const AxisCompatLibrary *l = g_ptr_array_index(libraries, i);

				check_id(l->key);
				if (l->key == NULL || l->folders->len > AXIS_COMPAT_MAX_FOLDERS)
					abort();
				check_text(l->title);
				for (j = 0; j < l->folders->len; j++) {
					check_text(g_ptr_array_index(l->folders, j));
					if (strlen(g_ptr_array_index(l->folders, j)) > 4 * AXIS_COMPAT_MAX_PATH_CHARS)
						abort();
				}
			}
			g_ptr_array_unref(libraries);
		}
		break;
	}
	case 2: {
		GPtrArray *devices = axis_compat_plex_parse_devices(body, length, &problem);
		guint i;

		if (devices != NULL) {
			if (devices->len > AXIS_COMPAT_MAX_DEVICES)
				abort();
			for (i = 0; i < devices->len; i++) {
				const AxisCompatDevice *d = g_ptr_array_index(devices, i);

				check_id(d->client_id);
				check_text(d->name);
				check_text(d->platform);
				if (d->plex_id < -1)
					abort();
			}
			g_ptr_array_unref(devices);
		}
		break;
	}
	case 3: {
		GPtrArray *accounts = axis_compat_plex_parse_accounts(body, length, &problem);
		guint i;

		if (accounts != NULL) {
			for (i = 0; i < accounts->len; i++) {
				const AxisCompatAccount *a = g_ptr_array_index(accounts, i);

				check_id(a->id);
				check_text(a->name);
			}
			g_ptr_array_unref(accounts);
		}
		break;
	}
	case 4: {
		GPtrArray *sessions = axis_compat_plex_parse_sessions(body, length, &problem);

		if (sessions != NULL) {
			check_sessions(sessions);
			g_ptr_array_unref(sessions);
		}
		break;
	}
	case 5: {
		AxisCompatHistory history;
		guint i;

		memset(&history, 0, sizeof(history));
		if (axis_compat_plex_parse_history(body, length, &history, &problem)) {
			if (history.plays->len > AXIS_COMPAT_MAX_HISTORY)
				abort();
			for (i = 0; i < history.plays->len; i++) {
				const AxisCompatPlay *p = g_ptr_array_index(history.plays, i);

				check_id(p->account_id);
				check_id(p->library_key);
				check_id(p->rating_key);
				if (p->device_id < -1 || p->viewed_at < -1 || p->viewed_at > G_GINT64_CONSTANT(7258118400))
					abort();
			}
		}
		axis_compat_history_clear(&history);
		break;
	}
	default: {
		AxisCompatItem item;

		memset(&item, 0, sizeof(item));
		if (axis_compat_plex_parse_item(body, length, &item, &problem)) {
			check_text(item.file);
			if (item.file != NULL && strlen(item.file) > 4 * AXIS_COMPAT_MAX_PATH_CHARS)
				abort();
		}
		axis_compat_item_clear(&item);
		break;
	}
	}
	/* A refusal says why without quoting the input. */
	if (problem != NULL) {
		check_text(problem);
		if (strlen(problem) > 200)
			abort();
	}
	g_free(problem);
	return 0;
}

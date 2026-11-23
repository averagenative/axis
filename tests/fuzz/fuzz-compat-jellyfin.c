/*
 * fuzz-compat-jellyfin.c - libFuzzer target for the parsers of a Jellyfin
 * server's answers, the evidence made of a session that states why it is
 * converted, and the verdict function with conversions blamed on a property
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the parser ('0' identity, '1' libraries, '2'
 * devices, '3' sessions, '4' item); the rest is the body of an answer.
 * Whatever it is, the parser must return without crashing or allocating
 * without bound, and what it returns must keep the promises of
 * axis-compat-jellyfin.h and axis-compat-model.h:
 *
 *  - every string is valid UTF-8 without control characters, and ids are
 *    made of the characters an id may have (a device id that is not is
 *    hashed into one);
 *  - every number is unknown or within its limits, every list within its
 *    maximum length, and a stated reason at most 64 characters;
 *  - the evidence made of a session never claims more than it says: a
 *    container only with a direct play or as the container not taken, a
 *    property only with a conversion of the codec, nothing for a session
 *    that lists another version of the item, a stated reason that is clean
 *    and short, and a codec-level failure only from a reason the table says
 *    blames the codec;
 *  - the verdict function never says "safe" without a play that covers the
 *    combination asked about, and never says "risky" for a failure blamed on
 *    a property the combination does not share.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compat/axis-compat-evidence.h"
#include "compat/axis-compat-jellyfin.h"
#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-reasons.h"
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
	if (s->bitrate_kbps < -1 || s->bitrate_kbps > G_GINT64_CONSTANT(1000000000000))
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
	check_text(e->stated_reason);
	if (strpbrk(e->codec, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != NULL)
		abort();
	/* A container is evidence of a play with no conversion, or the one the
	 * server says was not taken. */
	if (e->container != NULL && e->effect != AXIS_EFFECT_DIRECT &&
	    !(e->effect == AXIS_EFFECT_CONVERTED_CODEC && e->property == AXIS_PROPERTY_CONTAINER))
		abort();
	/* Only a conversion of the codec blames a property. */
	if (e->property != AXIS_PROPERTY_NONE && e->effect != AXIS_EFFECT_CONVERTED_CODEC)
		abort();
	if (e->stated_reason != NULL && g_utf8_strlen(e->stated_reason, -1) > AXIS_COMPAT_MAX_REASON_CHARS)
		abort();
	if (e->out_codec != NULL && e->effect != AXIS_EFFECT_CONVERTED_CODEC && e->effect != AXIS_EFFECT_CONVERTED_LIMIT &&
	    e->effect != AXIS_EFFECT_CONVERTED_UNCLEAR)
		abort();
	if (e->hdr < -1 || e->hdr > 1 || e->bit_depth < 0 || e->bit_depth > 64 || e->channels < 0 || e->channels > 256)
		abort();
}

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
		obs[i].property = e->property;
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
	if (axis_compat_device_verdict(&want, obs, n, NULL, NULL) == AXIS_VERDICT_RISKY) {
		gboolean blamed = FALSE;

		/* Some conversion of the codec was blamed on something the
		 * combination shares, or on the codec. */
		for (i = 0; i < n; i++)
			blamed = blamed || (obs[i].effect == AXIS_EFFECT_CONVERTED_CODEC &&
				axis_compat_fails_property(&obs[i].combo, obs[i].property, &want));
		if (!blamed)
			abort();
	}
	if (axis_compat_device_verdict(&want, NULL, 0, NULL, NULL) != AXIS_VERDICT_UNKNOWN)
		abort();
}

static void
check_session_evidence(
	const AxisCompatSession   *s,
	const AxisCompatFileFacts *facts
){
	GPtrArray *evidence = axis_compat_classify_session(s, facts);
	guint j;

	if (s->ambiguous && evidence->len != 0)
		abort();
	for (j = 0; j < evidence->len; j++) {
		const AxisCompatEvidence *e = g_ptr_array_index(evidence, j);
		guint k;

		check_evidence(e);
		/* A row that says the codec failed (and blames no property) exists
		 * only because a stated reason of the table blames the codec. */
		if (e->effect == AXIS_EFFECT_CONVERTED_CODEC && e->property == AXIS_PROPERTY_NONE &&
		    s->transcode.reasons != NULL) {
			gboolean listed = FALSE;

			for (k = 0; k < s->transcode.reasons->len; k++) {
				const AxisCompatReason *r = axis_compat_reason_lookup(g_ptr_array_index(s->transcode.reasons, k));

				listed = listed || (r != NULL && r->effect == AXIS_EFFECT_CONVERTED_CODEC &&
					r->property == AXIS_PROPERTY_NONE);
			}
			if (!listed)
				abort();
		}
		/* A property row exists only because a reason that blames it is stated. */
		if (e->property != AXIS_PROPERTY_NONE && s->transcode.reasons != NULL) {
			gboolean listed = FALSE;

			for (k = 0; k < s->transcode.reasons->len; k++) {
				const AxisCompatReason *r = axis_compat_reason_lookup(g_ptr_array_index(s->transcode.reasons, k));

				listed = listed || (r != NULL && r->property == e->property);
			}
			if (!listed)
				abort();
		}
	}
	check_verdicts(evidence);
	g_ptr_array_unref(evidence);
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
		if (s->bandwidth_kbps < -1 || s->media_bitrate_kbps < -1)
			abort();
		if (s->streams->len > AXIS_COMPAT_MAX_STREAMS)
			abort();
		for (j = 0; j < s->streams->len; j++)
			check_stream(g_ptr_array_index(s->streams, j));
		if (s->transcode.reasons != NULL) {
			if (s->transcode.reasons->len == 0 || s->transcode.reasons->len > AXIS_COMPAT_MAX_REASONS)
				abort();
			for (j = 0; j < s->transcode.reasons->len; j++) {
				const gchar *reason = g_ptr_array_index(s->transcode.reasons, j);

				check_text(reason);
				if (g_utf8_strlen(reason, -1) > AXIS_COMPAT_MAX_REASON_CHARS)
					abort();
			}
		}

		check_session_evidence(s, NULL);
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
		check_session_evidence(s, &facts);
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
	choice = (guint)(data[0] % 5);
	body = (const gchar *)data + 1;
	length = size - 1;

	switch (choice) {
	case 0: {
		AxisCompatIdentity identity;

		memset(&identity, 0, sizeof(identity));
		axis_compat_jellyfin_parse_identity(body, length, &identity, &problem);
		check_id(identity.machine_id);
		check_text(identity.version);
		check_text(identity.name);
		axis_compat_identity_clear(&identity);
		break;
	}
	case 1: {
		GPtrArray *libraries = axis_compat_jellyfin_parse_libraries(body, length, &problem);
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
				if (g_strcmp0(l->kind, "movie") != 0 && g_strcmp0(l->kind, "show") != 0 &&
				    g_strcmp0(l->kind, "artist") != 0 && g_strcmp0(l->kind, "other") != 0)
					abort();
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
		GPtrArray *devices = axis_compat_jellyfin_parse_devices(body, length, &problem);
		guint i;

		if (devices != NULL) {
			if (devices->len > AXIS_COMPAT_MAX_DEVICES)
				abort();
			for (i = 0; i < devices->len; i++) {
				const AxisCompatDevice *d = g_ptr_array_index(devices, i);

				if (d->client_id == NULL)
					abort();
				check_id(d->client_id);
				check_text(d->name);
				check_text(d->product);
				if (d->plex_id != -1)
					abort();
			}
			g_ptr_array_unref(devices);
		}
		break;
	}
	case 3: {
		GPtrArray *sessions = axis_compat_jellyfin_parse_sessions(body, length, &problem);

		if (sessions != NULL) {
			check_sessions(sessions);
			g_ptr_array_unref(sessions);
		}
		break;
	}
	default: {
		AxisCompatItem item;

		memset(&item, 0, sizeof(item));
		if (axis_compat_jellyfin_parse_item(body, length, &item, &problem)) {
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
	{
		/* Any text becomes a key or nothing, never an invalid key. */
		g_autofree gchar *copy = g_strndup((const gchar *)data, size);
		g_autofree gchar *key = axis_compat_jellyfin_key(copy);

		check_id(key);
	}
	return 0;
}

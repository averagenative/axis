/*
 * axis-compat-evidence.c - what one observed session proves about the device
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-evidence.h"

#include <string.h>

#include "compat/axis-compat-reasons.h"
#include "probe/axis-probe-model.h"

/* ── Words ──────────────────────────────────────────────────────── */

static const struct
{
	AxisCompatEffect effect;
	const gchar     *name;
	const gchar     *label;
} effects[] = {
	{ AXIS_EFFECT_UNKNOWN, "unknown", "not known" },
	{ AXIS_EFFECT_DIRECT, "direct", "played directly" },
	{ AXIS_EFFECT_PASSTHROUGH, "passthrough", "passed through" },
	{ AXIS_EFFECT_CONVERTED_CODEC, "converted_codec", "converted: the device did not take the codec" },
	{ AXIS_EFFECT_CONVERTED_LIMIT, "converted_limit", "converted: a bandwidth or quality limit" },
	{ AXIS_EFFECT_CONVERTED_UNCLEAR, "converted_unclear", "converted: cause unclear" }
};

const gchar *
axis_compat_effect_to_string(AxisCompatEffect effect)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(effects); i++) {
		if (effects[i].effect == effect)
			return effects[i].name;
	}
	return "unknown";
}

gboolean
axis_compat_effect_from_string(
	const gchar      *text,
	AxisCompatEffect *effect
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(effects); i++) {
		if (strcmp(effects[i].name, text) == 0) {
			*effect = effects[i].effect;
			return TRUE;
		}
	}
	*effect = AXIS_EFFECT_UNKNOWN;
	return FALSE;
}

const gchar *
axis_compat_effect_label(AxisCompatEffect effect)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(effects); i++) {
		if (effects[i].effect == effect)
			return effects[i].label;
	}
	return "not known";
}

static const struct
{
	AxisCompatProperty property;
	const gchar       *name;
	const gchar       *label;
} properties[] = {
	{ AXIS_PROPERTY_NONE, "", "" },
	{ AXIS_PROPERTY_CONTAINER, "container", "the container" },
	{ AXIS_PROPERTY_PROFILE, "profile", "the profile" },
	{ AXIS_PROPERTY_BIT_DEPTH, "bit_depth", "the bit depth" },
	{ AXIS_PROPERTY_LEVEL, "level", "the level" },
	{ AXIS_PROPERTY_SUBTITLE_FORMAT, "subtitle_format", "the subtitle format" }
};

const gchar *
axis_compat_property_to_string(AxisCompatProperty property)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(properties); i++) {
		if (properties[i].property == property)
			return properties[i].name;
	}
	return "";
}

gboolean
axis_compat_property_from_string(
	const gchar        *text,
	AxisCompatProperty *property
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(properties); i++) {
		if (strcmp(properties[i].name, text) == 0) {
			*property = properties[i].property;
			return TRUE;
		}
	}
	*property = AXIS_PROPERTY_NONE;
	return FALSE;
}

const gchar *
axis_compat_property_label(AxisCompatProperty property)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(properties); i++) {
		if (properties[i].property == property)
			return properties[i].label;
	}
	return "";
}

void
axis_compat_evidence_free(AxisCompatEvidence *evidence)
{
	if (evidence == NULL)
		return;
	g_free(evidence->codec);
	g_free(evidence->profile);
	g_free(evidence->res_class);
	g_free(evidence->container);
	g_free(evidence->out_codec);
	g_free(evidence->stated_reason);
	g_free(evidence);
}

/* ── Names ──────────────────────────────────────────────────────── */

static gboolean
name_is_clean(
	const gchar *name,
	gsize        max
){
	const gchar *p;

	if (name == NULL || name[0] == '\0' || strlen(name) > max)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != '-' && *p != '+')
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_compat_container_family(const gchar *name)
{
	g_autofree gchar *lower = NULL;

	if (name == NULL || name[0] == '\0' || strlen(name) > 64)
		return NULL;
	lower = g_ascii_strdown(name, -1);
	if (strstr(lower, "matroska") != NULL || strcmp(lower, "mkv") == 0 || strcmp(lower, "webm") == 0)
		return g_strdup("mkv");
	if (strncmp(lower, "mov,mp4", 7) == 0 || strcmp(lower, "mp4") == 0 || strcmp(lower, "m4v") == 0 ||
	    strcmp(lower, "mov") == 0 || strcmp(lower, "m4a") == 0 || strcmp(lower, "m4b") == 0)
		return g_strdup("mp4");
	if (strcmp(lower, "mp3") == 0 || strcmp(lower, "flac") == 0 || strcmp(lower, "ogg") == 0)
		return g_steal_pointer(&lower);
	return name_is_clean(lower, 16) ? g_steal_pointer(&lower) : NULL;
}

gchar *
axis_compat_codec_name(const gchar *name)
{
	g_autofree gchar *lower = NULL;

	if (name == NULL || name[0] == '\0' || strlen(name) > 32)
		return NULL;
	lower = g_ascii_strdown(name, -1);
	if (strcmp(lower, "avc") == 0 || strcmp(lower, "avc1") == 0 || strcmp(lower, "x264") == 0)
		return g_strdup("h264");
	if (strcmp(lower, "h265") == 0 || strcmp(lower, "hvc1") == 0 || strcmp(lower, "hev1") == 0 ||
	    strcmp(lower, "x265") == 0)
		return g_strdup("hevc");
	if (strcmp(lower, "dca") == 0)
		return g_strdup("dts");
	if (strcmp(lower, "ac-3") == 0)
		return g_strdup("ac3");
	if (strcmp(lower, "e-ac-3") == 0)
		return g_strdup("eac3");
	return name_is_clean(lower, 32) ? g_steal_pointer(&lower) : NULL;
}

/* ── Choosing the stream that played ────────────────────────────── */

static const AxisCompatStream *
pick_stream(
	const AxisCompatSession *session,
	gint                     type
){
	const AxisCompatStream *only = NULL;
	const AxisCompatStream *decided = NULL;
	const AxisCompatStream *selected = NULL;
	guint n = 0;
	guint n_decided = 0;
	guint n_selected = 0;
	guint i;

	for (i = 0; session->streams != NULL && i < session->streams->len; i++) {
		const AxisCompatStream *s = g_ptr_array_index(session->streams, i);

		if (s->type != type)
			continue;
		n++;
		only = s;
		if (s->decision != AXIS_COMPAT_DECISION_NONE) {
			n_decided++;
			decided = s;
		}
		if (s->selected == AXIS_COMPAT_YES) {
			n_selected++;
			selected = s;
		}
	}
	if (n == 1)
		return only;
	/* In a conversion the server states a decision for the streams in use. */
	if (session->transcode.present && n_decided == 1)
		return decided;
	if (n_selected == 1)
		return selected;
	return NULL;
}

/* ── Classification ─────────────────────────────────────────────── */

static AxisCompatDecision
transcode_decision(
	const AxisCompatSession *session,
	gint                     kind
){
	switch (kind) {
	case AXIS_COMPAT_STREAM_VIDEO:
		return session->transcode.video_decision;
	case AXIS_COMPAT_STREAM_AUDIO:
		return session->transcode.audio_decision;
	default:
		return session->transcode.subtitle_decision;
	}
}

typedef struct
{
	gboolean limited;	/* remote, and the bandwidth is below the source's bitrate */
} Context;

static AxisCompatEffect
explain_conversion(
	const AxisCompatSession *session,
	const Context           *context,
	gint                     kind,
	const gchar             *source_codec,
	const gchar             *out_codec,
	gint                     source_height
){
	g_autofree gchar *source = axis_compat_codec_name(source_codec);
	g_autofree gchar *out = axis_compat_codec_name(out_codec);

	if (kind == AXIS_COMPAT_STREAM_SUBTITLE)
		return AXIS_EFFECT_CONVERTED_UNCLEAR;
	if (context->limited)
		return AXIS_EFFECT_CONVERTED_LIMIT;
	if (source != NULL && out != NULL && strcmp(source, out) != 0)
		return AXIS_EFFECT_CONVERTED_CODEC;
	if (kind == AXIS_COMPAT_STREAM_VIDEO && source != NULL && out != NULL &&
	    session->transcode.height > 0 && source_height > 0 && session->transcode.height < source_height)
		return AXIS_EFFECT_CONVERTED_LIMIT;
	return AXIS_EFFECT_CONVERTED_UNCLEAR;
}

static gboolean
same_codec(
	const gchar *a,
	const gchar *b
){
	g_autofree gchar *na = axis_compat_codec_name(a);
	g_autofree gchar *nb = axis_compat_codec_name(b);

	return na != NULL && nb != NULL && strcmp(na, nb) == 0;
}

/* The stream of @kind that played and the codec it had; %FALSE when nothing
 * certain can be said about it. */
static gboolean
pick_source(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts,
	gint                       kind,
	const AxisCompatStream   **stream,
	const gchar              **source_codec_raw
){
	const AxisCompatStream *s = pick_stream(session, kind);
	const gchar *server_codec;
	const gchar *facts_codec = NULL;

	if (s == NULL)
		return FALSE;

	/* What the server says the source stream was. */
	server_codec = s->codec;
	if (server_codec == NULL && session->transcode.present) {
		server_codec = kind == AXIS_COMPAT_STREAM_VIDEO ? session->transcode.source_video_codec :
			kind == AXIS_COMPAT_STREAM_AUDIO ? session->transcode.source_audio_codec : NULL;
	}
	if (facts != NULL)
		facts_codec = kind == AXIS_COMPAT_STREAM_VIDEO ? facts->video_codec :
			kind == AXIS_COMPAT_STREAM_AUDIO ? facts->audio_codec : NULL;
	/* The probe is trusted over the server's summary; when they disagree
	 * about the codec the match is in doubt and nothing is concluded. */
	if (facts_codec != NULL && server_codec != NULL && !same_codec(facts_codec, server_codec))
		return FALSE;
	*source_codec_raw = facts_codec != NULL ? facts_codec : server_codec;
	*stream = s;
	return *source_codec_raw != NULL;
}

/* The container of the played file in one spelling, or %NULL when it is not
 * known or the server and the probe disagree. */
static gchar *
played_container(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts
){
	g_autofree gchar *server_container = axis_compat_container_family(session->container);
	g_autofree gchar *facts_container = facts != NULL ? axis_compat_container_family(facts->container) : NULL;

	if (server_container != NULL && facts_container != NULL && strcmp(server_container, facts_container) != 0)
		return NULL;
	return g_strdup(facts_container != NULL ? facts_container : server_container);
}

/* The evidence of @effect about the stream @s (with @property when the
 * server blames one). */
static AxisCompatEvidence *
make_evidence(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts,
	gboolean                   remote,
	gint                       kind,
	const AxisCompatStream    *s,
	const gchar               *source_codec_raw,
	const gchar               *out_codec_raw,
	AxisCompatEffect           effect,
	AxisCompatProperty         property
){
	AxisCompatEvidence *ev = g_new0(AxisCompatEvidence, 1);

	ev->kind = kind;
	ev->codec = axis_compat_codec_name(source_codec_raw);
	ev->effect = effect;
	ev->property = property;
	ev->remote = remote;
	ev->hdr = -1;
	if (ev->codec == NULL) {
		axis_compat_evidence_free(ev);
		return NULL;
	}
	if (effect == AXIS_EFFECT_CONVERTED_CODEC || effect == AXIS_EFFECT_CONVERTED_LIMIT ||
	    effect == AXIS_EFFECT_CONVERTED_UNCLEAR)
		ev->out_codec = axis_compat_codec_name(out_codec_raw);

	if (kind == AXIS_COMPAT_STREAM_VIDEO) {
		const gchar *res = NULL;

		ev->profile = facts != NULL && facts->video_profile != NULL ? g_ascii_strdown(facts->video_profile, -1) :
			g_strdup(s->profile);
		ev->bit_depth = facts != NULL && facts->bit_depth > 0 ? facts->bit_depth : (s->bit_depth > 0 ? s->bit_depth : 0);
		if (facts != NULL && facts->res_class != NULL)
			res = facts->res_class;
		else if (s->width > 0 && s->height > 0)
			res = axis_probe_resolution_class(s->width, s->height);
		ev->res_class = g_strdup(res);
		ev->hdr = facts != NULL && facts->hdr >= 0 ? facts->hdr : (gint)s->hdr;
		if (ev->hdr < -1 || ev->hdr > 1)
			ev->hdr = -1;
	} else if (kind == AXIS_COMPAT_STREAM_AUDIO) {
		ev->profile = g_strdup(s->profile);
		ev->channels = facts != NULL && facts->audio_channels > 0 ? facts->audio_channels :
			(s->channels > 0 ? s->channels : 0);
	}

	/* The container counts only for a play with no conversion at all, or as
	 * the container the server says was not taken. */
	if (effect == AXIS_EFFECT_DIRECT ||
	    (effect == AXIS_EFFECT_CONVERTED_CODEC && property == AXIS_PROPERTY_CONTAINER))
		ev->container = played_container(session, facts);
	return ev;
}

static AxisCompatEvidence *
evidence_for(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts,
	const Context             *context,
	gboolean                   remote,
	gint                       kind
){
	const AxisCompatStream *s = NULL;
	AxisCompatDecision decided;
	const gchar *source_codec_raw = NULL;
	const gchar *out_codec_raw = NULL;
	AxisCompatEffect effect;

	if (!pick_source(session, facts, kind, &s, &source_codec_raw))
		return NULL;

	if (session->transcode.present)
		out_codec_raw = kind == AXIS_COMPAT_STREAM_VIDEO ? session->transcode.video_codec :
			kind == AXIS_COMPAT_STREAM_AUDIO ? session->transcode.audio_codec : NULL;

	/* What happened to it. */
	if (!session->transcode.present) {
		if (session->part_decision != AXIS_COMPAT_DECISION_DIRECT ||
		    (s->decision != AXIS_COMPAT_DECISION_NONE && s->decision != AXIS_COMPAT_DECISION_DIRECT))
			effect = AXIS_EFFECT_UNKNOWN;
		else
			effect = AXIS_EFFECT_DIRECT;
	} else {
		AxisCompatDecision from_session = transcode_decision(session, kind);

		decided = s->decision != AXIS_COMPAT_DECISION_NONE ? s->decision : from_session;
		if (s->decision != AXIS_COMPAT_DECISION_NONE && from_session != AXIS_COMPAT_DECISION_NONE &&
		    s->decision != from_session)
			effect = AXIS_EFFECT_UNKNOWN;
		else if (decided == AXIS_COMPAT_DECISION_COPY || decided == AXIS_COMPAT_DECISION_DIRECT)
			/* Inside a conversion the container is the server's, not the file's. */
			effect = AXIS_EFFECT_PASSTHROUGH;
		else if (decided == AXIS_COMPAT_DECISION_TRANSCODE)
			effect = explain_conversion(session, context, kind, source_codec_raw, out_codec_raw,
				s->height > 0 ? s->height : -1);
		else
			effect = AXIS_EFFECT_UNKNOWN;
	}
	return make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw, effect, AXIS_PROPERTY_NONE);
}

/* ── Reasons the server states ──────────────────────────────────── */

static gint
effect_rank(AxisCompatEffect effect)
{
	switch (effect) {
	case AXIS_EFFECT_CONVERTED_CODEC:
		return 3;
	case AXIS_EFFECT_CONVERTED_LIMIT:
		return 2;
	default:
		return 1;
	}
}

/* The reasons joined, cut to what a row may hold. */
static gchar *
joined_reasons(GPtrArray *names)
{
	g_autofree gchar *joined = NULL;
	gchar *cut;

	if (names->len == 0)
		return NULL;
	g_ptr_array_add(names, NULL);
	joined = g_strjoinv(",", (gchar **)names->pdata);
	g_ptr_array_set_size(names, names->len - 1);
	/* Reasons are cleaned names; cut at a character boundary anyway. */
	if (g_utf8_strlen(joined, -1) <= AXIS_COMPAT_MAX_REASON_CHARS)
		return g_steal_pointer(&joined);
	cut = g_utf8_offset_to_pointer(joined, AXIS_COMPAT_MAX_REASON_CHARS);
	return g_strndup(joined, (gsize)(cut - joined));
}

/* The evidence of one kind of stream when the server states reasons for
 * the conversion: each reason through the table.  The streams the server
 * passed through are passed through; the streams it converted get what
 * the reasons that concern them say, or cause unclear when none does. */
static void
add_stated(
	GPtrArray                 *out,
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts,
	gboolean                   remote,
	gint                       kind,
	gboolean                   main_stream
){
	const AxisCompatStream *s = NULL;
	const gchar *source_codec_raw = NULL;
	const gchar *out_codec_raw;
	AxisCompatDecision decided;
	AxisCompatDecision from_session;
	AxisCompatEvidence *ev;
	g_autoptr(GPtrArray) base_names = g_ptr_array_new();
	GPtrArray *property_names[AXIS_PROPERTY_SUBTITLE_FORMAT + 1];
	AxisCompatEffect base_effect = AXIS_EFFECT_CONVERTED_UNCLEAR;
	gboolean converted;
	guint i;
	gint p;

	if (!pick_source(session, facts, kind, &s, &source_codec_raw))
		return;
	out_codec_raw = kind == AXIS_COMPAT_STREAM_VIDEO ? session->transcode.video_codec :
		kind == AXIS_COMPAT_STREAM_AUDIO ? session->transcode.audio_codec : NULL;
	from_session = transcode_decision(session, kind);
	decided = s->decision != AXIS_COMPAT_DECISION_NONE ? s->decision : from_session;
	if (s->decision != AXIS_COMPAT_DECISION_NONE && from_session != AXIS_COMPAT_DECISION_NONE &&
	    s->decision != from_session)
		decided = AXIS_COMPAT_DECISION_UNKNOWN;

	for (p = 0; p <= AXIS_PROPERTY_SUBTITLE_FORMAT; p++)
		property_names[p] = g_ptr_array_new();

	converted = decided == AXIS_COMPAT_DECISION_TRANSCODE;
	if (converted) {
		gboolean have_base = FALSE;
		gboolean have_property = FALSE;
		gboolean burned = FALSE;

		for (i = 0; i < session->transcode.reasons->len; i++) {
			const AxisCompatReason *reason = axis_compat_reason_lookup(g_ptr_array_index(session->transcode.reasons, i));

			burned = burned || (reason != NULL && reason->scope == AXIS_REASON_SUBTITLE);
		}
		for (i = 0; i < session->transcode.reasons->len; i++) {
			const gchar *name = g_ptr_array_index(session->transcode.reasons, i);
			const AxisCompatReason *reason = axis_compat_reason_lookup(name);

			if (reason != NULL && reason->scope == AXIS_REASON_CONTAINER)
				continue;
			if (!axis_compat_reason_applies(reason, kind))
				continue;
			if (reason != NULL && reason->property != AXIS_PROPERTY_NONE) {
				g_ptr_array_add(property_names[reason->property], (gpointer)name);
				have_property = TRUE;
				continue;
			}
			/* A reason the table does not contain is cause unclear. */
			if (!have_base || (reason != NULL && effect_rank(reason->effect) > effect_rank(base_effect)))
				base_effect = reason != NULL ? reason->effect : AXIS_EFFECT_CONVERTED_UNCLEAR;
			have_base = TRUE;
			g_ptr_array_add(base_names, (gpointer)name);
		}
		/* A property blamed explains the conversion; the codec's own row
		 * says nothing when nothing else was stated for it.  A picture
		 * that is re-encoded because a subtitle is burned into it is
		 * explained by the subtitle, and is no mystery of its own. */
		if (!have_base && !have_property && !(kind == AXIS_COMPAT_STREAM_VIDEO && burned)) {
			base_effect = AXIS_EFFECT_CONVERTED_UNCLEAR;
			have_base = TRUE;
		}
		if (have_base) {
			ev = make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw, base_effect,
				AXIS_PROPERTY_NONE);
			if (ev != NULL) {
				ev->stated_reason = joined_reasons(base_names);
				g_ptr_array_add(out, ev);
			}
		}
		for (p = AXIS_PROPERTY_CONTAINER + 1; p <= AXIS_PROPERTY_SUBTITLE_FORMAT; p++) {
			if (property_names[p]->len == 0)
				continue;
			ev = make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw,
				AXIS_EFFECT_CONVERTED_CODEC, (AxisCompatProperty)p);
			if (ev != NULL) {
				ev->stated_reason = joined_reasons(property_names[p]);
				g_ptr_array_add(out, ev);
			}
		}
	} else if (decided == AXIS_COMPAT_DECISION_COPY || decided == AXIS_COMPAT_DECISION_DIRECT) {
		ev = make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw,
			AXIS_EFFECT_PASSTHROUGH, AXIS_PROPERTY_NONE);
		if (ev != NULL)
			g_ptr_array_add(out, ev);
	} else {
		ev = make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw,
			AXIS_EFFECT_UNKNOWN, AXIS_PROPERTY_NONE);
		if (ev != NULL)
			g_ptr_array_add(out, ev);
	}

	/* The container the server says was not taken, shown against the main
	 * stream of the file, converted or not. */
	if (main_stream) {
		for (i = 0; i < session->transcode.reasons->len; i++) {
			const gchar *name = g_ptr_array_index(session->transcode.reasons, i);
			const AxisCompatReason *reason = axis_compat_reason_lookup(name);

			if (reason == NULL || reason->scope != AXIS_REASON_CONTAINER)
				continue;
			ev = make_evidence(session, facts, remote, kind, s, source_codec_raw, out_codec_raw, reason->effect,
				reason->property);
			/* Without a known container there is nothing to blame. */
			if (ev != NULL && ev->container != NULL) {
				ev->stated_reason = g_strdup(name);
				g_ptr_array_add(out, ev);
			} else {
				axis_compat_evidence_free(ev);
			}
			break;
		}
	}
	for (p = 0; p <= AXIS_PROPERTY_SUBTITLE_FORMAT; p++)
		g_ptr_array_unref(property_names[p]);
}

GPtrArray *
axis_compat_classify_session(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts
){
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_evidence_free);
	Context context;
	gboolean remote;
	gboolean stated;
	gboolean main_taken = FALSE;
	gint64 source_bitrate;
	gint kind;

	g_return_val_if_fail(session != NULL, out);

	remote = !(session->player_local == AXIS_COMPAT_YES || g_strcmp0(session->location, "lan") == 0);
	source_bitrate = facts != NULL && facts->bitrate_kbps > 0 ? facts->bitrate_kbps : session->media_bitrate_kbps;
	context.limited = remote && session->bandwidth_kbps > 0 && source_bitrate > 0 &&
		session->bandwidth_kbps < source_bitrate;
	stated = session->transcode.present && session->transcode.reasons != NULL &&
		session->transcode.reasons->len > 0;

	if (session->ambiguous)
		return out;
	for (kind = AXIS_COMPAT_STREAM_VIDEO; kind <= AXIS_COMPAT_STREAM_SUBTITLE; kind++) {
		if (stated) {
			const AxisCompatStream *s = NULL;
			const gchar *codec = NULL;
			gboolean main_stream = !main_taken && kind != AXIS_COMPAT_STREAM_SUBTITLE &&
				pick_source(session, facts, kind, &s, &codec);

			main_taken = main_taken || main_stream;
			add_stated(out, session, facts, remote, kind, main_stream);
		} else {
			AxisCompatEvidence *ev = evidence_for(session, facts, &context, remote, kind);

			if (ev != NULL)
				g_ptr_array_add(out, ev);
		}
	}
	return out;
}

/*
 * axis-profile-lock.c - the lock rule: which differences between two
 * definitions keep a revision's proof
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A revision is proven on real devices for the bytes it produces.  A new
 * revision keeps that proof only when nothing that can change those bytes
 * changed.  Every member of the format is in the table below with its
 * class; a member missing from it, or a change of kind, locks.  The
 * classes are deliberately cautious:
 *
 *  - output: codec, bit depth, HDR, encoders (another encoder makes other
 *    bytes), the quality rule, audio handling and the stereo bitrate, the
 *    languages kept (dropping or keeping a track changes what a device is
 *    offered, so either direction locks), subtitles, container, and every
 *    audio encoding member;
 *  - selection: the minimum saving and the "already efficient" threshold
 *    change only which files are converted, never what is made of one;
 *  - narrowing: fewer libraries, or no longer allowing lossy sources,
 *    carries the proof; more libraries, or newly allowing lossy sources,
 *    locks (the proof was made on what the revision converted then).
 */

#include "transcode/axis-profile.h"

#include <string.h>

static const AxisProfileMember members[] = {
	{ "kind", "Kind", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "codec", "Codec", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "bit_depth", "Bit depth", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "hdr", "HDR", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "encoders", "Encoders", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "quality", "Quality rule", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "audio", "Audio handling", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "stereo_bitrate", "Stereo track bitrate", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "keep_languages", "Audio languages kept", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "subtitles", "Subtitles", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "container", "Container", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "bitrate", "Bitrate", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "mono_bitrate", "Bitrate for mono sources", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "vbr_quality", "MP3 quality", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "reduce_to_cd", "Reduce to CD quality", AXIS_PROFILE_MEMBER_OUTPUT },
	{ "libraries", "Libraries", AXIS_PROFILE_MEMBER_NARROWING },
	{ "allow_lossy_source", "Lossy sources", AXIS_PROFILE_MEMBER_NARROWING },
	{ "efficient_bpp_milli", "Already efficient at", AXIS_PROFILE_MEMBER_SELECTION },
	{ "efficient_percent", "Already efficient at", AXIS_PROFILE_MEMBER_SELECTION },
	{ "min_saving_percent", "Minimum saving", AXIS_PROFILE_MEMBER_SELECTION },
	{ "min_saving_bytes", "Minimum saving in bytes", AXIS_PROFILE_MEMBER_SELECTION },
	{ "format", "Format", AXIS_PROFILE_MEMBER_OUTPUT },
	{ NULL, NULL, AXIS_PROFILE_MEMBER_OUTPUT }
};

const AxisProfileMember *
axis_profile_members(void)
{
	return members;
}

static const AxisProfileMember *
find(const gchar *name)
{
	guint i;

	for (i = 0; members[i].name != NULL; i++) {
		if (strcmp(members[i].name, name) == 0)
			return &members[i];
	}
	return NULL;
}

static gboolean
same_languages(
	const AxisProfileDefinition *a,
	const AxisProfileDefinition *b
){
	AxisProfileDefinition x = *a;
	AxisProfileDefinition y = *b;
	g_autofree gchar *cx = NULL;
	g_autofree gchar *cy = NULL;

	/* Compared in normal order, through the canonical text of a definition
	 * that differs in nothing else. */
	memset(&x, 0, sizeof(x));
	memset(&y, 0, sizeof(y));
	memcpy(x.languages, a->languages, sizeof(a->languages));
	memcpy(y.languages, b->languages, sizeof(b->languages));
	x.n_languages = a->n_languages;
	y.n_languages = b->n_languages;
	x.vbr_quality = y.vbr_quality = -1;
	cx = axis_profile_definition_canonical(&x);
	cy = axis_profile_definition_canonical(&y);
	return strcmp(cx, cy) == 0;
}

static gboolean
same_quality(
	const AxisProfileDefinition *a,
	const AxisProfileDefinition *b
){
	AxisProfileDefinition x;
	AxisProfileDefinition y;
	g_autofree gchar *cx = NULL;
	g_autofree gchar *cy = NULL;

	memset(&x, 0, sizeof(x));
	memset(&y, 0, sizeof(y));
	x.crf = a->crf;
	y.crf = b->crf;
	x.n_rules = MIN(a->n_rules, AXIS_PROFILE_MAX_RULES);
	y.n_rules = MIN(b->n_rules, AXIS_PROFILE_MAX_RULES);
	memcpy(x.rules, a->rules, sizeof(a->rules));
	memcpy(y.rules, b->rules, sizeof(b->rules));
	x.vbr_quality = y.vbr_quality = -1;
	cx = axis_profile_definition_canonical(&x);
	cy = axis_profile_definition_canonical(&y);
	return strcmp(cx, cy) == 0;
}

static gboolean
same_encoders(
	const AxisProfileDefinition *a,
	const AxisProfileDefinition *b
){
	guint i;

	if (a->n_encoders != b->n_encoders)
		return FALSE;
	for (i = 0; i < a->n_encoders && i < AXIS_ENCODER_COUNT; i++) {
		if (a->encoders[i] != b->encoders[i])
			return FALSE;
	}
	return TRUE;
}

static void
note(
	GPtrArray   *changed,
	const gchar *name
){
	const AxisProfileMember *m = find(name);

	if (changed == NULL)
		return;
	/* A locking change first, so that the first one names the reason. */
	if (m == NULL || m->klass == AXIS_PROFILE_MEMBER_OUTPUT)
		g_ptr_array_insert(changed, 0, (gpointer)name);
	else
		g_ptr_array_add(changed, (gpointer)name);
}

gboolean
axis_profile_carries_proof(
	const AxisProfileDefinition *before,
	const AxisProfileDefinition *after,
	GPtrArray                   *changed
){
	gboolean carries = TRUE;

	g_return_val_if_fail(before != NULL && after != NULL, FALSE);

	if (before->kind != after->kind) {
		note(changed, "kind");
		return FALSE;
	}

	/* Output: any difference locks. */
#define OUTPUT(name, differs) do { if (differs) { note(changed, name); carries = FALSE; } } while (0)
	OUTPUT("codec", before->codec != after->codec);
	OUTPUT("bit_depth", before->bit_depth != after->bit_depth);
	OUTPUT("hdr", before->keep_hdr != after->keep_hdr);
	OUTPUT("encoders", !same_encoders(before, after));
	OUTPUT("quality", !same_quality(before, after));
	OUTPUT("audio", before->audio != after->audio);
	OUTPUT("stereo_bitrate", before->stereo_bitrate != after->stereo_bitrate);
	OUTPUT("keep_languages", !same_languages(before, after));
	OUTPUT("subtitles", before->subtitles != after->subtitles);
	OUTPUT("container", before->container != after->container);
	OUTPUT("bitrate", before->bitrate != after->bitrate);
	OUTPUT("mono_bitrate", before->mono_bitrate != after->mono_bitrate);
	OUTPUT("vbr_quality", before->vbr_quality != after->vbr_quality);
	OUTPUT("reduce_to_cd", before->reduce_to_cd != after->reduce_to_cd);
#undef OUTPUT

	/* Narrowing: only fewer carries. */
	if (before->libraries != after->libraries) {
		note(changed, "libraries");
		if ((after->libraries & ~before->libraries) != 0)
			carries = FALSE;
	}
	if (before->allow_lossy != after->allow_lossy) {
		note(changed, "allow_lossy_source");
		if (after->allow_lossy)
			carries = FALSE;
	}

	/* Selection: carries. */
	if (before->efficient_milli != after->efficient_milli)
		note(changed, "efficient_bpp_milli");
	if (before->efficient_percent != after->efficient_percent)
		note(changed, "efficient_percent");
	if (before->min_saving_percent != after->min_saving_percent)
		note(changed, "min_saving_percent");
	if (before->min_saving_bytes != after->min_saving_bytes)
		note(changed, "min_saving_bytes");

	/* Belt and braces: if the canonical texts differ in a way none of the
	 * above saw, that is a member this table does not know: lock. */
	if (carries) {
		AxisProfileDefinition x = *before;
		AxisProfileDefinition y = *after;
		g_autofree gchar *cx = NULL;
		g_autofree gchar *cy = NULL;

		y.libraries = x.libraries;
		y.allow_lossy = x.allow_lossy;
		y.efficient_milli = x.efficient_milli;
		y.efficient_percent = x.efficient_percent;
		y.min_saving_percent = x.min_saving_percent;
		y.min_saving_bytes = x.min_saving_bytes;
		cx = axis_profile_definition_canonical(&x);
		cy = axis_profile_definition_canonical(&y);
		if (strcmp(cx, cy) != 0) {
			note(changed, "format");
			carries = FALSE;
		}
	}
	return carries;
}

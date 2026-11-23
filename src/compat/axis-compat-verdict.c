/*
 * axis-compat-verdict.c - the "harder than" rules and the verdicts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-verdict.h"

#include <string.h>

const gchar *
axis_compat_verdict_to_string(AxisCompatVerdict verdict)
{
	switch (verdict) {
	case AXIS_VERDICT_SAFE:
		return "safe";
	case AXIS_VERDICT_RISKY:
		return "risky";
	default:
		return "unknown";
	}
}

const gchar *
axis_compat_verdict_label(AxisCompatVerdict verdict)
{
	switch (verdict) {
	case AXIS_VERDICT_SAFE:
		return "Safe";
	case AXIS_VERDICT_RISKY:
		return "Risky";
	default:
		return "Unknown";
	}
}

AxisCompatVerdict
axis_compat_verdict_worse(
	AxisCompatVerdict a,
	AxisCompatVerdict b
){
	if (a == AXIS_VERDICT_RISKY || b == AXIS_VERDICT_RISKY)
		return AXIS_VERDICT_RISKY;
	if (a == AXIS_VERDICT_UNKNOWN || b == AXIS_VERDICT_UNKNOWN)
		return AXIS_VERDICT_UNKNOWN;
	return AXIS_VERDICT_SAFE;
}

const gchar *
axis_compat_why_label(AxisCompatWhy why)
{
	switch (why) {
	case AXIS_WHY_DIRECT:
		return "seen playing it directly";
	case AXIS_WHY_CONVERTED:
		return "seen having the codec converted for it";
	case AXIS_WHY_EASIER_ONLY:
		return "only easier combinations of this codec seen";
	case AXIS_WHY_PASSTHROUGH_ONLY:
		return "the codec was seen passed through, the container never played directly";
	case AXIS_WHY_LIMITED_ONLY:
		return "only conversions explained by a bandwidth or quality limit seen";
	case AXIS_WHY_UNCLEAR_ONLY:
		return "only conversions of unclear cause seen";
	case AXIS_WHY_PROPERTY_ONLY:
		return "only conversions blamed on another property (container, profile, bit depth) seen";
	default:
		return "never seen with this codec";
	}
}

gint
axis_compat_resolution_rank(const gchar *res_class)
{
	if (res_class == NULL)
		return 0;
	if (strcmp(res_class, "sd") == 0)
		return 1;
	if (strcmp(res_class, "720p") == 0)
		return 2;
	if (strcmp(res_class, "1080p") == 0)
		return 3;
	if (strcmp(res_class, "2160p") == 0)
		return 4;
	return 0;
}

void
axis_compat_evidence_combo(
	const AxisCompatEvidence *evidence,
	AxisCompatCombo          *combo
){
	combo->kind = evidence->kind;
	combo->codec = evidence->codec;
	combo->profile = evidence->profile;
	combo->bit_depth = evidence->bit_depth;
	combo->res_class = evidence->res_class;
	combo->hdr = evidence->hdr;
	combo->container = evidence->container;
}

static gboolean
same_text(
	const gchar *a,
	const gchar *b
){
	return a != NULL && b != NULL && g_ascii_strcasecmp(a, b) == 0;
}

static gboolean
covers_impl(
	const AxisCompatCombo *have,
	const AxisCompatCombo *want,
	gboolean               check_container
){
	if (have->kind != want->kind || !same_text(have->codec, want->codec))
		return FALSE;
	if (check_container && want->container != NULL && !same_text(have->container, want->container))
		return FALSE;
	if (want->profile != NULL && !same_text(have->profile, want->profile))
		return FALSE;
	if (want->bit_depth > 0 && (have->bit_depth <= 0 || have->bit_depth < want->bit_depth))
		return FALSE;
	if (want->res_class != NULL) {
		gint need = axis_compat_resolution_rank(want->res_class);
		gint got = axis_compat_resolution_rank(have->res_class);

		if (need == 0 || got == 0 || got < need)
			return FALSE;
	}
	if (want->hdr >= 0 && have->hdr != want->hdr)
		return FALSE;
	return TRUE;
}

gboolean
axis_compat_covers(
	const AxisCompatCombo *have,
	const AxisCompatCombo *want
){
	return covers_impl(have, want, TRUE);
}

gboolean
axis_compat_covers_codec(
	const AxisCompatCombo *have,
	const AxisCompatCombo *want
){
	return want->container == NULL && covers_impl(have, want, FALSE);
}

gboolean
axis_compat_fails(
	const AxisCompatCombo *failed,
	const AxisCompatCombo *want
){
	if (failed->kind != want->kind || !same_text(failed->codec, want->codec))
		return FALSE;
	if (failed->bit_depth > 0 && (want->bit_depth <= 0 || want->bit_depth < failed->bit_depth))
		return FALSE;
	if (failed->res_class != NULL) {
		gint was = axis_compat_resolution_rank(failed->res_class);
		gint is = axis_compat_resolution_rank(want->res_class);

		if (was == 0 || is == 0 || is < was)
			return FALSE;
	}
	if (failed->hdr == 1 && want->hdr != 1)
		return FALSE;
	return TRUE;
}

gboolean
axis_compat_fails_property(
	const AxisCompatCombo *failed,
	AxisCompatProperty     property,
	const AxisCompatCombo *want
){
	if (property == AXIS_PROPERTY_NONE)
		return axis_compat_fails(failed, want);
	if (failed->kind != want->kind || !same_text(failed->codec, want->codec))
		return FALSE;
	switch (property) {
	case AXIS_PROPERTY_CONTAINER:
		/* Not taking mkv shows nothing about mp4. */
		return same_text(failed->container, want->container);
	case AXIS_PROPERTY_PROFILE:
		return same_text(failed->profile, want->profile);
	case AXIS_PROPERTY_BIT_DEPTH:
		return failed->bit_depth > 0 && want->bit_depth >= failed->bit_depth;
	case AXIS_PROPERTY_SUBTITLE_FORMAT:
		return TRUE;
	default:
		/* A level is named by no combination. */
		return FALSE;
	}
}

/* How much a reason says about the codec; the most informative is shown. */
static gint
why_rank(AxisCompatWhy why)
{
	switch (why) {
	case AXIS_WHY_EASIER_ONLY:
		return 4;
	case AXIS_WHY_PASSTHROUGH_ONLY:
		return 3;
	case AXIS_WHY_UNCLEAR_ONLY:
		return 2;
	case AXIS_WHY_LIMITED_ONLY:
	case AXIS_WHY_PROPERTY_ONLY:
		return 1;
	default:
		return 0;
	}
}

AxisCompatVerdict
axis_compat_device_verdict(
	const AxisCompatCombo       *want,
	const AxisCompatObservation *obs,
	guint                        n,
	AxisCompatWhy               *why,
	gint                        *deciding
){
	gint risky_at = -1;
	gint safe_at = -1;
	AxisCompatWhy reason = AXIS_WHY_NO_EVIDENCE;
	guint i;

	for (i = 0; i < n; i++) {
		const AxisCompatObservation *o = &obs[i];

		switch (o->effect) {
		case AXIS_EFFECT_CONVERTED_CODEC:
			if (risky_at < 0 && axis_compat_fails_property(&o->combo, o->property, want))
				risky_at = (gint)i;
			break;
		case AXIS_EFFECT_DIRECT:
			if (safe_at < 0 && axis_compat_covers(&o->combo, want))
				safe_at = (gint)i;
			break;
		case AXIS_EFFECT_PASSTHROUGH:
			if (safe_at < 0 && axis_compat_covers_codec(&o->combo, want))
				safe_at = (gint)i;
			break;
		default:
			break;
		}
	}
	if (risky_at >= 0) {
		if (why != NULL)
			*why = AXIS_WHY_CONVERTED;
		if (deciding != NULL)
			*deciding = risky_at;
		return AXIS_VERDICT_RISKY;
	}
	if (safe_at >= 0) {
		if (why != NULL)
			*why = AXIS_WHY_DIRECT;
		if (deciding != NULL)
			*deciding = safe_at;
		return AXIS_VERDICT_SAFE;
	}

	/* Unknown: say what was seen, the most informative first. */
	for (i = 0; i < n; i++) {
		const AxisCompatObservation *o = &obs[i];
		AxisCompatWhy found;

		if (o->combo.kind != want->kind || !same_text(o->combo.codec, want->codec))
			continue;
		switch (o->effect) {
		case AXIS_EFFECT_DIRECT:
			found = AXIS_WHY_EASIER_ONLY;
			break;
		case AXIS_EFFECT_CONVERTED_CODEC:
			found = o->property == AXIS_PROPERTY_NONE ? AXIS_WHY_EASIER_ONLY : AXIS_WHY_PROPERTY_ONLY;
			break;
		case AXIS_EFFECT_PASSTHROUGH:
			found = AXIS_WHY_PASSTHROUGH_ONLY;
			break;
		case AXIS_EFFECT_CONVERTED_UNCLEAR:
			found = AXIS_WHY_UNCLEAR_ONLY;
			break;
		case AXIS_EFFECT_CONVERTED_LIMIT:
			found = AXIS_WHY_LIMITED_ONLY;
			break;
		default:
			continue;
		}
		if (why_rank(found) > why_rank(reason))
			reason = found;
	}
	if (why != NULL)
		*why = reason;
	if (deciding != NULL)
		*deciding = -1;
	return AXIS_VERDICT_UNKNOWN;
}

gint
axis_compat_recommend(
	const AxisCompatChoice *choices,
	guint                   n,
	gint                   *best,
	gint                   *next
){
	gint recommended = -1;
	gint best_i = -1;
	gint next_i = -1;
	guint i;

	for (i = 0; i < n; i++) {
		if (choices[i].saving <= 0)
			continue;
		if (choices[i].verdict == AXIS_VERDICT_SAFE &&
		    (recommended < 0 || choices[i].saving > choices[recommended].saving))
			recommended = (gint)i;
		if (best_i < 0 || choices[i].saving > choices[best_i].saving) {
			next_i = best_i;
			best_i = (gint)i;
		} else if (next_i < 0 || choices[i].saving > choices[next_i].saving) {
			next_i = (gint)i;
		}
	}
	if (best != NULL)
		*best = best_i;
	if (next != NULL)
		*next = next_i;
	return recommended;
}

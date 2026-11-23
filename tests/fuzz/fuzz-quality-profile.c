/*
 * fuzz-quality-profile.c - libFuzzer target for quality profile definitions
 * and the decision made with them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every input is tried two ways:
 *
 *  - as a definition's text (stored rows are outside input): the reader
 *    refuses it with at least one problem, each with a message, and returns
 *    nothing of it; or accepts it, and then its canonical text parses again
 *    to the same profile and the same text (a fixed point), and a decision
 *    made with it keeps the rules below;
 *  - as the bytes of a generated valid profile, a context (runtime, a file
 *    on disk, protocol, retention, players) and a few release names, some
 *    made of release words and one of the raw bytes.
 *
 * The rules every decision keeps: one verdict per release, the order a
 * permutation with the accepted first, ranked 1..n in key order; an accepted
 * release has no reason, a rejected one at least one, each a sentence; a
 * sample, a password, an archive or a program, no title, or a match that is
 * not "match" is never accepted, whatever the profile; nor an unwanted
 * quality without its switch, an unknown one without its, or one the
 * profile does not allow; with a file at or above the cutoff nothing is.
 * A broken property is printed before the abort.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quality/axis-quality.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
broken(
	const gchar *what,
	const gchar *text
){
	g_printerr("fuzz-quality-profile: %s: %s\n", what, text != NULL ? text : "(none)");
	abort();
}

/* ── The decision's rules ───────────────────────────────────────── */

static gboolean
never_acceptable(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *c
){
	const AxisRelease *r = c->release;
	const AxisQualityProfile *p = ctx->profile;
	AxisQuality q = axis_quality_of_release(r);

	if ((r->stop & (AXIS_RELEASE_STOP_SAMPLE | AXIS_RELEASE_STOP_PASSWORD | AXIS_RELEASE_STOP_ARCHIVE |
	                AXIS_RELEASE_STOP_EXECUTABLE | AXIS_RELEASE_STOP_OBFUSCATED | AXIS_RELEASE_STOP_SUBJECT)) != 0 ||
	    r->title == NULL)
		return TRUE;
	if (!ctx->no_title && c->match != AXIS_RELEASE_MATCH_YES)
		return TRUE;
	if (q == AXIS_QUALITY_UNKNOWN)
		return !p->allow_unknown;
	if (axis_quality_is_unwanted(q) && !p->allow_unwanted)
		return TRUE;
	return axis_quality_profile_tier(p, q) < 0;
}

static void
check_decision(
	const AxisQualityContext   *ctx,
	const AxisQualityCandidate *cands,
	guint                       n
){
	g_autoptr(AxisQualityDecision) d = axis_quality_decide(ctx, cands, n);
	g_autofree gboolean *seen = g_new0(gboolean, n + 1);
	guint i;
	guint k;

	if (d->verdicts == NULL || d->verdicts->len != n || d->n_accepted > n)
		broken("not one verdict per release", NULL);
	for (i = 0; i < n; i++) {
		const AxisQualityVerdict *v;

		if (d->order[i] >= n || seen[d->order[i]])
			broken("the order is not a permutation", NULL);
		seen[d->order[i]] = TRUE;
		v = g_ptr_array_index(d->verdicts, d->order[i]);
		if (i < d->n_accepted) {
			if (!v->accepted || v->rank != i + 1)
				broken("an accepted release is out of rank", cands[d->order[i]].name);
			if (i > 0) {
				const AxisQualityVerdict *above = g_ptr_array_index(d->verdicts, d->order[i - 1]);
				g_autofree gchar *why = axis_quality_explain_rank(ctx->profile, above, v);

				if (axis_quality_keys_compare(&above->keys, &v->keys, NULL) > 0)
					broken("the accepted are not in key order", cands[d->order[i]].name);
				if (why == NULL || why[0] == '\0')
					broken("a rank without a reason", NULL);
			}
		} else if (v->accepted || v->rank != 0) {
			broken("a rejected release has a rank", cands[d->order[i]].name);
		}
	}
	for (i = 0; i < n; i++) {
		const AxisQualityVerdict *v = g_ptr_array_index(d->verdicts, i);

		if (v->accepted && v->reasons->len != 0)
			broken("accepted with a reason", cands[i].name);
		if (!v->accepted && v->reasons->len == 0)
			broken("rejected without a reason", cands[i].name);
		if (v->accepted && never_acceptable(ctx, &cands[i]))
			broken("a release no profile may take was accepted", cands[i].name);
		if (v->accepted && d->cutoff_reached)
			broken("accepted although the file on disk is at the cutoff", cands[i].name);
		if (v->accepted && ctx->disk != NULL && !v->upgrade)
			broken("accepted over a file on disk without being an upgrade", cands[i].name);
		for (k = 0; k < v->reasons->len; k++) {
			const AxisQualityReason *r = g_ptr_array_index(v->reasons, k);

			if (r->code == NULL || r->code[0] == '\0' || r->text == NULL || r->text[0] == '\0' ||
			    !g_utf8_validate(r->text, -1, NULL))
				broken("a reason that is not a sentence", cands[i].name);
		}
		for (k = 0; k < v->lines->len; k++) {
			const AxisQualityScoreLine *l = g_ptr_array_index(v->lines, k);

			if (l->text == NULL || !g_utf8_validate(l->text, -1, NULL))
				broken("a score line that is not text", cands[i].name);
		}
	}
}

/* ── Input as bytes ─────────────────────────────────────────────── */

typedef struct
{
	const uint8_t *data;
	size_t         size;
	size_t         at;
} Input;

static guint
pick(
	Input *in,
	guint  n
){
	guint value;

	if (n <= 1)
		return 0;
	if (in->at + 2 > in->size) {
		in->at = in->size;
		return 0;
	}
	value = ((guint)in->data[in->at] << 8) | in->data[in->at + 1];
	in->at += 2;
	return value % n;
}

static AxisCompatVerdict
players(
	const AxisCompatCombo *want,
	gpointer               user_data
){
	guint seed = GPOINTER_TO_UINT(user_data);

	return (AxisCompatVerdict)((g_str_hash(want->codec != NULL ? want->codec : "") + seed) % 3);
}

static void
generate_rule(
	Input           *in,
	AxisQualityRule *rule
){
	static const gchar *const groups[] = { "GRP", "FLUX", "yify", "D-Z0N3", NULL };
	static const gchar *const words[] = { "UPSCALED", "HC", "x264", "Film", NULL };
	static const gchar *const channels[] = { "2.0", "5.1", "7.1", "7.1.4", NULL };
	static const gchar *const languages[] = { "de", "fr", "en", "ja", NULL };
	static const gchar *const services[] = { "amazon", "netflix", "disney", NULL };
	const gchar *const *values;
	guint n;
	guint i;

	memset(rule, 0, sizeof(*rule));
	rule->condition = (AxisQualityCondition)pick(in, AXIS_QUALITY_N_CONDITIONS);
	rule->effect = (AxisQualityEffect)pick(in, AXIS_QUALITY_N_EFFECTS);
	if (rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID)
		rule->weight = 1 + (gint)pick(in, AXIS_QUALITY_MAX_WEIGHT);
	if (!axis_quality_condition_has_values(rule->condition))
		return;
	switch (rule->condition) {
	case AXIS_QUALITY_IF_GROUP:    values = groups; break;
	case AXIS_QUALITY_IF_WORD:     values = words; break;
	case AXIS_QUALITY_IF_CHANNELS: values = channels; break;
	case AXIS_QUALITY_IF_LANGUAGE: values = languages; break;
	case AXIS_QUALITY_IF_SERVICE:  values = services; break;
	default:                       values = axis_quality_condition_values(rule->condition); break;
	}
	for (n = 0; values[n] != NULL; n++)
		;
	/* A few distinct values of the set. */
	for (i = 0; i < n && rule->n_values < AXIS_QUALITY_MAX_VALUES; i++) {
		if (pick(in, 3) == 0 || (i + 1 == n && rule->n_values == 0))
			g_strlcpy(rule->values[rule->n_values++], values[i], sizeof(rule->values[0]));
	}
}

/* A valid profile made from the bytes. */
static void
generate_profile(
	Input              *in,
	AxisQualityProfile *p
){
	AxisQuality pool[AXIS_QUALITY_N_LADDER];
	guint n;
	guint i;

	axis_quality_profile_init(p);
	for (i = 0; i < AXIS_QUALITY_N_LADDER; i++)
		pool[i] = (AxisQuality)(i + 1);
	n = 1 + pick(in, AXIS_QUALITY_N_LADDER);
	for (i = 0; i < n; i++) {
		guint j = i + pick(in, AXIS_QUALITY_N_LADDER - i);
		AxisQuality swap = pool[i];

		pool[i] = pool[j];
		pool[j] = swap;
		p->allowed[i] = pool[i];
		p->tier[i] = i == 0 ? 0 : p->tier[i - 1] + pick(in, 2);
	}
	p->n_allowed = n;
	p->cutoff = p->allowed[pick(in, n)];
	p->allow_unknown = pick(in, 2);
	p->allow_unwanted = pick(in, 2);
	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++) {
		p->sizes[i].min = (gint)pick(in, 40);
		p->sizes[i].max = pick(in, 3) == 0 ? 0 : p->sizes[i].min + 1 + (gint)pick(in, 600);
	}
	p->max_size_mb = pick(in, 2) == 0 ? 0 : 1 + pick(in, 100000);
	p->n_rules = pick(in, 7);
	for (i = 0; i < p->n_rules; i++)
		generate_rule(in, &p->rules[i]);
	p->upgrades = pick(in, 4) != 0;
	p->upgrade_revisions = pick(in, 2);
	p->upgrade_score = pick(in, 2);
	p->min_score_gain = p->upgrade_score ? 1 + (gint)pick(in, 300) : 0;
	p->players = pick(in, 2) == 0 ? 0 : 1 + (gint)pick(in, AXIS_QUALITY_MAX_WEIGHT);
	/* Two rules that came out the same: keep the first few that check. */
	while (!axis_quality_profile_check(p, NULL) && p->n_rules > 0)
		p->n_rules--;
	if (!axis_quality_profile_check(p, NULL))
		broken("a generated profile does not check", NULL);
}

/* A release name of words a name is made of. */
static gchar *
generate_name(Input *in)
{
	static const gchar *const parts[][8] = {
		{ "Some.Film", "Another.Film", "The.Film", "Film", "", NULL },
		{ ".2021", ".1999", "", NULL },
		{ ".PROPER", ".REPACK", ".REAL.PROPER", ".Extended", ".GERMAN", ".MULTi", ".3D", "", },
		{ ".480p", ".576p", ".720p", ".1080p", ".1080i", ".2160p", "", NULL },
		{ ".WEB-DL", ".WEBRip", ".BluRay", ".BluRay.REMUX", ".HDTV", ".DVDRip", ".HDCAM", ".TS" },
		{ ".DV", ".HDR", ".DV.HDR", ".HDR10Plus", ".10bit", ".HC", ".AMZN", "" },
		{ ".DDP5.1", ".TrueHD.7.1.Atmos", ".DTS-HD.MA.5.1", ".AAC2.0", "", NULL },
		{ ".x264", ".x265", ".H.264", ".AV1", "", NULL },
		{ "-GRP", "-FLUX", "-YIFY", "-sample", ".rar", ".exe", "", NULL }
	};
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(parts); i++) {
		guint n;

		for (n = 0; n < 8 && parts[i][n] != NULL; n++)
			;
		g_string_append(out, parts[i][pick(in, n)]);
	}
	return g_string_free(out, FALSE);
}

static void
as_generated(
	const uint8_t *data,
	size_t         size
){
	Input in = { data, size, 0 };
	AxisQualityProfile profile;
	AxisQualityContext ctx;
	AxisQualityTitle title;
	AxisQualityOnDisk disk;
	AxisRelease *releases[6];
	gchar *names[6];
	AxisQualityCandidate cands[6];
	AxisRelease *disk_fields = NULL;
	gchar *disk_name = NULL;
	guint n;
	guint i;

	generate_profile(&in, &profile);
	memset(&ctx, 0, sizeof(ctx));
	memset(&title, 0, sizeof(title));
	memset(&disk, 0, sizeof(disk));
	ctx.profile = &profile;
	ctx.title = &title;
	title.runtime_minutes = pick(&in, 2) == 0 ? 0 : 1 + (gint)pick(&in, 400);
	title.noun = pick(&in, 2) == 0 ? "film" : "episode";
	if (pick(&in, 2) == 0) {
		static const gchar *const english[] = { "en", NULL };

		title.languages = english;
	}
	if (pick(&in, 2) == 0) {
		disk_name = generate_name(&in);
		disk_fields = axis_release_parse_full(disk_name, -1, 2027);
		disk.quality = (AxisQuality)pick(&in, AXIS_QUALITY_N_LADDER + 1);
		disk.fields = disk_fields;
		disk.name = disk_name;
		disk.converted = pick(&in, 3) == 0;
		disk.source_quality = (AxisQuality)pick(&in, AXIS_QUALITY_N_LADDER + 1);
		ctx.disk = &disk;
	}
	ctx.protocol = (AxisQualityProtocolPreference)pick(&in, 3);
	ctx.retention_days = (gint)pick(&in, 4000);
	ctx.now_us = G_GINT64_CONSTANT(1800000000000000);
	ctx.no_title = pick(&in, 4) == 0;
	if (pick(&in, 2) == 0) {
		ctx.players = players;
		ctx.players_data = GUINT_TO_POINTER(pick(&in, 3));
	}

	n = 1 + pick(&in, 5);
	for (i = 0; i < n; i++) {
		names[i] = generate_name(&in);
		releases[i] = axis_release_parse_full(names[i], -1, 2027);
		memset(&cands[i], 0, sizeof(cands[i]));
		cands[i].release = releases[i];
		cands[i].name = names[i];
		cands[i].match = (AxisReleaseMatch)pick(&in, 3);
		cands[i].size_bytes = pick(&in, 5) == 0 ? -1 : (gint64)pick(&in, 60000) * 1000000;
		cands[i].protocol = pick(&in, 2) == 0 ? AXIS_DOWNLOAD_USENET : AXIS_DOWNLOAD_TORRENT;
		cands[i].seeders = (gint)pick(&in, 300) - 1;
		cands[i].published_us = pick(&in, 2) == 0 ? 0 : ctx.now_us - (gint64)pick(&in, 6000) * 86400 * 1000000;
	}
	/* And the bytes themselves, as a name. */
	names[n] = g_strndup((const gchar *)data, MIN(size, 600));
	releases[n] = axis_release_parse_full(names[n], -1, 2027);
	memset(&cands[n], 0, sizeof(cands[n]));
	cands[n].release = releases[n];
	cands[n].name = names[n];
	cands[n].match = AXIS_RELEASE_MATCH_YES;
	cands[n].size_bytes = -1;
	n++;

	check_decision(&ctx, cands, n);
	for (i = 0; i < n; i++) {
		axis_release_free(releases[i]);
		g_free(names[i]);
	}
	axis_release_free(disk_fields);
	g_free(disk_name);
}

/* ── Input as a definition ──────────────────────────────────────── */

static void
as_text(
	const uint8_t *data,
	size_t         size
){
	g_autoptr(GPtrArray) problems = axis_quality_problems_new();
	g_autoptr(GPtrArray) facts = NULL;
	AxisQualityProfile profile;
	AxisQualityProfile again;
	AxisQualityProfile zero;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	guint i;

	memset(&zero, 0, sizeof(zero));
	if (!axis_quality_profile_parse((const gchar *)data, (gssize)size, &profile, problems)) {
		if (problems->len == 0)
			broken("refused without a problem", NULL);
		for (i = 0; i < problems->len; i++) {
			const AxisQualityProblem *p = g_ptr_array_index(problems, i);

			if (p->message == NULL || p->message[0] == '\0' || p->field == NULL)
				broken("a problem without a message", NULL);
		}
		if (memcmp(&profile, &zero, sizeof(zero)) != 0)
			broken("a refused definition left something behind", NULL);
		return;
	}
	if (problems->len != 0)
		broken("accepted with a problem", NULL);
	first = axis_quality_profile_canonical(&profile);
	if (!axis_quality_profile_parse(first, -1, &again, NULL))
		broken("the canonical text of a valid profile is refused", first);
	second = axis_quality_profile_canonical(&again);
	if (strcmp(first, second) != 0)
		broken("the canonical text is not a fixed point", second);
	if (memcmp(&profile, &again, sizeof(profile)) != 0)
		broken("parsing the canonical text gives another profile", first);
	if (strlen(first) > AXIS_QUALITY_MAX_TEXT)
		broken("the canonical text is longer than the format allows", first);
	if (axis_quality_profile_format_of(first, -1) != AXIS_QUALITY_FORMAT)
		broken("the canonical text does not say its format", first);
	facts = axis_quality_profile_describe(&profile);
	if (facts->len == 0)
		broken("a profile with nothing to say", first);
	for (i = 0; i < profile.n_rules; i++) {
		g_autofree gchar *words = axis_quality_rule_describe(&profile.rules[i]);

		if (words == NULL || !g_utf8_validate(words, -1, NULL))
			broken("a rule without words", first);
	}

	/* A decision with it, on a few fixed names. */
	{
		static const gchar *const fixed[] = {
			"Some.Film.2021.1080p.WEB-DL.DDP5.1.H.264-GRP",
			"Some.Film.2021.2160p.UHD.BluRay.REMUX.DV.HDR.HEVC.TrueHD.7.1.Atmos-GRP",
			"Some.Film.2021.HDCAM.x264-GRP",
			"Some.Film.2021.1080p.BluRay.x264-GRP-sample",
			"Some.Film.2021.x264-GRP"
		};
		AxisRelease *releases[G_N_ELEMENTS(fixed)];
		AxisQualityCandidate cands[G_N_ELEMENTS(fixed)];
		AxisQualityContext ctx;
		AxisQualityTitle title;

		memset(&ctx, 0, sizeof(ctx));
		memset(&title, 0, sizeof(title));
		title.runtime_minutes = 100;
		ctx.profile = &profile;
		ctx.title = &title;
		for (i = 0; i < G_N_ELEMENTS(fixed); i++) {
			releases[i] = axis_release_parse_full(fixed[i], -1, 2027);
			memset(&cands[i], 0, sizeof(cands[i]));
			cands[i].release = releases[i];
			cands[i].name = fixed[i];
			cands[i].match = AXIS_RELEASE_MATCH_YES;
			cands[i].size_bytes = (gint64)(i + 1) * 3000000000;
			cands[i].protocol = AXIS_DOWNLOAD_TORRENT;
			cands[i].seeders = (gint)i * 10;
		}
		check_decision(&ctx, cands, G_N_ELEMENTS(fixed));
		for (i = 0; i < G_N_ELEMENTS(fixed); i++)
			axis_release_free(releases[i]);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	as_text(data, size);
	as_generated(data, size);
	return 0;
}

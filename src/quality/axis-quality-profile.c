/*
 * axis-quality-profile.c - the profile: its checks, the strict JSON reader,
 * the canonical writer, and the profile in words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The JSON grammar is the probe's bounded reader (axis-probe-json.h); what
 * is checked here is format 1 of axis-quality.h.  Every check refuses:
 * there is no default for a missing member, no conversion of a wrong type
 * and no clamping of an out-of-range value.  A problem's message never
 * holds any of the input (an unknown member is not named), so it can be
 * shown and logged as it is.
 */

#include "quality/axis-quality-private.h"

#include <stdlib.h>
#include <string.h>

#include "probe/axis-probe-json.h"

/* ── Problems ───────────────────────────────────────────────────── */

static void
problem_free(AxisQualityProblem *problem)
{
	if (problem == NULL)
		return;
	g_free(problem->field);
	g_free(problem->message);
	g_free(problem);
}

GPtrArray *
axis_quality_problems_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)problem_free);
}

const gchar *
axis_quality_problems_find(
	const GPtrArray *problems,
	const gchar     *field
){
	guint i;

	for (i = 0; problems != NULL && i < problems->len; i++) {
		const AxisQualityProblem *p = g_ptr_array_index((GPtrArray *)problems, i);

		if (g_strcmp0(p->field, field) == 0)
			return p->message;
	}
	return NULL;
}

void
axis_quality_problem_add(
	GPtrArray   *problems,
	const gchar *field,
	const gchar *message
){
	AxisQualityProblem *p;

	if (problems == NULL)
		return;
	p = g_new0(AxisQualityProblem, 1);
	p->field = g_strdup(field != NULL ? field : "");
	p->message = g_strdup(message);
	g_ptr_array_add(problems, p);
}

static void
rule_problem(
	GPtrArray   *problems,
	guint        index,
	const gchar *member,
	const gchar *message
){
	g_autofree gchar *field = member != NULL ? g_strdup_printf("rules.%u.%s", index + 1, member) :
		g_strdup_printf("rules.%u", index + 1);

	axis_quality_problem_add(problems, field, message);
}

/* ── The profile ────────────────────────────────────────────────── */

void
axis_quality_profile_init(AxisQualityProfile *profile)
{
	guint q;

	g_return_if_fail(profile != NULL);

	memset(profile, 0, sizeof(*profile));
	for (q = 1; q <= AXIS_QUALITY_N_LADDER; q++)
		profile->sizes[q] = axis_quality_default_size((AxisQuality)q);
	profile->upgrades = TRUE;
	profile->upgrade_revisions = TRUE;
}

gint
axis_quality_profile_tier(
	const AxisQualityProfile *profile,
	AxisQuality               quality
){
	guint i;

	for (i = 0; i < profile->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		if (profile->allowed[i] == quality)
			return (gint)profile->tier[i];
	}
	return -1;
}

guint
axis_quality_profile_n_tiers(const AxisQualityProfile *profile)
{
	guint n = MIN(profile->n_allowed, AXIS_QUALITY_N_LADDER);

	return n == 0 ? 0 : profile->tier[n - 1] + 1;
}

static gboolean
in_range(
	gint64 value,
	gint64 low,
	gint64 high
){
	return value >= low && value <= high;
}

static gboolean
rules_equal(
	const AxisQualityRule *a,
	const AxisQualityRule *b
){
	AxisQualityRule x = *a;
	AxisQualityRule y = *b;
	guint i;

	if (x.condition != y.condition || x.effect != y.effect || x.n_values != y.n_values)
		return FALSE;
	axis_quality_rule_sort_values(&x);
	axis_quality_rule_sort_values(&y);
	for (i = 0; i < x.n_values && i < AXIS_QUALITY_MAX_VALUES; i++) {
		if (!axis_quality_value_equal(x.condition, x.values[i], y.values[i]))
			return FALSE;
	}
	return TRUE;
}

static gboolean
check_rule(
	const AxisQualityRule *rule,
	guint                  index,
	GPtrArray             *problems
){
	gboolean ok = TRUE;
	guint i;
	guint j;

	if ((guint)rule->condition >= AXIS_QUALITY_N_CONDITIONS) {
		rule_problem(problems, index, "if", "The condition is not one this version knows.");
		return FALSE;
	}
	if ((guint)rule->effect >= AXIS_QUALITY_N_EFFECTS) {
		rule_problem(problems, index, "then", "The effect is require, reject, prefer or avoid.");
		ok = FALSE;
	} else if (rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID) {
		if (!in_range(rule->weight, 1, AXIS_QUALITY_MAX_WEIGHT)) {
			rule_problem(problems, index, "weight", "A preference weighs 1 to 1000.");
			ok = FALSE;
		}
	} else if (rule->weight != 0) {
		rule_problem(problems, index, "weight", "Only prefer and avoid take a weight.");
		ok = FALSE;
	}
	if (!axis_quality_condition_has_values(rule->condition)) {
		if (rule->n_values != 0) {
			rule_problem(problems, index, "is", "This condition takes no values.");
			ok = FALSE;
		}
		return ok;
	}
	if (rule->n_values < 1 || rule->n_values > AXIS_QUALITY_MAX_VALUES) {
		rule_problem(problems, index, "is", "A rule has one to sixteen values.");
		return FALSE;
	}
	for (i = 0; i < rule->n_values; i++) {
		if (memchr(rule->values[i], '\0', sizeof(rule->values[i])) == NULL ||
		    !axis_quality_value_valid(rule->condition, rule->values[i])) {
			rule_problem(problems, index, "is", "A value is not one this condition takes.");
			return FALSE;
		}
		for (j = 0; j < i; j++) {
			if (axis_quality_value_equal(rule->condition, rule->values[j], rule->values[i])) {
				rule_problem(problems, index, "is", "A value is there twice.");
				return FALSE;
			}
		}
	}
	return ok;
}

gboolean
axis_quality_profile_check(
	const AxisQualityProfile *profile,
	GPtrArray                *problems
){
	gboolean ok = TRUE;
	gboolean cutoff_found = FALSE;
	guint seen = 0;
	guint i;
	guint j;

	g_return_val_if_fail(profile != NULL, FALSE);

	if (profile->n_allowed < 1 || profile->n_allowed > AXIS_QUALITY_N_LADDER) {
		axis_quality_problem_add(problems, "qualities", "Allow at least one quality.");
		ok = FALSE;
	} else {
		for (i = 0; i < profile->n_allowed; i++) {
			guint q = (guint)profile->allowed[i];

			if (q < 1 || q > AXIS_QUALITY_N_LADDER) {
				axis_quality_problem_add(problems, "qualities", "A quality is not one of the ladder's.");
				ok = FALSE;
				break;
			}
			if ((seen & (1u << q)) != 0) {
				axis_quality_problem_add(problems, "qualities", "A quality is listed twice.");
				ok = FALSE;
				break;
			}
			seen |= 1u << q;
			if (i == 0 ? profile->tier[0] != 0 :
			    (profile->tier[i] != profile->tier[i - 1] && profile->tier[i] != profile->tier[i - 1] + 1)) {
				axis_quality_problem_add(problems, "qualities", "The order of the qualities is broken.");
				ok = FALSE;
				break;
			}
			if (profile->allowed[i] == profile->cutoff)
				cutoff_found = TRUE;
		}
		if (ok && !cutoff_found) {
			axis_quality_problem_add(problems, "cutoff", "The cutoff is one of the allowed qualities.");
			ok = FALSE;
		}
	}
	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++) {
		const AxisQualitySize *s = &profile->sizes[i];
		g_autofree gchar *field = g_strdup_printf("sizes.%s", axis_quality_name((AxisQuality)i));

		if (!in_range(s->min, 0, AXIS_QUALITY_MAX_MB_PER_MIN) || !in_range(s->max, 0, AXIS_QUALITY_MAX_MB_PER_MIN)) {
			axis_quality_problem_add(problems, field, "A size limit is 0 (none) to 2000 MiB per minute.");
			ok = FALSE;
		} else if (s->min > 0 && s->max > 0 && s->max <= s->min) {
			axis_quality_problem_add(problems, field, "The maximum must be above the minimum.");
			ok = FALSE;
		}
	}
	if (!in_range(profile->max_size_mb, 0, AXIS_QUALITY_MAX_SIZE_MB)) {
		axis_quality_problem_add(problems, "max_size_mb", "The largest release is 0 (no limit) to 1000 GiB.");
		ok = FALSE;
	}
	if (profile->n_rules > AXIS_QUALITY_MAX_RULES) {
		axis_quality_problem_add(problems, "rules", "A profile has at most 32 rules.");
		ok = FALSE;
	} else {
		for (i = 0; i < profile->n_rules; i++) {
			if (!check_rule(&profile->rules[i], i, problems)) {
				ok = FALSE;
				continue;
			}
			for (j = 0; j < i; j++) {
				if (rules_equal(&profile->rules[j], &profile->rules[i])) {
					rule_problem(problems, i, NULL, "The same rule is there twice.");
					ok = FALSE;
					break;
				}
			}
		}
	}
	if (profile->upgrade_score) {
		if (!in_range(profile->min_score_gain, 1, AXIS_QUALITY_MAX_SCORE_GAIN)) {
			axis_quality_problem_add(problems, "upgrades.min_score_gain",
				"A better score counts from 1 to 10000 points more.");
			ok = FALSE;
		}
	} else if (profile->min_score_gain != 0) {
		axis_quality_problem_add(problems, "upgrades.min_score_gain",
			"A score gain only applies when a better score counts.");
		ok = FALSE;
	}
	if (!in_range(profile->players, 0, AXIS_QUALITY_MAX_WEIGHT)) {
		axis_quality_problem_add(problems, "players", "The players preference weighs 0 (off) to 1000.");
		ok = FALSE;
	}
	if (!in_range(profile->allow_unknown, 0, 1) || !in_range(profile->allow_unwanted, 0, 1) ||
	    !in_range(profile->upgrades, 0, 1) || !in_range(profile->upgrade_revisions, 0, 1) ||
	    !in_range(profile->upgrade_score, 0, 1) || !in_range(profile->upgrade_unknown, 0, 1)) {
		axis_quality_problem_add(problems, "", "A switch is on or off.");
		ok = FALSE;
	}
	return ok;
}

/* ── Normal order ───────────────────────────────────────────────── */

/* Within a tier the qualities best first; a rule's values sorted. */
static void
normalize(AxisQualityProfile *profile)
{
	guint n = MIN(profile->n_allowed, AXIS_QUALITY_N_LADDER);
	guint start = 0;
	guint i;
	guint j;

	while (start < n) {
		guint end = start + 1;

		while (end < n && profile->tier[end] == profile->tier[start])
			end++;
		/* Insertion sort, best (highest on the ladder) first. */
		for (i = start + 1; i < end; i++) {
			AxisQuality q = profile->allowed[i];

			for (j = i; j > start && axis_quality_compare(profile->allowed[j - 1], q) < 0; j--)
				profile->allowed[j] = profile->allowed[j - 1];
			profile->allowed[j] = q;
		}
		start = end;
	}
	for (i = 0; i < profile->n_rules && i < AXIS_QUALITY_MAX_RULES; i++)
		axis_quality_rule_sort_values(&profile->rules[i]);
}

/* ── Reading ────────────────────────────────────────────────────── */

typedef struct
{
	const AxisJsonNode *object;
	const gchar        *prefix;	/* for problems: "", "upgrades.", "rules.3." */
	GPtrArray          *problems;
	gboolean            ok;
} Reader;

static void
fail(
	Reader      *r,
	const gchar *name,
	const gchar *message
){
	g_autofree gchar *field = name != NULL ? g_strconcat(r->prefix, name, NULL) : g_strndup(r->prefix,
		r->prefix[0] != '\0' ? strlen(r->prefix) - 1 : 0);

	axis_quality_problem_add(r->problems, field, message);
	r->ok = FALSE;
}

static const AxisJsonNode *
member(
	Reader      *r,
	const gchar *name
){
	return axis_json_member(r->object, name);
}

static const AxisJsonNode *
need(
	Reader      *r,
	const gchar *name
){
	const AxisJsonNode *node = member(r, name);

	if (node == NULL)
		fail(r, name, "This is missing.");
	return node;
}

static void
refuse(
	Reader      *r,
	const gchar *name
){
	if (member(r, name) != NULL)
		fail(r, name, "This does not apply here.");
}

/* A whole number: a number that does not fit an int becomes G_MININT32,
 * which every bound refuses. */
static gboolean
read_int(
	Reader      *r,
	const gchar *name,
	gint64      *value
){
	const AxisJsonNode *node = need(r, name);

	if (node == NULL)
		return FALSE;
	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_integer(node, G_MININT64, G_MAXINT64, value)) {
		fail(r, name, "This is a whole number.");
		return FALSE;
	}
	return TRUE;
}

static gint
narrow(gint64 value)
{
	return value >= G_MININT32 && value <= G_MAXINT32 ? (gint)value : G_MININT32;
}

static gboolean
read_boolean(
	Reader      *r,
	const gchar *name,
	gboolean    *value
){
	const AxisJsonNode *node = need(r, name);

	if (node == NULL)
		return FALSE;
	if (axis_json_type(node) != AXIS_JSON_BOOLEAN || !axis_json_get_boolean(node, value)) {
		fail(r, name, "This is true or false.");
		return FALSE;
	}
	return TRUE;
}

/* Every member is one of @known and none is there twice. */
static void
check_members(
	Reader             *r,
	const gchar *const *known
){
	guint n = axis_json_length(r->object);
	guint i;
	guint j;

	if (n > 32) {
		fail(r, NULL, "This has more members than the format has.");
		return;
	}
	for (i = 0; i < n; i++) {
		const gchar *name = axis_json_member_name(r->object, i);

		if (name == NULL || !g_strv_contains(known, name)) {
			fail(r, NULL, "This has a member the format does not define.");
			continue;
		}
		for (j = 0; j < i; j++) {
			if (g_strcmp0(axis_json_member_name(r->object, j), name) == 0)
				fail(r, name, "This is given twice.");
		}
	}
}

static void
read_qualities(
	Reader             *r,
	AxisQualityProfile *profile
){
	const AxisJsonNode *node = need(r, "qualities");
	guint t;
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) < 1 ||
	    axis_json_length(node) > AXIS_QUALITY_N_LADDER) {
		fail(r, "qualities", "This is a list of 1 to 24 tiers, each a list of qualities.");
		return;
	}
	for (t = 0; t < axis_json_length(node); t++) {
		const AxisJsonNode *tier = axis_json_element(node, t);

		if (axis_json_type(tier) != AXIS_JSON_ARRAY || axis_json_length(tier) < 1) {
			fail(r, "qualities", "A tier is a list of one or more qualities.");
			return;
		}
		for (i = 0; i < axis_json_length(tier); i++) {
			const AxisJsonNode *item = axis_json_element(tier, i);
			const gchar *text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;
			AxisQuality q;

			if (text == NULL || !axis_quality_from_name(text, &q)) {
				fail(r, "qualities", "A quality is not one of the ladder's.");
				return;
			}
			if (profile->n_allowed >= AXIS_QUALITY_N_LADDER) {
				fail(r, "qualities", "A quality is listed twice.");
				return;
			}
			profile->allowed[profile->n_allowed] = q;
			profile->tier[profile->n_allowed] = t;
			profile->n_allowed++;
		}
	}
}

static void
read_cutoff(
	Reader             *r,
	AxisQualityProfile *profile
){
	const AxisJsonNode *node = need(r, "cutoff");
	const gchar *text;

	if (node == NULL)
		return;
	text = axis_json_type(node) == AXIS_JSON_STRING ? axis_json_string(node) : NULL;
	if (text == NULL || !axis_quality_from_name(text, &profile->cutoff))
		fail(r, "cutoff", "The cutoff is the name of a quality.");
}

static void
read_sizes(
	Reader             *r,
	AxisQualityProfile *profile
){
	const AxisJsonNode *node = need(r, "sizes");
	guint present = 0;
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_OBJECT || axis_json_length(node) > 64) {
		fail(r, "sizes", "This is an object with a pair of limits for each quality.");
		return;
	}
	for (i = 0; i < axis_json_length(node); i++) {
		const gchar *name = axis_json_member_name(node, i);
		const AxisJsonNode *pair = axis_json_member_value(node, i);
		g_autofree gchar *field = NULL;
		AxisQuality q;
		gint64 low = 0;
		gint64 high = 0;

		if (name == NULL || !axis_quality_from_name(name, &q)) {
			fail(r, "sizes", "The sizes name a quality the ladder does not have.");
			continue;
		}
		field = g_strdup_printf("sizes.%s", name);
		if ((present & (1u << q)) != 0) {
			axis_quality_problem_add(r->problems, field, "This is given twice.");
			r->ok = FALSE;
			continue;
		}
		present |= 1u << q;
		if (axis_json_type(pair) != AXIS_JSON_ARRAY || axis_json_length(pair) != 2 ||
		    axis_json_type(axis_json_element(pair, 0)) != AXIS_JSON_NUMBER ||
		    axis_json_type(axis_json_element(pair, 1)) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(axis_json_element(pair, 0), G_MININT64, G_MAXINT64, &low) ||
		    !axis_json_get_integer(axis_json_element(pair, 1), G_MININT64, G_MAXINT64, &high)) {
			axis_quality_problem_add(r->problems, field, "A quality's sizes are a minimum and a maximum, whole "
				"mebibytes per minute.");
			r->ok = FALSE;
			continue;
		}
		profile->sizes[q].min = narrow(low);
		profile->sizes[q].max = narrow(high);
	}
	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++) {
		if ((present & (1u << i)) == 0) {
			g_autofree gchar *field = g_strdup_printf("sizes.%s", axis_quality_name((AxisQuality)i));

			axis_quality_problem_add(r->problems, field, "This is missing.");
			r->ok = FALSE;
		}
	}
}

static void
read_rule(
	Reader             *parent,
	const AxisJsonNode *node,
	guint               index,
	AxisQualityRule    *rule
){
	static const gchar *const known[] = { "if", "is", "then", "weight", NULL };
	g_autofree gchar *prefix = g_strdup_printf("rules.%u.", index + 1);
	const AxisJsonNode *item;
	Reader r;
	const gchar *text;
	gboolean condition_ok = FALSE;
	gboolean effect_ok = FALSE;

	memset(rule, 0, sizeof(*rule));
	if (axis_json_type(node) != AXIS_JSON_OBJECT) {
		rule_problem(parent->problems, index, NULL, "A rule is an object.");
		parent->ok = FALSE;
		return;
	}
	r.object = node;
	r.prefix = prefix;
	r.problems = parent->problems;
	r.ok = TRUE;
	check_members(&r, known);

	item = need(&r, "if");
	if (item != NULL) {
		text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;
		condition_ok = text != NULL && axis_quality_condition_from_name(text, &rule->condition);
		if (!condition_ok)
			fail(&r, "if", "The condition is not one this version knows.");
	}
	item = need(&r, "then");
	if (item != NULL) {
		text = axis_json_type(item) == AXIS_JSON_STRING ? axis_json_string(item) : NULL;
		effect_ok = text != NULL && axis_quality_effect_from_name(text, &rule->effect);
		if (!effect_ok)
			fail(&r, "then", "The effect is require, reject, prefer or avoid.");
	}
	if (effect_ok && (rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID)) {
		gint64 weight = 0;

		if (read_int(&r, "weight", &weight))
			rule->weight = narrow(weight);
	} else if (effect_ok) {
		refuse(&r, "weight");
	}
	if (condition_ok && axis_quality_condition_has_values(rule->condition)) {
		item = need(&r, "is");
		if (item != NULL) {
			guint i;

			if (axis_json_type(item) != AXIS_JSON_ARRAY || axis_json_length(item) < 1 ||
			    axis_json_length(item) > AXIS_QUALITY_MAX_VALUES) {
				fail(&r, "is", "This is a list of one to sixteen values.");
			} else {
				for (i = 0; i < axis_json_length(item); i++) {
					const AxisJsonNode *v = axis_json_element(item, i);

					text = axis_json_type(v) == AXIS_JSON_STRING ? axis_json_string(v) : NULL;
					if (text == NULL || strlen(text) > AXIS_QUALITY_VALUE_MAX) {
						fail(&r, "is", "A value is not one this condition takes.");
						break;
					}
					g_strlcpy(rule->values[rule->n_values], text, sizeof(rule->values[0]));
					rule->n_values++;
				}
			}
		}
	} else if (condition_ok) {
		refuse(&r, "is");
	}
	if (!r.ok)
		parent->ok = FALSE;
}

static void
read_rules(
	Reader             *r,
	AxisQualityProfile *profile
){
	const AxisJsonNode *node = need(r, "rules");
	guint i;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) > AXIS_QUALITY_MAX_RULES) {
		fail(r, "rules", "This is a list of at most 32 rules.");
		return;
	}
	for (i = 0; i < axis_json_length(node); i++) {
		read_rule(r, axis_json_element(node, i), i, &profile->rules[profile->n_rules]);
		profile->n_rules++;
	}
}

static void
read_upgrades(
	Reader             *r,
	AxisQualityProfile *profile
){
	static const gchar *const known[] = { "allowed", "revisions", "score", "min_score_gain", "unknown", NULL };
	const AxisJsonNode *node = need(r, "upgrades");
	Reader u;

	if (node == NULL)
		return;
	if (axis_json_type(node) != AXIS_JSON_OBJECT) {
		fail(r, "upgrades", "This is an object of the upgrade rules.");
		return;
	}
	u.object = node;
	u.prefix = "upgrades.";
	u.problems = r->problems;
	u.ok = TRUE;
	check_members(&u, known);
	read_boolean(&u, "allowed", &profile->upgrades);
	read_boolean(&u, "revisions", &profile->upgrade_revisions);
	if (read_boolean(&u, "score", &profile->upgrade_score) && profile->upgrade_score) {
		gint64 gain = 0;

		if (read_int(&u, "min_score_gain", &gain))
			profile->min_score_gain = narrow(gain);
	} else {
		refuse(&u, "min_score_gain");
	}
	/* The one optional member: absent is false. */
	if (member(&u, "unknown") != NULL)
		read_boolean(&u, "unknown", &profile->upgrade_unknown);
	if (!u.ok)
		r->ok = FALSE;
}

gint
axis_quality_profile_format_of(
	const gchar *text,
	gssize       length
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *format;
	gsize size;
	gint64 value = 0;

	if (text == NULL)
		return 0;
	size = length < 0 ? strlen(text) : (gsize)length;
	if (size > AXIS_QUALITY_MAX_TEXT)
		return 0;
	doc = axis_json_parse(text, size, NULL);
	if (doc == NULL)
		return 0;
	format = axis_json_member(axis_json_doc_get_root(doc), "format");
	if (axis_json_type(format) != AXIS_JSON_NUMBER || !axis_json_get_integer(format, 0, G_MAXINT32, &value))
		return 0;
	return (gint)value;
}

gboolean
axis_quality_profile_parse(
	const gchar        *text,
	gssize              length,
	AxisQualityProfile *profile,
	GPtrArray          *problems
){
	static const gchar *const known[] = {
		"format", "qualities", "cutoff", "unknown", "unwanted", "sizes", "max_size_mb", "rules", "upgrades",
		"players", NULL
	};
	g_autoptr(AxisJsonDoc) doc = NULL;
	AxisQualityProfile local;
	AxisQualityProfile *out = profile != NULL ? profile : &local;
	Reader r;
	gsize size;
	gint64 value = 0;

	memset(out, 0, sizeof(*out));
	if (text == NULL) {
		axis_quality_problem_add(problems, "", "There is no definition.");
		return FALSE;
	}
	size = length < 0 ? strlen(text) : (gsize)length;
	if (size > AXIS_QUALITY_MAX_TEXT) {
		axis_quality_problem_add(problems, "", "The definition is longer than the format allows.");
		return FALSE;
	}
	doc = axis_json_parse(text, size, NULL);
	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_OBJECT) {
		axis_quality_problem_add(problems, "", "The definition is not a JSON object.");
		return FALSE;
	}
	r.object = axis_json_doc_get_root(doc);
	r.prefix = "";
	r.problems = problems;
	r.ok = TRUE;

	/* Another format is not read at all: its members may mean something
	 * else. */
	if (!read_int(&r, "format", &value))
		return FALSE;
	if (value != AXIS_QUALITY_FORMAT) {
		fail(&r, "format", value > AXIS_QUALITY_FORMAT ? "The definition was written by a newer version of "
			"Axis, in a format this version does not read." : "The definition is of a format this version does "
			"not read.");
		return FALSE;
	}
	check_members(&r, known);
	read_qualities(&r, out);
	read_cutoff(&r, out);
	read_boolean(&r, "unknown", &out->allow_unknown);
	read_boolean(&r, "unwanted", &out->allow_unwanted);
	read_sizes(&r, out);
	if (read_int(&r, "max_size_mb", &value))
		out->max_size_mb = value;
	read_rules(&r, out);
	read_upgrades(&r, out);
	if (read_int(&r, "players", &value))
		out->players = narrow(value);
	if (!r.ok || !axis_quality_profile_check(out, problems)) {
		memset(out, 0, sizeof(*out));
		return FALSE;
	}
	normalize(out);
	return TRUE;
}

/* ── Writing ────────────────────────────────────────────────────── */

/* @value as a JSON string: only the characters JSON needs escaped are. */
static void
append_string(
	GString     *out,
	const gchar *value
){
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)value; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_printf(out, "\\%c", *p);
		else if (*p < 0x20)
			g_string_append_printf(out, "\\u%04x", *p);
		else
			g_string_append_c(out, (gchar)*p);
	}
	g_string_append_c(out, '"');
}

/* A quality's name, or "?" (which parse() refuses) for no quality. */
static const gchar *
quality_or_mark(AxisQuality quality)
{
	return (guint)quality >= 1 && (guint)quality <= AXIS_QUALITY_N_LADDER ? axis_quality_name(quality) : "?";
}

static int
compare_names(
	const void *a,
	const void *b
){
	return strcmp(axis_quality_name(*(const AxisQuality *)a), axis_quality_name(*(const AxisQuality *)b));
}

gchar *
axis_quality_profile_canonical(const AxisQualityProfile *profile)
{
	GString *out = g_string_new("{");
	AxisQualityProfile copy;
	AxisQuality names[AXIS_QUALITY_N_LADDER];
	guint n;
	guint i;
	guint j;

	g_return_val_if_fail(profile != NULL, g_strdup("{}"));

	copy = *profile;
	normalize(&copy);
	n = MIN(copy.n_allowed, AXIS_QUALITY_N_LADDER);

	g_string_append(out, "\"cutoff\":");
	append_string(out, quality_or_mark(copy.cutoff));
	g_string_append_printf(out, ",\"format\":%d,\"max_size_mb\":%" G_GINT64_FORMAT ",\"players\":%d", AXIS_QUALITY_FORMAT,
		copy.max_size_mb, copy.players);

	/* Tiers that do not start at 0 or do not rise by one are written with an
	 * empty tier, which parse() refuses: the writer never turns a broken
	 * profile into a valid one. */
	g_string_append(out, ",\"qualities\":[");
	for (i = 0; i < n; i++) {
		if (i == 0)
			g_string_append(out, copy.tier[0] == 0 ? "[" : "[],[");
		else if (copy.tier[i] == copy.tier[i - 1])
			g_string_append_c(out, ',');
		else if (copy.tier[i] == copy.tier[i - 1] + 1)
			g_string_append(out, "],[");
		else
			g_string_append(out, "],[],[");
		append_string(out, quality_or_mark(copy.allowed[i]));
	}
	g_string_append(out, n > 0 ? "]]" : "]");

	g_string_append(out, ",\"rules\":[");
	for (i = 0; i < copy.n_rules && i < AXIS_QUALITY_MAX_RULES; i++) {
		const AxisQualityRule *rule = &copy.rules[i];
		const gchar *condition = axis_quality_condition_name(rule->condition);
		const gchar *effect = axis_quality_effect_name(rule->effect);

		g_string_append(out, i > 0 ? ",{\"if\":" : "{\"if\":");
		append_string(out, condition != NULL ? condition : "?");
		if (condition == NULL || axis_quality_condition_has_values(rule->condition) || rule->n_values > 0) {
			g_string_append(out, ",\"is\":[");
			for (j = 0; j < rule->n_values && j < AXIS_QUALITY_MAX_VALUES; j++) {
				if (j > 0)
					g_string_append_c(out, ',');
				/* A value without its end is written as one no condition takes. */
				append_string(out, memchr(rule->values[j], '\0', sizeof(rule->values[j])) != NULL ?
					rule->values[j] : "?");
			}
			g_string_append_c(out, ']');
		}
		g_string_append(out, ",\"then\":");
		append_string(out, effect != NULL ? effect : "?");
		if (effect == NULL || rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID ||
		    rule->weight != 0)
			g_string_append_printf(out, ",\"weight\":%d", rule->weight);
		g_string_append_c(out, '}');
	}
	g_string_append_c(out, ']');

	/* The sizes by name, in byte order. */
	for (i = 0; i < AXIS_QUALITY_N_LADDER; i++)
		names[i] = (AxisQuality)(i + 1);
	qsort(names, AXIS_QUALITY_N_LADDER, sizeof(names[0]), compare_names);
	g_string_append(out, ",\"sizes\":{");
	for (i = 0; i < AXIS_QUALITY_N_LADDER; i++) {
		const AxisQualitySize *s = &copy.sizes[names[i]];

		g_string_append_printf(out, "%s\"%s\":[%d,%d]", i > 0 ? "," : "", axis_quality_name(names[i]), s->min,
			s->max);
	}
	g_string_append_c(out, '}');

	g_string_append_printf(out, ",\"unknown\":%s,\"unwanted\":%s", copy.allow_unknown ? "true" : "false",
		copy.allow_unwanted ? "true" : "false");
	g_string_append_printf(out, ",\"upgrades\":{\"allowed\":%s", copy.upgrades ? "true" : "false");
	if (copy.upgrade_score || copy.min_score_gain != 0)
		g_string_append_printf(out, ",\"min_score_gain\":%d", copy.min_score_gain);
	g_string_append_printf(out, ",\"revisions\":%s,\"score\":%s", copy.upgrade_revisions ? "true" : "false",
		copy.upgrade_score ? "true" : "false");
	/* Written only when it is on (absent is off): every definition stored
	 * before it existed is still in canonical form. */
	if (copy.upgrade_unknown)
		g_string_append(out, ",\"unknown\":true");
	g_string_append(out, "}}");
	return g_string_free(out, FALSE);
}

gboolean
axis_quality_profile_equal(
	const AxisQualityProfile *a,
	const AxisQualityProfile *b
){
	g_autofree gchar *x = axis_quality_profile_canonical(a);
	g_autofree gchar *y = axis_quality_profile_canonical(b);

	return strcmp(x, y) == 0;
}

/* ── In words ───────────────────────────────────────────────────── */

static void
fact(
	GPtrArray   *facts,
	const gchar *label,
	gchar       *text
){
	AxisQualityFact *f = g_new0(AxisQualityFact, 1);

	f->label = label;
	f->text = text;
	g_ptr_array_add(facts, f);
}

static void
fact_free(AxisQualityFact *f)
{
	g_free(f->text);
	g_free(f);
}

GPtrArray *
axis_quality_profile_describe(const AxisQualityProfile *profile)
{
	GPtrArray *facts = g_ptr_array_new_with_free_func((GDestroyNotify)fact_free);
	g_autoptr(GPtrArray) tiers = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) upgrades = g_ptr_array_new_with_free_func(g_free);
	guint n = MIN(profile->n_allowed, AXIS_QUALITY_N_LADDER);
	guint start = 0;

	g_return_val_if_fail(profile != NULL, facts);

	while (start < n) {
		g_autoptr(GPtrArray) members = g_ptr_array_new_with_free_func(g_free);
		guint end = start;

		while (end < n && profile->tier[end] == profile->tier[start])
			g_ptr_array_add(members, g_strdup(axis_quality_label(profile->allowed[end++])));
		if (members->len > 1) {
			g_autofree gchar *joined = axis_quality_join(members, " or ");

			g_ptr_array_add(tiers, g_strdup_printf("%s (as equals)", joined));
		} else {
			g_ptr_array_add(tiers, g_strdup(g_ptr_array_index(members, 0)));
		}
		start = end;
	}
	if (profile->allow_unknown)
		g_ptr_array_add(tiers, g_strdup("then a release of unknown quality"));
	fact(facts, "Qualities, best first", tiers->len > 0 ? axis_quality_join(tiers, ", then ") : g_strdup("none"));
	fact(facts, "Upgraded until", g_strdup_printf("%s (the cutoff)", axis_quality_label(profile->cutoff)));
	fact(facts, "Unknown quality", g_strdup(profile->allow_unknown ? "accepted, after every quality listed" :
		"rejected (the name must say its source and resolution)"));
	fact(facts, "Cinema recordings and pre-release copies", g_strdup(profile->allow_unwanted ?
		"accepted where the qualities list them" : "never accepted, whatever the qualities list"));
	if (profile->max_size_mb > 0) {
		g_autofree gchar *size = axis_quality_format_size(profile->max_size_mb * 1048576);

		fact(facts, "Largest release", g_strdup_printf("%s, whatever its runtime", size));
	} else {
		fact(facts, "Largest release", g_strdup("no limit beyond the sizes per minute"));
	}
	if (!profile->upgrades) {
		fact(facts, "Upgrades", g_strdup("never: the first acceptable release stays"));
	} else {
		g_ptr_array_add(upgrades, g_strdup("a quality higher in the order"));
		if (profile->upgrade_revisions)
			g_ptr_array_add(upgrades, g_strdup("a proper or repack of the same quality"));
		if (profile->upgrade_score)
			g_ptr_array_add(upgrades, g_strdup_printf("the same quality with a score %d or more higher",
				profile->min_score_gain));
		fact(facts, "Upgrades", axis_quality_join(upgrades, " or "));
	}
	fact(facts, "A file of unknown source", g_strdup(profile->upgrades && profile->upgrade_unknown ?
		"upgraded by a release of its resolution or higher that states its source" :
		"never upgraded (a file found in the library whose name states no source stays)"));
	fact(facts, "The household's players", profile->players > 0 ? g_strdup_printf("avoid what they would need "
		"transcoded, by %d (from the compatibility report)", profile->players) : g_strdup("not considered"));
	return facts;
}

/* The names of the flags set in @flags, joined. */
static gchar *
flag_words(
	const gchar *(*name_of)(guint flag),
	guint         flags
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; i < 31; i++) {
		if ((flags & (1u << i)) != 0 && name_of(1u << i) != NULL)
			g_ptr_array_add(names, g_strdup(name_of(1u << i)));
	}
	return names->len > 0 ? axis_quality_join(names, ", ") : NULL;
}

/* A list of numbers written as ranges ("1-3, 5"). */
static gchar *
number_words(
	const gint *numbers,
	guint       count
){
	GString *out = g_string_new(NULL);
	guint i = 0;

	while (i < count && i < AXIS_RELEASE_MAX_NUMBERS) {
		guint j = i;

		while (j + 1 < count && j + 1 < AXIS_RELEASE_MAX_NUMBERS && numbers[j + 1] == numbers[j] + 1)
			j++;
		if (out->len > 0)
			g_string_append(out, ", ");
		if (j > i)
			g_string_append_printf(out, "%d to %d", numbers[i], numbers[j]);
		else
			g_string_append_printf(out, "%d", numbers[i]);
		i = j + 1;
	}
	return g_string_free(out, FALSE);
}

static void
fact_has(
	GPtrArray            *facts,
	const gchar          *label,
	AxisQualityCondition  condition,
	const AxisRelease    *release
){
	gchar *text = axis_quality_rule_has_text(condition, release);

	if (text != NULL)
		fact(facts, label, text);
}

GPtrArray *
axis_quality_release_describe(const AxisRelease *release)
{
	GPtrArray *facts = g_ptr_array_new_with_free_func((GDestroyNotify)fact_free);
	AxisQuality quality;
	gchar *text;

	g_return_val_if_fail(release != NULL, facts);

	quality = axis_quality_of_release(release);
	fact(facts, "Title", g_strdup(release->title != NULL ? release->title : "none could be read"));
	if (release->year > 0)
		fact(facts, "Year", g_strdup_printf("%d", release->year));
	if (release->season_first >= 0) {
		g_autofree gchar *episodes = release->episode_count > 0 ?
			number_words(release->episodes, release->episode_count) : NULL;

		if (release->season_last != release->season_first)
			fact(facts, "Seasons", g_strdup_printf("%d to %d", release->season_first, release->season_last));
		else
			fact(facts, "Season", g_strdup_printf("%d%s", release->season_first, release->episode_count == 0 ?
				", the whole season" : ""));
		if (episodes != NULL)
			fact(facts, release->episode_count == 1 ? "Episode" : "Episodes", g_strdup(episodes));
	} else if (release->episode_count > 0) {
		fact(facts, "Episodes", number_words(release->episodes, release->episode_count));
	}
	if (release->absolute_count > 0)
		fact(facts, "Absolute episode numbers",number_words(release->absolute, release->absolute_count));
	if (release->air_year > 0)
		fact(facts, "Air date", g_strdup_printf("%04d-%02d-%02d", release->air_year, release->air_month,
			release->air_day));
	if (axis_quality_source_label(release->source) != NULL)
		fact(facts, "Source", g_strdup(axis_quality_source_label(release->source)));
	if (axis_release_resolution_name(release->resolution) != NULL)
		fact(facts, "Resolution", g_strdup_printf("%s%s", axis_release_resolution_name(release->resolution),
			release->interlaced ? " (interlaced)" : ""));
	{
		g_autofree gchar *where = axis_quality_explain_place(release);

		/* "Unknown: ..." reads as the start of a sentence here. */
		if (quality == AXIS_QUALITY_UNKNOWN && g_str_has_prefix(where, "unknown"))
			where[0] = 'U';
		fact(facts, "Rank among qualities",g_steal_pointer(&where));
	}
	if (release->service != NULL)
		fact(facts, "Streaming service", g_strdup(axis_quality_value_label(AXIS_QUALITY_IF_SERVICE,
			release->service)));
	fact_has(facts, "Video codec", AXIS_QUALITY_IF_CODEC, release);
	fact_has(facts, "Bit depth", AXIS_QUALITY_IF_BIT_DEPTH, release);
	fact_has(facts, "HDR", AXIS_QUALITY_IF_HDR, release);
	fact_has(facts, "Audio", AXIS_QUALITY_IF_AUDIO, release);
	if (release->channels[0] != '\0')
		fact(facts, "Channels", g_strdup(release->channels));
	fact_has(facts, "Edition", AXIS_QUALITY_IF_EDITION, release);
	fact_has(facts, "Languages", AXIS_QUALITY_IF_LANGUAGE, release);
	fact_has(facts, "Language marks", AXIS_QUALITY_IF_LANGUAGE_MARK, release);
	if ((release->language_marks & AXIS_RELEASE_LANG_HARDSUB) != 0)
		fact(facts, "Subtitles", g_strdup("hard-coded"));
	fact_has(facts, "Proper or repack",AXIS_QUALITY_IF_REVISION, release);
	if (release->version > 0)
		fact(facts, "Version", g_strdup_printf("v%d", release->version));
	if ((text = flag_words(axis_release_mark_name, release->marks)) != NULL)
		fact(facts, "Other tags in the name", text);
	if ((text = flag_words(axis_release_stop_name, release->stop)) != NULL)
		fact(facts, "Marks that rule it out", text);
	if (release->group != NULL)
		fact(facts, "Group", g_strdup(release->group));
	if (release->unknown != NULL && release->unknown->len > 0) {
		g_autoptr(GPtrArray) words = g_ptr_array_new_with_free_func(g_free);
		guint i;

		for (i = 0; i < release->unknown->len; i++)
			g_ptr_array_add(words, g_strdup(g_ptr_array_index(release->unknown, i)));
		fact(facts, "Words not understood", axis_quality_join(words, ", "));
	}
	return facts;
}

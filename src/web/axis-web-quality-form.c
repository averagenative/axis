/*
 * axis-web-quality-form.c - editing a quality profile: the form, its
 * buttons that change the form without script, its per-field errors, and
 * the save
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The whole form is posted by every button.  "Save" checks the profile with
 * the format's own checks (axis_quality_profile_check(), the same that every
 * stored definition passes) and stores it, or shows each problem beside its
 * field with every other value kept.  The other buttons change only the
 * form and show it again, saving nothing: Up and Down move a quality in the
 * order (one in a tier of equals leaves the tier), "Equal to the one above"
 * joins the tier above, Remove and Allow take a quality out of the order and
 * put one back at its end, and rules are added and removed.  The order
 * travels in a hidden field ("a,b;c": tiers separated by ";", equals by
 * ","), checked like everything else.
 *
 * Reflected text: the name is the text content of a textarea; every other
 * submitted value reaches an attribute only after being cut to the
 * characters a number, a quality name or a list of rule values is made of.
 */

#include "web/axis-web-quality-private.h"

#include "core/axis-size.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── The form ───────────────────────────────────────────────────── */

typedef struct
{
	gchar              *name;
	gint                revision;
	AxisQualityProfile  profile;
	GHashTable         *texts;	/* field -> submitted text (reflected), shown again */
	GHashTable         *errors;	/* field -> message */
	gchar              *general;
} Form;

static void
form_init(Form *form)
{
	memset(form, 0, sizeof(*form));
	axis_quality_profile_init(&form->profile);
	form->texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	form->errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void
form_clear(Form *form)
{
	g_free(form->name);
	g_free(form->general);
	g_clear_pointer(&form->texts, g_hash_table_unref);
	g_clear_pointer(&form->errors, g_hash_table_unref);
	memset(form, 0, sizeof(*form));
}

static void
form_error(
	Form        *form,
	const gchar *field,
	const gchar *message
){
	if (!g_hash_table_contains(form->errors, field))
		g_hash_table_insert(form->errors, g_strdup(field), g_strdup(message));
}

static void
set_text(
	Form        *form,
	const gchar *field,
	gchar       *text
){
	g_hash_table_insert(form->texts, g_strdup(field), text);
}

static const gchar *
get_text(
	Form        *form,
	const gchar *field
){
	const gchar *text = g_hash_table_lookup(form->texts, field);

	return text != NULL ? text : "";
}

/* A submitted field, kept (reflected) to be shown again. */
static const gchar *
field_text(
	GHashTable  *fields,
	Form        *form,
	const gchar *field
){
	const gchar *text = fields != NULL ? g_hash_table_lookup(fields, field) : NULL;

	set_text(form, field, axis_web_quality_reflect(text));
	return text != NULL ? text : "";
}

static gboolean
field_yes(
	GHashTable  *fields,
	const gchar *field
){
	return fields != NULL && g_strcmp0(g_hash_table_lookup(fields, field), "yes") == 0;
}

/* A whole number in [low, high]; empty is 0. */
static gint
read_int(
	GHashTable  *fields,
	Form        *form,
	const gchar *field,
	gint64       low,
	gint64       high,
	const gchar *message
){
	g_autofree gchar *text = g_strstrip(g_strdup(field_text(fields, form, field)));
	gint64 value = 0;

	if (text[0] == '\0')
		return 0;
	if (!g_ascii_string_to_signed(text, 10, low, high, &value, NULL)) {
		form_error(form, field, message);
		return G_MININT32;
	}
	return (gint)value;
}

/* The order as the hidden field has it: tiers by ";", equals by ",". */
static void
read_order(
	const gchar        *text,
	AxisQualityProfile *p
){
	g_auto(GStrv) tiers = g_strsplit(text != NULL ? text : "", ";", AXIS_QUALITY_N_LADDER + 1);
	guint seen = 0;
	guint tier = 0;
	guint t;
	guint i;

	p->n_allowed = 0;
	for (t = 0; tiers[t] != NULL && t < AXIS_QUALITY_N_LADDER; t++) {
		g_auto(GStrv) names = g_strsplit(tiers[t], ",", AXIS_QUALITY_N_LADDER + 1);
		gboolean any = FALSE;

		for (i = 0; names[i] != NULL; i++) {
			AxisQuality q;

			if (!axis_quality_from_name(g_strstrip(names[i]), &q) || (seen & (1u << q)) != 0 ||
			    p->n_allowed >= AXIS_QUALITY_N_LADDER)
				continue;
			seen |= 1u << q;
			p->allowed[p->n_allowed] = q;
			p->tier[p->n_allowed] = tier;
			p->n_allowed++;
			any = TRUE;
		}
		if (any)
			tier++;
	}
}

static gchar *
write_order(const AxisQualityProfile *p)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		if (i > 0)
			g_string_append_c(out, p->tier[i] == p->tier[i - 1] ? ',' : ';');
		g_string_append(out, axis_quality_name(p->allowed[i]));
	}
	return g_string_free(out, FALSE);
}

static gchar *
size_field(
	const gchar *end,
	AxisQuality  q
){
	return g_strdup_printf("%s_%s", end, axis_quality_name(q));
}

static gchar *
rule_field(
	guint        n,
	const gchar *member
){
	return g_strdup_printf("rule%u_%s", n, member);
}

/* A rule's values as typed: split at commas and spaces; the fixed sets and
 * codes in lower case. */
static void
read_values(
	const gchar     *text,
	AxisQualityRule *rule,
	Form            *form,
	const gchar     *field
){
	g_auto(GStrv) parts = g_strsplit_set(text, ", ", -1);
	guint i;

	rule->n_values = 0;
	for (i = 0; parts[i] != NULL; i++) {
		g_autofree gchar *value = NULL;

		if (parts[i][0] == '\0')
			continue;
		value = rule->condition == AXIS_QUALITY_IF_GROUP || rule->condition == AXIS_QUALITY_IF_WORD ?
			g_strdup(parts[i]) : g_ascii_strdown(parts[i], -1);
		if (rule->n_values >= AXIS_QUALITY_MAX_VALUES || strlen(value) > AXIS_QUALITY_VALUE_MAX) {
			form_error(form, field, "A rule has at most sixteen values, each at most 32 characters.");
			return;
		}
		g_strlcpy(rule->values[rule->n_values], value, sizeof(rule->values[0]));
		rule->n_values++;
	}
}

/* The submitted form as a draft: every value kept as typed, the errors of
 * what could not be read at all by field. */
static void
form_read(
	GHashTable *fields,
	Form       *form
){
	AxisQualityProfile *p = &form->profile;
	const gchar *name = fields != NULL ? g_hash_table_lookup(fields, "name") : NULL;
	const gchar *text;
	gint64 n_rules = 0;
	guint q;
	guint i;

	form_init(form);
	form->name = name != NULL ? axis_quality_clean_name(name) : NULL;
	if (form->name == NULL) {
		form->name = g_strdup(name != NULL && g_utf8_validate(name, -1, NULL) ? name : "");
		form_error(form, "name", "A profile needs a name.");
	}
	text = fields != NULL ? g_hash_table_lookup(fields, "revision") : NULL;
	form->revision = text != NULL ? (gint)g_ascii_strtoll(text, NULL, 10) : 0;

	read_order(fields != NULL ? g_hash_table_lookup(fields, "qualities") : NULL, p);
	for (q = 1; q <= AXIS_QUALITY_N_LADDER; q++) {
		g_autofree gchar *min = size_field("min", (AxisQuality)q);
		g_autofree gchar *max = size_field("max", (AxisQuality)q);
		g_autofree gchar *error = size_field("size", (AxisQuality)q);
		const gchar *message = "A size is a whole number of MiB per minute, 0 to 2000.";

		p->sizes[q].min = read_int(fields, form, min, 0, AXIS_QUALITY_MAX_MB_PER_MIN, message);
		p->sizes[q].max = read_int(fields, form, max, 0, AXIS_QUALITY_MAX_MB_PER_MIN, message);
		if (g_hash_table_contains(form->errors, min) || g_hash_table_contains(form->errors, max))
			form_error(form, error, message);
	}
	text = field_text(fields, form, "cutoff");
	if (!axis_quality_from_name(text, &p->cutoff))
		p->cutoff = AXIS_QUALITY_UNKNOWN;
	p->allow_unknown = field_yes(fields, "unknown");
	p->allow_unwanted = field_yes(fields, "unwanted");
	{
		g_autofree gchar *gib = g_strstrip(g_strdup(field_text(fields, form, "max_size_gb")));
		gchar *end = NULL;
		gdouble value = 0;

		if (gib[0] != '\0') {
			value = g_ascii_strtod(gib, &end);
			if (end == gib || *end != '\0' || !isfinite(value) || value < 0 || value > 1000) {
				form_error(form, "max_size_gb", "The largest release is a number of GiB, such as 25 or 6.5, from 0 "
					"(no limit) to 1000.");
				value = -1;
			}
		}
		p->max_size_mb = value < 0 ? -1 : (gint64)llround(value * 1024);
	}
	p->upgrades = field_yes(fields, "upgrades");
	p->upgrade_revisions = field_yes(fields, "revisions");
	p->upgrade_score = field_yes(fields, "score");
	p->upgrade_unknown = field_yes(fields, "upgrade_unknown");
	{
		gint gain = read_int(fields, form, "min_score_gain", 0, AXIS_QUALITY_MAX_SCORE_GAIN,
			"The score gain is a whole number from 1 to 10000.");

		p->min_score_gain = p->upgrade_score ? gain : 0;
	}
	p->players = read_int(fields, form, "players", 0, AXIS_QUALITY_MAX_WEIGHT,
		"The weight is a whole number from 0 (off) to 1000.");

	text = fields != NULL ? g_hash_table_lookup(fields, "n_rules") : NULL;
	if (text == NULL || !g_ascii_string_to_signed(text, 10, 0, AXIS_QUALITY_MAX_RULES, &n_rules, NULL))
		n_rules = 0;
	p->n_rules = 0;
	for (i = 1; i <= (guint)n_rules; i++) {
		g_autofree gchar *if_field = rule_field(i, "if");
		g_autofree gchar *is_field = rule_field(i, "is");
		g_autofree gchar *then_field = rule_field(i, "then");
		g_autofree gchar *weight_field = rule_field(i, "weight");
		AxisQualityRule *rule = &p->rules[p->n_rules++];
		gint weight;

		memset(rule, 0, sizeof(*rule));
		if (!axis_quality_condition_from_name(field_text(fields, form, if_field), &rule->condition)) {
			form_error(form, if_field, "Choose one of the conditions offered.");
			rule->condition = (AxisQualityCondition)AXIS_QUALITY_N_CONDITIONS;
		}
		if (!axis_quality_effect_from_name(field_text(fields, form, then_field), &rule->effect)) {
			form_error(form, then_field, "Choose one of the effects offered.");
			rule->effect = (AxisQualityEffect)AXIS_QUALITY_N_EFFECTS;
		}
		read_values(field_text(fields, form, is_field), rule, form, is_field);
		weight = read_int(fields, form, weight_field, 0, AXIS_QUALITY_MAX_WEIGHT,
			"A weight is a whole number from 1 to 1000.");
		rule->weight = rule->effect == AXIS_QUALITY_PREFER || rule->effect == AXIS_QUALITY_AVOID ? weight : 0;
	}
}

/* The form filled from a stored profile (the edit page as it opens). */
static void
form_from(
	Form                    *form,
	const AxisQualityStored *stored
){
	const AxisQualityProfile *p = &stored->profile;
	guint q;
	guint i;

	form_init(form);
	form->profile = *p;
	form->name = g_strdup(stored->name);
	form->revision = stored->revision;
	for (q = 1; q <= AXIS_QUALITY_N_LADDER; q++) {
		g_autofree gchar *min = size_field("min", (AxisQuality)q);
		g_autofree gchar *max = size_field("max", (AxisQuality)q);

		set_text(form, min, g_strdup_printf("%d", p->sizes[q].min));
		set_text(form, max, g_strdup_printf("%d", p->sizes[q].max));
	}
	set_text(form, "cutoff", g_strdup(axis_quality_name(p->cutoff)));
	/* In GiB, to four decimals: a limit in mebibytes reads back as it was. */
	set_text(form, "max_size_gb", axis_size_number_as((guint64)MAX(p->max_size_mb, 0) * 1048576, 3, 4));
	set_text(form, "min_score_gain", p->upgrade_score ? g_strdup_printf("%d", p->min_score_gain) : g_strdup(""));
	set_text(form, "players", g_strdup_printf("%d", p->players));
	for (i = 0; i < p->n_rules; i++) {
		const AxisQualityRule *rule = &p->rules[i];
		g_autofree gchar *if_field = rule_field(i + 1, "if");
		g_autofree gchar *is_field = rule_field(i + 1, "is");
		g_autofree gchar *then_field = rule_field(i + 1, "then");
		g_autofree gchar *weight_field = rule_field(i + 1, "weight");
		GString *values = g_string_new(NULL);
		guint k;

		for (k = 0; k < rule->n_values; k++)
			g_string_append_printf(values, "%s%s", k > 0 ? ", " : "", rule->values[k]);
		set_text(form, if_field, g_strdup(axis_quality_condition_name(rule->condition)));
		set_text(form, is_field, g_string_free(values, FALSE));
		set_text(form, then_field, g_strdup(axis_quality_effect_name(rule->effect)));
		set_text(form, weight_field, rule->weight > 0 ? g_strdup_printf("%d", rule->weight) : g_strdup(""));
	}
}

/* Where a problem of the format is shown. */
static gchar *
field_of(const gchar *member)
{
	guint n = 0;
	gchar part[16];
	AxisQuality q;

	if (g_str_has_prefix(member, "sizes.") && axis_quality_from_name(member + 6, &q))
		return size_field("size", q);
	if (strcmp(member, "max_size_mb") == 0)
		return g_strdup("max_size_gb");
	if (strcmp(member, "upgrades.min_score_gain") == 0)
		return g_strdup("min_score_gain");
	if (sscanf(member, "rules.%u.%15s", &n, part) == 2 && n >= 1 && n <= AXIS_QUALITY_MAX_RULES)
		return rule_field(n, part);
	if (sscanf(member, "rules.%u", &n) == 1 && n >= 1 && n <= AXIS_QUALITY_MAX_RULES)
		return rule_field(n, "if");
	return member[0] != '\0' ? g_strdup(member) : NULL;
}

/* The format's own checks, each problem beside its field.  Returns whether
 * the form may be saved. */
static gboolean
form_check(Form *form)
{
	g_autoptr(GPtrArray) problems = axis_quality_problems_new();
	guint i;

	axis_quality_profile_check(&form->profile, problems);
	for (i = 0; i < problems->len; i++) {
		const AxisQualityProblem *p = g_ptr_array_index(problems, i);
		g_autofree gchar *field = field_of(p->field);

		if (field != NULL)
			form_error(form, field, p->message);
		else if (form->general == NULL)
			form->general = g_strdup(p->message);
	}
	return g_hash_table_size(form->errors) == 0 && form->general == NULL;
}

/* ── The buttons that change the form ───────────────────────────── */

/* The order as tiers, to move qualities about. */
typedef struct
{
	guint       n;
	AxisQuality q[AXIS_QUALITY_N_LADDER];
} Tier;

typedef struct
{
	guint n;
	Tier  t[AXIS_QUALITY_N_LADDER + 1];
} Tiers;

static void
tiers_of(
	const AxisQualityProfile *p,
	Tiers                    *tiers
){
	guint i;

	memset(tiers, 0, sizeof(*tiers));
	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		if (i == 0 || p->tier[i] != p->tier[i - 1])
			tiers->n++;
		tiers->t[tiers->n - 1].q[tiers->t[tiers->n - 1].n++] = p->allowed[i];
	}
}

static void
tiers_to(
	const Tiers        *tiers,
	AxisQualityProfile *p
){
	guint t;
	guint i;
	guint out = 0;
	guint tier = 0;

	for (t = 0; t < tiers->n; t++) {
		if (tiers->t[t].n == 0)
			continue;
		for (i = 0; i < tiers->t[t].n && out < AXIS_QUALITY_N_LADDER; i++) {
			p->allowed[out] = tiers->t[t].q[i];
			p->tier[out] = tier;
			out++;
		}
		tier++;
	}
	p->n_allowed = out;
}

/* Where @q is: its tier and its place in it; FALSE when it is not there. */
static gboolean
tiers_find(
	const Tiers *tiers,
	AxisQuality  q,
	guint       *t,
	guint       *i
){
	for (*t = 0; *t < tiers->n; (*t)++) {
		for (*i = 0; *i < tiers->t[*t].n; (*i)++) {
			if (tiers->t[*t].q[*i] == q)
				return TRUE;
		}
	}
	return FALSE;
}

static void
tier_take(
	Tier *tier,
	guint at
){
	memmove(&tier->q[at], &tier->q[at + 1], (tier->n - at - 1) * sizeof(tier->q[0]));
	tier->n--;
}

/* A tier of @q alone inserted at @at. */
static void
tiers_insert(
	Tiers       *tiers,
	guint        at,
	AxisQuality  q
){
	memmove(&tiers->t[at + 1], &tiers->t[at], (tiers->n - at) * sizeof(tiers->t[0]));
	memset(&tiers->t[at], 0, sizeof(tiers->t[0]));
	tiers->t[at].q[0] = q;
	tiers->t[at].n = 1;
	tiers->n++;
}

static void
tiers_drop_empty(Tiers *tiers)
{
	guint t = 0;

	while (t < tiers->n) {
		if (tiers->t[t].n == 0) {
			memmove(&tiers->t[t], &tiers->t[t + 1], (tiers->n - t - 1) * sizeof(tiers->t[0]));
			tiers->n--;
		} else {
			t++;
		}
	}
}

/* Applies a button: whether it was one. */
static gboolean
apply_op(
	Form        *form,
	const gchar *op
){
	AxisQualityProfile *p = &form->profile;
	const gchar *colon = op != NULL ? strchr(op, ':') : NULL;
	g_autofree gchar *verb = colon != NULL ? g_strndup(op, (gsize)(colon - op)) : g_strdup(op != NULL ? op : "");
	const gchar *arg = colon != NULL ? colon + 1 : "";
	Tiers tiers;
	AxisQuality q = AXIS_QUALITY_UNKNOWN;
	guint t = 0;
	guint i = 0;

	if (strcmp(verb, "add_rule") == 0) {
		guint n = p->n_rules + 1;
		g_autofree gchar *if_field = rule_field(n, "if");
		g_autofree gchar *is_field = rule_field(n, "is");
		g_autofree gchar *then_field = rule_field(n, "then");
		g_autofree gchar *weight_field = rule_field(n, "weight");

		if (p->n_rules >= AXIS_QUALITY_MAX_RULES) {
			form->general = g_strdup("A profile has at most 32 rules.");
			return TRUE;
		}
		memset(&p->rules[p->n_rules], 0, sizeof(p->rules[0]));
		p->rules[p->n_rules].condition = AXIS_QUALITY_IF_CODEC;
		p->rules[p->n_rules].effect = AXIS_QUALITY_PREFER;
		p->rules[p->n_rules].weight = 10;
		p->n_rules++;
		set_text(form, if_field, g_strdup("codec"));
		set_text(form, is_field, g_strdup(""));
		set_text(form, then_field, g_strdup("prefer"));
		set_text(form, weight_field, g_strdup("10"));
		return TRUE;
	}
	if (strcmp(verb, "remove_rule") == 0) {
		guint64 n = 0;
		guint k;

		if (!g_ascii_string_to_unsigned(arg, 10, 1, p->n_rules, &n, NULL))
			return FALSE;
		memmove(&p->rules[n - 1], &p->rules[n], (p->n_rules - n) * sizeof(p->rules[0]));
		p->n_rules--;
		/* The texts of the rules after it move up one. */
		for (k = (guint)n; k <= p->n_rules; k++) {
			static const gchar *const members[] = { "if", "is", "then", "weight" };
			guint m;

			for (m = 0; m < G_N_ELEMENTS(members); m++) {
				g_autofree gchar *to = rule_field(k, members[m]);
				g_autofree gchar *from = rule_field(k + 1, members[m]);

				set_text(form, to, g_strdup(get_text(form, from)));
				g_hash_table_remove(form->errors, to);
			}
		}
		return TRUE;
	}
	if (!axis_quality_from_name(arg, &q))
		return FALSE;
	tiers_of(p, &tiers);
	if (strcmp(verb, "allow") == 0) {
		if (tiers_find(&tiers, q, &t, &i))
			return TRUE;
		tiers_insert(&tiers, tiers.n, q);
		tiers_to(&tiers, p);
		return TRUE;
	}
	if (!tiers_find(&tiers, q, &t, &i))
		return strcmp(verb, "up") == 0 || strcmp(verb, "down") == 0 || strcmp(verb, "join") == 0 ||
			strcmp(verb, "remove") == 0;
	if (strcmp(verb, "remove") == 0) {
		tier_take(&tiers.t[t], i);
	} else if (strcmp(verb, "join") == 0) {
		if (t > 0) {
			tier_take(&tiers.t[t], i);
			tiers.t[t - 1].q[tiers.t[t - 1].n++] = q;
		}
	} else if (strcmp(verb, "up") == 0) {
		if (tiers.t[t].n > 1) {
			tier_take(&tiers.t[t], i);
			tiers_insert(&tiers, t, q);
		} else if (t > 0) {
			Tier swap = tiers.t[t - 1];

			tiers.t[t - 1] = tiers.t[t];
			tiers.t[t] = swap;
		}
	} else if (strcmp(verb, "down") == 0) {
		if (tiers.t[t].n > 1) {
			tier_take(&tiers.t[t], i);
			tiers_insert(&tiers, t + 1, q);
		} else if (t + 1 < tiers.n) {
			Tier swap = tiers.t[t + 1];

			tiers.t[t + 1] = tiers.t[t];
			tiers.t[t] = swap;
		}
	} else {
		return FALSE;
	}
	tiers_drop_empty(&tiers);
	tiers_to(&tiers, p);
	/* A cutoff taken out of the order is chosen again. */
	if (axis_quality_profile_tier(p, p->cutoff) < 0)
		set_text(form, "cutoff", g_strdup(""));
	return TRUE;
}

/* ── Rendering ──────────────────────────────────────────────────── */

typedef struct
{
	HtmxNode *container;
	Form     *form;
} Render;

static HtmxNode *
error_text(
	Form        *form,
	const gchar *field
){
	const gchar *error = g_hash_table_lookup(form->errors, field);
	HtmxNode *p;

	if (error == NULL)
		return NULL;
	p = axis_web_storage_text("field-error", error);
	htmx_element_set_attribute(HTMX_ELEMENT(p), "role", "alert");
	return p;
}

/* A field's label, its control, its help and its error. */
static void
add_field(
	Render      *r,
	const gchar *field,
	const gchar *label_text,
	HtmxNode    *control,
	const gchar *help
){
	HtmxDiv *div = htmx_div_new();
	g_autofree gchar *id = g_strdup_printf("quality-%s", field);
	g_autofree gchar *help_id = g_strdup_printf("quality-%s-help", field);
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxNode *error = error_text(r->form, field);

	htmx_element_add_class(HTMX_ELEMENT(div), "field");
	if (error != NULL) {
		htmx_element_add_class(HTMX_ELEMENT(div), "setting-invalid");
		htmx_element_set_attribute(HTMX_ELEMENT(control), "aria-invalid", "true");
	}
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_element_set_id(HTMX_ELEMENT(control), id);
	if (help != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(control), "aria-describedby", help_id);
	BROWSER_ADD(div, label);
	BROWSER_ADD(div, control);
	if (help != NULL) {
		HtmxNode *p = axis_web_storage_text("help", help);

		htmx_element_set_id(HTMX_ELEMENT(p), help_id);
		BROWSER_ADD(div, p);
	}
	if (error != NULL)
		BROWSER_ADD(div, error);
	BROWSER_ADD(r->container, div);
}

static HtmxNode *
text_input(
	Form        *form,
	const gchar *field
){
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);
	g_autofree gchar *safe = axis_web_quality_reflect(get_text(form, field));

	htmx_input_set_name(input, field);
	htmx_input_set_value(input, safe);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "spellcheck", "false");
	return HTMX_NODE(input);
}

static HtmxNode *
hidden_text(
	Form        *form,
	const gchar *field
){
	g_autofree gchar *safe = axis_web_quality_reflect(get_text(form, field));

	return axis_web_auth_hidden(field, safe);
}

static HtmxNode *
checkbox(
	const gchar *field,
	const gchar *text,
	gboolean     checked
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	g_autofree gchar *id = g_strdup_printf("quality-%s", field);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	htmx_input_set_name(input, field);
	htmx_input_set_value(input, "yes");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), text);
	BROWSER_ADD(div, input);
	BROWSER_ADD(div, label);
	return HTMX_NODE(div);
}

static HtmxNode *
fieldset(
	const gchar *legend,
	const gchar *css
){
	HtmxFieldset *set = htmx_fieldset_new();

	htmx_element_add_class(HTMX_ELEMENT(set), "profile-group");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(set), css);
	BROWSER_ADD(set, htmx_legend_new_with_text(legend));
	return HTMX_NODE(set);
}

/* A button that posts the form with @op. */
static HtmxNode *
op_button(
	const gchar *op,
	const gchar *text,
	const gchar *label,
	gboolean     enabled,
	const gchar *css
){
	HtmxButton *button = htmx_button_new();

	htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
	htmx_element_set_attribute(HTMX_ELEMENT(button), "name", "op");
	htmx_element_set_attribute(HTMX_ELEMENT(button), "value", op);
	if (css == NULL) {
		htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	} else {
		g_auto(GStrv) classes = g_strsplit(css, " ", -1);
		guint i;

		for (i = 0; classes[i] != NULL; i++) {
			if (classes[i][0] != '\0')
				htmx_element_add_class(HTMX_ELEMENT(button), classes[i]);
		}
	}
	if (label != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", label);
	if (!enabled)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "disabled", "disabled");
	htmx_node_set_text_content(HTMX_NODE(button), text);
	return HTMX_NODE(button);
}

/* A size input of a quality's row, with its label read by screen readers. */
static HtmxNode *
size_input(
	Form        *form,
	const gchar *end,
	AxisQuality  q,
	const gchar *label_text
){
	HtmxDiv *div = htmx_div_new();
	g_autofree gchar *field = size_field(end, q);
	g_autofree gchar *id = g_strdup_printf("quality-%s", field);
	g_autofree gchar *text = g_strdup_printf("%s, %s (MiB per minute)", axis_quality_label(q), label_text);
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxNode *input = text_input(form, field);
	HtmxSpan *visible = htmx_span_new();
	g_autofree gchar *other = size_field(strcmp(end, "min") == 0 ? "max" : "min", q);
	g_autofree gchar *row = size_field("size", q);

	htmx_element_add_class(HTMX_ELEMENT(div), "quality-size");
	/* This field could not be read, or the pair is wrong as a pair. */
	if (g_hash_table_contains(form->errors, field) || (g_hash_table_contains(form->errors, row) &&
	    !g_hash_table_contains(form->errors, other))) {
		htmx_element_add_class(HTMX_ELEMENT(div), "setting-invalid");
		htmx_element_set_attribute(HTMX_ELEMENT(input), "aria-invalid", "true");
	}
	htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(label), text);
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "inputmode", "numeric");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "5");
	htmx_element_add_class(HTMX_ELEMENT(visible), "quality-size-word");
	htmx_element_set_attribute(HTMX_ELEMENT(visible), "aria-hidden", "true");
	htmx_node_set_text_content(HTMX_NODE(visible), label_text);
	BROWSER_ADD(div, label);
	BROWSER_ADD(div, visible);
	BROWSER_ADD(div, input);
	return HTMX_NODE(div);
}

static HtmxNode *
order_fieldset(Form *form)
{
	const AxisQualityProfile *p = &form->profile;
	HtmxNode *set = fieldset("Qualities, best first", NULL);
	HtmxOl *list = htmx_ol_new();
	HtmxNode *error = error_text(form, "qualities");
	g_autoptr(GString) others = g_string_new(NULL);
	HtmxDiv *rest = htmx_div_new();
	gboolean any_other = FALSE;
	guint i;
	guint q;

	BROWSER_ADD(set, axis_web_storage_text("help", "A release of a quality higher in the list ranks first; qualities "
		"on one line are equals. The sizes are MiB (mebibytes) per minute of the runtime, 0 for no limit: a "
		"release outside them is rejected."));
	if (error != NULL)
		BROWSER_ADD(set, error);
	htmx_element_add_class(HTMX_ELEMENT(list), "quality-order");
	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		AxisQuality qa = p->allowed[i];
		const gchar *label = axis_quality_label(qa);
		const gchar *name = axis_quality_name(qa);
		gboolean equal = i > 0 && p->tier[i] == p->tier[i - 1];
		gboolean grouped = equal || (i + 1 < p->n_allowed && p->tier[i + 1] == p->tier[i]);
		g_autofree gchar *place = equal ? g_strdup("=") : g_strdup_printf("%u", p->tier[i] + 1);
		g_autofree gchar *up = g_strdup_printf("up:%s", name);
		g_autofree gchar *down = g_strdup_printf("down:%s", name);
		g_autofree gchar *join = g_strdup_printf("join:%s", name);
		g_autofree gchar *remove = g_strdup_printf("remove:%s", name);
		g_autofree gchar *up_label = g_strdup_printf("Move %s up", label);
		g_autofree gchar *down_label = g_strdup_printf("Move %s down", label);
		g_autofree gchar *join_label = g_strdup_printf("Make %s equal to the one above", label);
		g_autofree gchar *remove_label = g_strdup_printf("Remove %s", label);
		g_autofree gchar *size_error = size_field("size", qa);
		HtmxLi *row = htmx_li_new();
		HtmxSpan *where = htmx_span_new();
		HtmxSpan *what = htmx_span_new();
		HtmxDiv *moves = htmx_div_new();
		HtmxNode *row_error = error_text(form, size_error);

		htmx_element_add_class(HTMX_ELEMENT(row), "quality-row");
		if (equal)
			htmx_element_add_class(HTMX_ELEMENT(row), "quality-equal");
		htmx_element_add_class(HTMX_ELEMENT(where), "quality-place");
		htmx_element_set_attribute(HTMX_ELEMENT(where), "aria-hidden", "true");
		htmx_node_set_text_content(HTMX_NODE(where), place);
		htmx_element_add_class(HTMX_ELEMENT(what), "quality-name");
		htmx_node_set_text_content(HTMX_NODE(what), label);
		BROWSER_ADD(row, where);
		BROWSER_ADD(row, what);
		BROWSER_ADD(row, size_input(form, "min", qa, "at least"));
		BROWSER_ADD(row, size_input(form, "max", qa, "at most"));
		htmx_element_add_class(HTMX_ELEMENT(moves), "quality-moves");
		BROWSER_ADD(moves, op_button(up, "Up", up_label, i > 0, NULL));
		BROWSER_ADD(moves, op_button(down, "Down", down_label, i + 1 < p->n_allowed || grouped, NULL));
		BROWSER_ADD(moves, op_button(join, "Equal to above", join_label, i > 0 && !equal, NULL));
		BROWSER_ADD(moves, op_button(remove, "Remove", remove_label, p->n_allowed > 1, "button-small button-quiet"));
		BROWSER_ADD(row, moves);
		if (row_error != NULL)
			BROWSER_ADD(row, row_error);
		BROWSER_ADD(list, row);
	}
	BROWSER_ADD(set, list);

	/* The qualities not in the order: a button each; their sizes ride along. */
	htmx_element_add_class(HTMX_ELEMENT(rest), "quality-others");
	for (q = AXIS_QUALITY_N_LADDER; q >= 1; q--) {
		g_autofree gchar *min = size_field("min", (AxisQuality)q);
		g_autofree gchar *max = size_field("max", (AxisQuality)q);
		g_autofree gchar *op = NULL;
		g_autofree gchar *label = NULL;

		if (axis_quality_profile_tier(p, (AxisQuality)q) >= 0)
			continue;
		BROWSER_ADD(set, hidden_text(form, min));
		BROWSER_ADD(set, hidden_text(form, max));
		op = g_strdup_printf("allow:%s", axis_quality_name((AxisQuality)q));
		label = g_strdup_printf("Allow %s", axis_quality_label((AxisQuality)q));
		BROWSER_ADD(rest, op_button(op, axis_quality_label((AxisQuality)q), label, TRUE, NULL));
		any_other = TRUE;
	}
	if (any_other) {
		BROWSER_ADD(set, axis_web_storage_text("quality-others-head", "Not allowed (a button adds one at the end):"));
		BROWSER_ADD(set, rest);
	} else {
		g_object_unref(rest);
	}
	return set;
}

static HtmxNode *
select_input(
	const gchar        *field,
	const gchar        *current,
	const gchar *const *values,
	const gchar *const *labels
){
	HtmxSelect *select = htmx_select_new_with_name(field);
	guint i;

	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels != NULL ? labels[i] : values[i]);

		htmx_option_set_selected(option, g_strcmp0(current, values[i]) == 0);
		BROWSER_ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
limits_fieldset(Form *form)
{
	const AxisQualityProfile *p = &form->profile;
	HtmxNode *set = fieldset("Cutoff and limits", NULL);
	HtmxSelect *cutoff = htmx_select_new_with_name("cutoff");
	const gchar *current = get_text(form, "cutoff");
	Render r;
	guint i;

	r.container = set;
	r.form = form;
	if (axis_quality_profile_tier(p, p->cutoff) < 0)
		BROWSER_ADD(cutoff, htmx_option_new_with_value("", "(choose one)"));
	for (i = 0; i < p->n_allowed && i < AXIS_QUALITY_N_LADDER; i++) {
		HtmxOption *option = htmx_option_new_with_value(axis_quality_name(p->allowed[i]),
			axis_quality_label(p->allowed[i]));

		htmx_option_set_selected(option, g_strcmp0(current, axis_quality_name(p->allowed[i])) == 0);
		BROWSER_ADD(cutoff, option);
	}
	add_field(&r, "cutoff", "Upgraded until (the cutoff)", HTMX_NODE(cutoff), "Once the file on disk is of this "
		"quality or one above it in the order, no upgrade is sought.");
	BROWSER_ADD(set, checkbox("unknown", "Accept releases of unknown quality (the name states no source or no "
		"resolution), after every quality listed", p->allow_unknown));
	BROWSER_ADD(set, checkbox("unwanted", "Accept cinema recordings and pre-release copies (cam, telesync, telecine, "
		"screener, workprint) where the list above has them", p->allow_unwanted));
	add_field(&r, "max_size_gb", "Largest release (GiB)", text_input(form, "max_size_gb"), "No release above it, "
		"whatever its runtime; 0 for no limit. Without a runtime this is the only size limit.");
	return set;
}

static HtmxNode *
upgrades_fieldset(Form *form)
{
	const AxisQualityProfile *p = &form->profile;
	HtmxNode *set = fieldset("Upgrades", NULL);
	Render r;

	r.container = set;
	r.form = form;
	BROWSER_ADD(set, checkbox("upgrades", "Look for a better release while the file on disk is below the cutoff",
		p->upgrades));
	BROWSER_ADD(set, checkbox("revisions", "A proper or repack of the same quality counts as better",
		p->upgrade_revisions));
	BROWSER_ADD(set, checkbox("score", "A higher score at the same quality counts as better, by at least:",
		p->upgrade_score));
	add_field(&r, "min_score_gain", "Score gain", text_input(form, "min_score_gain"), "1 to 10000, when a higher "
		"score counts. A file Axis converted counts as the quality of its source.");
	BROWSER_ADD(set, checkbox("upgrade_unknown", "Upgrade a file found in the library whose name states no source, by "
		"a release of its resolution or higher that states one", p->upgrade_unknown));
	return set;
}

static HtmxNode *
rules_fieldset(Form *form)
{
	static const gchar *const condition_values[] = {
		"codec", "bit_depth", "hdr", "audio", "channels", "language", "language_mark", "edition", "service",
		"group", "word", "3d", "hardsub", "revision", NULL
	};
	static const gchar *const effect_values[] = { "require", "reject", "prefer", "avoid", NULL };
	static const gchar *const effect_labels[] = { "Require", "Reject", "Prefer (+weight)", "Avoid (-weight)", NULL };
	const gchar *condition_labels[AXIS_QUALITY_N_CONDITIONS + 1];
	const AxisQualityProfile *p = &form->profile;
	HtmxNode *set = fieldset("Rules on the release name", NULL);
	guint i;

	for (i = 0; i < AXIS_QUALITY_N_CONDITIONS; i++)
		condition_labels[i] = axis_quality_condition_label((AxisQualityCondition)i);
	condition_labels[AXIS_QUALITY_N_CONDITIONS] = NULL;
	BROWSER_ADD(set, axis_web_storage_text("help", "Each rule is a condition on what the name says and an effect. "
		"Require and reject decide whether a release is acceptable; prefer and avoid add to or take from its "
		"score, which ranks releases of the same quality. A rule holds when the name has any of its values."));
	for (i = 0; i < p->n_rules && i < AXIS_QUALITY_MAX_RULES; i++) {
		g_autofree gchar *heading = g_strdup_printf("Rule %u", i + 1);
		g_autofree gchar *if_field = rule_field(i + 1, "if");
		g_autofree gchar *is_field = rule_field(i + 1, "is");
		g_autofree gchar *then_field = rule_field(i + 1, "then");
		g_autofree gchar *weight_field = rule_field(i + 1, "weight");
		g_autofree gchar *remove = g_strdup_printf("remove_rule:%u", i + 1);
		g_autofree gchar *remove_label = g_strdup_printf("Remove rule %u", i + 1);
		HtmxNode *row = fieldset(heading, "profile-rule");
		Render inner;
		HtmxDiv *end = htmx_div_new();

		inner.container = row;
		inner.form = form;
		add_field(&inner, if_field, "Condition", select_input(if_field, get_text(form, if_field), condition_values,
			condition_labels), NULL);
		add_field(&inner, is_field, "Values", text_input(form, is_field), NULL);
		add_field(&inner, then_field, "Effect", select_input(then_field, get_text(form, then_field), effect_values,
			effect_labels), NULL);
		add_field(&inner, weight_field, "Weight", text_input(form, weight_field), NULL);
		htmx_element_add_class(HTMX_ELEMENT(end), "quality-rule-end");
		BROWSER_ADD(end, op_button(remove, "Remove", remove_label, TRUE, "button-quiet"));
		BROWSER_ADD(row, end);
		BROWSER_ADD(set, row);
	}
	{
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
		BROWSER_ADD(actions, op_button("add_rule", "Add a rule", NULL, p->n_rules < AXIS_QUALITY_MAX_RULES, ""));
		BROWSER_ADD(set, actions);
	}
	/* What each condition takes. */
	{
		HtmxDetails *details = htmx_details_new();
		HtmxSummary *summary = htmx_summary_new();
		HtmxNode *facts = axis_web_browser_facts_new();

		htmx_node_set_text_content(HTMX_NODE(summary), "What each condition takes");
		BROWSER_ADD(details, summary);
		for (i = 0; i < AXIS_QUALITY_N_CONDITIONS; i++) {
			const gchar *const *values = axis_quality_condition_values((AxisQualityCondition)i);
			g_autofree gchar *text = NULL;

			if (values != NULL) {
				text = g_strjoinv(", ", (gchar **)values);
			} else {
				switch ((AxisQualityCondition)i) {
				case AXIS_QUALITY_IF_CHANNELS:
					text = g_strdup("as the name writes them: 2.0, 5.1, 7.1, 7.1.4");
					break;
				case AXIS_QUALITY_IF_LANGUAGE:
					text = g_strdup("two-letter codes: en, de, fr, es, it, ja, ...");
					break;
				case AXIS_QUALITY_IF_SERVICE:
					text = g_strdup("amazon, netflix, disney, hbomax, max, apple, hulu, ...");
					break;
				case AXIS_QUALITY_IF_GROUP:
					text = g_strdup("release group names, as written, compared ignoring case");
					break;
				case AXIS_QUALITY_IF_WORD:
					text = g_strdup("words of letters and digits, compared ignoring case; the title's own words do "
						"not count");
					break;
				default:
					text = g_strdup("no values: leave Values empty");
					break;
				}
			}
			axis_web_browser_fact(facts, axis_quality_condition_label((AxisQualityCondition)i), text);
		}
		BROWSER_ADD(details, facts);
		BROWSER_ADD(set, details);
	}
	return set;
}

static HtmxNode *
players_fieldset(Form *form)
{
	HtmxNode *set = fieldset("The household's players", NULL);
	Render r;

	r.container = set;
	r.form = form;
	add_field(&r, "players", "Avoid what they would need transcoded, by", text_input(form, "players"),
		"0 is off. From the compatibility report: a release whose video or audio a player of the household is known "
		"to need the media server to convert loses this much score, and the decision says so.");
	return set;
}

static HtmxNode *
edit_form(
	HtmxRequest             *request,
	const AxisQualityStored *stored,
	Form                    *form
){
	g_autofree gchar *action = g_strdup_printf("/quality/%s/edit", stored->slug);
	g_autofree gchar *revision = g_strdup_printf("%d", form->revision);
	g_autofree gchar *order = write_order(&form->profile);
	g_autofree gchar *n_rules = g_strdup_printf("%u", form->profile.n_rules);
	g_autofree gchar *back = axis_web_quality_href(stored->slug);
	HtmxNode *html = axis_web_auth_post_form(request, action);
	HtmxNode *set;
	Render r;

	htmx_element_add_class(HTMX_ELEMENT(html), "form-wide");
	htmx_element_add_class(HTMX_ELEMENT(html), "quality-form");
	/* Enter in a field saves: the first submit button of the form is the
	 * one a browser presses for it, and the others move things. */
	{
		HtmxNode *first = op_button("save", "Save", NULL, TRUE, "visually-hidden");

		htmx_element_set_attribute(HTMX_ELEMENT(first), "tabindex", "-1");
		htmx_element_set_attribute(HTMX_ELEMENT(first), "aria-hidden", "true");
		axis_web_auth_add(html, first);
	}
	axis_web_auth_add(html, axis_web_auth_hidden("revision", revision));
	axis_web_auth_add(html, axis_web_auth_hidden("qualities", order));
	axis_web_auth_add(html, axis_web_auth_hidden("n_rules", n_rules));
	if (form->general != NULL)
		axis_web_auth_add(html, axis_web_auth_notice(TRUE, form->general));

	set = fieldset("What it is called", NULL);
	r.container = set;
	r.form = form;
	{
		/* A person's text: the text content of a textarea, never an
		 * attribute. */
		HtmxTextarea *name = htmx_textarea_new_with_name("name");

		htmx_textarea_set_rows(name, 1);
		htmx_element_add_class(HTMX_ELEMENT(name), "one-line");
		htmx_node_set_text_content(HTMX_NODE(name), form->name != NULL ? form->name : "");
		add_field(&r, "name", "Name", HTMX_NODE(name), "Up to 80 characters.");
	}
	axis_web_auth_add(html, set);
	axis_web_auth_add(html, order_fieldset(form));
	axis_web_auth_add(html, limits_fieldset(form));
	axis_web_auth_add(html, upgrades_fieldset(form));
	axis_web_auth_add(html, rules_fieldset(form));
	axis_web_auth_add(html, players_fieldset(form));
	{
		HtmxDiv *buttons = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(buttons), "actions");
		BROWSER_ADD(buttons, op_button("save", "Save this quality profile", NULL, TRUE, "button-primary"));
		BROWSER_ADD(buttons, axis_web_browser_link(back, "Back to the quality profile, without saving"));
		axis_web_auth_add(html, buttons);
	}
	return html;
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef struct
{
	gboolean           post;
	gchar             *op;
	gchar             *slug;
	gchar             *user;
	GHashTable        *fields;	/* a copy of what was submitted */
	AxisQualityStored *current;
	Form               form;
	gboolean           invalid;	/* a save the checks refused */
	gboolean           bad_op;
	gboolean           saved;
	gboolean           changed;
	GError            *error;
} EditWork;

static void
edit_work_free(gpointer data)
{
	EditWork *work = data;

	g_free(work->op);
	g_free(work->slug);
	g_free(work->user);
	g_clear_pointer(&work->fields, g_hash_table_unref);
	axis_quality_stored_free(work->current);
	form_clear(&work->form);
	g_clear_error(&work->error);
	g_free(work);
}

static void
edit_run(
	AxisWebQuality *self,
	gpointer        data
){
	EditWork *work = data;
	g_autoptr(GPtrArray) problems = NULL;

	if (!axis_quality_store_get(self->store, work->slug, &work->current, &work->error))
		return;
	if (work->current == NULL || work->current->built_in || !work->current->usable)
		return;
	if (!work->post) {
		form_from(&work->form, work->current);
		return;
	}
	form_read(work->fields, &work->form);
	if (g_strcmp0(work->op, "save") != 0) {
		work->bad_op = !apply_op(&work->form, work->op);
		/* What could not be read is said on save, not on every button. */
		g_hash_table_remove_all(work->form.errors);
		return;
	}
	if (!form_check(&work->form)) {
		work->invalid = TRUE;
		return;
	}
	problems = axis_quality_problems_new();
	if (axis_quality_store_save(self->store, work->slug, work->form.revision, work->form.name, &work->form.profile,
	        work->user, &work->changed, problems, &work->error))
		work->saved = TRUE;
}

static HtmxResponse *
built_in_page(
	HtmxRequest             *request,
	const AxisQualityStored *stored
){
	g_autofree gchar *title = g_strdup_printf("%s is built in", stored->name);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("Copy it instead");
	g_autofree gchar *back = axis_web_quality_href(stored->slug);
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	BROWSER_ADD(content, axis_web_auth_notice(TRUE, "A built-in profile cannot be changed. Copy it, and edit the "
		"copy: the copy starts as this profile is now."));
	BROWSER_ADD(card, axis_web_quality_copy_form(request, stored));
	BROWSER_ADD(content, card);
	BROWSER_ADD(content, axis_web_browser_link(back, "Back to the quality profile"));
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	htmx_response_set_status(response, 403);
	return response;
}

static HtmxResponse *
edit_page(
	HtmxRequest *request,
	EditWork    *work,
	guint        status
){
	const AxisQualityStored *stored = work->current;
	g_autofree gchar *title = g_strdup_printf("Edit quality profile: %s", stored->name);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card;
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	BROWSER_ADD(content, axis_web_storage_text(NULL, "Save this quality profile stores it; every other button changes "
		"this form only, and nothing is stored until you save."));
	BROWSER_ADD(content, axis_web_storage_text("help", "Whatever a profile says, these are never taken: a sample, a "
		"password-protected release, a name that ends in an archive's or a program's extension, a name with no "
		"readable title, and a release that is not the title asked for or could be more than one. Cinema "
		"recordings and pre-release copies are taken only by a profile whose own switch for them is on."));
	if (status == 422)
		BROWSER_ADD(content, axis_web_auth_notice(TRUE, "The profile was not saved: see the problems beside the "
			"fields. Everything else you entered is kept."));
	if (status == 409)
		BROWSER_ADD(content, axis_web_auth_notice(TRUE, work->error != NULL ? work->error->message :
			"The profile was not saved."));
	card = axis_web_storage_card("Settings for this quality profile");
	BROWSER_ADD(card, edit_form(request, stored, &work->form));
	BROWSER_ADD(content, card);
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
edit_done(
	AxisWebQuality *self,
	HtmxRequest    *request,
	gpointer        data
){
	EditWork *work = data;

	if (work->current == NULL && work->error == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such quality profile.");
	if (work->current != NULL && work->current->built_in)
		return built_in_page(request, work->current);
	if (work->current != NULL && !work->current->usable)
		return axis_web_error_respond(request, 409, "Not usable", "Its stored definition is not usable, so it cannot "
			"be edited; copy another profile instead.");
	if (work->bad_op)
		return axis_web_error_respond(request, 400, "Bad request", "That is not one of the form's buttons.");
	if (work->invalid)
		return edit_page(request, work, 422);
	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_CONFLICT))
			return edit_page(request, work, 409);
		if (g_error_matches(work->error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_INVALID)) {
			if (work->form.general == NULL)
				work->form.general = g_strdup(work->error->message);
			return edit_page(request, work, 422);
		}
		return axis_web_quality_error(request, work->error);
	}
	if (work->saved) {
		g_autofree gchar *location = g_strdup_printf("/quality/%s?done=%s", work->slug, work->changed ? "saved" :
			"unchanged");

		if (work->changed)
			axis_web_storage_audit(request, "quality profile %s saved", work->slug);
		return axis_web_auth_redirect(request, location);
	}
	(void)self;
	return edit_page(request, work, 200);
}

/* ── Handlers ───────────────────────────────────────────────────── */

static HtmxResponse *
handle_edit_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	GHashTable *query = htmx_request_get_query_params(request);
	EditWork *work;

	if (!axis_quality_slug_valid(slug))
		return axis_web_error_respond(request, 404, "Not found", "There is no such quality profile.");
	if (query != NULL && g_hash_table_size(query) > 0)
		return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
	work = g_new0(EditWork, 1);
	work->slug = g_strdup(slug);
	return axis_web_quality_dispatch(user_data, request, edit_run, edit_done, work, edit_work_free);
}

/* Every button: the fields are copied here, on the main loop, and read on
 * the worker with the profile as it is stored. */
static HtmxResponse *
handle_edit(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	const gchar *op = htmx_request_get_form_value(request, "op");
	GHashTable *fields = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	EditWork *work;

	if (!axis_quality_slug_valid(slug))
		return axis_web_error_respond(request, 404, "Not found", "There is no such quality profile.");
	if (op == NULL || op[0] == '\0' || strlen(op) > 64)
		return axis_web_error_respond(request, 400, "Bad request", "Save, or one of the form's buttons.");
	work = g_new0(EditWork, 1);
	work->post = TRUE;
	work->op = g_strdup(op);
	work->slug = g_strdup(slug);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (fields != NULL) {
		g_hash_table_iter_init(&iter, fields);
		while (g_hash_table_iter_next(&iter, &key, &value))
			g_hash_table_insert(work->fields, g_strdup(key), g_strdup(value));
	}
	return axis_web_quality_dispatch(user_data, request, edit_run, edit_done, work, edit_work_free);
}

void
axis_web_quality_register_form(
	AxisWebQuality *self,
	AxisWeb        *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/quality/:slug/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/quality/:slug/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit, self);
}

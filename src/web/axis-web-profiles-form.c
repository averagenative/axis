/*
 * axis-web-profiles-form.c - editing a profile: the form, its per-field
 * errors, the preview of what a change does, and the save
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The form's fields make a definition; the definition is checked by the
 * format's own checks (axis_profile_definition_check(), the same that
 * every stored definition passes), and each problem is shown beside the
 * field it is about, with every other submitted value kept.  "Preview"
 * shows, before anything is saved, what kind of change it is (one that can
 * alter the bytes produced locks the new revision; one that cannot keeps
 * the proof) and what it does to the eligibility counts.  "Save" adds a
 * revision, or nothing when nothing changed.
 *
 * Reflected text: the name and the note are text content of a textarea;
 * every other submitted value reaches an attribute only after being cut to
 * the characters a number, a code or a list of codes is made of.
 */

#include "web/axis-web-profiles-private.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "db/axis-db-conn.h"

#define GB 1e9

/* ── The form ───────────────────────────────────────────────────── */

typedef struct
{
	gchar                *name;
	gchar                *note;
	AxisProfileDefinition def;
	gboolean              lossy_ack;
	GHashTable           *errors;	/* field -> message (owned) */
	GHashTable           *texts;	/* field -> submitted text (owned), shown again */
	gchar                *general;	/* a problem of the whole */
} Form;

static void
form_clear(Form *form)
{
	g_free(form->name);
	g_free(form->note);
	g_free(form->general);
	g_clear_pointer(&form->errors, g_hash_table_unref);
	g_clear_pointer(&form->texts, g_hash_table_unref);
	memset(form, 0, sizeof(*form));
}

static void
form_init(Form *form)
{
	memset(form, 0, sizeof(*form));
	form->def.vbr_quality = -1;
	form->errors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	form->texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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

/* Only what a number, a code or a list of codes is made of: what reaches
 * an attribute again. */
static gchar *
reflect(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text != NULL ? text : ""; *p != '\0' && out->len < 64; p++) {
		if (g_ascii_isalnum(*p) || *p == '.' || *p == ',' || *p == ' ' || *p == '_' || *p == '-')
			g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

/* The submitted fields are a copy taken on the main loop: the worker reads
 * them, never the request. */
static const gchar *
field_text(
	GHashTable  *fields,
	Form        *form,
	const gchar *field
){
	const gchar *text = fields != NULL ? g_hash_table_lookup(fields, field) : NULL;

	g_hash_table_insert(form->texts, g_strdup(field), reflect(text));
	return text != NULL ? text : "";
}

/* A whole number; @optional: empty is 0. */
static gint64
form_int(
	GHashTable  *request,
	Form        *form,
	const gchar *field,
	gboolean     optional
){
	const gchar *text = field_text(request, form, field);
	gint64 value = 0;

	if (text[0] == '\0') {
		if (!optional)
			form_error(form, field, "This is needed.");
		return 0;
	}
	if (!g_ascii_string_to_signed(text, 10, -1000000000000000, 1000000000000000, &value, NULL)) {
		form_error(form, field, "This is a whole number.");
		return G_MININT32;
	}
	return value;
}

/* A decimal number scaled by @scale and rounded; empty is 0 when
 * @optional. */
static gint64
form_decimal(
	GHashTable  *request,
	Form        *form,
	const gchar *field,
	gdouble      scale,
	gboolean     optional
){
	const gchar *text = field_text(request, form, field);
	gchar *end = NULL;
	gdouble value;

	if (text[0] == '\0') {
		if (!optional)
			form_error(form, field, "This is needed.");
		return 0;
	}
	value = g_ascii_strtod(text, &end);
	if (end == text || *end != '\0' || !isfinite(value) || value < 0 || value * scale > 1e17) {
		form_error(form, field, "This is a number such as 0.15 or 50.");
		return -1;
	}
	return (gint64)llround(value * scale);
}

static gint
form_choice(
	GHashTable         *request,
	Form               *form,
	const gchar        *field,
	const gchar *const *choices
){
	const gchar *text = field_text(request, form, field);
	guint i;

	for (i = 0; choices[i] != NULL; i++) {
		if (strcmp(choices[i], text) == 0)
			return (gint)i;
	}
	form_error(form, field, "Choose one of the offered values.");
	return -1;
}

static gboolean
form_check(
	GHashTable  *request,
	const gchar *field
){
	return request != NULL && g_strcmp0(g_hash_table_lookup(request, field), "yes") == 0;
}

static const gchar *const video_codecs[] = { "hevc", "h264", NULL };
static const gchar *const audio_codecs[] = { "flac", "opus", "aac", "mp3", NULL };
static const gchar *const depths[] = { "8", "10", NULL };
static const gchar *const hdr_choices[] = { "skip", "keep", NULL };
static const gchar *const encoder_choices[] = { "", "software", "nvenc", "vaapi", NULL };
static const gchar *const audio_choices[] = { "copy", "copy_add_stereo", "stereo_first", NULL };
static const gchar *const subtitle_choices[] = { "copy", "text_only", NULL };
static const gchar *const container_choices[] = { "mkv", "mp4", NULL };
static const gchar *const resolution_choices[] = { "", "sd", "720p", "1080p", "2160p", "other", NULL };
static const gchar *const vbr_choices[] = { "0", "1", "2", "3", "4", NULL };

/* Where a definition's problem is shown. */
static const gchar *
form_field_of(
	const gchar *member,
	gchar      **owned
){
	static const struct
	{
		const gchar *member;
		const gchar *field;
	} map[] = {
		{ "quality.default", "crf" }, { "quality.rules", "rules" }, { "encoders", "encoder1" },
		{ "stereo_bitrate", "stereo_kbps" }, { "keep_languages", "languages" },
		{ "efficient_bpp_milli", "efficient_bpp" }, { "min_saving_bytes", "min_saving_gb" },
		{ "bitrate", "bitrate_kbps" }, { "mono_bitrate", "mono_kbps" }, { "allow_lossy_source", "allow_lossy" }
	};
	guint i;
	guint n = 0;
	gchar part[24];

	*owned = NULL;
	for (i = 0; i < G_N_ELEMENTS(map); i++) {
		if (strcmp(map[i].member, member) == 0)
			return map[i].field;
	}
	/* quality.rules.N[.member] */
	if (sscanf(member, "quality.rules.%u.%23s", &n, part) == 2 && n >= 1 && n <= AXIS_PROFILE_MAX_RULES) {
		const gchar *suffix = strcmp(part, "resolution") == 0 ? "res" : strcmp(part, "min_bytes") == 0 ? "min_gb" :
			strcmp(part, "max_bytes") == 0 ? "max_gb" : "crf";

		*owned = g_strdup_printf("rule%u_%s", n, suffix);
		return *owned;
	}
	if (sscanf(member, "quality.rules.%u", &n) == 1 && n >= 1 && n <= AXIS_PROFILE_MAX_RULES) {
		*owned = g_strdup_printf("rule%u_crf", n);
		return *owned;
	}
	return member[0] != '\0' ? member : NULL;
}

/* The submitted form as a definition of @kind, with every problem by
 * field.  Returns whether there is none. */
static gboolean
form_read(
	GHashTable      *request,
	AxisProfileKind  kind,
	gboolean         lossy_before,
	Form            *form
){
	AxisProfileDefinition *def = &form->def;
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	const gchar *name = g_hash_table_lookup(request, "name");
	const gchar *note = g_hash_table_lookup(request, "note");
	guint i;
	gint at;

	form_init(form);
	def->kind = kind;
	form->name = name != NULL ? axis_profile_clean_text(name, AXIS_PROFILE_NAME_MAX_CHARS, FALSE) : NULL;
	if (form->name == NULL) {
		form->name = g_strdup(name != NULL && g_utf8_validate(name, -1, NULL) ? name : "");
		form_error(form, "name", "A profile needs a name.");
	}
	form->note = note != NULL && note[0] != '\0' ? axis_profile_clean_text(note, AXIS_PROFILE_NOTE_MAX_CHARS, TRUE) :
		NULL;
	if (form->note == NULL)
		form->note = g_strdup("");

	if (kind == AXIS_PROFILE_KIND_VIDEO) {
		static const struct
		{
			const gchar   *field;
			AxisMediaType  type;
		} libraries[] = {
			{ "lib_movies", AXIS_MEDIA_MOVIES }, { "lib_tv", AXIS_MEDIA_TV }, { "lib_other", AXIS_MEDIA_OTHER }
		};
		const gchar *languages;

		for (i = 0; i < G_N_ELEMENTS(libraries); i++) {
			if (form_check(request, libraries[i].field))
				def->libraries |= AXIS_TARGET_ROOT(libraries[i].type);
		}
		at = form_choice(request, form, "codec", video_codecs);
		def->codec = at == 1 ? AXIS_TCODEC_H264 : AXIS_TCODEC_HEVC;
		at = form_choice(request, form, "bit_depth", depths);
		def->bit_depth = at == 1 ? 10 : 8;
		def->keep_hdr = form_choice(request, form, "hdr", hdr_choices) == 1;
		for (i = 0; i < AXIS_ENCODER_COUNT; i++) {
			g_autofree gchar *field = g_strdup_printf("encoder%u", i + 1);

			at = form_choice(request, form, field, encoder_choices);
			if (at > 0)
				def->encoders[def->n_encoders++] = (AxisEncoderFamily)(at - 1);
		}
		def->crf = (gint)form_int(request, form, "crf", FALSE);
		for (i = 0; i < AXIS_PROFILE_MAX_RULES; i++) {
			g_autofree gchar *res_field = g_strdup_printf("rule%u_res", i + 1);
			g_autofree gchar *min_field = g_strdup_printf("rule%u_min_gb", i + 1);
			g_autofree gchar *max_field = g_strdup_printf("rule%u_max_gb", i + 1);
			g_autofree gchar *crf_field = g_strdup_printf("rule%u_crf", i + 1);
			AxisProfileRule rule;

			memset(&rule, 0, sizeof(rule));
			at = form_choice(request, form, res_field, resolution_choices);
			rule.resolution = at > 0 ? at : 0;
			rule.min_bytes = form_decimal(request, form, min_field, GB, TRUE);
			rule.max_bytes = form_decimal(request, form, max_field, GB, TRUE);
			rule.crf = (gint)form_int(request, form, crf_field, TRUE);
			/* An empty row is no condition. */
			if (rule.resolution == 0 && rule.min_bytes == 0 && rule.max_bytes == 0 && rule.crf == 0)
				continue;
			if (rule.crf == 0)
				form_error(form, crf_field, "A condition needs its CRF.");
			/* A size of 0 typed into a condition is not "none". */
			if (rule.min_bytes == 0 && g_strcmp0(g_hash_table_lookup(form->texts, min_field), "") != 0)
				rule.min_bytes = -1;
			if (rule.max_bytes == 0 && g_strcmp0(g_hash_table_lookup(form->texts, max_field), "") != 0)
				rule.max_bytes = -1;
			/* Kept in the row it was typed in, so that a problem shows there. */
			def->rules[def->n_rules] = rule;
			def->n_rules++;
			if (def->n_rules != i + 1)
				form_error(form, crf_field, "Fill the conditions from the top, without empty rows between them.");
		}
		at = form_choice(request, form, "audio", audio_choices);
		def->audio = at == 1 ? AXIS_AUDIO_COPY_ADD_STEREO : at == 2 ? AXIS_AUDIO_STEREO_FIRST : AXIS_AUDIO_COPY;
		if (def->audio != AXIS_AUDIO_COPY)
			def->stereo_bitrate = (gint)(form_int(request, form, "stereo_kbps", FALSE) * 1000);
		else
			field_text(request, form, "stereo_kbps");
		languages = field_text(request, form, "languages");
		{
			g_auto(GStrv) parts = g_strsplit_set(languages, ", ", -1);

			for (i = 0; parts[i] != NULL; i++) {
				g_autofree gchar *code = g_ascii_strdown(parts[i], -1);

				if (code[0] == '\0')
					continue;
				if (def->n_languages >= AXIS_PROFILE_MAX_LANGUAGES || strlen(code) != 3) {
					form_error(form, "languages", "Languages are up to sixteen three-letter codes such as eng, fre.");
					break;
				}
				memcpy(def->languages[def->n_languages++], code, 4);
			}
		}
		at = form_choice(request, form, "subtitles", subtitle_choices);
		def->subtitles = at == 1 ? AXIS_SUBTITLES_PLAIN_TEXT : AXIS_SUBTITLES_COPY;
		at = form_choice(request, form, "container", container_choices);
		def->container = at == 1 ? AXIS_CONTAINER_MP4 : AXIS_CONTAINER_MATROSKA;
		def->efficient_milli = (gint)form_decimal(request, form, "efficient_bpp", 1000.0, FALSE);
	} else {
		if (form_check(request, "lib_music"))
			def->libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC);
		if (form_check(request, "lib_audiobooks"))
			def->libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS);
		at = form_choice(request, form, "codec", audio_codecs);
		def->codec = at == 1 ? AXIS_TCODEC_OPUS : at == 2 ? AXIS_TCODEC_AAC : at == 3 ? AXIS_TCODEC_MP3 :
			AXIS_TCODEC_FLAC;
		if (def->codec == AXIS_TCODEC_OPUS || def->codec == AXIS_TCODEC_AAC) {
			def->bitrate = (gint)(form_int(request, form, "bitrate_kbps", FALSE) * 1000);
			def->mono_bitrate = (gint)(form_int(request, form, "mono_kbps", TRUE) * 1000);
		} else {
			field_text(request, form, "bitrate_kbps");
			field_text(request, form, "mono_kbps");
		}
		if (def->codec == AXIS_TCODEC_MP3)
			def->vbr_quality = form_choice(request, form, "vbr_quality", vbr_choices);
		else
			field_text(request, form, "vbr_quality");
		if (def->codec == AXIS_TCODEC_FLAC) {
			def->reduce_to_cd = form_check(request, "reduce_to_cd");
		} else {
			def->allow_lossy = form_check(request, "allow_lossy");
			def->efficient_percent = (gint)form_int(request, form, "efficient_percent", FALSE);
		}
		form->lossy_ack = form_check(request, "lossy_ack");
		/* Lossy to lossy loses quality: allowing it needs saying so. */
		if (def->allow_lossy && !lossy_before && !form->lossy_ack)
			form_error(form, "lossy_ack", "Allowing lossy sources needs this acknowledgement.");
	}
	def->min_saving_percent = (gint)form_int(request, form, "min_saving_percent", FALSE);
	def->min_saving_bytes = form_decimal(request, form, "min_saving_gb", GB, TRUE);

	/* The format's own checks, problem by problem beside their fields. */
	axis_profile_definition_check(def, problems);
	for (i = 0; i < problems->len; i++) {
		const AxisProfileProblem *p = g_ptr_array_index(problems, i);
		g_autofree gchar *owned = NULL;
		const gchar *field = form_field_of(p->field, &owned);

		if (field != NULL)
			form_error(form, field, p->message);
		else if (form->general == NULL)
			form->general = g_strdup(p->message);
	}
	return g_hash_table_size(form->errors) == 0 && form->general == NULL;
}

/* The form filled from a definition (the edit page as it opens). */
static void
form_from(
	Form                      *form,
	const AxisProfileRevision *rev
){
	const AxisProfileDefinition *def = &rev->def;
	guint i;

#define SET(field, ...) g_hash_table_insert(form->texts, g_strdup(field), g_strdup_printf(__VA_ARGS__))
	form_init(form);
	form->def = *def;
	form->name = g_strdup(rev->name);
	form->note = g_strdup(rev->note);
	if (def->kind == AXIS_PROFILE_KIND_VIDEO) {
		GString *languages = g_string_new(NULL);

		SET("codec", "%s", axis_target_codec_to_string(def->codec));
		SET("bit_depth", "%d", def->bit_depth);
		SET("hdr", "%s", def->keep_hdr ? "keep" : "skip");
		for (i = 0; i < AXIS_ENCODER_COUNT; i++) {
			g_autofree gchar *field = g_strdup_printf("encoder%u", i + 1);

			g_hash_table_insert(form->texts, g_strdup(field), g_strdup(i < def->n_encoders ?
				axis_encoder_family_to_string(def->encoders[i]) : ""));
		}
		SET("crf", "%d", def->crf);
		for (i = 0; i < def->n_rules; i++) {
			const AxisProfileRule *r = &def->rules[i];
			g_autofree gchar *res = g_strdup_printf("rule%u_res", i + 1);
			g_autofree gchar *min = g_strdup_printf("rule%u_min_gb", i + 1);
			g_autofree gchar *max = g_strdup_printf("rule%u_max_gb", i + 1);
			g_autofree gchar *crf = g_strdup_printf("rule%u_crf", i + 1);
			gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

			SET(res, "%s", resolution_choices[r->resolution >= 0 && r->resolution <= 5 ? r->resolution : 0]);
			if (r->min_bytes > 0)
				SET(min, "%s", g_ascii_formatd(buffer, sizeof(buffer), "%g", (gdouble)r->min_bytes / GB));
			if (r->max_bytes > 0)
				SET(max, "%s", g_ascii_formatd(buffer, sizeof(buffer), "%g", (gdouble)r->max_bytes / GB));
			SET(crf, "%d", r->crf);
		}
		SET("audio", "%s", audio_choices[def->audio == AXIS_AUDIO_COPY_ADD_STEREO ? 1 :
			def->audio == AXIS_AUDIO_STEREO_FIRST ? 2 : 0]);
		if (def->stereo_bitrate > 0)
			SET("stereo_kbps", "%d", def->stereo_bitrate / 1000);
		for (i = 0; i < def->n_languages; i++)
			g_string_append_printf(languages, "%s%s", i > 0 ? ", " : "", def->languages[i]);
		SET("languages", "%s", languages->str);
		g_string_free(languages, TRUE);
		SET("subtitles", "%s", def->subtitles == AXIS_SUBTITLES_PLAIN_TEXT ? "text_only" : "copy");
		SET("container", "%s", def->container == AXIS_CONTAINER_MP4 ? "mp4" : "mkv");
		SET("efficient_bpp", "%d.%03d", def->efficient_milli / 1000, def->efficient_milli % 1000);
	} else {
		SET("codec", "%s", axis_target_codec_to_string(def->codec));
		if (def->bitrate > 0)
			SET("bitrate_kbps", "%d", def->bitrate / 1000);
		if (def->mono_bitrate > 0)
			SET("mono_kbps", "%d", def->mono_bitrate / 1000);
		if (def->vbr_quality >= 0)
			SET("vbr_quality", "%d", def->vbr_quality);
		if (def->efficient_percent > 0)
			SET("efficient_percent", "%d", def->efficient_percent);
	}
	SET("min_saving_percent", "%d", def->min_saving_percent);
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		SET("min_saving_gb", "%s", g_ascii_formatd(buffer, sizeof(buffer), "%g", (gdouble)def->min_saving_bytes / GB));
	}
#undef SET
}

/* ── Rendering ──────────────────────────────────────────────────── */

typedef struct
{
	HtmxNode *container;
	Form     *form;
} Render;

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
	g_autofree gchar *id = g_strdup_printf("profile-%s", field);
	g_autofree gchar *help_id = g_strdup_printf("profile-%s-help", field);
	g_autofree gchar *error_id = g_strdup_printf("profile-%s-error", field);
	const gchar *error = g_hash_table_lookup(r->form->errors, field);
	HtmxLabel *label = htmx_label_new_for(id);
	g_autofree gchar *described = error != NULL ? (help != NULL ? g_strdup_printf("%s %s", help_id, error_id) :
		g_strdup(error_id)) : (help != NULL ? g_strdup(help_id) : NULL);

	htmx_element_add_class(HTMX_ELEMENT(div), "field");
	if (error != NULL)
		htmx_element_add_class(HTMX_ELEMENT(div), "setting-invalid");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_element_set_id(HTMX_ELEMENT(control), id);
	if (described != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(control), "aria-describedby", described);
	if (error != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(control), "aria-invalid", "true");
	BROWSER_ADD(div, label);
	BROWSER_ADD(div, control);
	if (help != NULL) {
		HtmxNode *p = axis_web_storage_text("help", help);

		htmx_element_set_id(HTMX_ELEMENT(p), help_id);
		BROWSER_ADD(div, p);
	}
	if (error != NULL) {
		HtmxNode *p = axis_web_storage_text("field-error", error);

		htmx_element_set_id(HTMX_ELEMENT(p), error_id);
		htmx_element_set_attribute(HTMX_ELEMENT(p), "role", "alert");
		BROWSER_ADD(div, p);
	}
	BROWSER_ADD(r->container, div);
}

static HtmxNode *
text_input(
	Render      *r,
	const gchar *field
){
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);
	const gchar *value = g_hash_table_lookup(r->form->texts, field);
	g_autofree gchar *safe = reflect(value);

	htmx_input_set_name(input, field);
	htmx_input_set_value(input, safe);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	return HTMX_NODE(input);
}

static HtmxNode *
select_input(
	Render             *r,
	const gchar        *field,
	const gchar *const *values,
	const gchar *const *labels
){
	HtmxSelect *select = htmx_select_new_with_name(field);
	const gchar *current = g_hash_table_lookup(r->form->texts, field);
	guint i;

	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels != NULL ? labels[i] : values[i]);

		htmx_option_set_selected(option, g_strcmp0(current, values[i]) == 0);
		BROWSER_ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
checkbox(
	const gchar *field,
	const gchar *text,
	gboolean     checked,
	const gchar *error
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	g_autofree gchar *id = g_strdup_printf("profile-%s", field);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	if (error != NULL)
		htmx_element_add_class(HTMX_ELEMENT(div), "setting-invalid");
	htmx_input_set_name(input, field);
	htmx_input_set_value(input, "yes");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), text);
	BROWSER_ADD(div, input);
	BROWSER_ADD(div, label);
	if (error != NULL)
		BROWSER_ADD(div, axis_web_storage_text("field-error", error));
	return HTMX_NODE(div);
}

static HtmxNode *
fieldset(const gchar *legend)
{
	HtmxFieldset *set = htmx_fieldset_new();

	htmx_element_add_class(HTMX_ELEMENT(set), "profile-group");
	BROWSER_ADD(set, htmx_legend_new_with_text(legend));
	return HTMX_NODE(set);
}

static HtmxNode *
edit_form(
	HtmxRequest               *request,
	const AxisProfileRevision *rev,
	Form                      *form
){
	static const gchar *const encoder_labels[] = { "(none)", "software (x265, x264)", "NVIDIA NVENC", "VA-API", NULL };
	static const gchar *const audio_labels[] = {
		"copy every track", "copy, and add a stereo AAC track", "a stereo AAC track first, the original after", NULL
	};
	static const gchar *const resolution_labels[] = { "(any)", "SD", "720p", "1080p", "2160p", "other", NULL };
	g_autofree gchar *action = g_strdup_printf("/profiles/%s/edit", rev->slug);
	g_autofree gchar *expected = g_strdup_printf("%d", rev->number);
	HtmxNode *html = axis_web_auth_post_form(request, action);
	HtmxNode *set;
	Render r;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(html), "form-wide");
	axis_web_auth_add(html, axis_web_auth_hidden("expected", expected));
	if (form->general != NULL)
		axis_web_auth_add(html, axis_web_auth_notice(TRUE, form->general));
	r.form = form;

	set = fieldset("Name and note");
	r.container = set;
	{
		/* A person's text: text content of a textarea, never an attribute. */
		HtmxTextarea *name = htmx_textarea_new_with_name("name");
		HtmxTextarea *note = htmx_textarea_new_with_name("note");

		htmx_textarea_set_rows(name, 1);
		htmx_node_set_text_content(HTMX_NODE(name), form->name != NULL ? form->name : "");
		add_field(&r, "name", "Name", HTMX_NODE(name), "Up to 80 characters; shown as text, nothing else.");
		htmx_textarea_set_rows(note, 3);
		htmx_node_set_text_content(HTMX_NODE(note), form->note != NULL ? form->note : "");
		add_field(&r, "note", "Note", HTMX_NODE(note), "What it is for, for whoever reads it next (optional).");
	}
	axis_web_auth_add(html, set);

	set = fieldset("Libraries it is for");
	r.container = set;
	if (rev->kind == AXIS_PROFILE_KIND_VIDEO) {
		BROWSER_ADD(set, checkbox("lib_movies", "Movies", (form->def.libraries & AXIS_TARGET_ROOT(AXIS_MEDIA_MOVIES)) != 0,
			g_hash_table_lookup(form->errors, "libraries")));
		BROWSER_ADD(set, checkbox("lib_tv", "TV", (form->def.libraries & AXIS_TARGET_ROOT(AXIS_MEDIA_TV)) != 0, NULL));
		BROWSER_ADD(set, checkbox("lib_other", "Other video", (form->def.libraries & AXIS_TARGET_ROOT(AXIS_MEDIA_OTHER)) != 0,
			NULL));
	} else {
		BROWSER_ADD(set, checkbox("lib_music", "Music", (form->def.libraries & AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC)) != 0,
			g_hash_table_lookup(form->errors, "libraries")));
		BROWSER_ADD(set, checkbox("lib_audiobooks", "Audiobooks",
			(form->def.libraries & AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS)) != 0, NULL));
	}
	axis_web_auth_add(html, set);

	if (rev->kind == AXIS_PROFILE_KIND_VIDEO) {
		static const gchar *const codec_labels[] = { "HEVC", "H.264", NULL };
		static const gchar *const hdr_labels[] = {
			"skip HDR sources (an SDR profile)", "keep HDR10 and HLG (10-bit HEVC, metadata kept)", NULL
		};
		static const gchar *const subtitle_labels[] = { "copy every subtitle track", "plain text only", NULL };
		static const gchar *const container_labels[] = { "Matroska (MKV)", "MP4", NULL };

		set = fieldset("Picture");
		r.container = set;
		add_field(&r, "codec", "Codec", select_input(&r, "codec", video_codecs, codec_labels), NULL);
		add_field(&r, "bit_depth", "Bit depth", select_input(&r, "bit_depth", depths, NULL),
			"HEVC: 8 or 10; H.264: 8.");
		add_field(&r, "hdr", "HDR sources", select_input(&r, "hdr", hdr_choices, hdr_labels),
			"Dolby Vision and HDR10+ are never converted, and HDR is never made SDR, whatever this says.");
		for (i = 0; i < AXIS_ENCODER_COUNT; i++) {
			g_autofree gchar *field = g_strdup_printf("encoder%u", i + 1);
			g_autofree gchar *label = g_strdup_printf("Encoder, choice %u", i + 1);

			add_field(&r, field, label, select_input(&r, field, encoder_choices, encoder_labels), i == 0 ?
				"In order of preference; the first one the workers have is used." : NULL);
		}
		axis_web_auth_add(html, set);

		set = fieldset("Quality rule");
		r.container = set;
		add_field(&r, "crf", "Default CRF", text_input(&r, "crf"), "For a file no condition below matches. HEVC: an "
			"even number from 20 to 32; H.264: 18, 19, 20, 22, 24, 26 or 28. Lower is better and larger.");
		BROWSER_ADD(set, axis_web_storage_text("help", "Conditions, the first that matches a file wins: a resolution, "
			"a size (at least, and below, in GB), or both, and the CRF they give. Leave a row empty for none."));
		if (g_hash_table_lookup(form->errors, "rules") != NULL)
			BROWSER_ADD(set, axis_web_storage_text("field-error", g_hash_table_lookup(form->errors, "rules")));
		for (i = 0; i < AXIS_PROFILE_MAX_RULES; i++) {
			g_autofree gchar *res = g_strdup_printf("rule%u_res", i + 1);
			g_autofree gchar *min = g_strdup_printf("rule%u_min_gb", i + 1);
			g_autofree gchar *max = g_strdup_printf("rule%u_max_gb", i + 1);
			g_autofree gchar *crf = g_strdup_printf("rule%u_crf", i + 1);
			g_autofree gchar *heading = g_strdup_printf("Condition %u", i + 1);
			HtmxNode *row = fieldset(heading);
			Render inner;

			htmx_element_add_class(HTMX_ELEMENT(row), "profile-rule");
			inner.container = row;
			inner.form = form;
			add_field(&inner, res, "Resolution", select_input(&inner, res, resolution_choices, resolution_labels), NULL);
			add_field(&inner, min, "At least (GB)", text_input(&inner, min), NULL);
			add_field(&inner, max, "Below (GB)", text_input(&inner, max), NULL);
			add_field(&inner, crf, "CRF", text_input(&inner, crf), NULL);
			BROWSER_ADD(set, row);
		}
		axis_web_auth_add(html, set);

		set = fieldset("Sound and subtitles");
		r.container = set;
		add_field(&r, "audio", "Audio", select_input(&r, "audio", audio_choices, audio_labels),
			"Tracks are copied, never re-encoded: a stereo track is only ever added.");
		add_field(&r, "stereo_kbps", "Stereo track (kbit/s)", text_input(&r, "stereo_kbps"),
			"96 to 320, when a stereo track is added.");
		add_field(&r, "languages", "Audio languages to keep", text_input(&r, "languages"),
			"Three-letter codes, such as eng, fre; empty keeps every track. A track of unknown language is always "
			"kept, and a file that would be left without sound is not converted.");
		add_field(&r, "subtitles", "Subtitles", select_input(&r, "subtitles", subtitle_choices, subtitle_labels),
			"Plain text only skips a file with image or styled subtitles; MP4 needs it.");
		add_field(&r, "container", "Container", select_input(&r, "container", container_choices, container_labels),
			NULL);
		axis_web_auth_add(html, set);
	} else {
		static const gchar *const codec_labels[] = { "FLAC", "Opus", "AAC", "MP3", NULL };
		static const gchar *const vbr_labels[] = { "V0", "V1", "V2", "V3", "V4", NULL };
		gboolean lossy = form->def.allow_lossy;

		set = fieldset("Sound");
		r.container = set;
		add_field(&r, "codec", "Codec", select_input(&r, "codec", audio_codecs, codec_labels), NULL);
		add_field(&r, "bitrate_kbps", "Bitrate (kbit/s)", text_input(&r, "bitrate_kbps"), "Opus 16 to 256, AAC 32 "
			"to 320.");
		add_field(&r, "mono_kbps", "Bitrate for a mono source (kbit/s)", text_input(&r, "mono_kbps"),
			"Optional, below the bitrate.");
		add_field(&r, "vbr_quality", "MP3 quality", select_input(&r, "vbr_quality", vbr_choices, vbr_labels),
			"MP3 only.");
		BROWSER_ADD(set, checkbox("reduce_to_cd", "FLAC only: reduce more than 16 bits or 48 kHz to CD quality",
			form->def.reduce_to_cd, g_hash_table_lookup(form->errors, "reduce_to_cd")));
		BROWSER_ADD(set, checkbox("allow_lossy", "Convert lossy sources too (lossy to lossy loses quality)", lossy,
			g_hash_table_lookup(form->errors, "allow_lossy")));
		BROWSER_ADD(set, checkbox("lossy_ack", "I understand that converting a lossy source to another lossy one "
			"loses quality that cannot be had back", form->lossy_ack, g_hash_table_lookup(form->errors, "lossy_ack")));
		axis_web_auth_add(html, set);
	}

	set = fieldset("When a file is worth converting");
	r.container = set;
	if (rev->kind == AXIS_PROFILE_KIND_VIDEO)
		add_field(&r, "efficient_bpp", "Already efficient at (bits per pixel)", text_input(&r, "efficient_bpp"),
			"A source already in the profile's codec at or below this is left as it is: 0.010 to 1.000.");
	else
		add_field(&r, "efficient_percent", "Already efficient at (% of the bitrate)",
			text_input(&r, "efficient_percent"), "Lossy codecs: 100 to 400.");
	add_field(&r, "min_saving_percent", "Minimum saving (%)", text_input(&r, "min_saving_percent"),
		"5 to 95: never below 5%, and a file is never converted unless estimated smaller.");
	add_field(&r, "min_saving_gb", "Minimum saving (GB)", text_input(&r, "min_saving_gb"), "0 for none.");
	axis_web_auth_add(html, set);

	{
		HtmxDiv *buttons = htmx_div_new();
		HtmxButton *preview = htmx_button_new();
		HtmxButton *save = htmx_button_new();

		htmx_element_add_class(HTMX_ELEMENT(buttons), "actions");
		htmx_element_set_attribute(HTMX_ELEMENT(preview), "type", "submit");
		htmx_element_set_attribute(HTMX_ELEMENT(preview), "name", "action");
		htmx_element_set_attribute(HTMX_ELEMENT(preview), "value", "preview");
		htmx_node_set_text_content(HTMX_NODE(preview), "Preview the change");
		htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
		htmx_element_set_attribute(HTMX_ELEMENT(save), "name", "action");
		htmx_element_set_attribute(HTMX_ELEMENT(save), "value", "save");
		htmx_element_add_class(HTMX_ELEMENT(save), "button-primary");
		htmx_node_set_text_content(HTMX_NODE(save), "Save as a new version");
		BROWSER_ADD(buttons, preview);
		BROWSER_ADD(buttons, save);
		axis_web_auth_add(html, buttons);
	}
	return html;
}

/* ── The preview ────────────────────────────────────────────────── */

typedef struct
{
	gboolean            valid;
	gboolean            same;
	gboolean            carries;
	GPtrArray          *changed;	/* static member names */
	AxisEstimatePreview counts;
	gboolean            counted;
	gchar              *count_error;
} Preview;

static void
preview_clear(Preview *p)
{
	g_clear_pointer(&p->changed, g_ptr_array_unref);
	g_free(p->count_error);
	memset(p, 0, sizeof(*p));
}

static const gchar *
member_label(const gchar *name)
{
	const AxisProfileMember *members = axis_profile_members();
	guint i;

	for (i = 0; members[i].name != NULL; i++) {
		if (strcmp(members[i].name, name) == 0)
			return members[i].label;
	}
	return name;
}

static gboolean
member_locks(const gchar *name)
{
	const AxisProfileMember *members = axis_profile_members();
	guint i;

	for (i = 0; members[i].name != NULL; i++) {
		if (strcmp(members[i].name, name) == 0)
			return members[i].klass != AXIS_PROFILE_MEMBER_SELECTION;
	}
	return TRUE;
}

static HtmxNode *
counts_row(
	const gchar *label,
	gint64       before,
	gint64       after,
	gboolean     bytes
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *a = htmx_td_new();
	HtmxTd *b = htmx_td_new();
	HtmxTd *c = htmx_td_new();
	g_autofree gchar *x = bytes ? axis_web_browser_format_size((guint64)MAX(before, 0)) :
		axis_web_browser_format_count((guint64)MAX(before, 0));
	g_autofree gchar *y = bytes ? axis_web_browser_format_size((guint64)MAX(after, 0)) :
		axis_web_browser_format_count((guint64)MAX(after, 0));

	htmx_node_set_text_content(HTMX_NODE(a), label);
	htmx_element_add_class(HTMX_ELEMENT(b), "num");
	htmx_node_set_text_content(HTMX_NODE(b), x);
	htmx_element_add_class(HTMX_ELEMENT(c), "num");
	htmx_node_set_text_content(HTMX_NODE(c), y);
	BROWSER_ADD(row, a);
	BROWSER_ADD(row, b);
	BROWSER_ADD(row, c);
	return HTMX_NODE(row);
}

static HtmxNode *
preview_card(
	const AxisProfileRevision *rev,
	Form                      *form,
	const Preview             *p
){
	/* Counts and sizes, both in the size column's width. */
	static const AxisWebColumn columns[] = {
		{ "Figure", AXIS_WEB_COL_NAME },
		{ "Now", AXIS_WEB_COL_SIZE },
		{ "After the change", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("What this change would do");
	guint i;

	if (!p->valid) {
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, "The form has problems, shown beside each field: there is "
			"nothing to preview yet."));
		return card;
	}
	if (p->same) {
		gboolean renamed = g_strcmp0(form->name, rev->name) != 0 || g_strcmp0(form->note, rev->note) != 0;

		BROWSER_ADD(card, axis_web_storage_text(NULL, renamed ? "The definition is the same: only the name or the "
			"note change. Saving adds a version with the same definition, which keeps the current version's "
			"proof and its figures." : "Nothing changes: saving would add no version."));
	} else {
		GString *locks = g_string_new(NULL);
		GString *carries = g_string_new(NULL);

		for (i = 0; p->changed != NULL && i < p->changed->len; i++) {
			const gchar *name = g_ptr_array_index(p->changed, i);
			GString *into = member_locks(name) ? locks : carries;

			if (strstr(into->str, member_label(name)) == NULL)
				g_string_append_printf(into, "%s%s", into->len > 0 ? ", " : "", member_label(name));
		}
		if (locks->len > 0) {
			g_autofree gchar *text = g_strdup_printf("This change can alter the files produced (%s): the new version "
				"is locked until it is proven by hand from passed trials, whatever the current one is.", locks->str);

			BROWSER_ADD(card, axis_web_browser_warning(text));
		}
		if (carries->len > 0) {
			g_autofree gchar *text = g_strdup_printf("Changes that only decide which files are converted, never what "
				"is made of one: %s. On their own these keep the current version's proof.", carries->str);

			BROWSER_ADD(card, axis_web_storage_text(NULL, text));
		}
		g_string_free(locks, TRUE);
		g_string_free(carries, TRUE);
	}
	/* The lock the new version would have. */
	BROWSER_ADD(card, axis_web_storage_text("help", !axis_profile_revision_is_unlocked(rev) ?
		"The current version is locked, so the new one will be too: it cannot replace files until it is proven." :
		p->carries ? "The new version keeps the current version's proof: it stays proven." :
		"The new version will be locked."));

	if (p->count_error != NULL) {
		BROWSER_ADD(card, axis_web_auth_notice(TRUE, p->count_error));
	} else if (p->counted) {
		HtmxNode *table = axis_web_table_new("preview", columns);
		HtmxTbody *body = htmx_tbody_new();
		const AxisEstimateCounts *b = &p->counts.before;
		const AxisEstimateCounts *a = &p->counts.after;
		guint r;

		BROWSER_ADD(body, counts_row("Files it is for", b->scope, a->scope, FALSE));
		BROWSER_ADD(body, counts_row("Files that would be converted", b->converts, a->converts, FALSE));
		BROWSER_ADD(body, counts_row("Size of those files now", b->now, a->now, TRUE));
		BROWSER_ADD(body, counts_row("Estimated saving", b->now - b->mid, a->now - a->mid, TRUE));
		for (r = 1; r < AXIS_ESTIMATE_N_REASONS; r++) {
			g_autofree gchar *label = NULL;

			if (b->skips[r] == a->skips[r] || r == AXIS_ESTIMATE_NOT_APPLICABLE)
				continue;
			label = g_strdup_printf("Skipped: %s", axis_estimate_reason_label((AxisEstimateReason)r));
			BROWSER_ADD(body, counts_row(label, b->skips[r], a->skips[r], FALSE));
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
		if (!p->counts.complete) {
			g_autofree gchar *text = g_strdup_printf("Counted over the first %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
				" files before the time this preview may take ran out: the whole library is counted when the "
				"estimates are refreshed.", p->counts.read, p->counts.total);

			BROWSER_ADD(card, axis_web_storage_text("help", text));
		} else {
			BROWSER_ADD(card, axis_web_storage_text("help", "Every file in the library, decided now with each "
				"definition the way the estimates decide (an album as a whole). A preview saves and converts nothing."));
		}
	}
	return card;
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef enum
{
	STEP_OPEN,
	STEP_PREVIEW,
	STEP_SAVE
} Step;

typedef struct
{
	Step                 step;
	gchar               *slug;
	gchar               *user;
	gint                 expected;
	GHashTable          *fields;	/* a copy of what was submitted */
	AxisProfileRevision *current;
	gboolean             read;	/* the form was read (a POST) */
	Form                 form;
	gboolean             valid;
	Preview              preview;
	AxisProfileSaved     saved;
	GPtrArray           *problems;
	GError              *error;
} EditWork;

static void
edit_work_free(gpointer data)
{
	EditWork *work = data;

	g_free(work->slug);
	g_free(work->user);
	g_clear_pointer(&work->fields, g_hash_table_unref);
	axis_profile_revision_unref(work->current);
	form_clear(&work->form);
	preview_clear(&work->preview);
	g_clear_pointer(&work->problems, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

/* Whether the profile may be edited at all (the store says so again). */
static gboolean
editable(const AxisProfileRevision *rev)
{
	return rev != NULL && !rev->built_in && rev->usable && !rev->archived;
}

static void
edit_run(
	AxisWebProfiles *self,
	gpointer         data
){
	EditWork *work = data;
	AxisDbPool *pool = axis_estimator_get_pool(self->estimator);
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(pool, &work->error);
	if (conn == NULL)
		return;
	ok = axis_profile_store_current(conn, work->slug, &work->current, &work->error);
	axis_db_pool_release(pool, conn);
	if (!ok || !editable(work->current) || work->step == STEP_OPEN)
		return;

	work->read = TRUE;
	work->valid = form_read(work->fields, work->current->kind, work->current->def.allow_lossy, &work->form);
	if (!work->valid)
		return;
	if (work->step == STEP_SAVE) {
		work->problems = axis_profile_problems_new();
		axis_profile_store_save(pool, work->slug, work->expected, &work->form.def, work->form.name, work->form.note,
			work->user, &work->saved, work->problems, &work->error);
		return;
	}

	/* The kind of change, and its effect on the counts. */
	work->preview.valid = TRUE;
	work->preview.same = axis_profile_definition_equal(&work->current->def, &work->form.def);
	work->preview.changed = g_ptr_array_new();
	work->preview.carries = axis_profile_carries_proof(&work->current->def, &work->form.def, work->preview.changed);
	{
		g_autoptr(GError) local = NULL;
		AxisEstimateTarget before;
		AxisEstimateTarget built;
		AxisEstimateTarget after;
		AxisProfileIdentity identity;

		memset(&identity, 0, sizeof(identity));
		identity.slug = work->current->slug;
		identity.name = work->form.name;
		if (!axis_estimator_profile_target(self->estimator, work->slug, work->current->number, &before, NULL, &local) ||
		    !axis_profile_to_target(&work->form.def, &identity, &built)) {
			work->preview.count_error = g_strdup("The counts could not be worked out. Try the preview "
				"again in a moment.");
			return;
		}
		axis_estimator_configure(self->estimator, &built, &after);
		if (!axis_estimator_preview(self->estimator, &before, &after, AXIS_WEB_PROFILES_PREVIEW_MS,
		        &work->preview.counts, &local)) {
			g_message("profiles: the preview could not count: %s", local->message);
			work->preview.count_error = g_strdup("The counts could not be worked out in time; try again in a "
				"moment.");
			return;
		}
		work->preview.counted = TRUE;
	}
}

static HtmxResponse *
edit_page(
	HtmxRequest *request,
	EditWork    *work,
	guint        status
){
	const AxisProfileRevision *rev = work->current;
	g_autofree gchar *label = axis_web_profiles_label(rev);
	g_autofree gchar *title = g_strdup_printf("Edit conversion profile: %s", label);
	g_autofree gchar *back_text = g_strdup_printf("Back to Conversion profile: %s, without saving", label);
	g_autofree gchar *back = g_strdup_printf("/profiles/%s", rev->slug);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card;
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	{
		g_autofree gchar *line = g_strdup_printf("Editing version %d. Saving adds version %d; version %d and the "
			"earlier ones are kept as they are.", rev->number, rev->number + 1, rev->number);

		BROWSER_ADD(content, axis_web_storage_text(NULL, line));
	}
	BROWSER_ADD(content, axis_web_storage_text("help", "Whatever this form says, these are never converted: pinned "
		"files, Dolby Vision, uncertain HDR, HDR10+, interlaced or possibly interlaced video, HDR made SDR, files "
		"whose facts are missing or do not add up, and anything not estimated smaller by at least 5%."));
	if (status == 422)
		BROWSER_ADD(content, axis_web_auth_notice(TRUE, "The profile was not saved: see the problems beside the "
			"fields. Everything else you entered is kept."));
	if (work->step == STEP_PREVIEW)
		BROWSER_ADD(content, preview_card(rev, &work->form, &work->preview));
	card = axis_web_storage_card("Settings for this conversion profile");
	BROWSER_ADD(card, edit_form(request, rev, &work->form));
	BROWSER_ADD(content, card);
	BROWSER_ADD(content, axis_web_browser_link(back, back_text));
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
edit_done(
	AxisWebProfiles *self,
	HtmxRequest     *request,
	gpointer         data
){
	EditWork *work = data;

	/* The profile first: a built-in one is never edited. */
	if (work->current == NULL && work->error == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is no such profile.");
	if (work->current != NULL && work->current->built_in)
		return axis_web_error_respond(request, 403, "Built in", "A built-in profile cannot be changed; copy it and "
			"edit the copy.");
	if (work->current != NULL && !work->current->usable)
		return axis_web_error_respond(request, 409, "Not usable", "Its current definition is not usable, so it "
			"cannot be edited; make a copy of another profile instead.");
	if (work->current != NULL && work->current->archived)
		return axis_web_error_respond(request, 409, "Archived", "The profile is archived; select Offer this profile again on its page first.");
	if (work->read && !work->valid)
		return edit_page(request, work, 422);
	if (work->step == STEP_SAVE) {
		if (work->error != NULL && g_error_matches(work->error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_INVALID) &&
		    work->problems != NULL && work->problems->len > 0) {
			/* The checks in the store found what the form's did not: shown
			 * the same way. */
			guint i;

			for (i = 0; i < work->problems->len; i++) {
				const AxisProfileProblem *p = g_ptr_array_index(work->problems, i);

				if (work->form.general == NULL)
					work->form.general = g_strdup(p->message);
			}
		}
		if (work->error != NULL && work->current != NULL &&
		    (g_error_matches(work->error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_INVALID) ||
		     g_error_matches(work->error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_CONFLICT))) {
			if (work->form.general == NULL)
				work->form.general = g_strdup(work->error->message);
			return edit_page(request, work, g_error_matches(work->error, AXIS_PROFILE_ERROR,
				AXIS_PROFILE_ERROR_CONFLICT) ? 409 : 422);
		}
		if (work->error != NULL)
			return axis_web_profiles_error(request, work->error);
		{
			g_autofree gchar *location = g_strdup_printf("/profiles/%s?done=%s", work->slug, work->saved.created ?
				"saved" : "unchanged");

			if (work->saved.created)
				axis_web_storage_audit(request, "profile %s: revision %d saved (%s)", work->slug, work->saved.number,
					work->saved.carried ? "the proof carried forward" : "locked");
			else
				axis_web_storage_audit(request, "profile %s saved without a change: no revision added", work->slug);
			return axis_web_auth_redirect(request, location);
		}
	}
	if (work->error != NULL)
		return axis_web_profiles_error(request, work->error);
	if (work->step == STEP_OPEN)
		form_from(&work->form, work->current);
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

	if (!axis_profile_slug_valid(slug))
		return axis_web_error_respond(request, 404, "Not found", "There is no such profile.");
	if (query != NULL && g_hash_table_size(query) > 0)
		return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
	work = g_new0(EditWork, 1);
	work->step = STEP_OPEN;
	work->slug = g_strdup(slug);
	return axis_web_profiles_dispatch(user_data, request, edit_run, edit_done, work, edit_work_free);
}

/* Preview or save: the fields are copied here, on the main loop, and read
 * on the worker with the profile's current revision. */
static HtmxResponse *
handle_edit(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *slug = params != NULL ? g_hash_table_lookup(params, "slug") : NULL;
	const gchar *action = htmx_request_get_form_value(request, "action");
	const gchar *expected = htmx_request_get_form_value(request, "expected");
	GHashTable *fields = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	EditWork *work;
	gint64 number = 0;

	if (!axis_profile_slug_valid(slug))
		return axis_web_error_respond(request, 404, "Not found", "There is no such profile.");
	if (g_strcmp0(action, "preview") != 0 && g_strcmp0(action, "save") != 0)
		return axis_web_error_respond(request, 400, "Bad request", "Preview or save.");
	if (expected == NULL || !g_ascii_string_to_signed(expected, 10, 1, G_MAXINT32, &number, NULL))
		return axis_web_error_respond(request, 400, "Bad request", "The form does not say which version it edits.");

	work = g_new0(EditWork, 1);
	work->step = g_strcmp0(action, "save") == 0 ? STEP_SAVE : STEP_PREVIEW;
	work->slug = g_strdup(slug);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->expected = (gint)number;
	work->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (fields != NULL) {
		g_hash_table_iter_init(&iter, fields);
		while (g_hash_table_iter_next(&iter, &key, &value))
			g_hash_table_insert(work->fields, g_strdup(key), g_strdup(value));
	}
	return axis_web_profiles_dispatch(user_data, request, edit_run, edit_done, work, edit_work_free);
}

void
axis_web_profiles_register_form(
	AxisWebProfiles *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/profiles/:slug/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/profiles/:slug/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit, self);
}

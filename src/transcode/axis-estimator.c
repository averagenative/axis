/*
 * axis-estimator.c - the estimator object: settings, targets, the model
 * and its calibration, one file's estimates
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-estimator-load.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-analysis.h"

G_DEFINE_QUARK(axis-estimate-error-quark, axis_estimate_error)

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const encoder_choices[] = { "software", "nvenc", "vaapi", NULL };
static const gchar *const encoder_labels[] = { "Software (x265, x264)", "NVIDIA (NVENC)", "VA-API (Intel, AMD)", NULL };

/* The minimum saving, the "already efficient" thresholds and the 4K sizes
 * were settings before conversion profiles; they are members of a profile
 * now (a built-in's are fixed; a copy's are its own). */
static const AxisSettingInfo setting_table[] = {
	{
		AXIS_ESTIMATE_SETTING_ENCODER, AXIS_SETTING_CHOICE,
		"Encoder the video estimates assume",
		"Software (x265, x264), NVIDIA (NVENC) or VA-API (Intel, AMD): the one the workers have. A conversion "
		"profile that does not list it is estimated with its own first choice. Hardware encoders make larger files at a similar picture; "
		"the measurement encodes with this one too.",
		0, "software", 0, 0, 0, 0, encoder_choices
	},
	{
		AXIS_ESTIMATE_SETTING_FFMPEG, AXIS_SETTING_STRING,
		"ffmpeg for measuring samples",
		"The ffmpeg the sample measurement encodes with: a name found on the daemon's PATH, or an absolute "
		"path.",
		0, "ffmpeg", 1, 255, 0, 0, NULL
	},
	{
		AXIS_ESTIMATE_SETTING_SAMPLE_FILES, AXIS_SETTING_INTEGER,
		"Files measured per picture size",
		"How many files of each resolution class (or of audio) one measurement encodes excerpts of.",
		0, "3", 1, 10, 0, 0, NULL
	},
	{
		AXIS_ESTIMATE_SETTING_SAMPLE_SECONDS, AXIS_SETTING_INTEGER,
		"Length of a measured excerpt, in seconds",
		"Each file is measured on three excerpts of this length, at a quarter, half and three quarters of it.",
		0, "20", 2, 120, 0, 0, NULL
	},
	{
		AXIS_ESTIMATE_SETTING_SAMPLE_TIMEOUT, AXIS_SETTING_DURATION,
		"Time limit of one excerpt",
		"An excerpt that takes longer is stopped and its file left out. A 4K excerpt on a slow processor can "
		"take many minutes.",
		0, "30m", 30, 6 * 3600, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_estimator_get_setting_table(void)
{
	axis_setting_set_choice_labels(AXIS_ESTIMATE_SETTING_ENCODER, encoder_labels);
	return setting_table;
}

static gboolean
declared(
	AxisEstimator *self,
	const gchar   *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

gint64
axis_estimator_get_integer(
	AxisEstimator *self,
	const gchar   *key,
	gint64         fallback
){
	const AxisSettingInfo *info;

	if (self->settings == NULL)
		return fallback;
	info = axis_settings_lookup(self->settings, key, NULL);
	if (info == NULL)
		return fallback;
	switch (info->type) {
	case AXIS_SETTING_INTEGER:
		return axis_settings_get_integer(self->settings, key);
	case AXIS_SETTING_DURATION:
		return axis_settings_get_duration(self->settings, key);
	case AXIS_SETTING_BYTE_SIZE:
		return axis_settings_get_byte_size(self->settings, key);
	default:
		return fallback;
	}
}

gchar *
axis_estimator_get_string(
	AxisEstimator *self,
	const gchar   *key,
	const gchar   *fallback
){
	gchar *value = declared(self, key) ? axis_settings_get_string(self->settings, key) : NULL;

	if (value == NULL || value[0] == '\0') {
		g_free(value);
		return g_strdup(fallback);
	}
	return value;
}

void
axis_estimator_get_config(
	AxisEstimator      *self,
	AxisEstimateConfig *config
){
	g_autofree gchar *encoder = NULL;

	axis_estimate_config_init(config);
	encoder = axis_estimator_get_string(self, AXIS_ESTIMATE_SETTING_ENCODER, "software");
	if (!axis_encoder_family_from_string(encoder, &config->encoder))
		config->encoder = AXIS_ENCODER_SOFTWARE;
}

gchar *
axis_estimator_config_key(
	AxisEstimator *self,
	GPtrArray     *scheduled
){
	AxisEstimateConfig config;
	GString *key = g_string_new(NULL);
	guint i;

	axis_estimator_get_config(self, &config);
	g_string_append_printf(key, "model %d; %s; revisions", AXIS_ESTIMATE_MODEL_VERSION,
		axis_encoder_family_to_string(config.encoder));
	for (i = 0; scheduled != NULL && i < scheduled->len; i++) {
		const AxisProfileRevision *rev = g_ptr_array_index(scheduled, i);

		g_string_append_printf(key, "%s%" G_GINT64_FORMAT, i > 0 ? "," : " ", rev->id);
	}
	return g_string_free(key, FALSE);
}

gboolean
axis_estimator_scheduled(
	AxisDbConn  *conn,
	gboolean     requested,
	GPtrArray  **revisions,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	*revisions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_revision_unref);
	params[0] = requested ? "true" : "false";
	if (!axis_db_conn_exec(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
	        " WHERE r.id = p.current_revision_id AND (p.built_in"
	        "  OR EXISTS (SELECT 1 FROM conversion_profile_assignment a WHERE a.profile_id = p.id)"
	        "  OR ($1::boolean AND NOT p.archived AND EXISTS (SELECT 1 FROM estimate_refresh_request q"
	        "   JOIN conversion_profile_revision qr ON qr.id = q.revision_id WHERE qr.profile_id = p.id)))"
	        " ORDER BY p.built_in DESC, p.id", 1, params, &result, error)) {
		g_clear_pointer(revisions, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*revisions, axis_profile_revision_from_row(result, i, 0));
	return TRUE;
}

GArray *
axis_estimator_targets_of(
	AxisEstimator *self,
	GPtrArray     *revisions
){
	GArray *targets = g_array_new(FALSE, TRUE, sizeof(AxisEstimateTarget));
	AxisEstimateConfig config;
	guint i;

	axis_estimator_get_config(self, &config);
	for (i = 0; revisions != NULL && i < revisions->len; i++) {
		const AxisProfileRevision *rev = g_ptr_array_index(revisions, i);
		AxisEstimateTarget built;
		AxisEstimateTarget configured;

		if (!axis_profile_revision_target(rev, &built)) {
			g_message("estimate: profile %s revision %d is not estimated: %s", rev->slug, rev->number,
				rev->unusable != NULL ? rev->unusable : "its definition is not usable");
			continue;
		}
		axis_estimate_target_configure(&built, &config, &configured);
		g_array_append_val(targets, configured);
	}
	return targets;
}

gboolean
axis_estimator_profile_target(
	AxisEstimator        *self,
	const gchar          *slug,
	gint                  number,
	AxisEstimateTarget   *target,
	AxisProfileRevision **revision,
	GError              **error
){
	g_autoptr(AxisProfileRevision) found = NULL;
	AxisEstimateTarget built;
	AxisEstimateConfig config;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && target != NULL, FALSE);

	memset(target, 0, sizeof(*target));
	if (revision != NULL)
		*revision = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_profile_store_revision(conn, slug, number, &found, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (found == NULL) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND, "no such profile revision");
		return FALSE;
	}
	if (!axis_profile_revision_target(found, &built)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "the profile revision's "
			"definition is not usable");
		return FALSE;
	}
	axis_estimator_get_config(self, &config);
	axis_estimate_target_configure(&built, &config, target);
	if (revision != NULL)
		*revision = g_steal_pointer(&found);
	return TRUE;
}

AxisDbPool *
axis_estimator_get_pool(AxisEstimator *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->pool;
}

void
axis_estimator_configure(
	AxisEstimator            *self,
	const AxisEstimateTarget *built,
	AxisEstimateTarget       *target
){
	AxisEstimateConfig config;

	axis_estimator_get_config(self, &config);
	axis_estimate_target_configure(built, &config, target);
}

gboolean
axis_estimator_profile_targets(
	AxisEstimator  *self,
	gboolean        scheduled,
	GArray        **targets,
	GError        **error
){
	g_autoptr(GPtrArray) revisions = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && targets != NULL, FALSE);

	*targets = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (scheduled) {
		ok = axis_estimator_scheduled(conn, FALSE, &revisions, error);
	} else {
		ok = axis_db_conn_exec(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
			" WHERE r.id = p.current_revision_id AND NOT p.archived ORDER BY p.built_in DESC, p.id", 0, NULL, &result,
			error);
		if (ok) {
			revisions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_revision_unref);
			for (i = 0; i < axis_db_result_get_n_rows(result); i++)
				g_ptr_array_add(revisions, axis_profile_revision_from_row(result, i, 0));
		}
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*targets = axis_estimator_targets_of(self, revisions);
	return TRUE;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisEstimator *
axis_estimator_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
){
	AxisEstimator *self;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisEstimator, 1);
	self->ref_count = 1;
	self->enabled = TRUE;
	self->pool = pool;
	self->storage = axis_storage_ref(storage);
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	g_mutex_init(&self->lock);
	return self;
}

AxisEstimator *
axis_estimator_ref(AxisEstimator *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_estimator_unref(AxisEstimator *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	if (self->model != NULL)
		axis_estimator_model_unref(self->model);
	g_strfreev(self->tool_command);
	g_free(self->scratch_parent);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_estimator_set_enabled(
	AxisEstimator *self,
	gboolean       enabled
){
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->enabled, enabled ? 1 : 0);
}

gboolean
axis_estimator_is_enabled(AxisEstimator *self)
{
	return self != NULL && g_atomic_int_get(&self->enabled) != 0;
}

void
axis_estimator_get_targets(
	AxisEstimator      *self,
	AxisEstimateTarget  targets[AXIS_ESTIMATE_N_TARGETS]
){
	const AxisEstimateTarget *builtins = axis_estimate_builtin_targets();
	AxisEstimateConfig config;
	guint i;

	axis_estimator_get_config(self, &config);
	for (i = 0; i < AXIS_ESTIMATE_N_TARGETS; i++)
		axis_estimate_target_configure(&builtins[i], &config, &targets[i]);
}

gboolean
axis_estimator_get_target(
	AxisEstimator      *self,
	const gchar        *id,
	AxisEstimateTarget *target
){
	const AxisEstimateTarget *builtin = axis_estimate_builtin_target(id);
	AxisEstimateConfig config;

	if (builtin == NULL)
		return FALSE;
	axis_estimator_get_config(self, &config);
	axis_estimate_target_configure(builtin, &config, target);
	return TRUE;
}

/* ── The model ──────────────────────────────────────────────────── */

void
axis_estimator_model_unref(AxisEstimateModel *model)
{
	axis_estimate_model_free(model);
}

/* At most this many of the newest samples are read (a cell needs 20). */
#define MODEL_MAX_SAMPLES "200000"

/* A sample applies to its cell whatever revision measured it: what its
 * prediction needs is on the row. */
#define MODEL_SQL \
	"SELECT p.slug, s.codec, s.family, s.quality, s.part, s.source_codec, s.width, s.height, s.frame_rate," \
	" s.source_bitrate, s.sample_rate, s.channels, s.bit_depth, s.seconds, s.output_bitrate, max(s.id) OVER ()," \
	" s.revision_id, s.out_bit_depth, s.nominal_bitrate, s.reduce_to_cd FROM estimate_sample s" \
	" JOIN conversion_profile_revision r ON r.id = s.revision_id JOIN conversion_profile p ON p.id = r.profile_id" \
	" ORDER BY s.id DESC LIMIT " MODEL_MAX_SAMPLES

gboolean
axis_estimator_read_model(
	AxisDbConn         *conn,
	gboolean            in_transaction,
	AxisEstimateModel **model,
	gint64             *newest,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisEstimateModel *out;
	guint i;

	*model = NULL;
	*newest = 0;
	if (in_transaction ? !axis_db_conn_exec(conn, MODEL_SQL, 0, NULL, &result, error) :
	    !axis_estimator_timed_exec(conn, "20s", MODEL_SQL, 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		axis_db_result_get_int64(result, 0, 15, newest);
	out = axis_estimate_model_new();
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisEstimateSample sample;
		gint64 value = -1;

		memset(&sample, 0, sizeof(sample));
		sample.target = axis_db_result_get_text(result, i, 0);
		if (!axis_target_codec_from_string(axis_db_result_get_text(result, i, 1), &sample.codec) ||
		    !axis_encoder_family_from_string(axis_db_result_get_text(result, i, 2), &sample.family))
			continue;
		axis_db_result_get_int64(result, i, 3, &value);
		sample.quality = (gint)value;
		sample.part = axis_db_result_get_text(result, i, 4);
		sample.source_codec = axis_db_result_get_text(result, i, 5);
		value = -1;
		axis_db_result_get_int64(result, i, 6, &value);
		sample.width = (gint)value;
		value = -1;
		axis_db_result_get_int64(result, i, 7, &value);
		sample.height = (gint)value;
		sample.frame_rate = axis_db_result_is_null(result, i, 8) ? -1.0 :
			g_ascii_strtod(axis_db_result_get_text(result, i, 8), NULL);
		axis_db_result_get_int64(result, i, 9, &sample.source_bitrate);
		value = -1;
		axis_db_result_get_int64(result, i, 10, &value);
		sample.sample_rate = (gint)value;
		value = -1;
		axis_db_result_get_int64(result, i, 11, &value);
		sample.channels = (gint)value;
		value = -1;
		axis_db_result_get_int64(result, i, 12, &value);
		sample.bit_depth = (gint)value;
		sample.seconds = g_ascii_strtod(axis_db_result_get_text(result, i, 13), NULL);
		axis_db_result_get_int64(result, i, 14, &sample.output_bitrate);
		axis_db_result_get_int64(result, i, 16, &sample.revision_id);
		sample.has_output_facts = TRUE;
		value = 0;
		axis_db_result_get_int64(result, i, 17, &value);
		sample.out_bit_depth = (gint)value;
		value = 0;
		axis_db_result_get_int64(result, i, 18, &value);
		sample.nominal_bitrate = (gint)value;
		axis_db_result_get_boolean(result, i, 19, &sample.reduce_to_cd);
		/* A row the model does not accept is left out (the store's
		 * constraints make that rare). */
		axis_estimate_model_add_sample(out, &sample);
	}
	axis_estimate_model_finish(out);
	*model = out;
	return TRUE;
}

AxisEstimateModel *
axis_estimator_get_model(
	AxisEstimator  *self,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisEstimateModel *model = NULL;
	AxisEstimateModel *fresh = NULL;
	AxisDbConn *conn;
	gint64 newest = 0;
	gint64 read = 0;

	g_return_val_if_fail(self != NULL, NULL);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return NULL;
	if (!axis_db_conn_exec(conn, "SELECT coalesce(max(id), 0) FROM estimate_sample", 0, NULL, &result, error)) {
		axis_db_pool_release(self->pool, conn);
		return NULL;
	}
	axis_db_result_get_int64(result, 0, 0, &newest);

	g_mutex_lock(&self->lock);
	if (self->model_ready && self->model_sample == newest)
		model = axis_estimate_model_ref(self->model);
	g_mutex_unlock(&self->lock);
	if (model != NULL) {
		axis_db_pool_release(self->pool, conn);
		return model;
	}

	if (!axis_estimator_read_model(conn, FALSE, &fresh, &read, error)) {
		axis_db_pool_release(self->pool, conn);
		return NULL;
	}
	axis_db_pool_release(self->pool, conn);

	g_mutex_lock(&self->lock);
	if (self->model != NULL)
		axis_estimator_model_unref(self->model);
	self->model = axis_estimate_model_ref(fresh);
	self->model_sample = read;
	self->model_ready = TRUE;
	g_mutex_unlock(&self->lock);
	return fresh;
}

/* ── Recording ──────────────────────────────────────────────────── */

gboolean
axis_estimator_record_sample(
	AxisEstimator             *self,
	AxisDbConn                *conn,
	const AxisEstimateSample  *sample,
	const gchar               *origin,
	gint64                     job_id,
	gint64                     file_id,
	GError                   **error
){
	g_autoptr(AxisEstimateModel) check = NULL;
	g_autofree gchar *job = job_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, job_id) : NULL;
	g_autofree gchar *file = file_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, file_id) : NULL;
	g_autofree gchar *quality = NULL;
	g_autofree gchar *width = NULL;
	g_autofree gchar *height = NULL;
	g_autofree gchar *rate = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *sample_rate = NULL;
	g_autofree gchar *channels = NULL;
	g_autofree gchar *bits = NULL;
	g_autofree gchar *seconds = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *revision = NULL;
	g_autofree gchar *out_depth = NULL;
	g_autofree gchar *nominal = NULL;
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	const gchar *params[22];
	AxisEstimateSample filled;
	AxisDbConn *own = NULL;
	gboolean ok;

	g_return_val_if_fail(self != NULL && sample != NULL, FALSE);

	/* A sample keeps what predicting it needs: given, or (a sample made
	 * before profiles, a test) read from the built-in its target names. */
	filled = *sample;
	if (!filled.has_output_facts) {
		const AxisEstimateTarget *builtin = axis_estimate_builtin_target(sample->target);

		if (builtin == NULL) {
			g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
				"the sample does not say what it was encoded to");
			return FALSE;
		}
		filled.has_output_facts = TRUE;
		filled.out_bit_depth = builtin->bit_depth;
		filled.nominal_bitrate = builtin->audio_bitrate_mono > 0 && sample->channels == 1 ?
			builtin->audio_bitrate_mono : builtin->audio_bitrate;
		filled.reduce_to_cd = builtin->reduce_to_cd;
	}
	sample = &filled;

	/* What the model would not take is not stored. */
	check = axis_estimate_model_new();
	if (g_strcmp0(origin, "measured") != 0 && g_strcmp0(origin, "converted") != 0) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "a sample's origin is "
			"measured or converted");
		return FALSE;
	}
	if (!axis_estimate_model_add_sample(check, sample)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the sample is not one the model can use");
		return FALSE;
	}

	quality = g_strdup_printf("%d", sample->quality);
	if (sample->revision_id > 0)
		revision = g_strdup_printf("%" G_GINT64_FORMAT, sample->revision_id);
	if (strcmp(sample->part, "video") == 0)
		out_depth = g_strdup_printf("%d", sample->out_bit_depth == 8 ? 8 : 10);
	else
		nominal = g_strdup_printf("%d", MAX(sample->nominal_bitrate, 0));
	if (sample->width > 0)
		width = g_strdup_printf("%d", sample->width);
	if (sample->height > 0)
		height = g_strdup_printf("%d", sample->height);
	if (sample->frame_rate > 0) {
		g_ascii_formatd(buffer, sizeof(buffer), "%.6f", sample->frame_rate);
		rate = g_strdup(buffer);
	}
	source = g_strdup_printf("%" G_GINT64_FORMAT, sample->source_bitrate);
	if (sample->sample_rate > 0)
		sample_rate = g_strdup_printf("%d", sample->sample_rate);
	if (sample->channels > 0)
		channels = g_strdup_printf("%d", sample->channels);
	if (sample->bit_depth > 0)
		bits = g_strdup_printf("%d", sample->bit_depth);
	g_ascii_formatd(buffer, sizeof(buffer), "%.6f", sample->seconds);
	seconds = g_strdup(buffer);
	output = g_strdup_printf("%" G_GINT64_FORMAT, sample->output_bitrate);

	params[0] = origin;
	params[1] = job;
	params[2] = file;
	params[3] = sample->target;
	params[4] = axis_target_codec_to_string(sample->codec);
	params[5] = axis_encoder_family_to_string(sample->family);
	params[6] = quality;
	params[7] = sample->part;
	params[8] = sample->source_codec;
	params[9] = width;
	params[10] = height;
	params[11] = rate;
	params[12] = source;
	params[13] = sample_rate;
	params[14] = channels;
	params[15] = bits;
	params[16] = seconds;
	params[17] = output;
	params[18] = revision;
	params[19] = out_depth;
	params[20] = nominal;
	params[21] = sample->reduce_to_cd ? "true" : "false";

	if (conn == NULL) {
		own = axis_db_pool_acquire(self->pool, error);
		if (own == NULL)
			return FALSE;
		conn = own;
	}
	/* The revision it names, else the current revision of the profile
	 * @target is the slug of; neither is refused (a NULL revision). */
	ok = axis_db_conn_exec(conn,
		"INSERT INTO estimate_sample (origin, job_id, file_id, revision_id, codec, family, quality, part,"
		" source_codec, width, height, frame_rate, source_bitrate, sample_rate, channels, bit_depth, seconds,"
		" output_bitrate, out_bit_depth, nominal_bitrate, reduce_to_cd)"
		" SELECT $1::text, $2::bigint, (SELECT id FROM library_file WHERE id = $3::bigint),"
		" coalesce((SELECT id FROM conversion_profile_revision WHERE id = $19::bigint),"
		"  (SELECT current_revision_id FROM conversion_profile WHERE slug = $4::text)), $5::text,"
		" $6::text, $7::integer, $8::text, $9::text, $10::integer, $11::integer, $12::double precision,"
		" $13::bigint, $14::integer, $15::integer, $16::integer, $17::double precision, $18::bigint,"
		" $20::integer, $21::integer, $22::boolean",
		22, params, NULL, error);
	if (own != NULL)
		axis_db_pool_release(self->pool, own);
	return ok;
}

gboolean
axis_estimator_count_samples(
	AxisEstimator  *self,
	GHashTable    **counts,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GHashTable *out;
	guint i;

	*counts = NULL;
	/* By the profile whose revision recorded them. */
	if (!axis_db_pool_exec(self->pool, "SELECT p.slug, count(*) FROM estimate_sample s"
	        " JOIN conversion_profile_revision r ON r.id = s.revision_id JOIN conversion_profile p ON p.id = r.profile_id"
	        " GROUP BY p.slug", 0, NULL, &result, error))
		return FALSE;
	out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 *n = g_new0(gint64, 1);

		axis_db_result_get_int64(result, i, 1, n);
		g_hash_table_insert(out, g_strdup(axis_db_result_get_text(result, i, 0)), n);
	}
	*counts = out;
	return TRUE;
}

/* ── Evaluating loaded rows ─────────────────────────────────────── */

/* Whether @idx is decided with its album for @target. */
static gboolean
in_unit(
	const AxisEstimateTarget *target,
	const AxisEstimateIndex  *idx
){
	return target->media == AXIS_TARGET_AUDIO && idx->unit != NULL && idx->unit_files > 1 &&
		g_strcmp0(idx->kind, "audio") == 0;
}

static gchar *
unit_key(const AxisEstimateIndex *idx)
{
	return g_strdup_printf("%" G_GINT64_FORMAT "/%s", idx->root_id, idx->unit);
}

/* Decides every album of @rows' tracks, read whole by one statement, into
 * @units: "root/unit" -> (file id -> result). */
static gboolean
decide_units(
	AxisDbConn               *conn,
	AxisEstimateRows         *rows,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	GHashTable               *units,
	GError                  **error
){
	g_autoptr(GHashTable) asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GArray) wanted = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisEstimateRows) members = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	for (i = 0; i < rows->files->len; i++) {
		const AxisEstimateFile *file = g_ptr_array_index(rows->files, i);
		gchar *k;

		if (!in_unit(target, &file->index))
			continue;
		k = unit_key(&file->index);
		if (g_hash_table_contains(asked, k)) {
			g_free(k);
			continue;
		}
		g_hash_table_add(asked, k);
		g_array_append_val(wanted, file->index.file_id);
	}
	if (wanted->len == 0)
		return TRUE;
	if (!axis_estimator_load_units(conn, (const gint64 *)wanted->data, wanted->len, &members, error))
		return FALSE;

	/* The members by unit, each in id order (the statement's). */
	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; i < members->files->len; i++) {
		AxisEstimateFile *member = g_ptr_array_index(members->files, i);
		gchar *k;
		GPtrArray *group;

		if (member->index.unit == NULL)
			continue;
		k = unit_key(&member->index);
		group = g_hash_table_lookup(groups, k);
		if (group == NULL) {
			group = g_ptr_array_new();
			g_hash_table_insert(groups, k, group);
		} else {
			g_free(k);
		}
		g_ptr_array_add(group, member);
	}
	g_hash_table_iter_init(&iter, groups);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GPtrArray *group = value;
		g_autofree AxisEstimateResult *member_results = g_new0(AxisEstimateResult, MAX(group->len, 1));
		GHashTable *decided = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
		guint j;

		axis_estimate_evaluate((const AxisEstimateFile *const *)group->pdata, group->len, target, model,
			member_results);
		for (j = 0; j < group->len; j++) {
			const AxisEstimateFile *member = g_ptr_array_index(group, j);

			g_hash_table_insert(decided, g_memdup2(&member->index.file_id, sizeof(gint64)),
				g_memdup2(&member_results[j], sizeof(AxisEstimateResult)));
		}
		g_hash_table_insert(units, g_strdup(key), decided);
	}
	return TRUE;
}

gboolean
axis_estimator_evaluate_rows(
	AxisDbConn               *conn,
	AxisEstimateRows         *rows,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *results,
	GError                  **error
){
	g_autoptr(GHashTable) units = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_hash_table_unref);
	guint i;

	if (target->media == AXIS_TARGET_AUDIO && !decide_units(conn, rows, target, model, units, error))
		return FALSE;
	for (i = 0; i < rows->files->len; i++) {
		const AxisEstimateFile *file = g_ptr_array_index(rows->files, i);
		const AxisEstimateIndex *idx = &file->index;

		if (in_unit(target, idx)) {
			g_autofree gchar *key = unit_key(idx);
			GHashTable *decided = g_hash_table_lookup(units, key);
			AxisEstimateResult *mine = decided != NULL ? g_hash_table_lookup(decided, &idx->file_id) : NULL;

			if (mine != NULL) {
				results[i] = *mine;
			} else {
				/* It was not among its album's files a moment later: the
				 * album was not read whole. */
				axis_estimate_eligibility(file, target, model, &results[i]);
				if (results[i].reason == AXIS_ESTIMATE_ELIGIBLE || results[i].reason == AXIS_ESTIMATE_OFFLINE) {
					results[i].reason = AXIS_ESTIMATE_ALBUM;
					results[i].album_reason = AXIS_ESTIMATE_ALBUM;
					results[i].album_file = 0;
				}
			}
		} else {
			axis_estimate_eligibility(file, target, model, &results[i]);
		}
	}
	return TRUE;
}

gboolean
axis_estimator_estimate_file(
	AxisEstimator  *self,
	gint64          file_id,
	GArray        **estimates,
	GError        **error
){
	g_autoptr(AxisEstimateRows) rows = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) revisions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_revision_unref);
	g_autoptr(GArray) targets = NULL;
	g_autofree gchar *root = NULL;
	const gchar *params[1];
	AxisEstimateModel *model;
	const AxisEstimateFile *file;
	GArray *out;
	AxisDbConn *conn;
	guint i;

	g_return_val_if_fail(self != NULL && estimates != NULL, FALSE);

	*estimates = NULL;
	model = axis_estimator_get_model(self, error);
	if (model == NULL)
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_estimator_model_unref(model);
		return FALSE;
	}
	if (!axis_estimator_load_files(conn, &file_id, 1, &rows, error))
		goto fail;
	if (rows->files->len != 1) {
		g_set_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND, "there is no file %" G_GINT64_FORMAT,
			file_id);
		goto fail;
	}
	file = g_ptr_array_index(rows->files, 0);

	/* The built-in profiles, and any profile assigned to the file's root. */
	root = g_strdup_printf("%" G_GINT64_FORMAT, file->index.root_id);
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
	        " WHERE r.id = p.current_revision_id AND (p.built_in OR EXISTS (SELECT 1 FROM"
	        " conversion_profile_assignment a WHERE a.profile_id = p.id AND a.root_id = $1::bigint))"
	        " ORDER BY p.built_in DESC, p.id", 1, params, &result, error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(revisions, axis_profile_revision_from_row(result, i, 0));
	targets = axis_estimator_targets_of(self, revisions);

	out = g_array_new(FALSE, TRUE, sizeof(AxisFileEstimate));
	for (i = 0; i < targets->len; i++) {
		AxisEstimateTarget *target = &g_array_index(targets, AxisEstimateTarget, i);
		AxisFileEstimate estimate;
		AxisEstimateCellInfo info;

		if (g_strcmp0(file->index.kind, target->media == AXIS_TARGET_VIDEO ? "video" : "audio") != 0)
			continue;
		memset(&estimate, 0, sizeof(estimate));
		estimate.target = *target;
		if (!axis_estimator_evaluate_rows(conn, rows, target, model, &estimate.result, error)) {
			g_array_unref(out);
			goto fail;
		}
		axis_estimate_model_cell_info(model, estimate.result.cell, &info);
		estimate.state = estimate.result.sized ? info.state : AXIS_CALIBRATION_NONE;
		estimate.samples = estimate.result.sized ? info.samples : 0;
		g_array_append_val(out, estimate);
	}
	axis_db_pool_release(self->pool, conn);
	axis_estimator_model_unref(model);
	*estimates = out;
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	axis_estimator_model_unref(model);
	return FALSE;
}

/* ── Requests and status ────────────────────────────────────────── */

static gboolean
enqueue_refresh(
	AxisDbConn  *conn,
	const gchar *trigger,
	gint         priority,
	gint64      *id,
	gboolean    *created,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobSpec spec;

	json_object_set_string_member(payload, "trigger", trigger);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_ESTIMATE_REFRESH_KIND;
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = AXIS_ESTIMATE_REFRESH_KIND ":run";
	spec.max_attempts = AXIS_ESTIMATE_REFRESH_ATTEMPTS;
	return axis_job_enqueue(conn, &spec, id, created, error);
}

gboolean
axis_estimator_request_refresh(
	AxisEstimator  *self,
	gint64         *job_id,
	GError        **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = enqueue_refresh(conn, "request", AXIS_ESTIMATE_REQUEST_PRIORITY, job_id, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_estimator_request_profile_refresh(
	AxisEstimator  *self,
	const gchar    *slug,
	const gchar    *requested_by,
	gint64         *job_id,
	GError        **error
){
	g_autoptr(AxisProfileRevision) current = NULL;
	g_autofree gchar *id = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_profile_store_current(conn, slug, &current, error);
	if (ok && (current == NULL || !current->usable || current->archived)) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND, current == NULL ?
			"no such profile" : "the profile cannot be estimated");
		ok = FALSE;
	}
	if (ok) {
		id = g_strdup_printf("%" G_GINT64_FORMAT, current->id);
		params[0] = id;
		params[1] = requested_by;
		ok = axis_db_conn_exec(conn, "INSERT INTO estimate_refresh_request (revision_id, requested_by)"
			" VALUES ($1::bigint, $2::text) ON CONFLICT (revision_id) DO UPDATE SET requested_at = now(),"
			" requested_by = EXCLUDED.requested_by", 2, params, NULL, error);
	}
	if (ok)
		ok = enqueue_refresh(conn, "request", AXIS_ESTIMATE_REQUEST_PRIORITY, job_id, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_estimator_enqueue_due(
	AxisEstimator  *self,
	gboolean       *enqueued,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) scheduled = NULL;
	g_autofree gchar *key = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean due = FALSE;
	gboolean created = FALSE;

	g_return_val_if_fail(self != NULL, FALSE);

	if (enqueued != NULL)
		*enqueued = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_estimator_scheduled(conn, FALSE, &scheduled, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	key = axis_estimator_config_key(self, scheduled);
	params[0] = key;
	/* After an Analysis refresh (and not while one is queued or running,
	 * so that the two follow each other on the queue), when a sample was
	 * added, when the setting or the profiles on schedule changed (a
	 * built-in's new revision, an assignment, an edit of an assigned
	 * profile), or when figures were asked for a profile. */
	if (!axis_db_conn_exec(conn,
	        "WITH last AS (SELECT analysis_snapshot_id, last_sample_id, config->>'key' AS key"
	        "  FROM estimate_snapshot ORDER BY taken_at DESC, id DESC LIMIT 1)"
	        " SELECT (SELECT max(id) FROM analysis_snapshot) IS NOT NULL"
	        "  AND NOT EXISTS (SELECT 1 FROM job WHERE kind IN ('analysis.refresh', '" AXIS_ESTIMATE_REFRESH_KIND "')"
	        "   AND state IN ('queued', 'running'))"
	        "  AND (NOT EXISTS (SELECT 1 FROM last)"
	        "   OR (SELECT max(id) FROM analysis_snapshot) > coalesce((SELECT analysis_snapshot_id FROM last), 0)"
	        "   OR coalesce((SELECT max(id) FROM estimate_sample), 0) > (SELECT last_sample_id FROM last)"
	        "   OR (SELECT key FROM last) IS DISTINCT FROM $1::text"
	        "   OR EXISTS (SELECT 1 FROM estimate_refresh_request))",
	        1, params, &result, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_result_get_boolean(result, 0, 0, &due);
	if (due) {
		if (!enqueue_refresh(conn, "schedule", 0, NULL, &created, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
		if (enqueued != NULL)
			*enqueued = created;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

gboolean
axis_estimator_run_refresh_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *trigger = "schedule";

	if (payload != NULL && json_object_has_member(payload, "trigger")) {
		trigger = json_object_get_string_member_with_default(payload, "trigger", "");
		if (g_strcmp0(trigger, "schedule") != 0 && g_strcmp0(trigger, "request") != 0) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT,
				"the refresh job's trigger is not one this daemon knows");
			return FALSE;
		}
	}
	return axis_estimator_refresh(self, trigger, axis_job_context_get_id(context), context, NULL, error);
}

gboolean
axis_estimator_run_due_job(
	AxisEstimator   *self,
	AxisJobContext  *context,
	GError         **error
){
	return axis_estimator_enqueue_due(self, NULL, error);
}

void
axis_estimate_status_clear(AxisEstimateStatus *status)
{
	g_free(status->message);
	memset(status, 0, sizeof(*status));
}

gboolean
axis_estimator_get_status(
	AxisEstimator       *self,
	AxisEstimateStatus  *status,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;

	g_return_val_if_fail(self != NULL && status != NULL, FALSE);

	memset(status, 0, sizeof(*status));
	status->progress = -1;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id, state::text, coalesce(progress, -1)::text, progress_message FROM job"
	        " WHERE kind = '" AXIS_ESTIMATE_REFRESH_KIND "' AND state IN ('queued', 'running')"
	        " ORDER BY id LIMIT 1", 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1) {
		status->refreshing = TRUE;
		axis_db_result_get_int64(result, 0, 0, &status->job_id);
		status->running = g_strcmp0(axis_db_result_get_text(result, 0, 1), "running") == 0;
		status->progress = g_ascii_strtod(axis_db_result_get_text(result, 0, 2), NULL);
		status->message = g_strdup(axis_db_result_get_text(result, 0, 3));
	}
	return TRUE;
}

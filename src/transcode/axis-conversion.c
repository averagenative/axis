/*
 * axis-conversion.c - the conversions: the object, its settings, the
 * record, what the pages read, and the actions on a conversion
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-conversion-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-write.h"

G_DEFINE_QUARK(axis-conversion-error-quark, axis_conversion_error)

/* ── Words ──────────────────────────────────────────────────────── */

const gchar *
axis_conversion_outcome_to_string(AxisConversionOutcome outcome)
{
	return outcome == AXIS_CONVERSION_REPLACE ? "replace" : "trial";
}

gboolean
axis_conversion_outcome_from_string(
	const gchar           *text,
	AxisConversionOutcome *outcome
){
	if (g_strcmp0(text, "trial") == 0)
		*outcome = AXIS_CONVERSION_TRIAL;
	else if (g_strcmp0(text, "replace") == 0)
		*outcome = AXIS_CONVERSION_REPLACE;
	else
		return FALSE;
	return TRUE;
}

const gchar *
axis_conversion_check_label(guint number)
{
	static const gchar *const labels[] = {
		NULL,
		"The converted file is the one asked for",
		"The original is unchanged",
		"The converted file has the planned streams",
		"The converted picture has the planned properties",
		"The converted file decodes whole",
		"The converted file is as long as the original",
		"The copied tracks are the original's",
		"The picture matches the original's",
		"The saving is worth it",
		"The conditions still hold"
	};

	return number >= 1 && number <= AXIS_CONVERSION_N_CHECKS ? labels[number] : "?";
}

const gchar *
axis_conversion_state_label(const gchar *state)
{
	static const gchar *const states[][2] = {
		{ "queued", "Waiting for a worker" },
		{ "encoding", "Being converted on a worker" },
		{ "verifying", "Being checked" },
		{ "placing", "Being put in place" },
		{ "done", "Done" },
		{ "refused", "Refused: a condition did not hold" },
		{ "rejected", "Rejected by a check" },
		{ "failed", "Failed" },
		{ "cancelled", "Cancelled" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; states[i][0] != NULL; i++) {
		if (g_strcmp0(states[i][0], state) == 0)
			return states[i][1];
	}
	return "Unknown";
}

const gchar *
axis_conversion_checks_where_to_string(AxisConversionChecksWhere where)
{
	switch (where) {
	case AXIS_CONVERSION_CHECKS_WORKER:
		return "worker";
	case AXIS_CONVERSION_CHECKS_WORKER_ONLY:
		return "worker_only";
	default:
		return "server";
	}
}

const gchar *
axis_conversion_checks_where_label(AxisConversionChecksWhere where)
{
	switch (where) {
	case AXIS_CONVERSION_CHECKS_WORKER:
		return "On a worker when one is connected, else on this server";
	case AXIS_CONVERSION_CHECKS_WORKER_ONLY:
		return "Only on a worker: this server does no heavy work";
	default:
		return "On this server";
	}
}

const gchar *
axis_conversion_checks_where_sentence(AxisConversionChecksWhere where)
{
	switch (where) {
	case AXIS_CONVERSION_CHECKS_WORKER:
		return "A worker re-encodes each converted file and, when one is connected that has proved the file's "
			"volume, also makes the heavy measurements of its checks (a full decode and a picture comparison), "
			"which this server judges; with no such worker this server measures them itself.";
	case AXIS_CONVERSION_CHECKS_WORKER_ONLY:
		return "A worker re-encodes each converted file and makes the heavy measurements of its checks (a full "
			"decode and a picture comparison), which this server judges: this server does no heavy work, and "
			"without a worker conversions wait.";
	default:
		return "A worker re-encodes each converted file; this server then checks it itself, which includes a full "
			"decode and a picture comparison: heavy work for this server (Settings, \"Where converted files are "
			"checked\" can send it to a worker).";
	}
}

const gchar *const *
axis_conversion_held_kinds(void)
{
	static const gchar *const kinds[] = {
		AXIS_WORKER_KIND_CONVERT, AXIS_WORKER_KIND_VERIFY, AXIS_ESTIMATE_SAMPLE_KIND, AXIS_WORKER_KIND_SAMPLE, NULL
	};

	return kinds;
}

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const preset_choices[] = {
	"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL
};

static const gchar *const checks_where_choices[] = { "server", "worker", "worker_only", NULL };

/* The Workers page's names for the values above, in the same order. */
static const gchar *const checks_where_labels[] = {
	"On this server", "On a worker when one is connected, else on this server",
	"Only on a worker: this server does no heavy work", NULL
};

static const AxisSettingInfo setting_table[] = {
	{ AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, AXIS_SETTING_DURATION, "Keep replaced originals for",
	  "When a conversion replaces a file, the original is moved into .axis-recycle at the top of its disk and "
	  "kept this long, so that it could be put back by hand; its space stays used until then. 0: removed as soon "
	  "as the new file is in place, checked and recorded.", 0, "0", 0, 365 * 86400, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_TRIAL_RETENTION, AXIS_SETTING_DURATION, "Keep trial files for",
	  "A trial's converted file is kept in .axis-work at the top of its disk this long (or until it is removed "
	  "from its conversion's page), so that it can be played.", 0, "14d", 3600, 365 * 86400, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_LEFTOVER_GRACE, AXIS_SETTING_DURATION, "Remove work-area leftovers after",
	  "A file in .axis-work that no conversion accounts for (a worker's that died) is listed on the Conversions "
	  "page, and removed when it has been there this long. A name that is not one of Axis's is never removed.",
	  0, "1d", 3600, 30 * 86400, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_MIN_FREE, AXIS_SETTING_BYTE_SIZE, "Free space to keep when converting",
	  "A conversion is refused when the disk would have less than this free after its output (the estimate's "
	  "high figure), or less than the share below, whichever is larger.", 0, "10GiB", 0, 0, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT, AXIS_SETTING_INTEGER, "Free space to keep when converting, in percent",
	  "Of the disk's size; the larger of this and the amount above is kept free.", 0, "5", 0, 50, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_SIMILARITY, AXIS_SETTING_DECIMAL, "Lowest picture similarity of a converted file",
	  "Check 8 compares a second of the original and the new file at five points with SSIM (1 is the same "
	  "picture); every point's lowest figure must be at least this. It catches a wrong or damaged picture, not "
	  "small losses of quality.", 0, "0.85", 0, 0, 0.5, 0.999, NULL },
	{ AXIS_CONVERSION_SETTING_PROVE_TRIALS, AXIS_SETTING_INTEGER, "Passed trials needed to prove a conversion profile version",
	  "How many passed trials of a conversion profile version, on as many different files, an administrator needs before "
	  "marking it proven (which allows replacing files with it).", 0, "3", 1, 20, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_OTHER_OWNER, AXIS_SETTING_BOOLEAN, "Accept a converted file owned by another user",
	  "Off: a new file whose owner and group cannot be made the original's is rejected (a worker that runs as "
	  "another user). On: it keeps the worker's.", 0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_NETWORK_FS, AXIS_SETTING_BOOLEAN, "Replace files on network filesystems",
	  "Off: a replacement is refused on NFS, SMB and other network or FUSE filesystems, where renaming two "
	  "files at once is not certain to be atomic. Trials are allowed.", 0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_FFMPEG, AXIS_SETTING_STRING, "ffmpeg for checking converted files",
	  "The ffmpeg this server decodes and compares converted files with: a name found on its PATH, or an "
	  "absolute path.", 0, "ffmpeg", 1, 255, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_FFPROBE, AXIS_SETTING_STRING, "ffprobe for checking converted files",
	  "The ffprobe this server reads converted files with.", 0, "ffprobe", 1, 255, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_PRESET, AXIS_SETTING_CHOICE, "Software encoders' preset",
	  "x265's and x264's speed preset for conversions: slower presets make smaller files at the same quality.",
	  0, "medium", 0, 0, 0, 0, preset_choices },
	{ AXIS_CONVERSION_SETTING_CHECKS_WHERE, AXIS_SETTING_CHOICE, "Where converted files are checked",
	  "Where the heavy measurements of a converted file's checks are made (a full decode, the picture's packets, "
	  "the copied tracks, a picture comparison). On this server: the checks are measured here. On a worker when "
	  "one is connected: on a worker that is connected and has proved the file's disk, else on this server. Only "
	  "on a worker: this server does no heavy work; with no such worker a conversion waits. This server decides every check in "
	  "every mode, but a worker is trusted with the figures it measures.", 0, "server", 0, 0, 0, 0,
	  checks_where_choices },
	{ AXIS_CONVERSION_SETTING_WAIT_AFTER_START, AXIS_SETTING_BOOLEAN, "Conversion work waits after a restart",
	  "On: when the daemon starts, nothing is encoded, checked or sample-measured for conversions, here or on a "
	  "worker, until an administrator presses Resume conversion work on the Conversions page, so that a server "
	  "that stopped under the load does not start it again by itself. Downloads, imports and scans go on.", 0,
	  "true", 0, 0, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_CHECK_THREADS, AXIS_SETTING_INTEGER, "Threads for checks run on this server",
	  "How many threads each decoder and the filters of the checks' ffmpeg may use when this server measures a "
	  "converted file's checks (they always run at the lowest priority). Fewer take longer and load the server "
	  "less.", 0, "2", 1, 64, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_CONFIRM_WHOLE, AXIS_SETTING_BOOLEAN, "Confirm a worker's checks on the whole file",
	  "On: before a worker's measurements are used, this server reads the whole converted file and requires its "
	  "SHA-256 to be the one the worker read (about a minute per 10 GB on a spinning disk, at the lowest "
	  "priority). Off: only the file's stamp and samples of it are compared.", 0, "true", 0, 0, 0, 0, NULL },
	{ AXIS_CONVERSION_SETTING_RUN_FAILURES, AXIS_SETTING_INTEGER, "Pause a conversion run after failures in a row",
	  "A conversion run pauses itself, and says why, when this many of its files in a row were rejected by a check "
	  "of the converted file or failed (an encode that kept failing, a source that could not be read): something "
	  "they share (the conversion profile, a worker, a disk) is more likely than bad luck, and a person should look before it "
	  "goes through the rest of the list. A file the checks found not worth it, or that changed, does not count.",
	  0, "3", 1, 20, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_conversions_get_setting_table(void)
{
	axis_setting_set_choice_labels(AXIS_CONVERSION_SETTING_CHECKS_WHERE, checks_where_labels);
	return setting_table;
}

static gboolean
declared(
	AxisConversions *self,
	const gchar     *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

static const AxisSettingInfo *
default_of(const gchar *key)
{
	guint i;

	for (i = 0; setting_table[i].key != NULL; i++) {
		if (strcmp(setting_table[i].key, key) == 0)
			return &setting_table[i];
	}
	return NULL;
}

gint64
axis_conversion_setting_integer(
	AxisConversions *self,
	const gchar     *key,
	gint64           fallback
){
	const AxisSettingInfo *info = declared(self, key) ? axis_settings_lookup(self->settings, key, NULL) : NULL;

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

gdouble
axis_conversion_setting_decimal(
	AxisConversions *self,
	const gchar     *key,
	gdouble          fallback
){
	return declared(self, key) ? axis_settings_get_decimal(self->settings, key) : fallback;
}

gboolean
axis_conversion_setting_boolean(
	AxisConversions *self,
	const gchar     *key,
	gboolean         fallback
){
	return declared(self, key) ? axis_settings_get_boolean(self->settings, key) : fallback;
}

static gchar *
setting_string(
	AxisConversions *self,
	const gchar     *key
){
	gchar *value = declared(self, key) ? axis_settings_get_string(self->settings, key) : NULL;
	const AxisSettingInfo *info;

	if (value != NULL && value[0] != '\0')
		return value;
	g_free(value);
	info = default_of(key);
	return g_strdup(info != NULL ? info->default_value : "");
}

/* A program's command: the tuning's, else the setting's (found on PATH). */
static gchar **
tool_command(
	AxisConversions    *self,
	gchar             **tuned,
	const gchar        *key
){
	g_autofree gchar *name = NULL;
	gchar *path;
	gchar **argv;

	g_mutex_lock(&self->lock);
	if (tuned != NULL) {
		argv = g_strdupv(tuned);
		g_mutex_unlock(&self->lock);
		return argv;
	}
	g_mutex_unlock(&self->lock);
	name = setting_string(self, key);
	path = g_path_is_absolute(name) ? g_strdup(name) : g_find_program_in_path(name);
	argv = g_new0(gchar *, 2);
	argv[0] = path != NULL ? path : g_strdup(name);
	return argv;
}

gchar **
axis_conversion_ffmpeg(AxisConversions *self)
{
	return tool_command(self, self->ffmpeg, AXIS_CONVERSION_SETTING_FFMPEG);
}

gchar **
axis_conversion_ffprobe(AxisConversions *self)
{
	return tool_command(self, self->ffprobe, AXIS_CONVERSION_SETTING_FFPROBE);
}

void
axis_conversion_options(
	AxisConversions    *self,
	AxisConvertOptions *options
){
	g_autofree gchar *preset = NULL;
	guint i;

	memset(options, 0, sizeof(*options));
	g_mutex_lock(&self->lock);
	options->threads = self->tuning.threads;
	if (self->preset != NULL)
		preset = g_strdup(self->preset);
	g_mutex_unlock(&self->lock);
	if (preset == NULL)
		preset = setting_string(self, AXIS_CONVERSION_SETTING_PRESET);
	/* One of the table's own strings: nothing of a setting reaches the
	 * vector but a word of the table. */
	options->software_preset = "medium";
	for (i = 0; preset_choices[i] != NULL; i++) {
		if (strcmp(preset_choices[i], preset) == 0)
			options->software_preset = preset_choices[i];
	}
}

void
axis_conversion_options_recorded(
	const gchar        *preset,
	guint               threads,
	AxisConvertOptions *options
){
	guint i;

	memset(options, 0, sizeof(*options));
	options->threads = MIN(threads, 64);
	options->software_preset = "medium";
	for (i = 0; preset != NULL && preset_choices[i] != NULL; i++) {
		if (strcmp(preset_choices[i], preset) == 0)
			options->software_preset = preset_choices[i];
	}
}

AxisCompatVerdict
axis_conversion_verdict(
	AxisConversions *self,
	gint64           root_id,
	const gchar     *slug
){
	AxisConversionVerdictFunc func;
	gpointer data;

	g_mutex_lock(&self->lock);
	func = self->verdict;
	data = self->verdict_data;
	g_mutex_unlock(&self->lock);
	return func != NULL ? func(data, root_id, slug) : AXIS_VERDICT_UNKNOWN;
}

AxisConversionHookVerdict
axis_conversion_hook(
	AxisConversions *self,
	const gchar     *step,
	gint64           conversion_id
){
	AxisConversionHook hook;
	gpointer data;

	g_mutex_lock(&self->lock);
	hook = self->hook;
	data = self->hook_data;
	g_mutex_unlock(&self->lock);
	return hook != NULL ? hook(step, conversion_id, data) : AXIS_CONVERSION_HOOK_CONTINUE;
}

guint
axis_conversion_tool_timeout(
	AxisConversions *self,
	gint64           duration_ms
){
	guint tuned;
	gint64 ms;

	g_mutex_lock(&self->lock);
	tuned = self->tuning.tool_timeout_ms;
	g_mutex_unlock(&self->lock);
	if (tuned != 0)
		return tuned;
	ms = 600000 + 6 * MAX(duration_ms, 0);
	return (guint)MIN(ms, (gint64)AXIS_CONVERT_MAX_TIMEOUT_MS);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisConversions *
axis_conversions_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisStorage   *storage,
	AxisProbe     *probe,
	AxisEstimator *estimator
){
	AxisConversions *self;

	g_return_val_if_fail(pool != NULL && storage != NULL && probe != NULL && estimator != NULL, NULL);

	self = g_new0(AxisConversions, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->storage = axis_storage_ref(storage);
	self->probe = axis_probe_ref(probe);
	self->estimator = axis_estimator_ref(estimator);
	self->hold = axis_job_hold_new();
	g_mutex_init(&self->lock);
	return self;
}

AxisConversions *
axis_conversions_ref(AxisConversions *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_conversions_unref(AxisConversions *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	axis_storage_unref(self->storage);
	axis_probe_unref(self->probe);
	axis_estimator_unref(self->estimator);
	g_strfreev(self->ffmpeg);
	g_strfreev(self->ffprobe);
	g_free(self->preset);
	axis_job_hold_unref(self->hold);
	g_mutex_clear(&self->lock);
	g_free(self);
}

AxisStorage *
axis_conversions_get_storage(AxisConversions *self)
{
	return self->storage;
}

AxisConversionChecksWhere
axis_conversions_checks_where(AxisConversions *self)
{
	g_autofree gchar *value = setting_string(self, AXIS_CONVERSION_SETTING_CHECKS_WHERE);

	if (g_strcmp0(value, "worker") == 0)
		return AXIS_CONVERSION_CHECKS_WORKER;
	if (g_strcmp0(value, "worker_only") == 0)
		return AXIS_CONVERSION_CHECKS_WORKER_ONLY;
	return AXIS_CONVERSION_CHECKS_SERVER;
}

gint64
axis_conversions_keep_original(AxisConversions *self)
{
	return axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, 0);
}

/* ── Held after a restart ───────────────────────────────────────── */

AxisJobHold *
axis_conversions_get_hold(AxisConversions *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->hold;
}

void
axis_conversions_hold_after_start(AxisConversions *self)
{
	g_return_if_fail(self != NULL);

	if (!axis_conversion_setting_boolean(self, AXIS_CONVERSION_SETTING_WAIT_AFTER_START, TRUE))
		return;
	g_mutex_lock(&self->lock);
	self->held_since_us = g_get_real_time();
	g_mutex_unlock(&self->lock);
	axis_job_hold_set(self->hold, TRUE);
	g_message("conversions: conversion work is paused since the daemon started (convert.wait_after_start): "
		"nothing is encoded, checked or measured until an administrator resumes it on the Conversions page");
}

gboolean
axis_conversions_is_held(
	AxisConversions *self,
	gint64          *since_us
){
	gboolean held;

	g_return_val_if_fail(self != NULL, FALSE);

	held = axis_job_hold_is_held(self->hold);
	if (since_us != NULL) {
		g_mutex_lock(&self->lock);
		*since_us = held ? self->held_since_us : 0;
		g_mutex_unlock(&self->lock);
	}
	return held;
}

void
axis_conversions_set_on_resume(
	AxisConversions *self,
	void           (*func)(gpointer data),
	gpointer         data
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->on_resume = func;
	self->on_resume_data = data;
	g_mutex_unlock(&self->lock);
}

gboolean
axis_conversions_resume(
	AxisConversions  *self,
	const gchar      *user,
	gboolean         *was_held,
	GError          **error
){
	void (*func)(gpointer data);
	gpointer data;
	gboolean held;

	g_return_val_if_fail(self != NULL, FALSE);

	held = axis_job_hold_is_held(self->hold);
	if (was_held != NULL)
		*was_held = held;
	if (!held)
		return TRUE;
	axis_job_hold_set(self->hold, FALSE);
	g_mutex_lock(&self->lock);
	self->held_since_us = 0;
	func = self->on_resume;
	data = self->on_resume_data;
	g_mutex_unlock(&self->lock);
	g_message("audit: %s resumed conversion work (paused since the daemon started)", user != NULL ? user : "?");
	if (func != NULL)
		func(data);
	/* The checks that waited are queued by the tidy job. */
	return axis_conversions_request_tidy(self, error);
}

void
axis_conversions_set_verdict(
	AxisConversions           *self,
	AxisConversionVerdictFunc  func,
	gpointer                   data
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->verdict = func;
	self->verdict_data = data;
	g_mutex_unlock(&self->lock);
}

void
axis_conversions_set_tuning(
	AxisConversions            *self,
	const AxisConversionTuning *tuning
){
	g_return_if_fail(self != NULL && tuning != NULL);

	g_mutex_lock(&self->lock);
	g_strfreev(self->ffmpeg);
	g_strfreev(self->ffprobe);
	g_free(self->preset);
	self->tuning = *tuning;
	self->ffmpeg = tuning->ffmpeg != NULL ? g_strdupv((gchar **)tuning->ffmpeg) : NULL;
	self->ffprobe = tuning->ffprobe != NULL ? g_strdupv((gchar **)tuning->ffprobe) : NULL;
	self->preset = g_strdup(tuning->software_preset);
	self->tuning.ffmpeg = NULL;
	self->tuning.ffprobe = NULL;
	self->tuning.software_preset = NULL;
	g_mutex_unlock(&self->lock);
}

void
axis_conversions_set_hook(
	AxisConversions    *self,
	AxisConversionHook  hook,
	gpointer            data
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->hook = hook;
	self->hook_data = data;
	g_mutex_unlock(&self->lock);
}

/* ── Small things ───────────────────────────────────────────────── */

gchar *
axis_conversion_int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

gboolean
axis_conversion_split(
	const gchar  *root_path,
	const gchar  *rel_path,
	gchar       **folder,
	gchar       **name
){
	g_autoptr(GString) bytes = NULL;
	g_autofree gchar *normalised = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *problem = NULL;
	const gchar *slash;

	*folder = NULL;
	*name = NULL;
	if (root_path == NULL || rel_path == NULL || !axis_library_name_decode(rel_path, &bytes) ||
	    memchr(bytes->str, '\0', bytes->len) != NULL)
		return FALSE;
	slash = strrchr(bytes->str, '/');
	if (slash == NULL) {
		dir = g_strdup(root_path);
		*name = g_strdup(bytes->str);
	} else {
		g_autofree gchar *below = g_strndup(bytes->str, (gsize)(slash - bytes->str));

		dir = root_path[0] != '\0' ? g_strconcat(root_path, "/", below, NULL) : g_strdup(below);
		*name = g_strdup(slash + 1);
	}
	if ((*name)[0] == '\0' || !axis_storage_path_normalize(dir, FALSE, &normalised, &problem)) {
		g_clear_pointer(name, g_free);
		return FALSE;
	}
	*folder = g_steal_pointer(&normalised);
	return TRUE;
}

/* ── The row ────────────────────────────────────────────────────── */

#define ROW_COLUMNS \
	"c.id, coalesce(c.file_id, 0), coalesce(c.volume_id, 0), coalesce(c.root_id, 0), c.root_path, c.rel_path," \
	" c.new_rel_path, c.container, c.revision_id, c.outcome, c.state, c.requested_by, c.verdict, c.src_size," \
	" c.src_mtime_sec, c.src_mtime_nsec, c.src_ctime_sec, c.src_ctime_nsec, c.src_inode, c.src_nlink," \
	" c.source_quality, coalesce(c.job_id, 0), coalesce(c.finish_job_id, 0), coalesce(c.attempt, 0), c.worker," \
	" c.machine, c.encoder, c.args, c.output_name, coalesce(c.output_reported, 0), coalesce(c.output_inode, 0)," \
	" coalesce(c.output_size, 0), coalesce(c.output_mtime_sec, 0), coalesce(c.output_mtime_nsec, 0)," \
	" coalesce(c.output_ctime_sec, 0), coalesce(c.output_ctime_nsec, 0), c.step, c.temp_name, c.recycle_path," \
	" coalesce(c.recycle_inode, 0), coalesce(c.recycle_size, 0), c.recycle_state, c.trial_name, c.trial_state," \
	" c.checked_where, coalesce(c.verify_job_id, 0), c.verifier, c.check_plan, c.check_source_sample," \
	" (c.handed_inode IS NOT NULL), coalesce(c.handed_inode, 0), coalesce(c.handed_size, 0)," \
	" coalesce(c.handed_mtime_sec, 0), coalesce(c.handed_mtime_nsec, 0), coalesce(c.handed_ctime_sec, 0)," \
	" coalesce(c.handed_ctime_nsec, 0), c.evidence, c.checks_waiting, (c.passed_at IS NOT NULL)," \
	" coalesce(c.passed_inode, 0), coalesce(c.passed_size, 0), coalesce(c.passed_mtime_sec, 0)," \
	" coalesce(c.passed_mtime_nsec, 0), coalesce(c.passed_ctime_sec, 0), coalesce(c.passed_ctime_nsec, 0)," \
	" c.passed_sample, c.passed_digest, c.claims, c.plan_caps, c.plan_preset, coalesce(c.plan_threads, 0)," \
	" coalesce(c.checks_version, 0)"

void
axis_conversion_row_free(AxisConversionRow *row)
{
	if (row == NULL)
		return;
	g_free(row->root_path);
	g_free(row->rel_path);
	g_free(row->new_rel_path);
	g_free(row->container);
	g_free(row->state);
	g_free(row->requested_by);
	g_free(row->verdict);
	g_free(row->source_quality);
	g_free(row->worker);
	g_free(row->machine);
	g_free(row->encoder);
	g_free(row->args);
	g_free(row->output_name);
	g_free(row->step);
	g_free(row->temp_name);
	g_free(row->recycle_path);
	g_free(row->recycle_state);
	g_free(row->trial_name);
	g_free(row->trial_state);
	g_free(row->checked_where);
	g_free(row->verifier);
	g_free(row->check_plan);
	g_free(row->check_source_sample);
	g_free(row->evidence);
	g_free(row->checks_waiting);
	g_free(row->passed_sample);
	g_free(row->passed_digest);
	g_free(row->plan_caps);
	g_free(row->plan_preset);
	g_free(row);
}

static gchar *
text_at(
	AxisDbResult *result,
	guint         column
){
	return axis_db_result_is_null(result, 0, column) ? NULL : g_strdup(axis_db_result_get_text(result, 0, column));
}

static gint64
int_at(
	AxisDbResult *result,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, 0, column, &value);
	return value;
}

static AxisConversionRow *
row_from(AxisDbResult *result)
{
	AxisConversionRow *row = g_new0(AxisConversionRow, 1);
	g_autofree gchar *outcome = NULL;

	row->id = int_at(result, 0);
	row->file_id = int_at(result, 1);
	row->volume_id = int_at(result, 2);
	row->root_id = int_at(result, 3);
	row->root_path = text_at(result, 4);
	row->rel_path = text_at(result, 5);
	row->new_rel_path = text_at(result, 6);
	row->container = text_at(result, 7);
	row->revision_id = int_at(result, 8);
	outcome = text_at(result, 9);
	if (!axis_conversion_outcome_from_string(outcome, &row->outcome))
		row->outcome = AXIS_CONVERSION_TRIAL;
	row->state = text_at(result, 10);
	row->requested_by = text_at(result, 11);
	row->verdict = text_at(result, 12);
	row->src_size = int_at(result, 13);
	row->src_mtime_sec = int_at(result, 14);
	row->src_mtime_nsec = (gint)int_at(result, 15);
	row->src_ctime_sec = int_at(result, 16);
	row->src_ctime_nsec = (gint)int_at(result, 17);
	row->src_inode = int_at(result, 18);
	row->src_nlink = int_at(result, 19);
	row->source_quality = text_at(result, 20);
	row->job_id = int_at(result, 21);
	row->finish_job_id = int_at(result, 22);
	row->attempt = (gint)int_at(result, 23);
	row->worker = text_at(result, 24);
	row->machine = text_at(result, 25);
	row->encoder = text_at(result, 26);
	row->args = text_at(result, 27);
	row->output_name = text_at(result, 28);
	row->output_reported = int_at(result, 29);
	row->output_inode = int_at(result, 30);
	row->output_size = int_at(result, 31);
	row->output_mtime_sec = int_at(result, 32);
	row->output_mtime_nsec = (gint)int_at(result, 33);
	row->output_ctime_sec = int_at(result, 34);
	row->output_ctime_nsec = (gint)int_at(result, 35);
	row->step = text_at(result, 36);
	row->temp_name = text_at(result, 37);
	row->recycle_path = text_at(result, 38);
	row->recycle_inode = int_at(result, 39);
	row->recycle_size = int_at(result, 40);
	row->recycle_state = text_at(result, 41);
	row->trial_name = text_at(result, 42);
	row->trial_state = text_at(result, 43);
	row->checked_where = text_at(result, 44);
	row->verify_job_id = int_at(result, 45);
	row->verifier = text_at(result, 46);
	row->check_plan = text_at(result, 47);
	row->check_source_sample = text_at(result, 48);
	axis_db_result_get_boolean(result, 0, 49, &row->handed);
	row->handed_inode = int_at(result, 50);
	row->handed_size = int_at(result, 51);
	row->handed_mtime_sec = int_at(result, 52);
	row->handed_mtime_nsec = (gint)int_at(result, 53);
	row->handed_ctime_sec = int_at(result, 54);
	row->handed_ctime_nsec = (gint)int_at(result, 55);
	row->evidence = text_at(result, 56);
	row->checks_waiting = text_at(result, 57);
	axis_db_result_get_boolean(result, 0, 58, &row->passed);
	row->passed_inode = int_at(result, 59);
	row->passed_size = int_at(result, 60);
	row->passed_mtime_sec = int_at(result, 61);
	row->passed_mtime_nsec = (gint)int_at(result, 62);
	row->passed_ctime_sec = int_at(result, 63);
	row->passed_ctime_nsec = (gint)int_at(result, 64);
	row->passed_sample = text_at(result, 65);
	row->passed_digest = text_at(result, 66);
	row->claims = (gint)int_at(result, 67);
	row->plan_caps = text_at(result, 68);
	row->plan_preset = text_at(result, 69);
	row->plan_threads = (gint)int_at(result, 70);
	row->checks_version = (gint)int_at(result, 71);
	return row;
}

gboolean
axis_conversion_row_load(
	AxisDbConn          *conn,
	gint64               id,
	gboolean             for_update,
	AxisConversionRow  **row,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = axis_conversion_int_text(id);
	const gchar *params[1];

	*row = NULL;
	params[0] = text;
	if (!axis_db_conn_exec(conn, for_update ? "SELECT " ROW_COLUMNS " FROM conversion c WHERE c.id = $1::bigint FOR UPDATE" :
	        "SELECT " ROW_COLUMNS " FROM conversion c WHERE c.id = $1::bigint", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*row = row_from(result);
	return TRUE;
}

gboolean
axis_conversion_row_by_job(
	AxisDbConn          *conn,
	gint64               job,
	AxisConversionRow  **row,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = axis_conversion_int_text(job);
	const gchar *params[1];

	*row = NULL;
	params[0] = text;
	if (!axis_db_conn_exec(conn, "SELECT " ROW_COLUMNS " FROM conversion c WHERE c.job_id = $1::bigint FOR UPDATE", 1,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*row = row_from(result);
	return TRUE;
}

gboolean
axis_conversion_row_by_verify_job(
	AxisDbConn          *conn,
	gint64               job,
	AxisConversionRow  **row,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = axis_conversion_int_text(job);
	const gchar *params[1];

	*row = NULL;
	params[0] = text;
	if (!axis_db_conn_exec(conn, "SELECT " ROW_COLUMNS " FROM conversion c WHERE c.verify_job_id = $1::bigint"
	        " FOR UPDATE", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*row = row_from(result);
	return TRUE;
}

gboolean
axis_conversion_set_state(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *state,
	gint          failed_check,
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *id_text = axis_conversion_int_text(id);
	g_autofree gchar *check = failed_check > 0 ? axis_conversion_int_text(failed_check) : NULL;
	const gchar *params[4];

	params[0] = id_text;
	params[1] = state;
	params[2] = check;
	params[3] = text;
	return axis_db_conn_exec(conn, "UPDATE conversion SET state = $2, failed_check = coalesce($3::integer, failed_check),"
		" result_text = coalesce(left($4, 2000), result_text), updated_at = now(),"
		" finished_at = CASE WHEN $2 IN ('done', 'refused', 'rejected', 'failed', 'cancelled') THEN now() ELSE NULL END"
		" WHERE id = $1::bigint", 4, params, NULL, error);
}

gboolean
axis_conversion_record_check(
	AxisDbConn   *conn,
	gint64        id,
	guint         number,
	const gchar  *result,
	const gchar  *detail,
	GError      **error
){
	g_autofree gchar *id_text = axis_conversion_int_text(id);
	g_autofree gchar *n = axis_conversion_int_text(number);
	g_autofree gchar *clean = axis_library_text_clean(detail != NULL ? detail : "", 2000);
	const gchar *params[4];

	params[0] = id_text;
	params[1] = n;
	params[2] = result;
	params[3] = clean != NULL ? clean : "";
	return axis_db_conn_exec(conn, "INSERT INTO conversion_check (conversion_id, number, result, detail)"
		" VALUES ($1::bigint, $2::integer, $3, $4) ON CONFLICT (conversion_id, number) DO UPDATE"
		" SET result = EXCLUDED.result, detail = EXCLUDED.detail, checked_at = now()", 4, params, NULL, error);
}

gboolean
axis_conversion_record_check_pool(
	AxisDbPool   *pool,
	gint64        id,
	guint         number,
	const gchar  *result,
	const gchar  *detail,
	GError      **error
){
	AxisDbConn *conn = axis_db_pool_acquire(pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_record_check(conn, id, number, result, detail, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

gboolean
axis_conversion_queue_finish(
	AxisDbConn  *conn,
	gint64       id,
	gint64       delay_ms,
	gint64      *job,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *key = g_strdup_printf(AXIS_CONVERSION_FINISH_KEY "%" G_GINT64_FORMAT, id);
	g_autofree gchar *id_text = axis_conversion_int_text(id);
	g_autofree gchar *job_text = NULL;
	const gchar *params[2];
	AxisJobSpec spec;
	gint64 job_id = 0;

	json_object_set_int_member(payload, "conversion", id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_CONVERSION_FINISH_KIND;
	spec.queue = AXIS_CONVERSION_QUEUE;
	spec.payload = payload;
	spec.delay_ms = delay_ms;
	spec.dedupe_key = key;
	spec.max_attempts = AXIS_CONVERSION_FINISH_ATTEMPTS;
	if (!axis_job_enqueue(conn, &spec, &job_id, NULL, error))
		return FALSE;
	job_text = axis_conversion_int_text(job_id);
	params[0] = id_text;
	params[1] = job_text;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET finish_job_id = $2::bigint, updated_at = now()"
	        " WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	if (job != NULL)
		*job = job_id;
	return TRUE;
}

gboolean
axis_conversions_request_tidy(
	AxisConversions  *self,
	GError          **error
){
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_CONVERSION_TIDY_KIND;
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.dedupe_key = "schedule:" AXIS_CONVERSION_TIDY_SCHEDULE;
	spec.max_attempts = 1;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_enqueue(conn, &spec, NULL, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── What the pages read ────────────────────────────────────────── */

#define RECORD_COLUMNS \
	"c.id, coalesce(c.file_id, 0), coalesce(c.volume_id, 0), coalesce(c.root_id, 0), r.name, c.rel_path," \
	" c.new_rel_path, c.revision_id, pr.name, p.slug, pr.number, c.outcome, c.state, c.requested_by," \
	" (extract(epoch FROM c.requested_at) * 1000000)::bigint, c.verdict, c.src_size, c.source_quality," \
	" coalesce(c.estimate_bytes, -1), coalesce(c.job_id, 0), coalesce(c.finish_job_id, 0), coalesce(c.attempt, 0)," \
	" c.worker, c.encoder, c.output_name, coalesce(c.output_size, c.output_reported, 0), c.step, c.recycle_path," \
	" c.recycle_state, coalesce((extract(epoch FROM c.recycle_remove_after) * 1000000)::bigint, 0), c.trial_name," \
	" c.trial_state, coalesce((extract(epoch FROM c.trial_remove_after) * 1000000)::bigint, 0)," \
	" coalesce(c.failed_check, 0), c.result_text, coalesce((extract(epoch FROM c.finished_at) * 1000000)::bigint, 0)," \
	" j.state::text, coalesce(j.progress, 0), j.progress_message, c.checked_where, c.verifier, c.checks_waiting," \
	" vj.state::text, coalesce(vj.progress, 0), vj.progress_message, CASE WHEN vj.state = 'running' THEN vj.worker_id END," \
	" coalesce(c.run_id, 0), coalesce((extract(epoch FROM c.set_aside_at) * 1000000)::bigint, 0), c.set_aside_by," \
	" c.set_aside_reason, (SELECT count(*) FROM conversion_check k WHERE k.conversion_id = c.id" \
	" AND k.result = 'passed') = " G_STRINGIFY(AXIS_CONVERSION_N_CHECKS) ", coalesce(c.checks_version, 0)"

#define RECORD_FROM \
	" FROM conversion c JOIN conversion_profile_revision pr ON pr.id = c.revision_id" \
	" JOIN conversion_profile p ON p.id = pr.profile_id LEFT JOIN library_root r ON r.id = c.root_id" \
	" LEFT JOIN job j ON j.id = c.job_id LEFT JOIN job vj ON vj.id = c.verify_job_id"

void
axis_conversion_record_free(AxisConversionRecord *record)
{
	if (record == NULL)
		return;
	g_free(record->root_name);
	g_free(record->rel_path);
	g_free(record->new_rel_path);
	g_free(record->profile_name);
	g_free(record->profile_slug);
	g_free(record->outcome);
	g_free(record->state);
	g_free(record->requested_by);
	g_free(record->verdict);
	g_free(record->source_quality);
	g_free(record->worker);
	g_free(record->encoder);
	g_free(record->output_name);
	g_free(record->step);
	g_free(record->recycle_path);
	g_free(record->recycle_state);
	g_free(record->trial_name);
	g_free(record->trial_state);
	g_free(record->result_text);
	g_free(record->job_state);
	g_free(record->job_message);
	g_free(record->checked_where);
	g_free(record->verifier);
	g_free(record->checks_waiting);
	g_free(record->verify_state);
	g_free(record->verify_message);
	g_free(record->verify_holder);
	g_free(record->set_aside_by);
	g_free(record->set_aside_reason);
	g_free(record);
}

static gchar *
cell_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : g_strdup(axis_db_result_get_text(result, row, column));
}

static gint64
cell_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static AxisConversionRecord *
record_from(
	AxisDbResult *result,
	guint         i
){
	AxisConversionRecord *r = g_new0(AxisConversionRecord, 1);
	const gchar *progress = axis_db_result_get_text(result, i, 37);

	r->id = cell_int(result, i, 0);
	r->file_id = cell_int(result, i, 1);
	r->volume_id = cell_int(result, i, 2);
	r->root_id = cell_int(result, i, 3);
	r->root_name = cell_text(result, i, 4);
	r->rel_path = cell_text(result, i, 5);
	r->new_rel_path = cell_text(result, i, 6);
	r->revision_id = cell_int(result, i, 7);
	r->profile_name = cell_text(result, i, 8);
	r->profile_slug = cell_text(result, i, 9);
	r->revision_number = (gint)cell_int(result, i, 10);
	r->outcome = cell_text(result, i, 11);
	r->state = cell_text(result, i, 12);
	r->requested_by = cell_text(result, i, 13);
	r->requested_us = cell_int(result, i, 14);
	r->verdict = cell_text(result, i, 15);
	r->src_size = cell_int(result, i, 16);
	r->source_quality = cell_text(result, i, 17);
	r->estimate_bytes = cell_int(result, i, 18);
	r->job_id = cell_int(result, i, 19);
	r->finish_job_id = cell_int(result, i, 20);
	r->attempt = (gint)cell_int(result, i, 21);
	r->worker = cell_text(result, i, 22);
	r->encoder = cell_text(result, i, 23);
	r->output_name = cell_text(result, i, 24);
	r->output_size = cell_int(result, i, 25);
	r->step = cell_text(result, i, 26);
	r->recycle_path = cell_text(result, i, 27);
	r->recycle_state = cell_text(result, i, 28);
	r->recycle_remove_us = cell_int(result, i, 29);
	r->trial_name = cell_text(result, i, 30);
	r->trial_state = cell_text(result, i, 31);
	r->trial_remove_us = cell_int(result, i, 32);
	r->failed_check = (gint)cell_int(result, i, 33);
	r->result_text = cell_text(result, i, 34);
	r->finished_us = cell_int(result, i, 35);
	r->job_state = cell_text(result, i, 36);
	r->job_progress = progress != NULL ? g_ascii_strtod(progress, NULL) : 0.0;
	r->job_message = cell_text(result, i, 38);
	r->checked_where = cell_text(result, i, 39);
	r->verifier = cell_text(result, i, 40);
	r->checks_waiting = cell_text(result, i, 41);
	r->verify_state = cell_text(result, i, 42);
	{
		const gchar *verify_progress = axis_db_result_get_text(result, i, 43);

		r->verify_progress = verify_progress != NULL ? g_ascii_strtod(verify_progress, NULL) : 0.0;
	}
	r->verify_message = cell_text(result, i, 44);
	r->verify_holder = cell_text(result, i, 45);
	r->run_id = cell_int(result, i, 46);
	r->set_aside_us = cell_int(result, i, 47);
	r->set_aside_by = cell_text(result, i, 48);
	r->set_aside_reason = cell_text(result, i, 49);
	axis_db_result_get_boolean(result, i, 50, &r->all_passed);
	r->checks_version = (gint)cell_int(result, i, 51);
	/* What it counts for in proving its revision, by the marks. */
	if (g_strcmp0(r->outcome, "trial") == 0) {
		if (g_strcmp0(r->state, "rejected") == 0 && r->failed_check >= 3 && r->failed_check <= 8) {
			r->cleared_by = axis_conversion_rejection_cleared(r->failed_check, r->checks_version);
			r->counts_against = r->set_aside_us == 0 && r->cleared_by == NULL;
		} else if (g_strcmp0(r->state, "done") == 0 && r->all_passed) {
			r->voided_by = axis_conversion_pass_voided(r->checks_version);
		}
	}
	return r;
}

static gboolean
records(
	AxisConversions  *self,
	const gchar      *where,
	const gchar      *arg,
	GPtrArray       **out,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = g_strdup_printf("SELECT " RECORD_COLUMNS RECORD_FROM " %s", where);
	const gchar *params[1];
	GPtrArray *list;
	guint i;

	*out = NULL;
	params[0] = arg;
	if (!axis_db_pool_exec(self->pool, sql, arg != NULL ? 1 : 0, arg != NULL ? params : NULL, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_record_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(list, record_from(result, i));
	*out = list;
	return TRUE;
}

gboolean
axis_conversions_list(
	AxisConversions  *self,
	guint             limit,
	GPtrArray       **out,
	GError          **error
){
	g_autofree gchar *n = axis_conversion_int_text(CLAMP(limit, 1, 1000));

	return records(self, "ORDER BY c.id DESC LIMIT $1::integer", n, out, error);
}

gboolean
axis_conversions_for_file(
	AxisConversions  *self,
	gint64            file_id,
	GPtrArray       **out,
	GError          **error
){
	g_autofree gchar *id = axis_conversion_int_text(file_id);

	return records(self, "WHERE c.file_id = $1::bigint ORDER BY c.id DESC LIMIT 50", id, out, error);
}

void
axis_conversion_check_free(AxisConversionCheck *check)
{
	if (check == NULL)
		return;
	g_free(check->result);
	g_free(check->detail);
	g_free(check);
}

gboolean
axis_conversions_get(
	AxisConversions        *self,
	gint64                  conversion_id,
	AxisConversionRecord  **record,
	GPtrArray             **checks,
	GError                **error
){
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_conversion_int_text(conversion_id);
	const gchar *params[1];
	guint n;
	guint i;

	*record = NULL;
	if (checks != NULL)
		*checks = NULL;
	if (!records(self, "WHERE c.id = $1::bigint", id, &found, error))
		return FALSE;
	if (found->len != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such conversion.");
		return FALSE;
	}
	if (checks != NULL) {
		GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_check_free);

		params[0] = id;
		if (!axis_db_pool_exec(self->pool, "SELECT number, result, detail FROM conversion_check"
		        " WHERE conversion_id = $1::bigint ORDER BY number", 1, params, &rows, error)) {
			g_ptr_array_unref(list);
			return FALSE;
		}
		for (n = 1; n <= AXIS_CONVERSION_N_CHECKS; n++) {
			AxisConversionCheck *check = g_new0(AxisConversionCheck, 1);

			check->number = n;
			for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
				if (cell_int(rows, i, 0) == n) {
					check->result = cell_text(rows, i, 1);
					check->detail = cell_text(rows, i, 2);
				}
			}
			if (check->result == NULL)
				check->result = g_strdup("pending");
			g_ptr_array_add(list, check);
		}
		*checks = list;
	}
	*record = g_ptr_array_steal_index(found, 0);
	return TRUE;
}

void
axis_conversion_leftover_free(AxisConversionLeftover *leftover)
{
	if (leftover == NULL)
		return;
	g_free(leftover->volume_name);
	g_free(leftover->name);
	g_free(leftover);
}

gboolean
axis_conversions_leftovers(
	AxisConversions  *self,
	GPtrArray       **out,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *grace = axis_conversion_int_text(axis_conversion_setting_integer(self,
		AXIS_CONVERSION_SETTING_LEFTOVER_GRACE, 86400));
	const gchar *params[1];
	GPtrArray *list;
	guint i;

	*out = NULL;
	params[0] = grace;
	if (!axis_db_pool_exec(self->pool, "SELECT l.volume_id, v.name, l.name, l.size, l.ours,"
	        " (extract(epoch FROM l.first_seen_at) * 1000000)::bigint,"
	        " CASE WHEN l.ours THEN (extract(epoch FROM l.first_seen_at + $1::bigint * interval '1 second') * 1000000)::bigint"
	        "  ELSE 0 END"
	        " FROM conversion_leftover l JOIN volume v ON v.id = l.volume_id ORDER BY v.name, l.name LIMIT 500", 1, params,
	        &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_leftover_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionLeftover *l = g_new0(AxisConversionLeftover, 1);
		gboolean ours = FALSE;

		l->volume_id = cell_int(result, i, 0);
		l->volume_name = cell_text(result, i, 1);
		l->name = cell_text(result, i, 2);
		l->size = cell_int(result, i, 3);
		axis_db_result_get_boolean(result, i, 4, &ours);
		l->ours = ours;
		l->first_seen_us = cell_int(result, i, 5);
		l->remove_us = cell_int(result, i, 6);
		g_ptr_array_add(list, l);
	}
	*out = list;
	return TRUE;
}

void
axis_conversion_trial_revision_free(AxisConversionTrialRevision *row)
{
	if (row == NULL)
		return;
	g_free(row->profile_name);
	g_free(row->proof_method);
	g_free(row);
}

/* ── The checks' rules: the marks ───────────────────────────────── */

/* Which way a check was wrong before a version (axis-conversion.h says which
 * mark to add for which change).  None yet: version 1 is the first that
 * records itself.  A mark for the fault of check 4 fixed in
 * fix-first-conversion would clear every rejection by check 4 judged before
 * versions were recorded, by the faulty rule or by the fixed one alike: those
 * are set aside by an administrator who looked at them instead. */
static const AxisConversionMark build_marks[] = {
	{ 0, AXIS_CONVERSION_MARK_STRICT, 0, NULL }
};
static const guint n_build_marks = 0;

static const AxisConversionMark *test_marks;
static guint n_test_marks;
static gboolean marks_from_test;

const AxisConversionMark *
axis_conversion_marks(guint *n_marks)
{
	if (marks_from_test) {
		*n_marks = n_test_marks;
		return test_marks;
	}
	*n_marks = n_build_marks;
	return build_marks;
}

void
axis_conversion_marks_set_for_test(
	const AxisConversionMark *marks,
	guint                     n_marks
){
	marks_from_test = marks != NULL;
	test_marks = marks;
	n_test_marks = marks != NULL ? n_marks : 0;
}

const AxisConversionMark *
axis_conversion_rejection_cleared(
	gint failed_check,
	gint version
){
	const AxisConversionMark *marks;
	guint n;
	guint i;

	marks = axis_conversion_marks(&n);
	for (i = 0; i < n; i++)
		if (marks[i].kind == AXIS_CONVERSION_MARK_STRICT && (gint)marks[i].check == failed_check &&
		    MAX(version, 0) < marks[i].before)
			return &marks[i];
	return NULL;
}

const AxisConversionMark *
axis_conversion_pass_voided(gint version)
{
	const AxisConversionMark *marks;
	guint n;
	guint i;

	marks = axis_conversion_marks(&n);
	for (i = 0; i < n; i++)
		if (marks[i].kind == AXIS_CONVERSION_MARK_LENIENT && MAX(version, 0) < marks[i].before)
			return &marks[i];
	return NULL;
}

gchar *
axis_conversion_bad_trial_sql(void)
{
	GString *sql = g_string_new("(c.outcome = 'trial' AND c.state = 'rejected' AND c.failed_check BETWEEN 3 AND 8"
		" AND c.set_aside_at IS NULL");
	const AxisConversionMark *marks;
	guint n;
	guint i;

	marks = axis_conversion_marks(&n);
	for (i = 0; i < n; i++)
		if (marks[i].kind == AXIS_CONVERSION_MARK_STRICT)
			g_string_append_printf(sql, " AND NOT (c.failed_check = %u AND coalesce(c.checks_version, 0) < %d)",
				marks[i].check, marks[i].before);
	g_string_append_c(sql, ')');
	return g_string_free(sql, FALSE);
}

gchar *
axis_conversion_passed_trial_sql(void)
{
	/* A passed check row for each (one checked before a check was added has
	 * fewer, and is not one). */
	GString *sql = g_string_new("(c.outcome = 'trial' AND c.state = 'done' AND (SELECT count(*) FROM conversion_check k"
		" WHERE k.conversion_id = c.id AND k.result = 'passed') = " G_STRINGIFY(AXIS_CONVERSION_N_CHECKS));
	const AxisConversionMark *marks;
	guint n;
	guint i;

	marks = axis_conversion_marks(&n);
	for (i = 0; i < n; i++)
		if (marks[i].kind == AXIS_CONVERSION_MARK_LENIENT)
			g_string_append_printf(sql, " AND coalesce(c.checks_version, 0) >= %d", marks[i].before);
	g_string_append_c(sql, ')');
	return g_string_free(sql, FALSE);
}

/* A rejected trial an administrator set aside. */
#define SET_ASIDE_TRIAL "(c.outcome = 'trial' AND c.set_aside_at IS NOT NULL)"

gboolean
axis_conversions_trial_revisions(
	AxisConversions  *self,
	GPtrArray       **out,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *passed = axis_conversion_passed_trial_sql();
	g_autofree gchar *bad = axis_conversion_bad_trial_sql();
	g_autofree gchar *sql = NULL;
	guint needed = (guint)axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_PROVE_TRIALS, 3);
	GPtrArray *list;
	guint i;

	*out = NULL;
	sql = g_strconcat("SELECT pr.id, pr.name, pr.number, count(DISTINCT c.file_id) FILTER (WHERE ", passed, "),"
		" count(*) FILTER (WHERE ", bad, "), pf.method, count(*) FILTER (WHERE " SET_ASIDE_TRIAL ")"
		" FROM conversion c JOIN conversion_profile_revision pr ON pr.id = c.revision_id"
		" LEFT JOIN conversion_profile_proof pf ON pf.revision_id = pr.id"
		" WHERE c.outcome = 'trial' GROUP BY pr.id, pr.name, pr.number, pf.method ORDER BY pr.id DESC LIMIT 100", NULL);
	if (!axis_db_pool_exec(self->pool, sql, 0, NULL, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_trial_revision_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionTrialRevision *r = g_new0(AxisConversionTrialRevision, 1);

		r->revision_id = cell_int(result, i, 0);
		r->profile_name = cell_text(result, i, 1);
		r->revision_number = (gint)cell_int(result, i, 2);
		r->passed = (guint)cell_int(result, i, 3);
		r->bad = (guint)cell_int(result, i, 4);
		r->proof_method = cell_text(result, i, 5);
		r->set_aside = (guint)cell_int(result, i, 6);
		r->proven = r->proof_method != NULL;
		r->needed = needed;
		g_ptr_array_add(list, r);
	}
	*out = list;
	return TRUE;
}

/* ── Proving by hand ────────────────────────────────────────────── */

void
axis_conversion_proving_free(AxisConversionProving *proving)
{
	if (proving == NULL)
		return;
	if (proving->revision != NULL)
		axis_profile_revision_unref(proving->revision);
	if (proving->trials != NULL)
		g_ptr_array_unref(proving->trials);
	g_free(proving->why_not);
	g_free(proving);
}

gboolean
axis_conversions_proving(
	AxisConversions        *self,
	gint64                  revision_id,
	AxisConversionProving **out,
	GError                **error
){
	g_autoptr(AxisConversionProving) proving = g_new0(AxisConversionProving, 1);
	g_autoptr(AxisDbResult) bad = NULL;
	g_autofree gchar *id = axis_conversion_int_text(revision_id);
	g_autofree gchar *bad_sql = axis_conversion_bad_trial_sql();
	g_autofree gchar *passed_sql = axis_conversion_passed_trial_sql();
	g_autofree gchar *count_sql = NULL;
	g_autofree gchar *where = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GHashTable) files = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	const gchar *params[1];
	AxisDbConn *conn;
	guint i;

	*out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_profile_store_revision_by_id(conn, revision_id, &proving->revision, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	params[0] = id;
	count_sql = g_strconcat("SELECT count(*) FILTER (WHERE ", bad_sql, "), count(*) FILTER (WHERE " SET_ASIDE_TRIAL ")"
		" FROM conversion c WHERE c.revision_id = $1::bigint", NULL);
	if (!axis_db_conn_exec(conn, count_sql, 1, params, &bad, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	if (proving->revision == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such profile version.");
		return FALSE;
	}
	proving->bad_trials = (guint)cell_int(bad, 0, 0);
	proving->set_aside = (guint)cell_int(bad, 0, 1);
	proving->needed = (guint)axis_conversion_setting_integer(self, AXIS_CONVERSION_SETTING_PROVE_TRIALS, 3);
	where = g_strconcat("WHERE c.revision_id = $1::bigint AND ", passed_sql, " AND c.file_id IS NOT NULL"
		" ORDER BY c.id DESC LIMIT 200", NULL);
	if (!records(self, where, id, &all, error))
		return FALSE;
	/* One per file: its newest. */
	proving->trials = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_record_free);
	for (i = 0; i < all->len; i++) {
		AxisConversionRecord *r = g_ptr_array_index(all, i);
		gint64 *key;

		if (g_hash_table_contains(files, &r->file_id))
			continue;
		key = g_new(gint64, 1);
		*key = r->file_id;
		g_hash_table_add(files, key);
		g_ptr_array_add(proving->trials, g_ptr_array_steal_index(all, i));
		i--;
	}
	if (!proving->revision->usable)
		proving->why_not = g_strdup("The revision's definition is not usable.");
	else if (proving->revision->kind != AXIS_PROFILE_KIND_VIDEO)
		proving->why_not = g_strdup("Only a video profile's revision can be proven by hand here.");
	else if (axis_profile_revision_is_unlocked(proving->revision))
		proving->why_not = g_strdup("The revision is proven already.");
	else if (proving->bad_trials > 0)
		proving->why_not = g_strdup_printf("%u of its trials were rejected by a check of the converted file itself, "
			"so it is not proven by hand. A trial rejected for a reason that is not the profile's (a check found wrong "
			"since) can be set aside on its page.", proving->bad_trials);
	else if (proving->trials->len < proving->needed)
		proving->why_not = g_strdup_printf("It has %u passed trial%s on different files; proving by hand needs %u "
			"(convert.prove_trials).", proving->trials->len, proving->trials->len == 1 ? "" : "s", proving->needed);
	proving->allowed = proving->why_not == NULL;
	*out = g_steal_pointer(&proving);
	return TRUE;
}

gboolean
axis_conversions_prove(
	AxisConversions  *self,
	gint64            revision_id,
	const gint64     *trials,
	guint             n_trials,
	const gchar      *user,
	GError          **error
){
	g_autoptr(AxisConversionProving) proving = NULL;
	g_autofree gchar *id = axis_conversion_int_text(revision_id);
	GString *list;
	const gchar *params[3];
	gboolean ok;
	guint i;
	guint j;

	if (user == NULL || user[0] == '\0') {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "Proving needs a person.");
		return FALSE;
	}
	if (!axis_conversions_proving(self, revision_id, &proving, error))
		return FALSE;
	if (!proving->allowed) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, proving->why_not);
		return FALSE;
	}
	/* Exactly the trials the confirmation named. */
	if (n_trials != proving->trials->len) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
			"The trials named are not the revision's passed trials now; look at the confirmation again.");
		return FALSE;
	}
	for (i = 0; i < n_trials; i++) {
		gboolean found = FALSE;

		for (j = 0; j < proving->trials->len; j++)
			found = found || ((AxisConversionRecord *)g_ptr_array_index(proving->trials, j))->id == trials[i];
		if (!found) {
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
				"The trials named are not the revision's passed trials now; look at the confirmation again.");
			return FALSE;
		}
	}
	list = g_string_new("{");
	for (i = 0; i < n_trials; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", trials[i]);
	g_string_append_c(list, '}');
	params[0] = id;
	params[1] = user;
	params[2] = list->str;
	ok = axis_db_pool_exec(self->pool, "INSERT INTO conversion_profile_proof (revision_id, method, recorded_by, trials)"
		" VALUES ($1::bigint, 'trials', $2, $3::bigint[])", 3, params, NULL, error);
	if (ok)
		g_message("audit: %s proved profile revision %" G_GINT64_FORMAT " by hand from trials %s", user, revision_id,
			list->str);
	g_string_free(list, TRUE);
	return ok;
}

gboolean
axis_conversions_set_aside(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	const gchar      *reason,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_conversion_int_text(conversion_id);
	g_autofree gchar *clean = reason != NULL ? axis_library_text_clean(reason, 500) : NULL;
	g_autofree gchar *who = user != NULL ? axis_library_text_clean(user, 200) : NULL;
	const gchar *params[3];

	if (who == NULL || who[0] == '\0') {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "Setting a trial aside needs a "
			"person.");
		return FALSE;
	}
	if (clean == NULL || g_strstrip(clean)[0] == '\0') {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "Say why the trial is set "
			"aside: the reason is kept with it.");
		return FALSE;
	}
	params[0] = id;
	params[1] = who;
	params[2] = clean;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET set_aside_at = now(), set_aside_by = $2,"
	        " set_aside_reason = $3, updated_at = now() WHERE id = $1::bigint AND outcome = 'trial'"
	        " AND state = 'rejected' AND failed_check BETWEEN 3 AND 8 AND set_aside_at IS NULL RETURNING revision_id", 3,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, "Only a trial rejected by a "
			"check of the converted file (3 to 8), and not set aside already, can be set aside.");
		return FALSE;
	}
	g_message("audit: %s set trial %" G_GINT64_FORMAT " (revision %s) aside: it counts neither for nor against proving; "
		"the reason given: %s", who, conversion_id, axis_db_result_get_text(result, 0, 0), clean);
	return TRUE;
}

/* ── Actions ────────────────────────────────────────────────────── */

gboolean
axis_conversions_cancel(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	GError          **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	AxisDbConn *conn;
	gboolean ok = FALSE;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!axis_conversion_row_load(conn, conversion_id, TRUE, &row, error))
		goto rollback;
	if (row == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such conversion.");
		goto rollback;
	}
	if (g_strcmp0(row->state, "queued") != 0 && g_strcmp0(row->state, "encoding") != 0) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE,
			"Only a conversion that waits for a worker or is being converted can be cancelled; one being checked or "
			"placed finishes what it is doing.");
		goto rollback;
	}
	if (row->job_id > 0) {
		GError *local = NULL;

		if (!axis_job_cancel(conn, row->job_id, NULL, &local) &&
		    !g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND)) {
			g_propagate_error(error, local);
			goto rollback;
		}
		g_clear_error(&local);
	}
	{
		g_autofree gchar *text = g_strdup_printf("Cancelled by %s.", user != NULL ? user : "an administrator");

		if (!axis_conversion_set_state(conn, conversion_id, "cancelled", 0, text, error))
			goto rollback;
	}
	ok = axis_db_conn_commit(conn, error);
	if (ok)
		g_message("audit: %s cancelled conversion %" G_GINT64_FORMAT, user != NULL ? user : "?", conversion_id);
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_conversions_give_back_for_run(
	AxisConversions  *self,
	gint64            conversion_id,
	gint64            entry_id,
	const gchar      *text,
	gboolean         *given,
	GError          **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autofree gchar *id = axis_conversion_int_text(conversion_id);
	g_autofree gchar *entry = axis_conversion_int_text(entry_id);
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok = FALSE;

	*given = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!axis_conversion_row_load(conn, conversion_id, TRUE, &row, error))
		goto rollback;
	/* Only what no worker took yet: nothing was made of it. */
	if (row == NULL || g_strcmp0(row->state, "queued") != 0) {
		ok = axis_db_conn_commit(conn, error);
		goto out;
	}
	if (row->job_id > 0) {
		GError *local = NULL;

		if (!axis_job_cancel(conn, row->job_id, NULL, &local) &&
		    !g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND)) {
			g_propagate_error(error, local);
			goto rollback;
		}
		g_clear_error(&local);
	}
	if (!axis_conversion_set_state(conn, conversion_id, "cancelled", 0, text, error))
		goto rollback;
	params[0] = entry;
	params[1] = id;
	if (!axis_db_conn_exec(conn, "UPDATE conversion_run_entry SET state = 'waiting', conversion_id = NULL, outcome = NULL,"
	        " asked_at = NULL WHERE id = $1::bigint AND state = 'asked' AND conversion_id = $2::bigint", 2, params, NULL,
	        error))
		goto rollback;
	ok = axis_db_conn_commit(conn, error);
	*given = ok;
	if (ok)
		g_message("conversions: conversion %" G_GINT64_FORMAT " given back by its conversion run: %s", conversion_id, text);
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	axis_db_pool_release(self->pool, conn);
	return ok;
}

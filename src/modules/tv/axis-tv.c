/*
 * axis-tv.c - the TV module: its lifetime, settings and jobs, adding,
 * changing and removing series, monitoring seasons and episodes, queueing
 * searches, the blocklist, giving up on a grab, and reading for the pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "modules/tv/axis-tv-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-registry.h"

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const monitor_choices[] = { "all", "future", "latest", "first", "none", NULL };
static const gchar *const protocol_choices[] = { "any", "usenet", "torrent", NULL };

/* The words the Settings page shows for the values above, in the same order
 * (the series form's own words for the first). */
static const gchar *const monitor_labels[] = {
	"Every episode", "Future episodes only", "The latest season", "The first season", "Nothing yet", NULL
};
static const gchar *const protocol_labels[] = { "Either", "Usenet", "Torrent", NULL };

static const AxisSettingInfo settings_table[] = {
	{ AXIS_TV_SETTING_AIR_DELAY, AXIS_SETTING_DURATION, "Search for an episode this long after its air date",
	  "This long after the start of its air date (UTC): the details provider gives a date, not a time, and releases come "
	  "some time after an episode airs.", 0, "1h", 0, 7 * 86400, 0, 0, NULL },
	{ AXIS_TV_SETTING_PACK_SHARE, AXIS_SETTING_INTEGER, "Search for a whole season when this share of it is wanted, in percent",
	  "A season with at least two wanted aired episodes, and at least this percentage of its aired monitored ones "
	  "wanted, is sought as a whole season first, then episode by episode.", 0, "50", 1, 100, 0, 0, NULL },
	{ AXIS_TV_SETTING_MONITOR, AXIS_SETTING_CHOICE, "What to monitor in a new series",
	  "What a new series monitors unless chosen otherwise: every episode, future episodes only, the latest "
	  "season, the first season, or nothing yet.", 0, "all", 0, 0, 0, 0, monitor_choices },
	{ AXIS_TV_SETTING_PER_RUN, AXIS_SETTING_INTEGER, "TV searches per 15-minute run",
	  "At most this many searches (an episode or a season each) are queued each time the scheduler runs (every 15 "
	  "minutes): the episodes aired in the last week first, then the backlog, the least recently searched first; at "
	  "most three of one series.", 0, "5", 1, 100, 0, 0, NULL },
	{ AXIS_TV_SETTING_FLOOR, AXIS_SETTING_DURATION, "Shortest time between two searches of an episode",
	  "The scheduler does not search one episode again sooner than this. Each search that grabs nothing doubles "
	  "it, up to the limit below.", 0, "6h", 3600, 7 * 86400, 0, 0, NULL },
	{ AXIS_TV_SETTING_FLOOR_MAX, AXIS_SETTING_DURATION, "Longest time between two searches of an episode",
	  "Where the doubling stops.", 0, "7d", 3600, 90 * 86400, 0, 0, NULL },
	{ AXIS_TV_SETTING_GIVE_UP, AXIS_SETTING_DURATION, "Give up on a stalled TV download after",
	  "A download that has not grown for this long is put on the series' blocklist, removed from its download "
	  "client with "
	  "its data, and the next best release is sought.", 0, "6h", 600, 30 * 86400, 0, 0, NULL },
	{ AXIS_TV_SETTING_PROTOCOL, AXIS_SETTING_CHOICE, "Prefer TV releases from",
	  "Between two releases the quality profile ranks equal, the one from this kind of indexer goes first.", 0, "any", 0,
	  0, 0, 0, protocol_choices },
	{ AXIS_TV_SETTING_RETENTION, AXIS_SETTING_INTEGER, "Usenet retention for TV searches, in days",
	  "How long your Usenet provider keeps articles; an older Usenet release is rejected. 0: not known.", 0, "0", 0,
	  100000, 0, 0, NULL },
	{ AXIS_TV_SETTING_LANGUAGES, AXIS_SETTING_STRING_LIST, "Languages wanted in TV releases",
	  "Two-letter codes (en, de, fr), one per line. A release that names its languages, none of them wanted and "
	  "with no multi-language mark, is rejected. Empty: no check.", 0, NULL, 0, 16, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static gboolean
declared(
	AxisTv      *self,
	const gchar *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

gint64
axis_tv_setting_duration(
	AxisTv      *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_duration(self->settings, key) : fallback;
}

gint64
axis_tv_setting_integer(
	AxisTv      *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_integer(self->settings, key) : fallback;
}

gchar *
axis_tv_setting_string(
	AxisTv      *self,
	const gchar *key,
	const gchar *fallback
){
	return declared(self, key) ? axis_settings_get_string(self->settings, key) : g_strdup(fallback);
}

void
axis_tv_get_rules(
	AxisTv      *self,
	AxisTvRules *rules
){
	axis_tv_rules_init(rules, self->clock_us > 0 ? self->clock_us : g_get_real_time());
	rules->air_delay_s = axis_tv_setting_duration(self, AXIS_TV_SETTING_AIR_DELAY, 3600);
	rules->give_up_after_s = axis_tv_setting_duration(self, AXIS_TV_SETTING_GIVE_UP, 6 * 3600);
}

void
axis_tv_set_clock(
	AxisTv *self,
	gint64  now_us
){
	self->clock_us = now_us;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisTv *
axis_tv_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisIndexers     *indexers,
	AxisDownloads    *downloads,
	AxisStorage      *storage,
	AxisQualityStore *quality
){
	AxisTv *self;

	g_return_val_if_fail(pool != NULL && meta != NULL && indexers != NULL && downloads != NULL && storage != NULL &&
		quality != NULL, NULL);

	self = g_new0(AxisTv, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->meta = axis_meta_ref(meta);
	self->indexers = axis_indexers_ref(indexers);
	self->downloads = axis_downloads_ref(downloads);
	self->storage = axis_storage_ref(storage);
	self->quality = axis_quality_store_ref(quality);
	self->acquire = axis_acquire_new(pool, settings, indexers, downloads, storage, axis_tv_acquire_info(), self);
	/* The series are kept fresh (their seasons too), and the profiles they
	 * use are kept. */
	axis_meta_add_reference(meta, "series", "SELECT title_id FROM series");
	axis_quality_store_add_user(quality, "series", "series", "SELECT quality_profile_id FROM series");
	/* Whose numbering the series follow: a provider followed has its notice
	 * on every page (add-metadata-tvdb). */
	axis_tv_note_followed(self);
	return self;
}

AxisTv *
axis_tv_ref(AxisTv *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_tv_unref(AxisTv *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_acquire_free(self->acquire);
	axis_meta_unref(self->meta);
	axis_indexers_unref(self->indexers);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	axis_quality_store_unref(self->quality);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_free(self);
}

void
axis_tv_set_compat(
	AxisTv     *self,
	AxisCompat *compat
){
	axis_acquire_set_compat(self->acquire, compat);
}

void
axis_tv_stop(AxisTv *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_tv_set_claim_hook(
	AxisTv               *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
){
	axis_acquire_set_claim_hook(self->acquire, hook, user_data);
}

/* ── Jobs ───────────────────────────────────────────────────────── */

static gint64
payload_int(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

static const gchar *
payload_text(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

static gboolean
search_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisTv *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	gint64 series_id = payload_int(payload, "series");
	gint64 item = payload_int(payload, "item");
	const gchar *cause = payload_text(payload, "cause");
	const gchar *user = payload_text(payload, "user");
	AxisAcquireSearchReport report;
	AxisTvScope scope;

	if (series_id <= 0 || !axis_tv_scope_from_name(payload_text(payload, "scope"), &scope) ||
	    !axis_acquire_cause_valid(cause)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "the job does not name a series, what "
			"to search for and a cause");
		return FALSE;
	}
	if (user != NULL && (strlen(user) > 200 || !g_utf8_validate(user, -1, NULL)))
		user = NULL;
	if (!axis_tv_run_search(self, series_id, scope, item, cause, user, axis_job_context_get_cancellable(context),
	        &report, error))
		return FALSE;
	if (report.skipped != NULL)
		axis_job_context_set_progress(context, 1.0, report.skipped);
	else if (report.outcome != NULL)
		axis_job_context_set_progress(context, 1.0, report.outcome);
	return TRUE;
}

static gboolean
follow_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisAcquireFollowReport report;

	if (!axis_tv_run_follow(user_data, &report, error))
		return FALSE;
	if (report.failed + report.settled + report.completed > 0)
		g_info("series: %u grabs dealt with after a failure, %u claims settled, %u completions recorded, %u "
			"searches for the next best", report.failed, report.settled, report.completed, report.retries);
	return TRUE;
}

static gboolean
schedule_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisTvScheduleReport report;

	if (!axis_tv_run_schedule(user_data, &report, error))
		return FALSE;
	axis_tv_note_followed(user_data);
	if (report.queued > 0 || report.synced > 0)
		g_info("series: %u series followed the metadata; %u searches queued (%u of seasons), %u episodes due of %u "
			"wanted", report.synced, report.queued, report.seasons, report.due, report.considered);
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_tv_web_register(user_data, web);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_TV_SEARCH_KIND, AXIS_ACQUIRE_SEARCH_QUEUE, 2, search_handler,
	  "Searching for episodes of a series", AXIS_JOB_CLASS_WORK },
	{ AXIS_TV_FOLLOW_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, follow_handler,
	  "Following the series' downloads", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_TV_SCHEDULE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, schedule_handler,
	  "Updating the episode lists and choosing episodes to search for", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_TV_FOLLOW_SCHEDULE, AXIS_TV_FOLLOW_KIND, NULL, AXIS_TV_FOLLOW_INTERVAL_S },
	{ AXIS_TV_SCHEDULE_SCHEDULE, AXIS_TV_SCHEDULE_KIND, NULL, AXIS_TV_SCHEDULE_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

/* Media -> TV, after Movies. */
static const AxisNavEntry nav_entries[] = {
	{ "All", "/series", 110, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ "Calendar", "/series/calendar", 120, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ "Wanted", "/series/wanted", 130, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ "History", "/series/history", 140, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends[] = {
	AXIS_META_MODULE_ID, AXIS_INDEXER_MODULE_ID, AXIS_DOWNLOAD_MODULE_ID, AXIS_QUALITY_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_tv_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_TV_MODULE_ID;
		info.name = "TV";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "The series Axis looks after: add one from its page with what to monitor, and Axis "
			"searches for each episode once it has aired and until it is found, a season as a whole when several "
			"of its episodes are wanted, grabs each episode once, follows the download and imports the episodes "
			"file by file. Every release considered is kept with its verdict, its reasons and the episodes it holds.";
		info.settings = settings_table;
		axis_setting_set_choice_labels(AXIS_TV_SETTING_MONITOR, monitor_labels);
		axis_setting_set_choice_labels(AXIS_TV_SETTING_PROTOCOL, protocol_labels);
		info.depends_on = depends;
	}
	return &info;
}

/* ── Adding, changing, removing ─────────────────────────────────── */

typedef struct
{
	gint64  profile_id;
	gchar  *profile_name;
	gint64  root_id;
	gchar  *root_name;
} Checked;

static void
checked_clear(Checked *checked)
{
	g_free(checked->profile_name);
	g_free(checked->root_name);
}

/* A new series' root is on a disk that can be written to now: a stale page
 * or a hand-made post naming one that is away or mounted read-only is
 * refused (its episodes would never be imported). */
static gboolean
check_writable(
	AxisTv       *self,
	gint64        root_id,
	const gchar  *root_name,
	GError      **error
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *why;

	if (!axis_storage_get_root(self->storage, root_id, &root, &local)) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "The library folder %s cannot be read now: %s",
			root_name, local->message);
		return FALSE;
	}
	volume = axis_storage_get_volume(self->storage, root->volume_id);
	why = volume != NULL ? axis_volume_state_unwritable_why(volume->state) : "its disk is not known";
	if (why == NULL)
		return TRUE;
	g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "The library folder %s cannot be written to now: %s. "
		"Choose another library folder, or add the series once it can be written to again", root_name, why);
	return FALSE;
}

static gboolean
check_spec(
	AxisTv            *self,
	AxisDbConn        *conn,
	const AxisTvSpec  *spec,
	Checked           *checked,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisQualityStored) stored = NULL;
	g_autofree gchar *root = NULL;
	const gchar *params[1];
	AxisQualityProfile profile;

	memset(checked, 0, sizeof(*checked));
	if (spec->profile_id > 0) {
		if (!axis_quality_store_read_id(conn, spec->profile_id, &stored, error))
			return FALSE;
	} else {
		g_autofree gchar *slug = axis_quality_store_default_slug(self->quality);

		if (!axis_quality_store_read(conn, slug, &stored, error))
			return FALSE;
	}
	if (stored == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "There is no such quality profile.");
		return FALSE;
	}
	if (!axis_quality_store_load(conn, stored->id, &profile, NULL)) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "That quality profile cannot be "
			"used: its settings are not valid.");
		return FALSE;
	}
	checked->profile_id = stored->id;
	checked->profile_name = g_strdup(stored->name);
	if (spec->root_id <= 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "Choose a destination library folder.");
		return FALSE;
	}
	root = axis_tv_int_text(spec->root_id);
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT media_type, name FROM library_root WHERE id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0 ||
	    g_strcmp0(axis_db_result_get_text(result, 0, 0), axis_media_type_to_string(AXIS_MEDIA_TV)) != 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "That is not a library folder for "
			"TV.");
		return FALSE;
	}
	checked->root_id = spec->root_id;
	checked->root_name = g_strdup(axis_db_result_get_text(result, 0, 1));
	/* "As the other app had it" is no choice of a person's: only bringing a
	 * series over adds one so. */
	if ((guint)spec->monitor > AXIS_TV_MONITOR_BROUGHT ||
	    (spec->monitor == AXIS_TV_MONITOR_BROUGHT) != (spec->brought_from != NULL)) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "Choose what to monitor.");
		return FALSE;
	}
	return TRUE;
}

/*
 * The numbering a new series follows: its record provider's own (@record),
 * or @asked when that is another one the series can follow now.  A series is
 * added under the numbering it keeps: never under one to be switched after.
 * So another source is taken only when its provider is offered (its key set)
 * and the metadata store holds that source's seasons for the title, which is
 * what the series' episodes are made from in this same transaction.
 */
static gboolean
check_source(
	AxisTv       *self,
	AxisDbConn   *conn,
	const gchar  *title_text,
	const gchar  *record,
	const gchar  *asked,
	GError      **error
){
	g_autoptr(AxisDbResult) held = NULL;
	g_autofree gchar *owner = NULL;
	const AxisMetaProviderInfo *info;
	const gchar *params[2];

	if (asked == NULL || g_strcmp0(asked, record) == 0)
		return TRUE;
	owner = axis_meta_source_provider(asked);
	info = owner != NULL ? axis_meta_provider_find(self->meta, owner) : NULL;
	if (info == NULL || !axis_tv_source_is_tvdb(asked) || !axis_meta_provider_offered(self->meta, info)) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "The series cannot follow %s now: %s has no key in "
			"Axis.", axis_tv_source_label(asked), info != NULL ? info->label : "its provider");
		return FALSE;
	}
	params[0] = title_text;
	params[1] = asked;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM meta_episode WHERE title_id = $1::bigint AND source = $2 LIMIT 1", 2,
	        params, &held, error))
		return FALSE;
	if (axis_db_result_get_n_rows(held) == 0) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "The series cannot follow %s now: Axis does not hold "
			"that order's episodes for it. Read again once %s answers.", axis_tv_source_label(asked), info->label);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_tv_add_in_transaction(
	AxisTv            *self,
	AxisDbConn        *conn,
	const AxisTvSpec  *spec,
	gint64            *series_id,
	GError           **error
){
	g_autoptr(AxisDbResult) title = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *title_text = axis_tv_int_text(spec->title_id);
	g_autofree gchar *profile_text = NULL;
	g_autofree gchar *root_text = NULL;
	g_autofree gchar *monitor = g_ascii_strdown(axis_tv_monitor_label(spec->monitor), -1);
	Checked checked;
	const gchar *params[8];
	const AxisMetaProviderInfo *record;
	gboolean ok;

	params[0] = title_text;
	if (!axis_db_conn_exec(conn, "SELECT kind, provider FROM meta_title WHERE id = $1::bigint", 1, params, &title,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(title) == 0 || g_strcmp0(axis_db_result_get_text(title, 0, 0), "series") != 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "That is not a series Axis knows.");
		return FALSE;
	}
	/* A series follows its record provider's own numbering, named here.  One
	 * Axis knows from a secondary provider alone (TheTVDB) cannot be added
	 * until a series can follow it (add-metadata-tvdb, V3). */
	record = axis_meta_provider_find(self->meta, axis_db_result_get_text(title, 0, 1));
	if (record == NULL || record->secondary) {
		g_set_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "Axis knows this series from %s alone: it can be "
			"added once TMDb knows it.", record != NULL ? record->label :
			g_strcmp0(axis_db_result_get_text(title, 0, 1), "tvdb") == 0 ? "TheTVDB" : "another provider");
		return FALSE;
	}
	if (!check_spec(self, conn, spec, &checked, error) ||
	    !check_source(self, conn, title_text, axis_db_result_get_text(title, 0, 1), spec->source, error)) {
		checked_clear(&checked);
		return FALSE;
	}
	profile_text = axis_tv_int_text(checked.profile_id);
	root_text = axis_tv_int_text(checked.root_id);
	params[0] = title_text;
	params[1] = profile_text;
	params[2] = root_text;
	params[3] = axis_tv_monitor_name(spec->monitor);
	params[4] = spec->monitor == AXIS_TV_MONITOR_ALL || spec->monitor == AXIS_TV_MONITOR_FUTURE ? "true" : "false";
	params[5] = spec->user;
	/* The numbering it follows, from the start. */
	params[6] = spec->source != NULL ? spec->source : axis_db_result_get_text(title, 0, 1);
	/* Its kind: anime only when the form's box was ticked or the other app
	 * has it so; never by anything Axis reads of the title. */
	params[7] = spec->anime ? AXIS_TV_KIND_ANIME : AXIS_TV_KIND_STANDARD;
	ok = axis_db_conn_exec(conn, "INSERT INTO series (title_id, quality_profile_id, root_id, monitor_choice,"
		" monitor_new, added_by, episode_source, kind) VALUES ($1::bigint, $2::bigint, $3::bigint, $4, $5::boolean, $6,"
		" $7, $8) RETURNING id", 8, params, &inserted, &local);
	if (!ok) {
		checked_clear(&checked);
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_EXISTS, "that series is among your series "
				"already");
			return FALSE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	axis_db_result_get_int64(inserted, 0, 0, series_id);
	ok = axis_tv_store_series_one(conn, *series_id, &series, error) && series != NULL &&
		axis_tv_sync_in_transaction(self, conn, series, spec, NULL, error);
	/* Its episodes already on disk (identified) are in the library at once:
	 * only the missing monitored ones are wanted. */
	if (ok) {
		guint linked = 0;
		g_autofree gchar *on_disk = NULL;

		ok = axis_tv_link_identified(conn, *series_id, spec->title_id, &linked, error);
		/* What is to be got must go where it can be imported (a series
		 * managed where its files are, searching nothing, need not; nor one
		 * migrated, which searches nothing until a person starts it). */
		ok = ok && ((linked > 0 && !spec->search_now) || spec->brought_from != NULL ||
			check_writable(self, checked.root_id, checked.root_name, error));
		if (linked > 0)
			on_disk = g_strdup_printf(" %u episode %s on disk already, and those episodes are in the library.", linked,
				linked == 1 ? "file was" : "files were");
		if (ok && spec->brought_from != NULL)
			ok = axis_tv_store_eventf(conn, series, 0, "added", spec->user, error, "Migrated from %s%s%s, with the "
				"quality profile %s, the library folder %s, numbered as %s numbers it, monitored as it was there.%s%s "
				"Axis does not search for it until searching is started.", spec->brought_from,
				spec->user != NULL ? " by " : "", spec->user != NULL ? spec->user : "", checked.profile_name,
				checked.root_name, axis_tv_source_label(series->episode_source), on_disk != NULL ? on_disk : "",
				spec->anime ? " Marked as anime, as it is there." : "");
		else if (ok && spec->anime)
			/* An anime series added by hand: its history says what it was
			 * marked as and which numbering that gave it. */
			ok = axis_tv_store_eventf(conn, series, 0, "added", spec->user, error, "Added%s%s, with the quality "
				"profile %s, the destination %s, monitoring %s%s. Marked as anime, numbered as %s numbers it%s.%s",
				spec->user != NULL ? " by " : "", spec->user != NULL ? spec->user : "", checked.profile_name,
				checked.root_name, monitor, spec->specials ? " and the specials" : "",
				axis_tv_source_label(series->episode_source),
				g_strcmp0(series->episode_source, "tvdb") == 0 ? "" : ", which has no absolute numbers",
				on_disk != NULL ? on_disk : "");
		else
			ok = ok && axis_tv_store_eventf(conn, series, 0, "added", spec->user, error, "Added%s%s, with the quality "
				"profile %s, the destination %s, monitoring %s%s.%s", spec->user != NULL ? " by " : "",
				spec->user != NULL ? spec->user : "", checked.profile_name, checked.root_name, monitor,
				spec->specials ? " and the specials" : "", on_disk != NULL ? on_disk : "");
	}
	if (ok && spec->search_now && spec->brought_from == NULL) {
		guint queued = 0;

		ok = axis_tv_plan_series(self, conn, *series_id, "added", spec->user, 50, NULL, &queued, error);
	}
	checked_clear(&checked);
	return ok;
}

gboolean
axis_tv_add(
	AxisTv            *self,
	const AxisTvSpec  *spec,
	gint64            *series_id,
	GError           **error
){
	AxisMetaTitleAnswer answer;
	AxisTvSpec numbered = *spec;
	AxisDbConn *conn;
	gint64 id = 0;
	gboolean ok;
	gboolean series;

	/* Its record and its seasons' episodes first, outside the transaction:
	 * the provider may be slow. */
	memset(&answer, 0, sizeof(answer));
	series = axis_meta_get_title(self->meta, spec->title_id, AXIS_TV_META_WAIT_MS, &answer, NULL) &&
		answer.title != NULL && answer.title->kind == AXIS_META_KIND_SERIES;
	if (series)
		axis_tv_fetch_seasons(self, spec->title_id, NULL, AXIS_TV_META_WAIT_MS);
	axis_meta_title_answer_clear(&answer);
	/* Marked as anime by the person adding it, and no numbering named: the
	 * one that carries absolute numbers, when it can be had (question 1). */
	if (series && spec->anime && spec->source == NULL && spec->brought_from == NULL)
		numbered.source = axis_tv_anime_source(self, spec->title_id);
	spec = &numbered;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_tv_add_in_transaction(self, conn, spec, &id, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && series_id != NULL)
		*series_id = id;
	return ok;
}

/* The series locked and read in the open transaction, or NOT_FOUND. */
static gboolean
locked_series(
	AxisDbConn     *conn,
	gint64          series_id,
	AxisTvSeries  **series,
	GError        **error
){
	gboolean found = FALSE;

	*series = NULL;
	if (!axis_tv_store_lock(conn, series_id, &found, error))
		return FALSE;
	if (!found || !axis_tv_store_series_one(conn, series_id, series, error) || *series == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	return TRUE;
}

typedef gboolean (*InTransaction)(AxisTv *self, AxisDbConn *conn, gpointer data, GError **error);

static gboolean
in_transaction(
	AxisTv         *self,
	InTransaction   func,
	gpointer        data,
	GError        **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && func(self, conn, data, error) && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

typedef struct
{
	gint64            series_id;
	const AxisTvSpec *spec;
	gint64            item;
	gint              season;
	gboolean          flag;
	const gchar      *user;
	guint             count;
	AxisTvEdited      edited;
} Change;

/* With a new profile: every wanted episode searched for again now, as the
 * scheduler groups them, their waits set back; and the item the change was
 * made from, whatever the scheduler would make of it. */
static gboolean
search_with_new_profile(
	AxisTv      *self,
	AxisDbConn  *conn,
	Change      *change,
	GError     **error
){
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	g_autofree gchar *floor = axis_tv_int_text(axis_tv_setting_duration(self, AXIS_TV_SETTING_FLOOR, 6 * 3600));
	const gchar *params[2];
	gboolean created = FALSE;
	guint queued = 0;

	params[0] = id;
	params[1] = floor;
	if (!axis_db_conn_exec(conn, "UPDATE series_episode SET next_search_at = NULL, search_floor_s = $2::integer"
	        " WHERE series_id = $1::bigint AND gone_at IS NULL AND monitored", 2, params, NULL, error) ||
	    !axis_tv_plan_series(self, conn, change->series_id, "profile", change->user, 50, NULL, &queued, error))
		return FALSE;
	if (change->item > 0) {
		if (!axis_tv_store_enqueue_search(conn, change->series_id, AXIS_TV_SCOPE_EPISODE, change->item, "profile",
		        change->user, &created, error))
			return FALSE;
		queued += created ? 1 : 0;
	} else if (change->season >= 0) {
		if (!axis_tv_store_enqueue_search(conn, change->series_id, AXIS_TV_SCOPE_SEASON, change->season, "profile",
		        change->user, &created, error))
			return FALSE;
		queued += created ? 1 : 0;
	}
	change->edited.searches = queued;
	return TRUE;
}

static gboolean
edit_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GString) changes = g_string_new(NULL);
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	g_autofree gchar *profile = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *searched = NULL;
	Checked checked;
	const gchar *params[3];
	gboolean ok;

	if (!locked_series(conn, change->series_id, &series, error))
		return FALSE;
	if (!check_spec(self, conn, change->spec, &checked, error)) {
		checked_clear(&checked);
		return FALSE;
	}
	change->edited.profile_changed = checked.profile_id != series->profile_id;
	if (change->edited.profile_changed)
		g_string_append_printf(changes, "the quality profile to %s", checked.profile_name);
	if (checked.root_id != series->root_id)
		g_string_append_printf(changes, "%sthe destination to %s", changes->len > 0 ? ", " : "", checked.root_name);
	if (changes->len == 0) {
		checked_clear(&checked);
		return TRUE;
	}
	change->edited.changed = TRUE;
	profile = axis_tv_int_text(checked.profile_id);
	root = axis_tv_int_text(checked.root_id);
	params[0] = id;
	params[1] = profile;
	params[2] = root;
	ok = axis_db_conn_exec(conn, "UPDATE series SET quality_profile_id = $2::bigint, root_id = $3::bigint,"
		" updated_at = now() WHERE id = $1::bigint", 3, params, NULL, error);
	/* A new profile: what it wants is searched for now, not after the waits
	 * the old one's verdicts earned.  A new destination alone changes no
	 * verdict. */
	ok = ok && (!change->edited.profile_changed || search_with_new_profile(self, conn, change, error));
	if (ok && change->edited.profile_changed && change->edited.searches == 0) {
		/* Nothing queued: because nothing is wanted, or because what is
		 * missing was migrated and is held.  The history says which. */
		g_autoptr(AxisDbResult) apart = NULL;
		gint64 n = 0;

		ok = axis_db_conn_exec(conn, "SELECT count(*) FROM series_episode e WHERE e.series_id = $1::bigint"
			" AND e.gone_at IS NULL AND " AXIS_TV_EPISODE_HELD_SQL("e"), 1, params, &apart, error);
		if (ok && axis_db_result_get_n_rows(apart) > 0)
			axis_db_result_get_int64(apart, 0, 0, &n);
		searched = axis_tv_hold_stops(series->search_hold) ? g_strdup(" The series was migrated and is held, so "
			"nothing is searched for until searching is started.") : n > 0 ? g_strdup_printf(" Nothing is searched for: %"
			G_GINT64_FORMAT " %s held apart because Sonarr has a file for %s that Axis does not see, and nothing else of "
			"it is wanted under the profile now.", n, n == 1 ? "episode is" : "episodes are", n == 1 ? "it" : "each") :
			g_strdup(" Nothing of it is wanted under it now, so nothing is searched for.");
	} else if (change->edited.profile_changed) {
		searched = g_strdup_printf(" %u %s queued for its wanted episodes with it.", change->edited.searches,
			change->edited.searches == 1 ? "search is" : "searches are");
	}
	ok = ok && axis_tv_store_eventf(conn, series, 0, "changed", change->user, error, "Changed%s%s: %s.%s",
		change->user != NULL ? " by " : "", change->user != NULL ? change->user : "", changes->str,
		searched != NULL ? searched : "");
	checked_clear(&checked);
	return ok;
}

gboolean
axis_tv_edit_full(
	AxisTv            *self,
	gint64             series_id,
	const AxisTvSpec  *spec,
	gint64             episode,
	gint               season,
	AxisTvEdited      *edited,
	GError           **error
){
	Change change;
	gboolean ok;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.spec = spec;
	change.user = spec->user;
	change.item = episode;
	change.season = season;
	ok = in_transaction(self, edit_one, &change, error);
	if (ok && edited != NULL)
		*edited = change.edited;
	return ok;
}

gboolean
axis_tv_edit(
	AxisTv            *self,
	gint64             series_id,
	const AxisTvSpec  *spec,
	GError           **error
){
	return axis_tv_edit_full(self, series_id, spec, 0, -1, NULL, error);
}

/* ── The kind of a series (add-anime-support, A1) ───────────────── */

gboolean
axis_tv_tvdb_offered(AxisTv *self)
{
	const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");

	return tvdb != NULL && axis_meta_provider_offered(self->meta, tvdb);
}

const gchar *
axis_tv_anime_source(
	AxisTv *self,
	gint64  title_id
){
	g_autoptr(AxisDbResult) held = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *title = axis_tv_int_text(title_id);
	const gchar *params[1];

	/* THE answer to the proposal's question 1, in this one function: TheTVDB's
	 * aired order from the start, when Axis has a key and TheTVDB lists the
	 * series; else whatever any new series follows.  (The other answer, "as
	 * every new series", is this function returning NULL.) */
	if (!axis_tv_tvdb_offered(self))
		return NULL;
	/* A series whose episodes Identify holds on disk ("Manage this series")
	 * keeps the numbering those files were read under: under another, a file
	 * named for an episode that numbering lacks would be linked to nothing and
	 * its episode wanted again.  Its numbering is changed on the page that
	 * shows how each episode would map first. */
	{
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, NULL);
		gboolean on_disk = TRUE;

		if (conn == NULL)
			return NULL;
		if (!axis_tv_store_is_on_disk(conn, title_id, &on_disk, NULL))
			on_disk = TRUE;
		axis_db_pool_release(self->pool, conn);
		if (on_disk)
			return NULL;
	}
	/* The whole order, asked for when it is not fresh; a provider that does
	 * not answer leaves what the store holds. */
	axis_tv_fetch_seasons(self, title_id, "tvdb", AXIS_TV_META_WAIT_MS);
	params[0] = title;
	if (!axis_db_pool_exec(self->pool, "SELECT 1 FROM meta_episode WHERE title_id = $1::bigint AND source = 'tvdb'"
	        " LIMIT 1", 1, params, &held, &local)) {
		g_debug("series: whether TheTVDB lists a title could not be read: %s", local->message);
		return NULL;
	}
	return axis_db_result_get_n_rows(held) > 0 ? "tvdb" : NULL;
}

static gboolean
kind_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	const gchar *params[2];

	if (!locked_series(conn, change->series_id, &series, error))
		return FALSE;
	if (series->anime == change->flag)
		return TRUE;
	change->count = 1;
	params[0] = id;
	params[1] = change->flag ? AXIS_TV_KIND_ANIME : AXIS_TV_KIND_STANDARD;
	/* One write and one line: no file, link, episode, monitoring or numbering
	 * changes with the kind, and nothing is searched for. */
	return axis_db_conn_exec(conn, "UPDATE series SET kind = $2, updated_at = now() WHERE id = $1::bigint", 2, params,
			NULL, error) &&
		axis_tv_store_eventf(conn, series, 0, "changed", change->user, error, change->flag ?
			"Marked as anime%s%s. Nothing else changed: no file, no link and no numbering." :
			"No longer marked as anime%s%s. Nothing else changed: no file, no link and no numbering.",
			change->user != NULL ? " by " : "", change->user != NULL ? change->user : "");
}

gboolean
axis_tv_set_kind(
	AxisTv       *self,
	gint64        series_id,
	gboolean      anime,
	const gchar  *user,
	gboolean     *changed,
	GError      **error
){
	Change change;
	gboolean ok;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.flag = anime;
	change.user = user;
	ok = in_transaction(self, kind_one, &change, error);
	if (ok && changed != NULL)
		*changed = change.count > 0;
	return ok;
}

/* The monitored episodes of a season with no air date and no file, not in
 * flight: the scheduler never takes them; a person may. */
static gboolean
undated_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", change->series_id);
	guint found = 0;
	guint i;

	if (!locked_series(conn, change->series_id, &series, error) ||
	    !axis_tv_store_episodes(conn, ids, FALSE, &episodes, error))
		return FALSE;
	for (i = 0; i < episodes->len && change->count < 100; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		gboolean created = FALSE;

		/* (Of a held series: a person's searches, which pass by their cause;
		 * never an episode held apart.) */
		if (e->season != change->season || e->gone || !e->monitored || e->air_date != NULL || e->n_files > 0 ||
		    axis_tv_episode_in_flight(e) || axis_tv_hold_stops(e->search_hold))
			continue;
		found++;
		if (!axis_tv_store_enqueue_search(conn, change->series_id, AXIS_TV_SCOPE_EPISODE, e->id, "manual",
		        change->user, &created, error))
			return FALSE;
		change->count += created ? 1 : 0;
	}
	if (found == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "no monitored episode of that season is "
			"missing without an air date");
		return FALSE;
	}
	if (change->count == 0)
		return TRUE;
	{
		g_autofree gchar *where = change->season == 0 ? g_strdup("the specials") :
			g_strdup_printf("season %d", change->season);

		return axis_tv_store_eventf(conn, series, 0, "searched", change->user, error, "A search of the %u %s of %s "
			"with no air date asked for%s%s: %u %s queued.", found, found == 1 ? "episode" : "episodes", where,
			change->user != NULL ? " by " : "", change->user != NULL ? change->user : "", change->count,
			change->count == 1 ? "search" : "searches");
	}
}

gboolean
axis_tv_request_undated(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	const gchar  *user,
	guint        *queued,
	GError      **error
){
	Change change;
	gboolean ok;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.season = season;
	change.user = user;
	ok = in_transaction(self, undated_one, &change, error);
	if (ok && queued != NULL)
		*queued = change.count;
	return ok;
}

static gboolean
remove_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];

	if (!locked_series(conn, change->series_id, &series, error))
		return FALSE;
	params[0] = id;
	/* What a person unlinked by hand from the series is no longer held back by
	 * it (library_hand_unlink names an owner without a foreign key). */
	if (!axis_db_conn_exec(conn, "DELETE FROM library_hand_unlink WHERE module = '" AXIS_TV_MODULE_ID "'"
	        " AND owner_id = $1::bigint", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "DELETE FROM series WHERE id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	label = axis_tv_series_label(series->title, series->year);
	text = g_strdup_printf("Removed%s%s. No file was touched, and any download in progress was left with its client.",
		change->user != NULL ? " by " : "", change->user != NULL ? change->user : "");
	return axis_tv_store_event(conn, 0, series->title_id, label, 0, "removed", text, change->user, 0, 0, error);
}

gboolean
axis_tv_remove(
	AxisTv       *self,
	gint64        series_id,
	const gchar  *user,
	GError      **error
){
	Change change;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.user = user;
	if (!in_transaction(self, remove_one, &change, error))
		return FALSE;
	axis_tv_note_followed(self);
	return TRUE;
}

static gboolean
season_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	g_autoptr(AxisDbResult) episodes = NULL;
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	g_autofree gchar *season = axis_tv_int_text(change->season);
	const gchar *params[3];

	if (!locked_series(conn, change->series_id, &series, error))
		return FALSE;
	params[0] = id;
	params[1] = season;
	params[2] = change->flag ? "true" : "false";
	if (!axis_db_conn_exec(conn, "UPDATE series_season SET monitored = $3::boolean WHERE series_id = $1::bigint"
	        " AND number = $2::integer RETURNING id", 3, params, &updated, error))
		return FALSE;
	if (axis_db_result_get_n_rows(updated) == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such season");
		return FALSE;
	}
	/* Its episodes with it; one monitored now is searched for on the next run. */
	if (!axis_db_conn_exec(conn, "UPDATE series_episode SET monitored = $3::boolean,"
	        " next_search_at = CASE WHEN $3::boolean THEN NULL ELSE next_search_at END"
	        " WHERE series_id = $1::bigint AND season_number = $2::integer AND gone_at IS NULL"
	        " AND monitored IS DISTINCT FROM $3::boolean RETURNING id", 3, params, &episodes, error))
		return FALSE;
	change->count = axis_db_result_get_n_rows(episodes);
	return axis_tv_store_eventf(conn, series, 0, "monitored", change->user, error, "%s%s%s%s%s%s (%u %s changed).",
		change->season == 0 ? "The specials" : "Season ", change->season == 0 ? "" : season,
		change->flag ? " monitored" : " not monitored", change->user != NULL ? " by " : "",
		change->user != NULL ? change->user : "", "", change->count, change->count == 1 ? "episode" : "episodes");
}

gboolean
axis_tv_set_season_monitored(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	gboolean      monitored,
	const gchar  *user,
	GError      **error
){
	Change change;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.season = season;
	change.flag = monitored;
	change.user = user;
	return in_transaction(self, season_one, &change, error);
}

static gboolean
episode_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Change *change = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	g_autofree gchar *id = axis_tv_int_text(change->series_id);
	g_autofree gchar *episode = axis_tv_int_text(change->item);
	g_autofree gchar *label = NULL;
	const gchar *params[3];

	if (!locked_series(conn, change->series_id, &series, error))
		return FALSE;
	params[0] = id;
	params[1] = episode;
	params[2] = change->flag ? "true" : "false";
	if (!axis_db_conn_exec(conn, "UPDATE series_episode SET monitored = $3::boolean,"
	        " next_search_at = CASE WHEN $3::boolean THEN NULL ELSE next_search_at END"
	        " WHERE series_id = $1::bigint AND id = $2::bigint RETURNING season_number, number", 3, params, &updated,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(updated) == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such episode");
		return FALSE;
	}
	{
		gint64 season = 0;
		gint64 number = 0;

		axis_db_result_get_int64(updated, 0, 0, &season);
		axis_db_result_get_int64(updated, 0, 1, &number);
		label = axis_tv_episode_label((gint)season, (gint)number);
	}
	return axis_tv_store_eventf(conn, series, change->item, "monitored", change->user, error, "%s %s%s%s.", label,
		change->flag ? "monitored" : "not monitored", change->user != NULL ? " by " : "",
		change->user != NULL ? change->user : "");
}

gboolean
axis_tv_set_episode_monitored(
	AxisTv       *self,
	gint64        series_id,
	gint64        episode_id,
	gboolean      monitored,
	const gchar  *user,
	GError      **error
){
	Change change;

	memset(&change, 0, sizeof(change));
	change.series_id = series_id;
	change.item = episode_id;
	change.flag = monitored;
	change.user = user;
	return in_transaction(self, episode_one, &change, error);
}

/* ── Asking for a search ────────────────────────────────────────── */

gboolean
axis_tv_request_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	AxisDbConn *conn;
	gboolean made = FALSE;
	gboolean ok;

	if (created != NULL)
		*created = FALSE;
	if (!axis_acquire_cause_valid(cause)) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "The search could not be asked for.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && locked_series(conn, series_id, &series, error);
	/* A person's Search now: the hold of the one episode it names, or of the
	 * whole series when it is for all of it (axis_tv_store_end_hold()). */
	if (ok && strcmp(cause, "manual") == 0)
		ok = axis_tv_store_end_hold(conn, series, scope, item, user, "Search now", error);
	if (ok && scope == AXIS_TV_SCOPE_EPISODE) {
		g_autoptr(AxisDbResult) row = NULL;
		g_autofree gchar *series_text = axis_tv_int_text(series_id);
		g_autofree gchar *episode = axis_tv_int_text(item);
		const gchar *params[2];

		params[0] = series_text;
		params[1] = episode;
		ok = axis_db_conn_exec(conn, "SELECT season_number, number FROM series_episode WHERE series_id = $1::bigint"
			" AND id = $2::bigint AND gone_at IS NULL", 2, params, &row, error);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such episode");
			ok = FALSE;
		}
		ok = ok && axis_tv_store_enqueue_search(conn, series_id, scope, item, cause, user, &made, error);
		if (ok && made && strcmp(cause, "manual") == 0) {
			gint64 season = 0;
			gint64 number = 0;
			g_autofree gchar *label = NULL;

			axis_db_result_get_int64(row, 0, 0, &season);
			axis_db_result_get_int64(row, 0, 1, &number);
			label = axis_tv_episode_label((gint)season, (gint)number);
			ok = axis_tv_store_eventf(conn, series, item, "searched", user, error, "A search of %s asked for%s%s.",
				label, user != NULL ? " by " : "", user != NULL ? user : "");
		}
	} else if (ok && scope == AXIS_TV_SCOPE_SEASON) {
		ok = axis_tv_store_enqueue_search(conn, series_id, scope, item, cause, user, &made, error);
		if (ok && made && strcmp(cause, "manual") == 0)
			ok = axis_tv_store_eventf(conn, series, 0, "searched", user, error, "A search of %s %" G_GINT64_FORMAT
				" asked for%s%s.", item == 0 ? "the specials," : "season", item, user != NULL ? " by " : "",
				user != NULL ? user : "");
	} else if (ok) {
		guint queued = 0;

		/* Every wanted episode: what the scheduler would queue, at once. */
		ok = axis_tv_plan_series(self, conn, series_id, cause, user, 50, NULL, &queued, error);
		made = queued > 0;
		if (ok && strcmp(cause, "manual") == 0)
			ok = axis_tv_store_eventf(conn, series, 0, "searched", user, error, "A search of every wanted episode "
				"asked for%s%s: %u %s queued.", user != NULL ? " by " : "", user != NULL ? user : "", queued,
				queued == 1 ? "search" : "searches");
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && created != NULL)
		*created = made;
	return ok;
}

gboolean
axis_tv_request_manual_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *user,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *series_text = axis_tv_int_text(series_id);
	g_autofree gchar *item_text = axis_tv_int_text(item);
	g_autofree gchar *what = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean made = FALSE;
	gboolean ok;

	if (created != NULL)
		*created = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* (A manual search only lists: its job passes a brought series' hold by
	 * its cause, and ends nothing of it.) */
	ok = axis_db_conn_begin(conn, error) && locked_series(conn, series_id, &series, error);
	/* What it is for must be in the series' data. */
	params[0] = series_text;
	params[1] = item_text;
	if (ok && scope == AXIS_TV_SCOPE_EPISODE) {
		ok = axis_db_conn_exec(conn, "SELECT season_number, number FROM series_episode WHERE series_id = $1::bigint"
			" AND id = $2::bigint AND gone_at IS NULL", 2, params, &row, error);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such episode");
			ok = FALSE;
		}
		if (ok) {
			gint64 season = 0;
			gint64 number = 0;

			axis_db_result_get_int64(row, 0, 0, &season);
			axis_db_result_get_int64(row, 0, 1, &number);
			what = axis_tv_episode_label((gint)season, (gint)number);
		}
	} else if (ok && scope == AXIS_TV_SCOPE_SEASON) {
		/* (Compared as a bigint: a number past a season's range is no
		 * season, not an error.) */
		ok = axis_db_conn_exec(conn, "SELECT 1 FROM series_episode WHERE series_id = $1::bigint"
			" AND season_number = $2::bigint AND gone_at IS NULL LIMIT 1", 2, params, &row, error);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such season");
			ok = FALSE;
		}
		what = item == 0 ? g_strdup("the specials") : g_strdup_printf("season %" G_GINT64_FORMAT, item);
	} else {
		item = 0;
		what = g_strdup("the whole series");
	}
	ok = ok && axis_tv_store_enqueue_search(conn, series_id, scope, item, "list", user, &made, error);
	if (ok && made)
		ok = axis_tv_store_eventf(conn, series, scope == AXIS_TV_SCOPE_EPISODE ? item : 0, "searched", user, error,
			"A manual search of %s asked for%s%s: it lists what the indexers have and grabs nothing by itself.", what,
			user != NULL ? " by " : "", user != NULL ? user : "");
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && created != NULL)
		*created = made;
	return ok;
}

/* ── The blocklist, and giving up ───────────────────────────────── */

typedef struct
{
	Change   change;
	gint64   entry;
	guint    removed;
} Clear;

static gboolean
clear_one(
	AxisTv      *self,
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	Clear *clear = data;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) names = NULL;

	if (!locked_series(conn, clear->change.series_id, &series, error) ||
	    !axis_acquire_store_unblock(self->acquire, conn, series->id, clear->entry, &names, error))
		return FALSE;
	clear->removed = names->len;
	if (names->len == 0) {
		if (clear->entry > 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that release is not on the series' "
				"blocklist");
			return FALSE;
		}
		return TRUE;
	}
	if (names->len == 1)
		return axis_tv_store_eventf(conn, series, 0, "unblocked", clear->change.user, error, "%s taken off the "
			"blocklist%s%s.", (const gchar *)g_ptr_array_index(names, 0), clear->change.user != NULL ? " by " : "",
			clear->change.user != NULL ? clear->change.user : "");
	return axis_tv_store_eventf(conn, series, 0, "unblocked", clear->change.user, error, "The blocklist cleared%s%s "
		"(%u releases).", clear->change.user != NULL ? " by " : "", clear->change.user != NULL ? clear->change.user :
		"", names->len);
}

gboolean
axis_tv_clear_blocklist(
	AxisTv       *self,
	gint64        series_id,
	gint64        entry_id,
	const gchar  *user,
	guint        *removed,
	GError      **error
){
	Clear clear;
	gboolean ok;

	memset(&clear, 0, sizeof(clear));
	clear.change.series_id = series_id;
	clear.change.user = user;
	clear.entry = entry_id;
	ok = in_transaction(self, clear_one, &clear, error);
	if (ok && removed != NULL)
		*removed = clear.removed;
	return ok;
}

gboolean
axis_tv_give_up(
	AxisTv       *self,
	gint64        series_id,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
){
	AxisAcquireFailure failure;
	gboolean handled = FALSE;
	gboolean ok;

	memset(&failure, 0, sizeof(failure));
	failure.close_reason = "given_up";
	failure.block_reason = "given_up";
	failure.text = g_strdup_printf("Given up on%s%s.", user != NULL ? " by " : "", user != NULL ? user : "");
	failure.remove = TRUE;
	failure.retry = TRUE;
	ok = axis_acquire_handle_failure(self->acquire, series_id, grab_id, &failure, user, TRUE, &handled, NULL, error);
	g_free(failure.text);
	if (ok && !handled) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that grab is not open, or not this "
			"series'");
		return FALSE;
	}
	return ok;
}

/* ── A release a person blocks (add-block-release) ──────────────── */

/* The acquisition layer's "not found" in the module's words. */
static gboolean
block_failed(
	GError      **error,
	const gchar  *words
){
	if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		g_clear_error(error);
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, words);
	} else if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
		/* An import of the download is placing its files: a conflict (409). */
		g_autofree gchar *busy = g_strdup((*error)->message);

		g_clear_error(error);
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT, busy);
	}
	return FALSE;
}

gboolean
axis_tv_block_candidate(
	AxisTv       *self,
	gint64        series_id,
	gint64        candidate_id,
	const gchar  *user,
	GError      **error
){
	if (!axis_acquire_block_candidate(self->acquire, series_id, candidate_id, user, NULL, error))
		return block_failed(error, "that release is not one of the series' searches'");
	return TRUE;
}

gboolean
axis_tv_block_find(
	AxisTv                 *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab,
	AxisTvSeries          **series,
	GError                **error
){
	*series = NULL;
	if (!axis_acquire_block_grab_find(self->acquire, grab_id, download_id, import_id, grab, error))
		return FALSE;
	if (*grab == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "that download is not one Axis grabbed "
			"for a series");
		return FALSE;
	}
	if (!axis_tv_get(self, (*grab)->owner_id, series, NULL, error)) {
		g_clear_pointer(grab, axis_acquire_block_grab_free);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_tv_block_grab(
	AxisTv       *self,
	gint64        series_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
){
	if (!axis_acquire_block_grab(self->acquire, series_id, grab_id, user, cancelled, error))
		return block_failed(error, "that download is not one Axis grabbed for this series");
	return TRUE;
}

/* ── Reading ────────────────────────────────────────────────────── */

gboolean
axis_tv_list(
	AxisTv      *self,
	GPtrArray  **series,
	GError     **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_tv_store_summaries(self, conn, AXIS_TV_SERIES_ALL, NULL, series, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_tv_list_counted(
	AxisTv      *self,
	gboolean     attention,
	GPtrArray  **series,
	GError     **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*series = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_tv_store_overview(self, conn, NULL, AXIS_TV_MEMBERS_OF_NONE, series, NULL, NULL, error) &&
		(!attention || axis_tv_store_overview_attention(self, conn, *series, 0, (*series)->len, error));
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(series, g_ptr_array_unref);
	return ok;
}

gboolean
axis_tv_get_by_titles(
	AxisTv      *self,
	GArray      *title_ids,
	GPtrArray  **series,
	GError     **error
){
	g_autoptr(GString) array = g_string_new("{");
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	for (i = 0; i < title_ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(title_ids, gint64, i));
	g_string_append_c(array, '}');
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_tv_store_summaries(self, conn, AXIS_TV_SERIES_TITLES, array->str, series, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_tv_get(
	AxisTv         *self,
	gint64          series_id,
	AxisTvSeries  **series,
	GPtrArray     **episodes,
	GError        **error
){
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*series = NULL;
	if (episodes != NULL)
		*episodes = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_tv_store_series_one(conn, series_id, series, error);
	if (ok && *series == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	if (ok && episodes != NULL)
		ok = axis_tv_store_episodes(conn, ids, TRUE, episodes, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(series, axis_tv_series_free);
	return ok;
}

void
axis_tv_releases_free(AxisTvReleases *releases)
{
	if (releases == NULL)
		return;
	axis_acquire_search_info_free(releases->search);
	g_free(releases->scope);
	g_clear_pointer(&releases->candidates, g_ptr_array_unref);
	g_clear_pointer(&releases->holds, g_ptr_array_unref);
	g_clear_pointer(&releases->mismatched, g_array_unref);
	g_clear_pointer(&releases->blocklist, g_ptr_array_unref);
	g_free(releases);
}

/* The search in @found (its id and scope, or no row) with its releases,
 * what each holds, and the blocklist. */
static gboolean
releases_read(
	AxisTv          *self,
	AxisDbConn      *conn,
	gint64           series_id,
	AxisDbResult    *found,
	AxisTvReleases  *releases,
	GError         **error
){
	g_autoptr(AxisDbResult) extra = NULL;
	gint64 search_id = 0;
	gboolean ok = TRUE;
	guint i;

	if (axis_db_result_get_n_rows(found) > 0) {
		axis_db_result_get_int64(found, 0, 0, &search_id);
		releases->scope = g_strdup(axis_db_result_get_text(found, 0, 1));
		ok = axis_acquire_store_search(self->acquire, conn, series_id, search_id, &releases->search, error) &&
			axis_acquire_store_candidates(self->acquire, conn, search_id, &releases->candidates, error);
		if (ok) {
			g_autofree gchar *search = axis_tv_int_text(search_id);
			const gchar *more[1];

			more[0] = search;
			ok = axis_db_conn_exec(conn, "SELECT id, holds, mismatched FROM series_candidate WHERE search_id = $1::bigint"
				" ORDER BY position", 1, more, &extra, error);
		}
	}
	if (releases->candidates == NULL)
		releases->candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_candidate_free);
	releases->holds = g_ptr_array_new_with_free_func(g_free);
	releases->mismatched = g_array_new(FALSE, TRUE, sizeof(gboolean));
	for (i = 0; ok && i < releases->candidates->len; i++) {
		const AxisAcquireCandidate *c = g_ptr_array_index(releases->candidates, i);
		gboolean mismatched = FALSE;
		gchar *holds = NULL;
		guint r;

		for (r = 0; extra != NULL && r < axis_db_result_get_n_rows(extra); r++) {
			gint64 id = 0;

			axis_db_result_get_int64(extra, r, 0, &id);
			if (id != c->id)
				continue;
			holds = g_strdup(axis_db_result_get_text(extra, r, 1));
			axis_db_result_get_boolean(extra, r, 2, &mismatched);
		}
		g_ptr_array_add(releases->holds, holds);
		g_array_append_val(releases->mismatched, mismatched);
	}
	return ok && axis_acquire_store_blocklist(self->acquire, conn, series_id, &releases->blocklist, error);
}

gboolean
axis_tv_get_releases(
	AxisTv           *self,
	gint64            series_id,
	gint64            episode_id,
	gint              season,
	AxisTvReleases  **releases_out,
	GError          **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autofree gchar *series = axis_tv_int_text(series_id);
	g_autofree gchar *episode = axis_tv_int_text(episode_id);
	g_autofree gchar *season_text = axis_tv_int_text(season);
	AxisTvReleases *releases = g_new0(AxisTvReleases, 1);
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean ok;

	*releases_out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_tv_releases_free(releases);
		return FALSE;
	}
	params[0] = series;
	params[1] = episode;
	params[2] = season_text;
	/* The last automatic search: a manual one has a page of its own. */
	ok = axis_db_conn_exec(conn, episode_id > 0 ?
		"SELECT id, scope FROM series_search WHERE series_id = $1::bigint AND episode_id = $2::bigint"
		" AND $3::integer IS NOT NULL AND cause <> 'list' ORDER BY id DESC LIMIT 1" :
		"SELECT id, scope FROM series_search WHERE series_id = $1::bigint AND $2::bigint = 0 AND scope = 'season'"
		" AND season_number = $3::integer AND cause <> 'list' ORDER BY id DESC LIMIT 1", 3, params, &found, error) &&
		releases_read(self, conn, series_id, found, releases, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_tv_releases_free(releases);
		return FALSE;
	}
	*releases_out = releases;
	return TRUE;
}

gboolean
axis_tv_get_manual(
	AxisTv           *self,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	AxisTvReleases  **releases_out,
	gboolean         *running,
	GError          **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(AxisDbResult) job = NULL;
	g_autofree gchar *series = axis_tv_int_text(series_id);
	g_autofree gchar *item_text = axis_tv_int_text(item);
	g_autofree gchar *key = axis_tv_manual_key(series_id, scope, item);
	AxisTvReleases *releases = g_new0(AxisTvReleases, 1);
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean ok;

	*releases_out = NULL;
	*running = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_tv_releases_free(releases);
		return FALSE;
	}
	params[0] = series;
	params[1] = axis_tv_scope_name(scope);
	params[2] = item_text;
	ok = axis_db_conn_exec(conn, "SELECT id, scope FROM series_search WHERE series_id = $1::bigint AND cause = 'list'"
		" AND scope = $2 AND CASE scope WHEN 'episode' THEN episode_id = $3::bigint"
		" WHEN 'season' THEN season_number = $3::bigint ELSE true END ORDER BY id DESC LIMIT 1", 3, params, &found,
		error) && releases_read(self, conn, series_id, found, releases, error) &&
		/* With the blocklist as it is now: a release a person blocked from this
		 * list says so at once (add-block-release). */
		axis_acquire_candidates_mark_blocked(self->acquire, conn, series_id, releases->candidates, error);
	params[0] = key;
	ok = ok && axis_db_conn_exec(conn, "SELECT 1 FROM job WHERE dedupe_key = $1 AND state IN ('queued', 'running')"
		" LIMIT 1", 1, params, &job, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_tv_releases_free(releases);
		return FALSE;
	}
	*running = axis_db_result_get_n_rows(job) > 0;
	*releases_out = releases;
	return TRUE;
}

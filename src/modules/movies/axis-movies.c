/*
 * axis-movies.c - the movies module: its lifetime, settings and jobs, and
 * adding, changing and removing films, queueing a search, the blocklist
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "modules/movies/axis-movies-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-registry.h"

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const availability_choices[] = { "announced", "in_cinemas", "released", NULL };
static const gchar *const protocol_choices[] = { "any", "usenet", "torrent", NULL };

/* The words the Settings page shows for the values above, in the same order. */
static const gchar *const availability_labels[] = { "Announced", "In cinemas", "Released", NULL };
static const gchar *const protocol_labels[] = { "Either", "Usenet", "Torrent", NULL };

static const AxisSettingInfo settings_table[] = {
	{ AXIS_MOVIES_SETTING_AVAILABILITY, AXIS_SETTING_CHOICE, "Search a new movie from",
	  "When a newly added movie may first be searched for: Announced (at once), In cinemas (from its release date) "
	  "or Released (a while after it, below). Each movie can be changed on its page.", 0, "released", 0, 0, 0, 0,
	  availability_choices },
	{ AXIS_MOVIES_SETTING_RELEASED_AFTER, AXIS_SETTING_DURATION, "A movie counts as released this long after its first release date",
	  "This long after its first release date. The details provider gives one date, the first release anywhere (usually "
	  "in cinemas); a digital or disc release usually follows some months later.", 0, "90d", 0, 3650 * 86400,
	  0, 0, NULL },
	{ AXIS_MOVIES_SETTING_PER_RUN, AXIS_SETTING_INTEGER, "Movie searches per 15-minute run",
	  "At most this many movies are searched for each time the scheduler runs (every 15 minutes), the least "
	  "recently searched first, so that the indexers are not hammered.", 0, "5", 1, 100, 0, 0, NULL },
	{ AXIS_MOVIES_SETTING_FLOOR, AXIS_SETTING_DURATION, "Shortest time between two searches of a movie",
	  "The scheduler does not search one movie again sooner than this. Each search that grabs nothing doubles it, "
	  "up to the limit below; it starts again from this when the movie's availability changes.", 0, "6h", 3600,
	  7 * 86400, 0, 0, NULL },
	{ AXIS_MOVIES_SETTING_FLOOR_MAX, AXIS_SETTING_DURATION, "Longest time between two searches of a movie",
	  "Where the doubling stops.", 0, "7d", 3600, 90 * 86400, 0, 0, NULL },
	{ AXIS_MOVIES_SETTING_GIVE_UP, AXIS_SETTING_DURATION, "Give up on a stalled movie download after",
	  "A download that has not grown for this long is put on the movie's blocklist, removed from its download client with "
	  "its data, and the next best release is sought. (The download client's own stall mark comes earlier.)", 0,
	  "6h", 600, 30 * 86400, 0, 0, NULL },
	{ AXIS_MOVIES_SETTING_PROTOCOL, AXIS_SETTING_CHOICE, "Prefer movie releases from",
	  "Between two releases the quality profile ranks equal, the one from this kind of indexer goes first.", 0, "any", 0,
	  0, 0, 0, protocol_choices },
	{ AXIS_MOVIES_SETTING_RETENTION, AXIS_SETTING_INTEGER, "Usenet retention for movie searches, in days",
	  "How long your Usenet provider keeps articles; an older Usenet release is rejected. 0: not known, nothing is "
	  "too old.", 0, "0", 0, 100000, 0, 0, NULL },
	{ AXIS_MOVIES_SETTING_LANGUAGES, AXIS_SETTING_STRING_LIST, "Languages wanted in movie releases",
	  "Two-letter codes (en, de, fr), one per line. A release that names its languages, none of them wanted and "
	  "with no multi-language mark, is rejected; one that names none is taken to be in the original language. "
	  "Empty: no check.", 0, NULL, 0, 16, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_movies_setting_table(void)
{
	axis_setting_set_choice_labels(AXIS_MOVIES_SETTING_AVAILABILITY, availability_labels);
	axis_setting_set_choice_labels(AXIS_MOVIES_SETTING_PROTOCOL, protocol_labels);
	return settings_table;
}

static gboolean
declared(
	AxisMovies  *self,
	const gchar *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

gint64
axis_movies_setting_duration(
	AxisMovies  *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_duration(self->settings, key) : fallback;
}

gint64
axis_movies_setting_integer(
	AxisMovies  *self,
	const gchar *key,
	gint64       fallback
){
	return declared(self, key) ? axis_settings_get_integer(self->settings, key) : fallback;
}

gchar *
axis_movies_setting_string(
	AxisMovies  *self,
	const gchar *key,
	const gchar *fallback
){
	return declared(self, key) ? axis_settings_get_string(self->settings, key) : g_strdup(fallback);
}

gchar **
axis_movies_setting_languages(AxisMovies *self)
{
	g_auto(GStrv) items = declared(self, AXIS_MOVIES_SETTING_LANGUAGES) ?
		axis_settings_get_string_list(self->settings, AXIS_MOVIES_SETTING_LANGUAGES) : NULL;
	GPtrArray *codes = g_ptr_array_new();
	guint i;

	for (i = 0; items != NULL && items[i] != NULL; i++) {
		g_autofree gchar *code = g_ascii_strdown(g_strstrip(items[i]), -1);

		if (strlen(code) == 2 && g_ascii_isalpha(code[0]) && g_ascii_isalpha(code[1]))
			g_ptr_array_add(codes, g_steal_pointer(&code));
	}
	g_ptr_array_add(codes, NULL);
	return (gchar **)g_ptr_array_free(codes, FALSE);
}

void
axis_movies_get_rules(
	AxisMovies     *self,
	AxisMovieRules *rules
){
	axis_movie_rules_init(rules, g_get_real_time());
	rules->released_after_days = (gint)(axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_RELEASED_AFTER,
		90 * 86400) / 86400);
	rules->give_up_after_s = axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_GIVE_UP, 6 * 3600);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisMovies *
axis_movies_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisIndexers     *indexers,
	AxisDownloads    *downloads,
	AxisStorage      *storage,
	AxisQualityStore *quality
){
	AxisMovies *self;

	g_return_val_if_fail(pool != NULL && meta != NULL && indexers != NULL && downloads != NULL && storage != NULL &&
		quality != NULL, NULL);

	self = g_new0(AxisMovies, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->meta = axis_meta_ref(meta);
	self->indexers = axis_indexers_ref(indexers);
	self->downloads = axis_downloads_ref(downloads);
	self->storage = axis_storage_ref(storage);
	self->quality = axis_quality_store_ref(quality);
	self->acquire = axis_acquire_new(pool, settings, indexers, downloads, storage, axis_movies_acquire_info(), self);
	/* The films are kept fresh, and the profiles they use are kept. */
	axis_meta_add_reference(meta, "movies", "SELECT title_id FROM movie");
	axis_quality_store_add_user(quality, "movie", "movies", "SELECT quality_profile_id FROM movie");
	return self;
}

AxisMovies *
axis_movies_ref(AxisMovies *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_movies_unref(AxisMovies *self)
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
axis_movies_set_compat(
	AxisMovies *self,
	AxisCompat *compat
){
	axis_acquire_set_compat(self->acquire, compat);
}

void
axis_movies_stop(AxisMovies *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_movies_set_claim_hook(
	AxisMovies          *self,
	AxisMoviesClaimHook  hook,
	gpointer             user_data
){
	axis_acquire_set_claim_hook(self->acquire, hook, user_data);
}

void
axis_movies_set_replace_hook(
	AxisMovies          *self,
	AxisMoviesClaimHook  hook,
	gpointer             user_data
){
	axis_acquire_set_replace_hook(self->acquire, hook, user_data);
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
cause_valid(const gchar *cause)
{
	return axis_acquire_cause_valid(cause);
}

static gboolean
search_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisMovies *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	gint64 movie_id = payload_int(payload, "movie");
	const gchar *cause = payload_text(payload, "cause");
	const gchar *user = payload_text(payload, "user");
	AxisMovieSearchReport report;

	if (movie_id <= 0 || !cause_valid(cause)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "the job does not name a movie and a cause");
		return FALSE;
	}
	if (user != NULL && (strlen(user) > 200 || !g_utf8_validate(user, -1, NULL)))
		user = NULL;
	if (!axis_movies_run_search(self, movie_id, cause, user, axis_job_context_get_cancellable(context), &report,
	        error))
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
	AxisMovieFollowReport report;

	if (!axis_movies_run_follow(user_data, &report, error))
		return FALSE;
	if (report.failed + report.settled + report.completed > 0)
		g_info("films: %u grabs dealt with after a failure, %u claims settled, %u completions recorded, %u searches "
			"for the next best", report.failed, report.settled, report.completed, report.retries);
	return TRUE;
}

static gboolean
schedule_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisMovieScheduleReport report;

	if (!axis_movies_run_schedule(user_data, &report, error))
		return FALSE;
	if (report.queued > 0)
		g_info("films: the scheduler queued %u searches (%u films due of %u monitored)", report.queued, report.due,
			report.considered);
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_movies_web_register(user_data, web);
}

static const AxisJobHandlerInfo handlers[] = {
	/* On the downloads queue: a long search never holds up a scan or the
	 * metadata. */
	{ AXIS_MOVIES_SEARCH_KIND, AXIS_ACQUIRE_SEARCH_QUEUE, 2, search_handler,
	  "Searching for a movie", AXIS_JOB_CLASS_WORK },
	{ AXIS_MOVIES_FOLLOW_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, follow_handler,
	  "Following the movies' downloads", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_MOVIES_SCHEDULE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, schedule_handler,
	  "Choosing movies to search for", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_MOVIES_FOLLOW_SCHEDULE, AXIS_MOVIES_FOLLOW_KIND, NULL, AXIS_MOVIES_FOLLOW_INTERVAL_S },
	{ AXIS_MOVIES_SCHEDULE_SCHEDULE, AXIS_MOVIES_SCHEDULE_KIND, NULL, AXIS_MOVIES_SCHEDULE_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

/* Media -> Movies, the first media type. */
static const AxisNavEntry nav_entries[] = {
	{ "All", "/films", 10, AXIS_NAV_GROUP_MEDIA, FALSE, "Movies", 0 },
	{ "Wanted", "/films/wanted", 20, AXIS_NAV_GROUP_MEDIA, FALSE, "Movies", 0 },
	{ "History", "/films/history", 30, AXIS_NAV_GROUP_MEDIA, FALSE, "Movies", 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends[] = {
	AXIS_META_MODULE_ID, AXIS_INDEXER_MODULE_ID, AXIS_DOWNLOAD_MODULE_ID, AXIS_QUALITY_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_movies_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_MOVIES_MODULE_ID;
		info.name = "Movies";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "The movies Axis looks after: add one from its page, and Axis searches the indexers, "
			"takes the best release the movie's quality profile accepts, hands it to a download client and "
			"follows it until it has arrived, trying the next best when one fails. Every release considered is "
			"kept with its verdict and reasons.";
		info.settings = settings_table;
		info.depends_on = depends;
	}
	return &info;
}

/* ── Adding, changing, removing ─────────────────────────────────── */

typedef struct
{
	gint64       profile_id;
	gchar       *profile_name;
	gint64       root_id;
	gchar       *root_name;
} Checked;

static void
checked_clear(Checked *checked)
{
	g_free(checked->profile_name);
	g_free(checked->root_name);
}

/* A new movie's root is on a disk that can be written to now: a stale page
 * or a hand-made post naming one that is away or mounted read-only is
 * refused (its searches and downloads would never be imported). */
static gboolean
check_writable(
	AxisMovies   *self,
	gint64        root_id,
	const gchar  *root_name,
	GError      **error
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *why;

	if (!axis_storage_get_root(self->storage, root_id, &root, &local)) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "The library folder %s cannot be read now: %s",
			root_name, local->message);
		return FALSE;
	}
	volume = axis_storage_get_volume(self->storage, root->volume_id);
	why = volume != NULL ? axis_volume_state_unwritable_why(volume->state) : "its disk is not known";
	if (why == NULL)
		return TRUE;
	g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "The library folder %s cannot be written to now: "
		"%s. Choose another library folder, or add the movie once it can be written to again", root_name, why);
	return FALSE;
}

/* The profile (0: the default) and the root of @spec, checked on @conn. */
static gboolean
check_spec(
	AxisMovies          *self,
	AxisDbConn          *conn,
	const AxisMovieSpec *spec,
	Checked             *checked,
	GError             **error
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
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "There is no such quality "
			"profile.");
		return FALSE;
	}
	if (!axis_quality_store_load(conn, stored->id, &profile, NULL)) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "That quality profile "
			"cannot be used: its settings are not valid.");
		return FALSE;
	}
	checked->profile_id = stored->id;
	checked->profile_name = g_strdup(stored->name);
	if (spec->root_id <= 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "Choose a destination library folder.");
		return FALSE;
	}
	root = axis_movies_int_text(spec->root_id);
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT media_type, name FROM library_root WHERE id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0 ||
	    g_strcmp0(axis_db_result_get_text(result, 0, 0), axis_media_type_to_string(AXIS_MEDIA_MOVIES)) != 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "That is not a library folder "
			"for movies.");
		return FALSE;
	}
	checked->root_id = spec->root_id;
	checked->root_name = g_strdup(axis_db_result_get_text(result, 0, 1));
	if ((guint)spec->availability > AXIS_MOVIE_RELEASED) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "Choose when to search "
			"from.");
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_movies_add_in_transaction(
	AxisMovies          *self,
	AxisDbConn          *conn,
	const AxisMovieSpec *spec,
	gint64              *movie_id,
	GError             **error
){
	g_autoptr(AxisDbResult) title = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autofree gchar *title_text = axis_movies_int_text(spec->title_id);
	g_autofree gchar *profile_text = NULL;
	g_autofree gchar *root_text = NULL;
	g_autofree gchar *floor = axis_movies_int_text(axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_FLOOR,
		6 * 3600));
	g_autoptr(GError) local = NULL;
	Checked checked;
	const gchar *params[7];
	guint identified = 0;
	gboolean ok;

	params[0] = title_text;
	if (!axis_db_conn_exec(conn, "SELECT kind FROM meta_title WHERE id = $1::bigint", 1, params, &title, error))
		return FALSE;
	if (axis_db_result_get_n_rows(title) == 0 || g_strcmp0(axis_db_result_get_text(title, 0, 0), "film") != 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "That is not a movie Axis "
			"knows.");
		return FALSE;
	}
	if (!check_spec(self, conn, spec, &checked, error)) {
		checked_clear(&checked);
		return FALSE;
	}
	profile_text = axis_movies_int_text(checked.profile_id);
	root_text = axis_movies_int_text(checked.root_id);
	params[0] = title_text;
	params[1] = profile_text;
	params[2] = root_text;
	params[3] = spec->monitored ? "true" : "false";
	params[4] = axis_movie_availability_name(spec->availability);
	params[5] = spec->user;
	params[6] = floor;
	ok = axis_db_conn_exec(conn, "INSERT INTO movie (title_id, quality_profile_id, root_id, monitored,"
		" min_availability, added_by, search_floor_s) VALUES ($1::bigint, $2::bigint, $3::bigint, $4::boolean, $5,"
		" $6, $7::integer) RETURNING id", 7, params, &inserted, &local);
	if (!ok) {
		checked_clear(&checked);
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_EXISTS, "that movie is among your movies "
				"already");
			return FALSE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	axis_db_result_get_int64(inserted, 0, 0, movie_id);
	/* Its files already identified in the library: in the library at once. */
	ok = axis_movies_link_identified(conn, *movie_id, spec->title_id, &identified, error);
	/* One to get must go where it can be imported (one managed where its
	 * file is, on a disk mounted read-only, need not).
	 * A movie migrated is not fetched for until a person starts its
	 * searches, and its file is where it is: its folder need not be writable
	 * now (bringing over works while the disks are read-only). */
	ok = ok && (identified > 0 || spec->brought_from != NULL ||
		check_writable(self, checked.root_id, checked.root_name, error));
	ok = ok && axis_movies_store_facts_one(conn, *movie_id, &facts, error) && facts != NULL;
	if (ok && spec->brought_from != NULL)
		/* The one line of a movie migrated (the other app's history is
		 * not brought). */
		ok = axis_movies_store_eventf(conn, facts, "added", spec->user, 0, 0, error,
			"Migrated from %s%s%s, with the quality profile %s, the destination %s and search from %s%s. Axis "
			"does not search for it until searching is started.", spec->brought_from,
			spec->user != NULL ? " by " : "", spec->user != NULL ? spec->user : "", checked.profile_name,
			checked.root_name, axis_movie_availability_label(spec->availability),
			spec->monitored ? "" : "; not monitored");
	else if (ok)
		ok = axis_movies_store_eventf(conn, facts, "added", spec->user, 0, 0, error,
			"Added%s%s, with the quality profile %s, the destination %s and search from %s%s%s.",
			spec->user != NULL ? " by " : "", spec->user != NULL ? spec->user : "", checked.profile_name,
			checked.root_name, axis_movie_availability_label(spec->availability),
			spec->monitored ? "" : "; not monitored",
			identified > 0 ? ". Its file was identified in the library already, so nothing was searched for" : "");
	/* Nothing is searched for a film whose file is there, nor for one
	 * migrated. */
	if (ok && spec->search_now && identified == 0 && spec->brought_from == NULL)
		ok = axis_movies_store_enqueue_search(conn, *movie_id, "added", spec->user, NULL, NULL, error);
	checked_clear(&checked);
	return ok;
}

gboolean
axis_movies_add(
	AxisMovies          *self,
	const AxisMovieSpec *spec,
	gint64              *movie_id,
	GError             **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gint64 id = 0;
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_movies_add_in_transaction(self, conn, spec, &id, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && movie_id != NULL)
		*movie_id = id;
	return ok;
}

/* With a new profile, as the scheduler would take it (axis-movies-follow.c)
 * but now: monitored, wanted under the new profile (missing, below its
 * cutoff, or its last search needing attention), out, nothing in flight or
 * queued, somewhere to go.  *@why: (nullable) static, why not. */
static gboolean
search_with_new_profile(
	AxisMovies       *self,
	AxisDbConn       *conn,
	gint64            movie_id,
	const gchar      *user,
	gboolean         *searched,
	const gchar     **why,
	GError          **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	AxisMovieRules rules;
	AxisMovieState state;
	gboolean created = FALSE;

	*searched = FALSE;
	*why = NULL;
	axis_movies_get_rules(self, &rules);
	if (!axis_movies_store_facts_one(conn, movie_id, &facts, error))
		return FALSE;
	if (facts == NULL)
		return TRUE;
	state = axis_movie_derive(facts, &rules, NULL);
	if (!facts->monitored)
		*why = "it is not monitored";
	else if (axis_movie_hold_stops(facts->search_hold))
		*why = "it was migrated, and searching for it has not been started";
	else if (state != AXIS_MOVIE_WANTED && state != AXIS_MOVIE_UPGRADE_WANTED && state != AXIS_MOVIE_ATTENTION)
		*why = state == AXIS_MOVIE_NOT_AVAILABLE ? "it is not out yet" : facts->n_files > 0 ?
			"its file in the library is at the profile's cutoff" : "it is not wanted now";
	else if (!axis_movie_available(facts, &rules, NULL))
		*why = "it is not out yet";
	else if (axis_movie_in_flight(facts))
		*why = "a grab of it is in flight";
	else if (facts->search_queued || facts->search_running)
		*why = "a search of it is queued already, and runs with the new profile";
	else if (facts->root_id == 0 || !axis_volume_state_is_usable(facts->volume_state) || !facts->profile_usable)
		*why = "it has nowhere to go now, or the profile cannot be used";
	if (*why != NULL)
		return TRUE;
	if (!axis_movies_store_enqueue_search(conn, movie_id, "profile", user, NULL, &created, error))
		return FALSE;
	*searched = created;
	if (!created)
		*why = "a search of it is queued already, and runs with the new profile";
	return TRUE;
}

static gboolean
edit_in_transaction(
	AxisMovies          *self,
	AxisDbConn          *conn,
	gint64               movie_id,
	const AxisMovieSpec *spec,
	AxisMovieEdited     *edited,
	GError             **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autoptr(GString) changes = g_string_new(NULL);
	const gchar *why = NULL;
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autofree gchar *profile_text = NULL;
	g_autofree gchar *root_text = NULL;
	g_autofree gchar *floor = axis_movies_int_text(axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_FLOOR,
		6 * 3600));
	gboolean found = FALSE;
	gboolean reset;
	Checked checked;
	const gchar *params[7];
	gboolean ok;

	if (!axis_movies_store_lock(conn, movie_id, &found, error))
		return FALSE;
	if (!found || !axis_movies_store_facts_one(conn, movie_id, &facts, error) || facts == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	if (!check_spec(self, conn, spec, &checked, error)) {
		checked_clear(&checked);
		return FALSE;
	}
	if (checked.profile_id != facts->profile_id)
		g_string_append_printf(changes, "%sthe quality profile to %s", changes->len > 0 ? ", " : "",
			checked.profile_name);
	if (checked.root_id != facts->root_id)
		g_string_append_printf(changes, "%sthe destination to %s", changes->len > 0 ? ", " : "", checked.root_name);
	if (spec->monitored != facts->monitored)
		g_string_append_printf(changes, "%s%s", changes->len > 0 ? ", " : "", spec->monitored ? "monitored" :
			"not monitored");
	if (spec->availability != facts->availability)
		g_string_append_printf(changes, "%swhat to search from to %s", changes->len > 0 ? ", " : "",
			axis_movie_availability_label(spec->availability));
	edited->monitored = spec->monitored;
	if (changes->len == 0) {
		checked_clear(&checked);
		return TRUE;
	}
	edited->changed = TRUE;
	edited->profile_changed = checked.profile_id != facts->profile_id;
	/* What it may be searched for, and when, changed: the floor starts over. */
	reset = spec->availability != facts->availability || checked.profile_id != facts->profile_id ||
		(spec->monitored && !facts->monitored);
	profile_text = axis_movies_int_text(checked.profile_id);
	root_text = axis_movies_int_text(checked.root_id);
	params[0] = id;
	params[1] = profile_text;
	params[2] = root_text;
	params[3] = spec->monitored ? "true" : "false";
	params[4] = axis_movie_availability_name(spec->availability);
	params[5] = reset ? "true" : "false";
	params[6] = floor;
	ok = axis_db_conn_exec(conn, "UPDATE movie SET quality_profile_id = $2::bigint, root_id = $3::bigint,"
		" monitored = $4::boolean, min_availability = $5, updated_at = now(),"
		" search_floor_s = CASE WHEN $6::boolean THEN $7::integer ELSE search_floor_s END,"
		" next_search_at = CASE WHEN $6::boolean THEN NULL ELSE next_search_at END"
		" WHERE id = $1::bigint", 7, params, NULL, error);
	/* A new profile: the film is searched for with it now, not after the
	 * wait the old one's verdicts earned.  A new destination alone changes
	 * no verdict. */
	ok = ok && (!edited->profile_changed || search_with_new_profile(self, conn, movie_id, spec->user,
		&edited->searched, &why, error));
	ok = ok && axis_movies_store_eventf(conn, facts, "changed", spec->user, 0, 0, error, "Changed%s%s: %s.%s%s%s%s",
		spec->user != NULL ? " by " : "", spec->user != NULL ? spec->user : "", changes->str,
		edited->searched ? " It is searched for again now with the new profile." : "",
		why != NULL ? " Nothing is searched for now: " : "", why != NULL ? why : "", why != NULL ? "." : "");
	checked_clear(&checked);
	return ok;
}

gboolean
axis_movies_edit_full(
	AxisMovies          *self,
	gint64               movie_id,
	const AxisMovieSpec *spec,
	AxisMovieEdited     *edited,
	GError             **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	AxisMovieEdited local;
	gboolean ok;

	memset(&local, 0, sizeof(local));
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && edit_in_transaction(self, conn, movie_id, spec, &local, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && edited != NULL)
		*edited = local;
	return ok;
}

gboolean
axis_movies_edit(
	AxisMovies          *self,
	gint64               movie_id,
	const AxisMovieSpec *spec,
	GError             **error
){
	return axis_movies_edit_full(self, movie_id, spec, NULL, error);
}

static gboolean
remove_in_transaction(
	AxisDbConn   *conn,
	gint64        movie_id,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *text = NULL;
	gboolean found = FALSE;
	gboolean on_disk = FALSE;
	const gchar *params[1];

	if (!axis_movies_store_lock(conn, movie_id, &found, error))
		return FALSE;
	if (!found || !axis_movies_store_facts_one(conn, movie_id, &facts, error) || facts == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	params[0] = id;
	/* Gone from the films, it is a movie on disk still when identification
	 * links its file. */
	/* What a person unlinked by hand from the movie is no longer held back by
	 * it (library_hand_unlink names an owner without a foreign key). */
	if (!axis_db_conn_exec(conn, "DELETE FROM library_hand_unlink WHERE module = '" AXIS_MOVIES_MODULE_ID "'"
	        " AND owner_id = $1::bigint", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "DELETE FROM movie WHERE id = $1::bigint", 1, params, NULL, error) ||
	    !axis_movies_store_on_disk(conn, facts->title_id, &on_disk, error))
		return FALSE;
	label = axis_movies_title_label(facts->title, facts->year);
	text = g_strdup_printf("Removed%s%s. No file was touched%s%s.", user != NULL ? " by " : "", user != NULL ? user : "",
		axis_movie_in_flight(facts) ? ", and its download in progress was left with its client" : "",
		on_disk ? "; its file is still on disk, so it stays in Movies as In library, not managed by Axis" : "");
	return axis_movies_store_event(conn, 0, facts->title_id, label, "removed", text, user, 0, 0, error);
}

gboolean
axis_movies_remove(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	GError      **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && remove_in_transaction(conn, movie_id, user, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── Asking for a search ────────────────────────────────────────── */

gboolean
axis_movies_request_search(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autofree gchar *from = NULL;
	AxisMovieRules rules;
	AxisDbConn *conn;
	gboolean made = FALSE;
	gboolean ok;

	if (created != NULL)
		*created = FALSE;
	if (!cause_valid(cause)) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "The search could not be asked for.");
		return FALSE;
	}
	axis_movies_get_rules(self, &rules);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_facts_one(conn, movie_id, &facts, error);
	if (ok && facts == NULL) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	if (ok && strcmp(cause, "manual") == 0) {
		if (!axis_movie_available(facts, &rules, &from)) {
			g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_AVAILABLE, "it is not available yet%s%s: "
				"change when to search from to search for it now", from != NULL ? ", not before " : "",
				from != NULL ? from : "");
			ok = FALSE;
		} else if (axis_movie_in_flight(facts)) {
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "A grab of it is under way: "
				"nothing is searched for until it has ended.");
			ok = FALSE;
		}
	}
	ok = ok && axis_db_conn_begin(conn, error);
	/* A person's Search now ends a brought movie's hold, before the search
	 * is queued: the job then finds it free. */
	if (ok && strcmp(cause, "manual") == 0)
		ok = axis_movies_store_end_hold(conn, facts, user, "Search now", error);
	ok = ok && axis_movies_store_enqueue_search(conn, movie_id, cause, user, NULL, &made, error);
	if (ok && made && strcmp(cause, "manual") == 0)
		ok = axis_movies_store_eventf(conn, facts, "searched", user, 0, 0, error, "Search asked for%s%s.",
			user != NULL ? " by " : "", user != NULL ? user : "");
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && created != NULL)
		*created = made;
	return ok;
}

gboolean
axis_movies_request_manual_search(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	gboolean     *created,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	AxisDbConn *conn;
	gboolean made = FALSE;
	gboolean ok;

	if (created != NULL)
		*created = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_facts_one(conn, movie_id, &facts, error);
	if (ok && facts == NULL) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	/* Whatever is wanted now: a person's look. */
	/* A person's manual search ends a brought movie's hold, as Search now
	 * does. */
	ok = ok && axis_db_conn_begin(conn, error) &&
		axis_movies_store_end_hold(conn, facts, user, "a manual search", error) &&
		axis_movies_store_enqueue_search(conn, movie_id, "list", user, NULL, &made, error);
	if (ok && made)
		ok = axis_movies_store_eventf(conn, facts, "searched", user, 0, 0, error, "A manual search asked for%s%s: it "
			"lists what the indexers have and grabs nothing by itself.", user != NULL ? " by " : "",
			user != NULL ? user : "");
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && created != NULL)
		*created = made;
	return ok;
}

gboolean
axis_movies_get_manual(
	AxisMovies            *self,
	gint64                 movie_id,
	AxisMovieSearchInfo  **search,
	GPtrArray            **candidates,
	gboolean              *running,
	GError               **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(AxisDbResult) job = NULL;
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *key = g_strdup_printf(AXIS_MOVIES_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":list", movie_id);
	const gchar *params[1];
	AxisDbConn *conn;
	gint64 search_id = 0;
	gboolean ok;

	*search = NULL;
	*candidates = NULL;
	*running = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = movie;
	ok = axis_db_conn_exec(conn, "SELECT id FROM movie_search WHERE movie_id = $1::bigint AND cause = 'list'"
		" ORDER BY id DESC LIMIT 1", 1, params, &found, error);
	if (ok && axis_db_result_get_n_rows(found) > 0) {
		axis_db_result_get_int64(found, 0, 0, &search_id);
		/* With the blocklist as it is now: a release a person blocked from this
		 * list says so at once (add-block-release). */
		ok = axis_acquire_store_search(self->acquire, conn, movie_id, search_id, search, error) &&
			axis_acquire_store_candidates(self->acquire, conn, search_id, candidates, error) &&
			axis_acquire_candidates_mark_blocked(self->acquire, conn, movie_id, *candidates, error);
	}
	params[0] = key;
	ok = ok && axis_db_conn_exec(conn, "SELECT 1 FROM job WHERE dedupe_key = $1 AND state IN ('queued', 'running')"
		" LIMIT 1", 1, params, &job, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		g_clear_pointer(search, axis_movie_search_info_free);
		g_clear_pointer(candidates, g_ptr_array_unref);
		return FALSE;
	}
	if (*candidates == NULL)
		*candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_candidate_free);
	*running = axis_db_result_get_n_rows(job) > 0;
	return TRUE;
}

/* ── The blocklist ──────────────────────────────────────────────── */

static gboolean
clear_in_transaction(
	AxisMovies   *self,
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        entry_id,
	const gchar  *user,
	guint        *removed,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autoptr(GPtrArray) deleted = NULL;
	gboolean found = FALSE;
	guint n;

	if (!axis_movies_store_lock(conn, movie_id, &found, error))
		return FALSE;
	if (!found || !axis_movies_store_facts_one(conn, movie_id, &facts, error) || facts == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	if (!axis_acquire_store_unblock(self->acquire, conn, movie_id, entry_id, &deleted, error))
		return FALSE;
	n = deleted->len;
	if (removed != NULL)
		*removed = n;
	if (n == 0) {
		if (entry_id > 0) {
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that release is not on the "
				"movie's blocklist");
			return FALSE;
		}
		return TRUE;
	}
	if (n == 1)
		return axis_movies_store_eventf(conn, facts, "unblocked", user, 0, 0, error, "%s taken off the blocklist%s%s.",
			(const gchar *)g_ptr_array_index(deleted, 0), user != NULL ? " by " : "", user != NULL ? user : "");
	return axis_movies_store_eventf(conn, facts, "unblocked", user, 0, 0, error, "The blocklist cleared%s%s (%u "
		"releases).", user != NULL ? " by " : "", user != NULL ? user : "", n);
}

gboolean
axis_movies_clear_blocklist(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        entry_id,
	const gchar  *user,
	guint        *removed,
	GError      **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && clear_in_transaction(self, conn, movie_id, entry_id, user, removed, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
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
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, words);
	} else if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
		/* An import of the download is placing its files: a conflict (409). */
		g_autofree gchar *busy = g_strdup((*error)->message);

		g_clear_error(error);
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, busy);
	}
	return FALSE;
}

gboolean
axis_movies_block_candidate(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        candidate_id,
	const gchar  *user,
	GError      **error
){
	if (!axis_acquire_block_candidate(self->acquire, movie_id, candidate_id, user, NULL, error))
		return block_failed(error, "that release is not one of the movie's searches'");
	return TRUE;
}

gboolean
axis_movies_block_find(
	AxisMovies             *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab,
	AxisMovieFacts        **facts,
	GError                **error
){
	*facts = NULL;
	if (!axis_acquire_block_grab_find(self->acquire, grab_id, download_id, import_id, grab, error))
		return FALSE;
	if (*grab == NULL) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that download is not one Axis "
			"grabbed for a movie");
		return FALSE;
	}
	if (!axis_movies_get(self, (*grab)->owner_id, facts, error)) {
		g_clear_pointer(grab, axis_acquire_block_grab_free);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_movies_block_grab(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
){
	if (!axis_acquire_block_grab(self->acquire, movie_id, grab_id, user, cancelled, error))
		return block_failed(error, "that download is not one Axis grabbed for this movie");
	return TRUE;
}

/* ── Giving up on a grab ────────────────────────────────────────── */

gboolean
axis_movies_give_up(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	AxisAcquireFailure failure;
	gboolean handled = FALSE;
	gboolean ok;

	if (!axis_movies_get(self, movie_id, &facts, error))
		return FALSE;
	if (facts->grab_id == 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "nothing of it is downloading or "
			"downloaded");
		return FALSE;
	}
	memset(&failure, 0, sizeof(failure));
	failure.close_reason = "given_up";
	failure.block_reason = "given_up";
	failure.text = g_strdup_printf("Given up on%s%s.", user != NULL ? " by " : "", user != NULL ? user : "");
	failure.remove = TRUE;
	failure.retry = TRUE;
	ok = axis_acquire_handle_failure(self->acquire, movie_id, facts->grab_id, &failure, user, TRUE, &handled, NULL,
		error);
	g_free(failure.text);
	if (ok && !handled) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that grab has been dealt with "
			"already");
		return FALSE;
	}
	return ok;
}

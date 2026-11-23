/*
 * axis-indexer-fanout.c - a search of every enabled indexer and source at
 * once, and the merging of what they found
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each one is searched as a search that names it is (its stored search, its
 * own limiter, its own place among the calls at it, its own deadline), each
 * in a thread of its own, so that one that is slow or does not answer costs
 * the others nothing: the whole waits for the last, and none takes longer
 * than its own deadline (with a backstop that cancels what is left).
 *
 * Before anything is asked:
 *  - a native indexer held after saying its limit is reached, or near its
 *    daily allowance for an automatic search (used up for one a person asked
 *    for), is passed over, with the reason in its part;
 *  - a source's indexer that is also configured natively (the same host, by
 *    axis_indexer_host_key(), where the source gives the indexer's address;
 *    else the same name as the native one's, or as its site goes by, "NewsOwl"
 *    for newsowl.example: Prowlarr 3.0.1 gives no address for a private indexer) is
 *    asked natively and not through the source: the source's search names the
 *    others when it can, and what still comes back from that indexer through
 *    the source is left out.  The result says so ("NewsOwl is asked directly,
 *    not through Prowlarr").
 *
 * The merge goes by priority (lower first, then by name): a release whose
 * group (axis_indexer_release_group_key(): a torrent's info-hash, a Usenet
 * release's exact name and size) was already found at another indexer is not
 * listed again but counted as the first one's alternative, with that
 * indexer's name; the fetch tries the alternatives (by the stored group key)
 * when the first cannot be fetched.  Two results of one indexer are not
 * merged: the indexer's own guid tells them apart (the kinds already merged
 * what it gave twice).
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

/* Names an alternative lists, at most. */
#define MAX_ALSO_AT 8

typedef struct
{
	AxisIndexers            *self;
	AxisIndexerSourceInfo   *source;	/* borrowed */
	AxisIndexerRequest       request;	/* its own copy */
	GArray                  *excluded;	/* gint64: a source's indexers asked natively */
	GCancellable            *cancel;	/* shared, borrowed */
	gboolean                 asked;
	gchar                   *passed;	/* why it was passed over */
	AxisIndexerSearchResult *result;
	GError                  *error;
	GThread                 *thread;
	GMutex                  *lock;	/* borrowed */
	GCond                   *cond;	/* borrowed */
	guint                   *n_done;	/* (lock) borrowed */
} Part;

static void
part_clear(Part *part)
{
	g_clear_pointer(&part->excluded, g_array_unref);
	g_free(part->passed);
	axis_indexer_search_result_free(part->result);
	g_clear_error(&part->error);
}

static gint
by_priority(
	gconstpointer a,
	gconstpointer b
){
	const AxisIndexerSourceInfo *x = *(const AxisIndexerSourceInfo *const *)a;
	const AxisIndexerSourceInfo *y = *(const AxisIndexerSourceInfo *const *)b;
	gint names;

	if (x->priority != y->priority)
		return x->priority < y->priority ? -1 : 1;
	names = g_utf8_collate(x->name, y->name);
	if (names != 0)
		return names;
	return x->id < y->id ? -1 : x->id > y->id;
}

static gboolean
in_list(
	const GArray *ids,
	gint64        id
){
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++) {
		if (g_array_index(ids, gint64, i) == id)
			return TRUE;
	}
	return FALSE;
}

/*
 * Remembers a native indexer: by its host key, and by the names it goes by
 * ("#newsowl": a '#' is in no host key): the name its site goes by, and its
 * own name here, both plain (axis_indexer_plain_name()).
 */
static void
note_native(
	GHashTable                  *keys,
	const AxisIndexerSourceInfo *source
){
	g_autofree gchar *site = axis_indexer_site_name(source->host);
	g_autofree gchar *own = axis_indexer_plain_name(source->name);

	if (source->host != NULL)
		g_hash_table_add(keys, g_strdup(source->host));
	if (site != NULL)
		g_hash_table_add(keys, g_strdup_printf("#%s", site));
	if (own[0] != '\0')
		g_hash_table_add(keys, g_strdup_printf("#%s", own));
}

/*
 * Whether @info, an indexer of a source, is a native indexer's site.  By its
 * host when the source says it (Prowlarr's list gives it for some indexers;
 * for a private one, as recorded from Prowlarr 3.0.1, it gives no address at
 * all), else by its name: an indexer called "NewsOwl" is the native indexer
 * called that, or the one at newsowl.example (api.newsowl.example is the same key).
 * An indexer of another name is not told to be one.
 */
static gboolean
is_native_site(
	const AxisIndexerInfo *info,
	GHashTable            *keys
){
	g_autofree gchar *plain = NULL;
	g_autofree gchar *key = NULL;

	if (info->host != NULL)
		return g_hash_table_contains(keys, info->host);
	plain = axis_indexer_plain_name(info->name);
	if (plain[0] == '\0')
		return FALSE;
	key = g_strdup_printf("#%s", plain);
	return g_hash_table_contains(keys, key);
}

/* The indexers of @source that are on and also configured natively. */
static GArray *
asked_natively(
	const AxisIndexerSourceInfo *source,
	GHashTable                  *native_hosts
){
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;

	for (i = 0; source->indexers != NULL && i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);

		if (info->enabled && is_native_site(info, native_hosts))
			g_array_append_val(ids, info->id);
	}
	return ids;
}

/* "12:30 UTC" */
static gchar *
clock_text(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%H:%M UTC") : g_strdup("later");
}

/* Why a native indexer is passed over now, or %NULL. */
static gchar *
why_passed(
	const AxisIndexerSourceInfo *source,
	gboolean                     automatic,
	gint64                       now_us
){
	const AxisIndexerAllowance *a = &source->allowance;

	if (!axis_indexer_source_spent(source, automatic, now_us))
		return NULL;
	if (source->held_until_us > now_us) {
		g_autofree gchar *until = clock_text(source->held_until_us);

		/* One that runs a definition: in the words of its hold's cause. */
		switch (axis_indexer_hold_cause(source, now_us)) {
		case AXIS_INDEXER_HOLD_DRIFT:
		case AXIS_INDEXER_HOLD_CHALLENGE:
			return g_strdup_printf("%s, so searches pass it over until %s", source->hold_message, until);
		case AXIS_INDEXER_HOLD_WAIT:
			return g_strdup_printf("the site asked Axis to wait, so it is not asked again until %s", until);
		default:
			return g_strdup_printf("it said its request limit is reached, so it is not asked again until %s", until);
		}
	}
	if (automatic && a->api_used < a->api_max)
		return g_strdup_printf("it has used %d of its %d API calls today, and the last %d are kept for searches you "
			"ask for", a->api_used, a->api_max, axis_indexer_allowance_reserve(a->api_max));
	return g_strdup_printf("its daily allowance of %d API calls is used up", a->api_max);
}

/* Prepares @part's request for @source: a source's indexers that are asked
 * natively are not named (when the source's search can name indexers),
 * and none left means it is not asked. */
static void
plan_source(
	Part        *part,
	GHashTable  *native_hosts
){
	const AxisIndexerSourceInfo *source = part->source;
	guint remaining = 0;
	gint64 ids[AXIS_INDEXER_MAX_INDEXER_IDS];
	gboolean too_many = FALSE;
	guint i;

	part->excluded = asked_natively(source, native_hosts);
	if (part->excluded->len == 0)
		return;
	for (i = 0; source->indexers != NULL && i < source->indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, i);
		gboolean wanted = part->request.n_indexers == 0;
		guint j;

		if (!info->enabled || in_list(part->excluded, info->id))
			continue;
		for (j = 0; j < part->request.n_indexers; j++)
			wanted = wanted || part->request.indexers[j] == info->id;
		if (!wanted)
			continue;
		if (remaining == AXIS_INDEXER_MAX_INDEXER_IDS) {
			too_many = TRUE;
			continue;
		}
		ids[remaining++] = info->id;
	}
	if (remaining == 0) {
		part->passed = g_strdup("every indexer of it that is on is also set up here, and is asked directly");
		return;
	}
	/* More than a request can name: it is asked for all, and what the
	 * indexers asked natively give through it is left out after. */
	if (!too_many) {
		memcpy(part->request.indexers, ids, sizeof(gint64) * remaining);
		part->request.n_indexers = remaining;
	}
}

static gpointer
part_thread(gpointer data)
{
	Part *part = data;

	axis_indexer_search_source(part->self, part->source->id, &part->request, part->cancel, &part->result,
		&part->error);
	g_mutex_lock(part->lock);
	(*part->n_done)++;
	g_cond_broadcast(part->cond);
	g_mutex_unlock(part->lock);
	return NULL;
}

static void
on_cancel(
	GCancellable *source,
	gpointer      user_data
){
	g_cancellable_cancel(G_CANCELLABLE(user_data));
}

/* Adds @name to @release's alternatives' names, once. */
static void
add_also_at(
	AxisIndexerRelease *release,
	const gchar        *name
){
	guint n = release->also_at != NULL ? g_strv_length(release->also_at) : 0;
	guint i;

	if (name == NULL || n >= MAX_ALSO_AT || g_strcmp0(release->indexer_name, name) == 0)
		return;
	for (i = 0; i < n; i++) {
		if (strcmp(release->also_at[i], name) == 0)
			return;
	}
	release->also_at = g_renew(gchar *, release->also_at, n + 2);
	release->also_at[n] = g_strdup(name);
	release->also_at[n + 1] = NULL;
}

/* The indexers of a source to leave out of its answer after the search: as
 * its list of indexers is now (the search may have read it again). */
static GArray *
excluded_after(
	AxisIndexers *self,
	Part         *part,
	GHashTable   *native_hosts
){
	g_autoptr(AxisIndexerSourceInfo) now = NULL;

	if (part->source->native || g_hash_table_size(native_hosts) == 0)
		return NULL;
	if (!axis_indexers_get_source(self, part->source->id, &now, NULL))
		return part->excluded != NULL ? g_array_ref(part->excluded) : NULL;
	return asked_natively(now, native_hosts);
}

static void
merge(
	AxisIndexers            *self,
	Part                    *parts,
	guint                    n,
	GHashTable              *native_hosts,
	AxisIndexerSearchResult *out
){
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GPtrArray) failed = g_ptr_array_new_with_free_func(g_free);
	gboolean first = TRUE;
	gboolean any_indexers = FALSE;
	guint answered = 0;
	guint n_asked = 0;
	guint i;
	guint k;

	for (i = 0; i < n; i++) {
		if (parts[i].asked)
			n_asked++;
	}
	out->releases = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_release_free);
	out->parts = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_search_part_free);
	out->cached = TRUE;
	out->n_indexers = 0;
	for (i = 0; i < n; i++) {
		Part *part = &parts[i];
		AxisIndexerSearchPart *shown = g_new0(AxisIndexerSearchPart, 1);
		const AxisIndexerSearchResult *r = part->result;
		g_autoptr(GArray) excluded = NULL;

		shown->source_id = part->source->id;
		shown->name = g_strdup(part->source->name);
		shown->kind = g_strdup(part->source->kind);
		shown->native = part->source->native;
		shown->priority = part->source->priority;
		shown->asked = part->asked;
		g_ptr_array_add(out->parts, shown);
		if (!part->asked) {
			shown->problem = g_strdup(part->passed);
			continue;
		}
		/* An indexer with nothing for this kind of search (YTS for TV, EZTV for
		 * a movie) made no request: it is shown as not asked, and is not one
		 * that answered. */
		if (r != NULL && r->not_asked != NULL) {
			shown->asked = FALSE;
			shown->not_asked = g_strdup(r->not_asked);
			continue;
		}
		/* A typed text search goes to every indexer, and one that cannot be
		 * asked by words (EZTV, by IMDb identifier only) has nothing to
		 * say, and nothing was asked of it: it is shown as not asked, as YTS
		 * is for TV.  When it alone is asked (a search that names it), it says
		 * why as an error. */
		if (r == NULL && n_asked > 1 && part->request.kind == AXIS_INDEXER_QUERY_TEXT &&
		    g_error_matches(part->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID)) {
			shown->asked = FALSE;
			shown->not_asked = g_strdup("it cannot be searched by words");
			continue;
		}
		if (r == NULL) {
			shown->problem = g_strdup(part->error != NULL ? part->error->message : "it gave no answer");
			continue;
		}
		answered++;
		shown->mode = r->mode;
		shown->fell_back = r->fell_back;
		shown->also_text = r->also_text;
		shown->cached = r->cached;
		shown->n_releases = r->releases->len;
		if (first) {
			out->mode = r->mode;
			out->fell_back = r->fell_back;
			out->also_text = r->also_text;
			out->created_us = r->created_us;
			out->expires_us = r->expires_us;
			first = FALSE;
		}
		out->cached = out->cached && r->cached;
		out->truncated = out->truncated || r->truncated;
		out->n_off_origin += r->n_off_origin;
		out->n_unusable += r->n_unusable;
		out->n_skipped += r->n_skipped;
		if (r->n_indexers >= 0) {
			out->n_indexers += r->n_indexers;
			any_indexers = TRUE;
		}
		out->created_us = MIN(out->created_us, r->created_us);
		out->expires_us = MIN(out->expires_us, r->expires_us);
		for (k = 0; r->failed != NULL && r->failed[k] != NULL && failed->len < AXIS_INDEXER_MAX_FAILED; k++)
			g_ptr_array_add(failed, g_strdup(r->failed[k]));
		excluded = excluded_after(self, part, native_hosts);
		for (k = 0; k < r->releases->len; k++) {
			const AxisIndexerRelease *release = g_ptr_array_index(r->releases, k);
			g_autofree gchar *group = NULL;
			AxisIndexerRelease *primary;
			AxisIndexerRelease *copy;

			if (excluded != NULL && release->indexer_id > 0 && in_list(excluded, release->indexer_id)) {
				shown->n_releases--;
				continue;
			}
			group = axis_indexer_release_group_key(release);
			primary = group != NULL ? g_hash_table_lookup(groups, group) : NULL;
			/* The same release at another indexer: an alternative. */
			if (primary != NULL && (primary->source_id != part->source->id || primary->indexer_id !=
			    release->indexer_id)) {
				/* A source's indexer is named with the source: "NewsOwl
				 * through Prowlarr". */
				g_autofree gchar *where = part->source->native || release->indexer_name == NULL ?
					g_strdup(release->indexer_name != NULL ? release->indexer_name : part->source->name) :
					g_strdup_printf("%s through %s", release->indexer_name, part->source->name);

				primary->n_alternatives++;
				add_also_at(primary, where);
				out->n_merged++;
				continue;
			}
			if (out->releases->len >= AXIS_INDEXER_MAX_RESULTS) {
				out->truncated = TRUE;
				break;
			}
			copy = axis_indexer_release_copy(release);
			copy->source_id = part->source->id;
			g_ptr_array_add(out->releases, copy);
			if (group != NULL && primary == NULL)
				g_hash_table_insert(groups, g_steal_pointer(&group), copy);
		}
	}
	if (!any_indexers)
		out->n_indexers = -1;
	if (answered == 0)
		out->cached = FALSE;
	if (failed->len > 0) {
		g_ptr_array_add(failed, NULL);
		out->failed = (gchar **)g_ptr_array_steal(failed, NULL);
	}
}

gboolean
axis_indexer_search_all(
	AxisIndexers              *self,
	const AxisIndexerRequest  *request,
	GCancellable              *cancellable,
	AxisIndexerSearchResult  **result,
	GError                   **error
){
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GPtrArray) enabled = g_ptr_array_new();
	g_autoptr(GHashTable) native_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) asked_hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	AxisIndexerSearchResult *out;
	GPtrArray *directly = NULL;
	gint64 now_us = g_get_real_time();
	gulong handler = 0;
	GMutex lock;
	GCond cond;
	guint n_done = 0;
	guint n_asked = 0;
	guint answered = 0;
	Part *parts;
	guint n;
	guint i;

	*result = NULL;
	if (!axis_indexers_list_sources(self, &sources, error))
		return FALSE;
	for (i = 0; i < sources->len; i++) {
		AxisIndexerSourceInfo *source = g_ptr_array_index(sources, i);

		if (!source->enabled)
			continue;
		g_ptr_array_add(enabled, source);
		if (source->native)
			note_native(native_hosts, source);
	}
	if (enabled->len == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE,
			"No indexer is switched on: add one, or connect Prowlarr, under System, Indexers.");
		return FALSE;
	}
	g_ptr_array_sort(enabled, by_priority);

	n = enabled->len;
	parts = g_new0(Part, n);
	g_mutex_init(&lock);
	g_cond_init(&cond);
	for (i = 0; i < n; i++) {
		Part *part = &parts[i];

		part->self = self;
		part->source = g_ptr_array_index(enabled, i);
		part->request = *request;
		part->cancel = cancel;
		part->lock = &lock;
		part->cond = &cond;
		part->n_done = &n_done;
		if (part->source->native)
			part->passed = why_passed(part->source, request->automatic, now_us);
		else
			plan_source(part, native_hosts);
		part->asked = part->passed == NULL;
		if (part->asked)
			n_asked++;
	}

	/* Which indexers a source is not asked for, because the indexer itself is
	 * asked: said on the page, so that two lines of the table that look alike
	 * are not taken for one asked twice. */
	for (i = 0; i < n; i++) {
		if (parts[i].source->native && parts[i].asked)
			note_native(asked_hosts, parts[i].source);
	}
	for (i = 0; i < n; i++) {
		const AxisIndexerSourceInfo *source = parts[i].source;
		guint j;

		for (j = 0; !source->native && parts[i].excluded != NULL && source->indexers != NULL &&
		     j < source->indexers->len; j++) {
			const AxisIndexerInfo *info = g_ptr_array_index(source->indexers, j);

			if (in_list(parts[i].excluded, info->id) && is_native_site(info, asked_hosts)) {
				if (directly == NULL)
					directly = g_ptr_array_new_with_free_func(g_free);
				g_ptr_array_add(directly, g_strdup_printf("%s is asked directly, not through %s", info->name,
					source->name));
			}
		}
	}

	if (cancellable != NULL)
		handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancel), cancel, NULL);
	if (n_asked == 1) {
		/* One: here, as a search that names it. */
		for (i = 0; i < n; i++) {
			if (parts[i].asked)
				part_thread(&parts[i]);
		}
	} else if (n_asked > 1) {
		guint wait_ms = self->tuning.wait_ms != 0 ? self->tuning.wait_ms : AXIS_INDEXER_WAIT_MS;
		guint request_ms = self->tuning.request_timeout_ms != 0 ? self->tuning.request_timeout_ms :
			AXIS_INDEXER_SLOW_TIMEOUT_MS;
		gint64 backstop_us = g_get_monotonic_time() + ((gint64)wait_ms * 2 + (gint64)request_ms * 2 + 5000) * 1000;

		for (i = 0; i < n; i++) {
			if (parts[i].asked)
				parts[i].thread = g_thread_new("indexer-search", part_thread, &parts[i]);
		}
		/* Each part ends by its own deadline; past the backstop, what is
		 * left is cancelled. */
		g_mutex_lock(&lock);
		while (n_done < n_asked) {
			if (!g_cond_wait_until(&cond, &lock, backstop_us)) {
				g_cancellable_cancel(cancel);
				backstop_us = G_MAXINT64;
			}
		}
		g_mutex_unlock(&lock);
		for (i = 0; i < n; i++) {
			if (parts[i].thread != NULL)
				g_thread_join(parts[i].thread);
		}
	}
	if (handler != 0)
		g_cancellable_disconnect(cancellable, handler);

	out = g_new0(AxisIndexerSearchResult, 1);
	merge(self, parts, n, native_hosts, out);
	out->asked_directly = directly;
	for (i = 0; i < n; i++) {
		if (parts[i].result != NULL)
			answered++;
	}
	if (answered == 1 && n_asked == 1) {
		for (i = 0; i < n; i++) {
			if (parts[i].result != NULL) {
				out->search_id = parts[i].result->search_id;
				out->source_id = parts[i].result->source_id;
			}
		}
	}

	if (answered == 0) {
		/* None answered: the first one's error, or why none was asked. */
		gboolean cancelled = g_cancellable_is_cancelled(cancellable) || g_atomic_int_get(&self->stopped);
		Part *first = NULL;
		GString *all = g_string_new(NULL);

		for (i = 0; i < n; i++) {
			const AxisIndexerSearchPart *shown = g_ptr_array_index(out->parts, i);

			if (parts[i].asked && parts[i].error != NULL && shown->problem != NULL && first == NULL)
				first = &parts[i];
			if (shown->problem != NULL)
				g_string_append_printf(all, "%s%s: %s", all->len > 0 ? "; " : "", shown->name, shown->problem);
		}
		if (cancelled) {
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_CANCELLED, "cancelled");
		} else if (first != NULL && n_asked == 1 && n == 1) {
			g_propagate_error(error, g_steal_pointer(&first->error));
		} else if (first != NULL) {
			g_set_error(error, AXIS_INDEXER_ERROR, first->error->code, "%s", all->str);
		} else {
			g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED,
				"No indexer could be asked now: %s", all->str);
		}
		g_string_free(all, TRUE);
		axis_indexer_search_result_free(out);
		out = NULL;
	}
	for (i = 0; i < n; i++)
		part_clear(&parts[i]);
	g_free(parts);
	g_mutex_clear(&lock);
	g_cond_clear(&cond);
	*result = out;
	return out != NULL;
}

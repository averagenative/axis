/*
 * axis-meta-art.c - artwork: fetched once from a provider's image origin,
 * checked, kept in the data directory by its content hash, read back by
 * that hash only, and cleaned up
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "meta/axis-meta-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db/axis-db-result.h"
#include "jobs/axis-job-error.h"
#include "meta/axis-meta-image.h"

/* A file in the artwork directory that the store does not know is removed
 * once it is this old (a fetch between writing it and storing its row is
 * not that slow). */
#define STRAY_AGE_S 3600

/* Refused images are forgotten after this. */
#define REFUSED_KEEP "30 days"

gboolean
axis_meta_artwork_hash_valid(const gchar *hash)
{
	guint i;

	if (hash == NULL)
		return FALSE;
	for (i = 0; i < 64; i++) {
		if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
			return FALSE;
	}
	return hash[64] == '\0';
}

/* Whether the artwork directory has @hash as a regular file. */
static gboolean
file_present(
	AxisMeta    *self,
	const gchar *hash,
	gint64      *size
){
	struct stat st;

	if (fstatat(self->art_fd, hash, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
		return FALSE;
	if (size != NULL)
		*size = (gint64)st.st_size;
	return TRUE;
}

/* ── Finding ────────────────────────────────────────────────────── */

/* The stored row of @ref: its hash (or %NULL for a refused or absent one),
 * whether there is a row, and whether a refusal is still believed. */
static gboolean
stored_row(
	AxisMeta     *self,
	AxisDbConn   *conn,
	const gchar  *provider,
	const gchar  *ref,
	const gchar  *size,
	gchar       **hash,
	gboolean     *refused_fresh,
	GError      **error
){
	AxisMetaFreshness freshness;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *negative = NULL;
	const gchar *params[4];

	*hash = NULL;
	*refused_fresh = FALSE;
	axis_meta_get_freshness(self, &freshness);
	negative = g_strdup_printf("%" G_GINT64_FORMAT, freshness.negative);
	params[0] = provider;
	params[1] = ref;
	params[2] = size;
	params[3] = negative;
	if (!axis_db_conn_exec(conn, "SELECT sha256, sha256 IS NULL AND fetched_at > now() - make_interval(secs =>"
	        " $4::double precision) FROM meta_artwork WHERE provider = $1 AND source_ref = $2 AND size = $3", 4, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	if (!axis_db_result_is_null(result, 0, 0))
		*hash = g_strdup(axis_db_result_get_text(result, 0, 0));
	axis_db_result_get_boolean(result, 0, 1, refused_fresh);
	return TRUE;
}

/* Kept as used: written at most hourly. */
static void
touch(
	AxisDbConn  *conn,
	const gchar *hash
){
	const gchar *params[1];

	params[0] = hash;
	(void)axis_db_conn_exec(conn, "UPDATE meta_artwork SET last_used_at = now() WHERE sha256 = $1"
		" AND last_used_at < now() - interval '1 hour'", 1, params, NULL, NULL);
}

/* Asks for @ref to be fetched, in the background: queued, nobody waits.
 * @began_us: when the caller began looking (before it read the store), so
 * that a fetch answered for another caller since is not made again. */
static void
queue_fetch(
	AxisMeta              *self,
	AxisMetaProviderEntry *entry,
	const gchar           *ref,
	AxisMetaArtSize        size,
	gint64                 began_us
){
	AxisMetaPending *pending = axis_meta_pending_new(AXIS_META_ASK_ART, entry);

	pending->began_us = began_us;
	pending->key = g_strdup_printf("art:%s:%s:%s", entry->info->name, axis_meta_art_size_to_string(size), ref);
	pending->text = g_strdup(ref);
	pending->size = size;
	axis_meta_ask(self, pending, 0, NULL);
}

gchar *
axis_meta_artwork_find(
	AxisMeta        *self,
	const gchar     *provider,
	const gchar     *ref,
	AxisMetaArtSize  size,
	gboolean         fetch
){
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, provider);
	const gchar *size_name = axis_meta_art_size_to_string(size);
	gint64 began = g_get_monotonic_time();
	g_autoptr(GError) error = NULL;
	gchar *hash = NULL;
	gboolean refused = FALSE;
	AxisDbConn *conn;

	if (self->art_fd < 0 || entry == NULL || size_name == NULL || !axis_meta_image_ref_valid(ref))
		return NULL;
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL)
		return NULL;
	if (!stored_row(self, conn, entry->info->name, ref, size_name, &hash, &refused, &error)) {
		axis_db_pool_release(self->pool, conn);
		g_debug("artwork: cannot read the store: %s", error->message);
		return NULL;
	}
	if (hash != NULL && file_present(self, hash, NULL))
		touch(conn, hash);
	else
		g_clear_pointer(&hash, g_free);
	axis_db_pool_release(self->pool, conn);
	if (hash == NULL && !refused && fetch)
		queue_fetch(self, entry, ref, size, began);
	return hash;
}

gchar *
axis_meta_artwork_find_shown(
	AxisMeta        *self,
	const gchar     *provider,
	const gchar     *ref,
	AxisMetaArtSize  size,
	gboolean         fetch,
	gboolean        *refused
){
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, provider);
	const gchar *size_name = axis_meta_art_size_to_string(size);
	gint64 began = g_get_monotonic_time();
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) shown = NULL;
	gchar *hash = NULL;
	gboolean refused_fresh = FALSE;
	gboolean is_shown = FALSE;
	const gchar *params[2];
	AxisDbConn *conn;

	if (refused != NULL)
		*refused = FALSE;
	if (self->art_fd < 0 || entry == NULL || size_name == NULL || !axis_meta_image_ref_valid(ref))
		return NULL;
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL)
		return NULL;
	if (!stored_row(self, conn, entry->info->name, ref, size_name, &hash, &refused_fresh, &error)) {
		axis_db_pool_release(self->pool, conn);
		g_debug("artwork: cannot read the store: %s", error->message);
		return NULL;
	}
	if (hash != NULL && file_present(self, hash, NULL)) {
		touch(conn, hash);
		axis_db_pool_release(self->pool, conn);
		return hash;
	}
	g_clear_pointer(&hash, g_free);
	if (refused != NULL)
		*refused = refused_fresh;
	if (refused_fresh || !fetch) {
		axis_db_pool_release(self->pool, conn);
		return NULL;
	}
	/* Only what a stored title shows: what the cleanup keeps, and nothing a
	 * request merely names. */
	params[0] = entry->info->name;
	params[1] = ref;
	if (axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM meta_title WHERE provider = $1 AND (poster_ref = $2"
	        " OR backdrop_ref = $2))", 2, params, &shown, &error))
		axis_db_result_get_boolean(shown, 0, 0, &is_shown);
	else
		g_debug("artwork: cannot read the store: %s", error->message);
	axis_db_pool_release(self->pool, conn);
	if (is_shown)
		queue_fetch(self, entry, ref, size, began);
	return NULL;
}

/* "{"a","b"}": a text[] of @items, each quoted. */
static gchar *
text_array(GPtrArray *items)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; i < items->len; i++) {
		const gchar *p;

		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, '"');
		for (p = g_ptr_array_index(items, i); *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

void
axis_meta_artwork_find_many(
	AxisMeta           *self,
	const gchar *const *providers,
	const gchar *const *refs,
	guint               n,
	AxisMetaArtSize     size,
	gboolean            fetch,
	gchar             **hashes
){
	const gchar *size_name = axis_meta_art_size_to_string(size);
	gint64 began = g_get_monotonic_time();
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_autoptr(GPtrArray) wanted = g_ptr_array_new();
	g_autoptr(GArray) at = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autoptr(GPtrArray) used = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gboolean *refused = NULL;
	g_autofree gchar *names_text = NULL;
	g_autofree gchar *refs_text = NULL;
	g_autofree gchar *negative = NULL;
	AxisMetaFreshness freshness;
	const gchar *params[4];
	AxisDbConn *conn;
	guint i;

	for (i = 0; i < n; i++)
		hashes[i] = NULL;
	if (n == 0 || self->art_fd < 0 || size_name == NULL)
		return;
	/* The titles whose provider and reference can have a stored image. */
	for (i = 0; i < n; i++) {
		AxisMetaProviderEntry *entry = axis_meta_entry_find(self, providers[i]);

		if (entry == NULL || !axis_meta_image_ref_valid(refs[i]))
			continue;
		g_ptr_array_add(names, (gpointer)entry->info->name);
		g_ptr_array_add(wanted, (gpointer)refs[i]);
		g_array_append_val(at, i);
	}
	if (at->len == 0)
		return;
	refused = g_new0(gboolean, n);
	names_text = text_array(names);
	refs_text = text_array(wanted);
	axis_meta_get_freshness(self, &freshness);
	negative = g_strdup_printf("%" G_GINT64_FORMAT, freshness.negative);
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL) {
		g_debug("artwork: cannot read the store: %s", error->message);
		return;
	}
	params[0] = names_text;
	params[1] = refs_text;
	params[2] = size_name;
	params[3] = negative;
	if (!axis_db_conn_exec(conn, "SELECT k.n, a.sha256, a.sha256 IS NULL AND a.fetched_at > now() - make_interval(secs =>"
	        " $4::double precision) FROM unnest($1::text[], $2::text[]) WITH ORDINALITY AS k (provider, ref, n)"
	        " JOIN meta_artwork a ON a.provider = k.provider AND a.source_ref = k.ref AND a.size = $3", 4, params,
	        &result, &error)) {
		axis_db_pool_release(self->pool, conn);
		g_debug("artwork: cannot read the store: %s", error->message);
		return;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 k = 0;
		guint index;

		if (!axis_db_result_get_int64(result, i, 0, &k) || k < 1 || (guint64)k > at->len)
			continue;
		index = g_array_index(at, guint, k - 1);
		axis_db_result_get_boolean(result, i, 2, &refused[index]);
		if (axis_db_result_is_null(result, i, 1) || hashes[index] != NULL)
			continue;
		if (file_present(self, axis_db_result_get_text(result, i, 1), NULL)) {
			hashes[index] = g_strdup(axis_db_result_get_text(result, i, 1));
			g_ptr_array_add(used, hashes[index]);
		}
	}
	/* Kept as used, all at once: written at most hourly. */
	if (used->len > 0) {
		g_autofree gchar *used_text = text_array(used);

		params[0] = used_text;
		(void)axis_db_conn_exec(conn, "UPDATE meta_artwork SET last_used_at = now() WHERE sha256 = ANY ($1::text[])"
			" AND last_used_at < now() - interval '1 hour'", 1, params, NULL, NULL);
	}
	axis_db_pool_release(self->pool, conn);
	for (i = 0; fetch && i < at->len; i++) {
		guint index = g_array_index(at, guint, i);

		if (hashes[index] == NULL && !refused[index])
			queue_fetch(self, axis_meta_entry_find(self, providers[index]), refs[index], size, began);
	}
}

/* ── Fetching and keeping ───────────────────────────────────────── */

/* Why @body is not kept, or %NULL when it is: the type its bytes show, the
 * type it was sent as, its size and its dimensions. */
static gchar *
check_image(
	GBytes            *body,
	const gchar       *declared,
	AxisMetaImageInfo *info
){
	gsize length = 0;
	const guint8 *data = g_bytes_get_data(body, &length);
	AxisMetaImageType claimed;

	if (length == 0)
		return g_strdup("it is empty");
	if (length > AXIS_META_ART_MAX_BYTES)
		return g_strdup("it is larger than the limit");
	if (!axis_meta_image_sniff(data, length, info))
		return g_strdup("it is not a JPEG, PNG or WebP image");
	if (declared != NULL) {
		claimed = axis_meta_image_type_from_mime(declared);
		if (claimed != info->type)
			return g_strdup_printf("it was sent as another type than the %s it is",
				axis_meta_image_type_to_mime(info->type));
	}
	if (info->width > AXIS_META_ART_MAX_SIDE || info->height > AXIS_META_ART_MAX_SIDE ||
	    (guint64)info->width * info->height > AXIS_META_ART_MAX_PIXELS)
		return g_strdup_printf("it is %u by %u pixels, beyond the limits", info->width, info->height);
	return NULL;
}

static gboolean
record_refusal(
	AxisMeta    *self,
	const gchar *provider,
	const gchar *ref,
	const gchar *size,
	const gchar *problem
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, &error);
	const gchar *params[4];
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	params[0] = provider;
	params[1] = ref;
	params[2] = size;
	params[3] = problem;
	ok = axis_db_conn_exec(conn, "INSERT INTO meta_artwork (provider, source_ref, size, problem)"
		" VALUES ($1, $2, $3, $4) ON CONFLICT (provider, source_ref, size) DO UPDATE SET sha256 = NULL,"
		" bytes = NULL, content_type = NULL, width = NULL, height = NULL, problem = EXCLUDED.problem,"
		" fetched_at = now(), last_used_at = now()", 4, params, NULL, &error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* Writes @body to a new temporary file beneath the artwork directory. */
static gchar *
write_temporary(
	AxisMeta  *self,
	GBytes    *body,
	GError   **error
){
	gsize length = 0;
	const guint8 *data = g_bytes_get_data(body, &length);
	gchar *name = g_strdup_printf(".tmp-%08x%08x", g_random_int(), g_random_int());
	gint fd = openat(self->art_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	gsize done = 0;
	gint saved;

	if (fd < 0) {
		saved = errno;
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "artwork cannot be written to the data "
			"directory (%s)", g_strerror(saved));
		g_free(name);
		return NULL;
	}
	while (done < length) {
		gssize n = write(fd, data + done, length - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (gsize)n;
	}
	saved = errno;
	if (done != length || fsync(fd) != 0) {
		if (done == length)
			saved = errno;
		close(fd);
		unlinkat(self->art_fd, name, 0);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "artwork cannot be written to the data "
			"directory (%s)", g_strerror(saved));
		g_free(name);
		return NULL;
	}
	if (close(fd) != 0) {
		saved = errno;
		unlinkat(self->art_fd, name, 0);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "artwork cannot be written to the data "
			"directory (%s)", g_strerror(saved));
		g_free(name);
		return NULL;
	}
	return name;
}

/* Moves the temporary file into place and stores the row, under the lock
 * the cleanup takes too. */
static gboolean
keep(
	AxisMeta                *self,
	const gchar             *provider,
	const gchar             *ref,
	const gchar             *size,
	const gchar             *temporary,
	const gchar             *hash,
	gsize                    bytes,
	const AxisMetaImageInfo *info,
	GError                 **error
){
	g_autofree gchar *lock = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_META_ART_LOCK);
	g_autofree gchar *bytes_text = g_strdup_printf("%" G_GSIZE_FORMAT, bytes);
	g_autofree gchar *width = g_strdup_printf("%u", info->width);
	g_autofree gchar *height = g_strdup_printf("%u", info->height);
	g_autoptr(GError) local = NULL;
	const gchar *params[8];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, &local);
	gint saved;

	if (conn == NULL) {
		unlinkat(self->art_fd, temporary, 0);
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	params[0] = lock;
	if (!axis_db_conn_begin(conn, &local) ||
	    !axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock($1::bigint)", 1, params, NULL, &local))
		goto failed;
	if (renameat(self->art_fd, temporary, self->art_fd, hash) != 0) {
		saved = errno;
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		unlinkat(self->art_fd, temporary, 0);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "artwork cannot be kept in the data "
			"directory (%s)", g_strerror(saved));
		return FALSE;
	}
	params[0] = provider;
	params[1] = ref;
	params[2] = size;
	params[3] = hash;
	params[4] = bytes_text;
	params[5] = axis_meta_image_type_to_mime(info->type);
	params[6] = width;
	params[7] = height;
	if (!axis_db_conn_exec(conn, "INSERT INTO meta_artwork (provider, source_ref, size, sha256, bytes, content_type,"
	        " width, height) VALUES ($1, $2, $3, $4, $5::bigint, $6, $7::integer, $8::integer)"
	        " ON CONFLICT (provider, source_ref, size) DO UPDATE SET sha256 = EXCLUDED.sha256, bytes = EXCLUDED.bytes,"
	        " content_type = EXCLUDED.content_type, width = EXCLUDED.width, height = EXCLUDED.height, problem = NULL,"
	        " fetched_at = now(), last_used_at = now()", 8, params, NULL, &local) ||
	    !axis_db_conn_commit(conn, &local))
		goto failed;
	axis_db_pool_release(self->pool, conn);
	return TRUE;
failed:
	/* A file already moved into place is a stray the cleanup removes. */
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	unlinkat(self->art_fd, temporary, 0);
	axis_meta_set_failed(error, local);
	return FALSE;
}

gboolean
axis_meta_art_fetch_now(
	AxisMeta         *self,
	AxisMetaFetch    *fetch,
	const gchar      *ref,
	AxisMetaArtSize   size,
	gchar           **hash_out,
	GError          **error
){
	const AxisMetaProviderInfo *info = fetch->provider->info;
	const gchar *size_name = axis_meta_art_size_to_string(size);
	g_autofree gchar *path = NULL;
	g_autofree gchar *declared = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *temporary = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) local = NULL;
	AxisMetaImageInfo image;

	if (hash_out != NULL)
		*hash_out = NULL;
	if (self->art_fd < 0) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED, "artwork is not kept without a data "
			"directory");
		return FALSE;
	}
	if (size_name == NULL || !axis_meta_image_ref_valid(ref)) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "that is not an image reference");
		return FALSE;
	}
	path = info->image_path(info, ref, size);
	if (path == NULL) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED, "the provider has no such image");
		return FALSE;
	}
	if (!axis_meta_fetch_image(fetch, path, &body, &declared, &local)) {
		if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND)) {
			record_refusal(self, info->name, ref, size_name, "the provider has no such image");
			g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED, "the provider has no such image");
			return FALSE;
		}
		if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED)) {
			g_info("artwork from %s refused: %s", info->label, local->message);
			record_refusal(self, info->name, ref, size_name, local->message);
			g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED, "the image was not kept: %s", local->message);
			return FALSE;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	problem = check_image(body, declared, &image);
	if (problem != NULL) {
		g_info("artwork from %s refused: %s", info->label, problem);
		record_refusal(self, info->name, ref, size_name, problem);
		g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED, "the image was not kept: %s", problem);
		return FALSE;
	}
	hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
	temporary = write_temporary(self, body, error);
	if (temporary == NULL)
		return FALSE;
	if (!keep(self, info->name, ref, size_name, temporary, hash, g_bytes_get_size(body), &image, error))
		return FALSE;
	if (hash_out != NULL)
		*hash_out = g_steal_pointer(&hash);
	return TRUE;
}

gboolean
axis_meta_artwork_fetch(
	AxisMeta         *self,
	const gchar      *provider,
	const gchar      *ref,
	AxisMetaArtSize   size,
	gchar           **hash,
	GError          **error
){
	AxisMetaProviderEntry *entry = axis_meta_entry_find(self, provider);
	const gchar *size_name = axis_meta_art_size_to_string(size);
	g_autofree gchar *stored = NULL;
	g_autoptr(GError) local = NULL;
	AxisMetaFetch fetch;
	gboolean refused = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	if (hash != NULL)
		*hash = NULL;
	if (entry == NULL || size_name == NULL || !axis_meta_image_ref_valid(ref)) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID, "that is not an image of a provider");
		return FALSE;
	}
	if (self->art_fd >= 0) {
		conn = axis_db_pool_acquire(self->pool, &local);
		if (conn == NULL) {
			axis_meta_set_failed(error, local);
			return FALSE;
		}
		ok = stored_row(self, conn, entry->info->name, ref, size_name, &stored, &refused, &local);
		axis_db_pool_release(self->pool, conn);
		if (!ok) {
			axis_meta_set_failed(error, local);
			return FALSE;
		}
		if (stored != NULL && file_present(self, stored, NULL)) {
			if (hash != NULL)
				*hash = g_steal_pointer(&stored);
			return TRUE;
		}
	}
	memset(&fetch, 0, sizeof(fetch));
	fetch.meta = self;
	fetch.provider = entry;
	fetch.cancellable = self->cancel;
	fetch.deadline_us = g_get_monotonic_time() +
		(gint64)(self->tuning.background_ms != 0 ? self->tuning.background_ms : AXIS_META_BACKGROUND_MS) * 1000;
	return axis_meta_art_fetch_now(self, &fetch, ref, size, hash, error);
}

/* ── Reading ────────────────────────────────────────────────────── */

gboolean
axis_meta_artwork_read(
	AxisMeta     *self,
	const gchar  *hash,
	GBytes      **data,
	gchar       **content_type,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	struct stat st;
	gint64 bytes = 0;
	guint8 *buffer;
	gsize done = 0;
	gint fd;

	*data = NULL;
	*content_type = NULL;
	if (!axis_meta_artwork_hash_valid(hash) || self->art_fd < 0) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such image");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL) {
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	params[0] = hash;
	if (!axis_db_conn_exec(conn, "SELECT content_type, bytes FROM meta_artwork WHERE sha256 = $1 LIMIT 1", 1, params,
	        &result, &local)) {
		axis_db_pool_release(self->pool, conn);
		axis_meta_set_failed(error, local);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(result) == 0 || !axis_db_result_get_int64(result, 0, 1, &bytes)) {
		axis_db_pool_release(self->pool, conn);
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such image");
		return FALSE;
	}
	touch(conn, hash);
	axis_db_pool_release(self->pool, conn);

	/* The name is the validated hash and nothing else. */
	fd = openat(self->art_fd, hash, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such image");
		return FALSE;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (gint64)st.st_size != bytes || bytes <= 0 ||
	    bytes > AXIS_META_ART_MAX_BYTES) {
		close(fd);
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such image");
		return FALSE;
	}
	buffer = g_malloc((gsize)bytes);
	while (done < (gsize)bytes) {
		gssize n = read(fd, buffer + done, (gsize)bytes - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += (gsize)n;
	}
	close(fd);
	if (done != (gsize)bytes) {
		g_free(buffer);
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such image");
		return FALSE;
	}
	*data = g_bytes_new_take(buffer, done);
	*content_type = g_strdup(axis_db_result_get_text(result, 0, 0));
	return TRUE;
}

/* ── Cleaning up ────────────────────────────────────────────────── */

static gboolean
cancelled(
	AxisJobContext  *context,
	GError         **error
){
	if (context != NULL && axis_job_context_is_cancelled(context)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
		return TRUE;
	}
	return FALSE;
}

/* Collects the hashes a DELETE ... RETURNING sha256 gave. */
static void
collect(
	AxisDbResult *result,
	GHashTable   *hashes,
	guint        *count
){
	guint i;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *hash = axis_db_result_get_text(result, i, 0);

		if (hash != NULL)
			g_hash_table_add(hashes, g_strdup(hash));
	}
	if (count != NULL)
		*count += axis_db_result_get_n_rows(result);
}

static gboolean
cleanup_rows(
	AxisMeta               *self,
	AxisDbConn             *conn,
	AxisMetaCleanupReport  *report,
	GHashTable             *known,
	GError                **error
){
	g_autofree gchar *lock = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_META_ART_LOCK);
	g_autofree gchar *cap = NULL;
	g_autoptr(GHashTable) dropped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(AxisDbResult) unreferenced = NULL;
	g_autoptr(AxisDbResult) over = NULL;
	g_autoptr(AxisDbResult) present = NULL;
	GHashTableIter iter;
	gpointer key;
	const gchar *params[1];
	guint i;

	cap = g_strdup_printf("%" G_GINT64_FORMAT, self->settings != NULL &&
		axis_settings_lookup(self->settings, AXIS_META_SETTING_ARTWORK_CAP, NULL) != NULL ?
		axis_settings_get_byte_size(self->settings, AXIS_META_SETTING_ARTWORK_CAP) :
		G_GINT64_CONSTANT(1024) * 1024 * 1024);
	params[0] = lock;
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock($1::bigint)", 1, params, NULL, error))
		return FALSE;
	/* What no stored title shows any more. */
	if (!axis_db_conn_exec(conn, "DELETE FROM meta_artwork a WHERE NOT EXISTS (SELECT 1 FROM meta_title t"
	        " WHERE t.provider = a.provider AND (t.poster_ref = a.source_ref OR t.backdrop_ref = a.source_ref))"
	        " RETURNING sha256", 0, NULL, &unreferenced, error))
		return FALSE;
	collect(unreferenced, dropped, &report->unreferenced);
	if (!axis_db_conn_exec(conn, "DELETE FROM meta_artwork WHERE sha256 IS NULL AND fetched_at < now() - interval '"
	        REFUSED_KEEP "'", 0, NULL, NULL, error))
		return FALSE;
	/* The cap: the most recently used are kept, up to it. */
	params[0] = cap;
	if (!axis_db_conn_exec(conn, "WITH h AS (SELECT sha256, max(bytes) AS bytes, max(last_used_at) AS used"
	        "   FROM meta_artwork WHERE sha256 IS NOT NULL GROUP BY sha256),"
	        " r AS (SELECT sha256, sum(bytes) OVER (ORDER BY used DESC, sha256) AS running FROM h)"
	        " DELETE FROM meta_artwork WHERE sha256 IN (SELECT sha256 FROM r WHERE running > $1::bigint)"
	        " RETURNING sha256", 1, params, &over, error))
		return FALSE;
	{
		g_autoptr(GHashTable) distinct = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		collect(over, distinct, NULL);
		report->over_cap += g_hash_table_size(distinct);
		collect(over, dropped, NULL);
	}

	/* What is left, and whether each file is there. */
	if (!axis_db_conn_exec(conn, "SELECT DISTINCT sha256 FROM meta_artwork WHERE sha256 IS NOT NULL", 0, NULL,
	        &present, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(present); i++) {
		const gchar *hash = axis_db_result_get_text(present, i, 0);

		if (!axis_meta_artwork_hash_valid(hash))
			continue;
		if (file_present(self, hash, NULL)) {
			g_hash_table_add(known, g_strdup(hash));
			continue;
		}
		params[0] = hash;
		if (!axis_db_conn_exec(conn, "DELETE FROM meta_artwork WHERE sha256 = $1", 1, params, NULL, error))
			return FALSE;
		report->missing++;
	}
	/* A file goes when no row names it any more. */
	g_hash_table_iter_init(&iter, dropped);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (g_hash_table_contains(known, key) || !axis_meta_artwork_hash_valid(key))
			continue;
		if (unlinkat(self->art_fd, key, 0) == 0)
			report->files_removed++;
	}
	return TRUE;
}

/* Files of the artwork directory nothing knows of, old enough. */
static void
cleanup_strays(
	AxisMeta              *self,
	GHashTable            *known,
	AxisMetaCleanupReport *report
){
	gint fd = openat(self->art_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	struct dirent *entry;
	DIR *dir;

	if (fd < 0)
		return;
	dir = fdopendir(fd);
	if (dir == NULL) {
		close(fd);
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		const gchar *name = entry->d_name;
		struct stat st;

		if (!axis_meta_artwork_hash_valid(name) && !g_str_has_prefix(name, ".tmp-"))
			continue;
		if (axis_meta_artwork_hash_valid(name) && g_hash_table_contains(known, name))
			continue;
		if (fstatat(self->art_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (now - (gint64)st.st_mtime < STRAY_AGE_S)
			continue;
		if (unlinkat(self->art_fd, name, 0) == 0)
			report->strays_removed++;
	}
	closedir(dir);
}

gboolean
axis_meta_run_cleanup(
	AxisMeta               *self,
	AxisJobContext         *context,
	AxisMetaCleanupReport  *report_out,
	GError                **error
){
	AxisMetaCleanupReport report;
	g_autoptr(GHashTable) known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(AxisDbResult) kept = NULL;
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;

	memset(&report, 0, sizeof(report));
	if (report_out != NULL)
		memset(report_out, 0, sizeof(*report_out));
	if (self->art_fd < 0)
		return TRUE;
	if (cancelled(context, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, &local) || !cleanup_rows(self, conn, &report, known, &local) ||
	    !axis_db_conn_commit(conn, &local)) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (context != NULL)
		axis_job_context_set_progress(context, 0.5, "looking for files nothing refers to");
	cleanup_strays(self, known, &report);
	if (axis_db_conn_exec(conn, "SELECT coalesce(sum(bytes), 0) FROM (SELECT DISTINCT sha256, bytes FROM meta_artwork"
	        " WHERE sha256 IS NOT NULL) AS d", 0, NULL, &kept, NULL)) {
		gint64 bytes = 0;

		if (axis_db_result_get_int64(kept, 0, 0, &bytes) && bytes > 0)
			report.bytes_kept = (guint64)bytes;
	}
	axis_db_pool_release(self->pool, conn);
	if (report.unreferenced + report.over_cap + report.missing + report.strays_removed > 0)
		g_info("artwork cleanup: %u no longer shown, %u over the cap, %u missing, %u files removed, %u strays",
			report.unreferenced, report.over_cap, report.missing, report.files_removed, report.strays_removed);
	if (report_out != NULL)
		*report_out = report;
	return TRUE;
}

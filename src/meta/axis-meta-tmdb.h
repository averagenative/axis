/*
 * axis-meta-tmdb.h - TMDb (The Movie Database) as a metadata provider for
 * films and series
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Needs the user's own TMDb "API Read Access Token" (Settings, Metadata),
 * sent as "Authorization: Bearer <token>" to https://api.themoviedb.org and
 * to nothing else.  The token never goes in a URL; the short version 3 "API
 * Key" is not used, and a value that looks like it is refused when it is
 * saved.  Posters and backdrops come from https://image.tmdb.org, without
 * the token, at three fixed sizes and only for a file name of a strict form.
 * Both origins are constants: no answer, setting or environment variable
 * changes them (tests copy the provider's info and point the copy at fake
 * servers on loopback).
 *
 * Calls, as few as can be:
 *   search       /3/search/movie, /3/search/tv
 *   a film       /3/movie/{id} with alternative_titles, external_ids and
 *                release_dates appended
 *   a series     /3/tv/{id} with alternative_titles and external_ids
 *                appended; /3/tv/{id}/season/{n} per season asked for
 *   by an id     /3/find/{id}?external_source=imdb_id or tvdb_id
 *
 * Settings (metadata.tmdb_*): key, rate (twenty a second), language
 * (en-US), region (US; whose release dates a film's date is taken from),
 * adult (off).  What the answers become: axis-meta-tmdb-parse.h.
 *
 * Not here: people and credits, collections, trailers, ratings, watch
 * providers, lists.
 */

#ifndef AXIS_META_TMDB_H
#define AXIS_META_TMDB_H

#include <glib.h>

#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

#define AXIS_META_TMDB_API_ORIGIN    "https://api.themoviedb.org"
#define AXIS_META_TMDB_IMAGE_ORIGIN  "https://image.tmdb.org"

#define AXIS_META_TMDB_SETTING_KEY       "metadata.tmdb_key"
#define AXIS_META_TMDB_SETTING_RATE      "metadata.tmdb_rate"
#define AXIS_META_TMDB_SETTING_LANGUAGE  "metadata.tmdb_language"
#define AXIS_META_TMDB_SETTING_REGION    "metadata.tmdb_region"
#define AXIS_META_TMDB_SETTING_ADULT     "metadata.tmdb_adult"

/* The notice TMDb's terms require wherever its data is shown. */
#define AXIS_META_TMDB_ATTRIBUTION \
	"This product uses the TMDb API but is not endorsed or certified by TMDb."

/**
 * axis_meta_tmdb_get_info:
 *
 * Returns: (transfer none): the provider, to give to axis_meta_add_provider();
 *   it lives as long as the program
 */
const AxisMetaProviderInfo *
axis_meta_tmdb_get_info(void);

G_END_DECLS

#endif /* AXIS_META_TMDB_H */

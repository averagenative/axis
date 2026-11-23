/*
 * axis-youtube-address.c - which addresses the YouTube downloader takes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "youtube/axis-youtube-address.h"

#include <string.h>

#define SCHEME "https://"

static gboolean
id_char(gchar c)
{
	return g_ascii_isalnum(c) || c == '-' || c == '_';
}

gboolean
axis_youtube_video_id_valid(const gchar *id)
{
	guint i;

	if (id == NULL)
		return FALSE;
	for (i = 0; i < AXIS_YOUTUBE_ID_LENGTH; i++) {
		if (!id_char(id[i]))
			return FALSE;
	}
	return id[AXIS_YOUTUBE_ID_LENGTH] == '\0';
}

gchar *
axis_youtube_address_for(const gchar *video_id)
{
	g_return_val_if_fail(axis_youtube_video_id_valid(video_id), NULL);

	return g_strconcat(AXIS_YOUTUBE_WATCH_PREFIX, video_id, NULL);
}

static gboolean
refuse(
	const gchar **problem,
	const gchar  *why
){
	if (problem != NULL)
		*problem = why;
	return FALSE;
}

/* The id at @text, which must end there or at one of @enders. */
static gboolean
id_at(
	const gchar *text,
	const gchar *enders,
	gchar       *id
){
	guint i;

	for (i = 0; i < AXIS_YOUTUBE_ID_LENGTH; i++) {
		if (!id_char(text[i]))
			return FALSE;
		id[i] = text[i];
	}
	id[AXIS_YOUTUBE_ID_LENGTH] = '\0';
	return text[AXIS_YOUTUBE_ID_LENGTH] == '\0' || strchr(enders, text[AXIS_YOUTUBE_ID_LENGTH]) != NULL;
}

/* The value of the query parameter "v" in @query (after the '?', up to a
 * '#'): the first one. */
static gboolean
query_video(
	const gchar *query,
	gchar       *id
){
	const gchar *p = query;

	while (*p != '\0' && *p != '#') {
		const gchar *end = p;

		while (*end != '\0' && *end != '&' && *end != '#')
			end++;
		if (end - p >= 2 && p[0] == 'v' && p[1] == '=')
			return id_at(p + 2, "&#", id);
		p = *end == '&' ? end + 1 : end;
	}
	return FALSE;
}

static gboolean
path_is(
	const gchar *path,
	const gchar *prefix
){
	gsize n = strlen(prefix);

	return strncmp(path, prefix, n) == 0;
}

gboolean
axis_youtube_address_parse(
	const gchar  *text,
	gchar       **video_id,
	const gchar **problem
){
	static const gchar *const hosts[] = { "www.youtube.com", "youtube.com", "m.youtube.com", "music.youtube.com" };
	g_autofree gchar *copy = NULL;
	g_autofree gchar *host = NULL;
	gchar id[AXIS_YOUTUBE_ID_LENGTH + 1];
	const gchar *rest;
	const gchar *path;
	const gchar *p;
	gboolean short_host;
	gboolean known = FALSE;
	gsize host_length;
	guint i;

	if (video_id != NULL)
		*video_id = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (text == NULL)
		return refuse(problem, "Paste the address of a YouTube video.");
	/* A paste often carries a space or a line end: those around it go. */
	if (strlen(text) > AXIS_YOUTUBE_ADDRESS_MAX + 64)
		return refuse(problem, "That address is too long to be a YouTube video's.");
	copy = g_strstrip(g_strdup(text));
	if (copy[0] == '\0')
		return refuse(problem, "Paste the address of a YouTube video.");
	if (strlen(copy) > AXIS_YOUTUBE_ADDRESS_MAX)
		return refuse(problem, "That address is too long to be a YouTube video's.");
	for (p = copy; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return refuse(problem, "An address has no spaces and only plain characters. Copy it from the browser's "
				"address bar.");
	}
	if (strncmp(copy, SCHEME, strlen(SCHEME)) != 0)
		return refuse(problem, "The address must begin with https://.");
	rest = copy + strlen(SCHEME);
	host_length = strcspn(rest, "/?#");
	if (host_length == 0 || host_length > 64)
		return refuse(problem, "That is not a YouTube address.");
	host = g_ascii_strdown(rest, (gssize)host_length);
	/* No user name and no port: a host name only. */
	if (strchr(host, '@') != NULL || strchr(host, ':') != NULL)
		return refuse(problem, "That is not a YouTube address.");
	short_host = strcmp(host, "youtu.be") == 0;
	for (i = 0; i < G_N_ELEMENTS(hosts); i++)
		known = known || strcmp(host, hosts[i]) == 0;
	if (!short_host && !known)
		return refuse(problem, "Only addresses at youtube.com and youtu.be are taken.");
	path = rest + host_length;

	if (short_host) {
		if (path[0] != '/' || !id_at(path + 1, "?#", id))
			return refuse(problem, "That address has no video in it.");
	} else if (path_is(path, "/playlist")) {
		return refuse(problem, "That is a playlist. This version downloads one video at a time: paste a video's "
			"address.");
	} else if (path_is(path, "/@") || path_is(path, "/channel/") || path_is(path, "/c/") || path_is(path, "/user/")) {
		return refuse(problem, "That is a channel. This version downloads one video at a time: paste a video's "
			"address.");
	} else if (path_is(path, "/watch?")) {
		if (!query_video(path + strlen("/watch?"), id))
			return refuse(problem, "That address has no video in it.");
	} else if (path_is(path, "/shorts/") || path_is(path, "/live/") || path_is(path, "/embed/")) {
		const gchar *at = strchr(path + 1, '/') + 1;

		if (!id_at(at, "?#", id))
			return refuse(problem, "That address has no video in it.");
	} else {
		return refuse(problem, "That address has no video in it.");
	}
	if (video_id != NULL)
		*video_id = g_strdup(id);
	return TRUE;
}

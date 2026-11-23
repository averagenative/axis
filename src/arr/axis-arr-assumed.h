/*
 * axis-arr-assumed.h - what Axis assumes about the HTTP API (version 3) of
 * Radarr and Sonarr, and what a running instance has confirmed of it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every fact below was written from knowledge of the version 3 API
 * (the numbers A1 and on are those of the design's table of assumptions,
 * kept here in full).  Each now says how it stands:
 *
 *   CONFIRMED (Radarr 6.4.4)   seen in a session against a running Radarr of
 *                              that version (2026-11-21, the shapes of the
 *                              answers only: no title, path or host of it is
 *                              kept anywhere);
 *   CONFIRMED (Sonarr 4.0.20)  seen in a session against a running Sonarr of
 *                              that version (2026-11-21, shapes only, as for
 *                              Radarr);
 *   PARTLY CONFIRMED           with what is still open;
 *   TO CONFIRM                 not seen yet (Radarr 3 to 5, Sonarr 3).
 *
 * No other file holds an assumption about the arr apps: the
 * client (axis-arr-client.c), the parser (axis-arr-parse.c) and the reading
 * (axis-arr-read.c) take each from here, and the tests' stand-ins
 * (tests/axis-test-radarr.c, tests/axis-test-sonarr.c, tests/fixtures/arr/)
 * answer as these say.  Sonarr's are A9 (its half), A11 to A15 and A19; the
 * session against Sonarr 4.0.20 disagreed with none of them, and each says
 * what is still open.
 * When the session shows otherwise, change it here and in the fixtures.
 *
 * Not assumptions (Axis's own limits, in axis-arr-parse.h and axis-arr.h):
 * how much of an answer is read, how many titles, pages and characters.
 */

#ifndef AXIS_ARR_ASSUMED_H
#define AXIS_ARR_ASSUMED_H

/* A1. Both apps serve their API under this path, below their address and
 * optional URL base (Radarr 3 to 6, Sonarr 3 and 4), and take the key in
 * this request header.  (They also take "apikey" in the query; Axis never
 * uses that form.)
 * CONFIRMED (Radarr 6.4.4, Sonarr 4.0.20) for the path and the header, at an
 * address without a URL base.  TO CONFIRM: an address with one. */
#define AXIS_ARR_API_PREFIX          "/api/v3"
#define AXIS_ARR_KEY_HEADER          "X-Api-Key"

/* A2. GET system/status answers an object naming the app and its version;
 * a wrong key answers 401 (a 403 is taken the same way).
 * CONFIRMED (Radarr 6.4.4): "appName" is "Radarr", "version" a string, a
 * wrong key 401.  (The answer also has "urlBase", "" when unset; not read.)
 * CONFIRMED (Sonarr 4.0.20): "appName" is "Sonarr", "version" a string, a
 * wrong key 401. */
#define AXIS_ARR_STATUS_PATH         AXIS_ARR_API_PREFIX "/system/status"
#define AXIS_ARR_STATUS_APP          "appName"
#define AXIS_ARR_STATUS_VERSION      "version"
#define AXIS_ARR_APP_NAME_RADARR     "Radarr"
#define AXIS_ARR_APP_NAME_SONARR     "Sonarr"
#define AXIS_ARR_STATUS_KEY_REFUSED  401
#define AXIS_ARR_STATUS_KEY_REFUSED2 403

/* A3. GET movie answers one array of every movie, unpaged, each an object
 * with these members.  "tmdbId" is a number, 0 or absent for none; "imdbId"
 * a string ("tt" and digits) or absent.
 * CONFIRMED (Radarr 6.4.4): one unpaged array; "id", "tmdbId",
 * "qualityProfileId" and "year" numbers, "monitored" and "hasFile" booleans,
 * "title", "minimumAvailability", "path" and "rootFolderPath" strings;
 * "imdbId" a string that is absent or null on some movies (which leaves a
 * movie whole: Axis matches by "tmdbId").  STILL OPEN: what "tmdbId" is for
 * a movie Radarr could not identify (none was seen; 0, null and absent are
 * all taken as none). */
#define AXIS_ARR_MOVIES_PATH         AXIS_ARR_API_PREFIX "/movie"
#define AXIS_ARR_MOVIE_ID            "id"
#define AXIS_ARR_MOVIE_TITLE         "title"
#define AXIS_ARR_MOVIE_YEAR          "year"
#define AXIS_ARR_MOVIE_TMDB          "tmdbId"
#define AXIS_ARR_MOVIE_IMDB          "imdbId"
#define AXIS_ARR_MOVIE_MONITORED     "monitored"
#define AXIS_ARR_MOVIE_AVAILABILITY  "minimumAvailability"
#define AXIS_ARR_MOVIE_PROFILE       "qualityProfileId"
#define AXIS_ARR_MOVIE_PATH          "path"
#define AXIS_ARR_MOVIE_ROOT          "rootFolderPath"
#define AXIS_ARR_MOVIE_HAS_FILE      "hasFile"

/* A4. A movie with a file carries it in that same answer, as an object
 * with the file's absolute path (as Radarr's own container sees it), its
 * size in bytes, its quality's name two objects down, and the release name
 * it came from.  (If it does not, GET moviefile?movieId=N is assumed to:
 * Axis does not ask it yet, and reports such a file as one Radarr gives no
 * path for.)
 * CONFIRMED (Radarr 6.4.4): every movie with "hasFile" carried "movieFile"
 * in the list; "path" absolute, "relativePath" a string, "size" a number,
 * "quality" an object holding "quality" (with "name") and "revision";
 * "sceneName" a string that is absent on some files.  The fallback request
 * was not needed and is TO CONFIRM. */
#define AXIS_ARR_MOVIE_FILE          "movieFile"
#define AXIS_ARR_FILE_PATH           "path"
#define AXIS_ARR_FILE_RELATIVE       "relativePath"
#define AXIS_ARR_FILE_SIZE           "size"
#define AXIS_ARR_FILE_QUALITY        "quality"
#define AXIS_ARR_FILE_QUALITY_INNER  "quality"
#define AXIS_ARR_FILE_QUALITY_NAME   "name"
#define AXIS_ARR_FILE_SCENE_NAME     "sceneName"
#define AXIS_ARR_MOVIE_FILE_PATH     AXIS_ARR_API_PREFIX "/moviefile?movieId=%" G_GINT64_FORMAT

/* A5. GET qualityprofile answers an array of objects with a number and a
 * name.
 * CONFIRMED (Radarr 6.4.4, Sonarr 4.0.20). */
#define AXIS_ARR_PROFILES_PATH       AXIS_ARR_API_PREFIX "/qualityprofile"
#define AXIS_ARR_PROFILE_ID          "id"
#define AXIS_ARR_PROFILE_NAME        "name"

/* A6. GET rootfolder answers an array of objects with a number, an absolute
 * path and whether the app can reach it.
 * CONFIRMED (Radarr 6.4.4): "path" without a trailing slash, "accessible" a
 * boolean (beside "freeSpace" and "unmappedFolders", not read).
 * CONFIRMED (Sonarr 4.0.20): the same. */
#define AXIS_ARR_FOLDERS_PATH        AXIS_ARR_API_PREFIX "/rootfolder"
#define AXIS_ARR_FOLDER_ID           "id"
#define AXIS_ARR_FOLDER_PATH         "path"
#define AXIS_ARR_FOLDER_ACCESSIBLE   "accessible"

/* A7. A movie's minimum availability is one of these words ("tba" and
 * "preDB" in older data), each with the availability Axis takes it as
 * (movie.min_availability).  Any other word gives Axis's default.
 * PARTLY CONFIRMED (Radarr 6.4.4): "announced" and "released" were seen,
 * and "inCinemas" is a value of that version.  "tba" and "preDB" were not
 * seen. */
#define AXIS_ARR_AVAILABILITY_TABLE(X) \
	X("announced", "announced") \
	X("inCinemas", "in_cinemas") \
	X("released",  "released") \
	X("tba",       "announced") \
	X("preDB",     "released")

/* A8. Quality names are the fixed set the apps ship, each with the quality
 * of Axis's ladder (quality/axis-quality-ladder.c) it is taken as.  A name
 * that is not here ("Unknown", "REGIONAL", "DVD-R", "Raw-HD", a custom one)
 * gives no quality from the arr app: the file's own name is read instead
 * (the file and its movie are kept either way: an unknown quality is never
 * a reason to skip).
 * CONFIRMED (Radarr 6.4.4) for the spelling of the 23 names a library of
 * 2,424 files held: BR-DISK, Bluray-480p, -576p, -720p, -1080p and -2160p,
 * CAM, DVD, DVD-R, HDTV-1080p, Remux-1080p and -2160p, SDTV, TELESYNC,
 * Unknown, WEBDL-480p, -720p, -1080p and -2160p, WEBRip-480p, -720p, -1080p
 * and -2160p.  All but "DVD-R" and "Unknown" are in the table.  The other
 * names of the table were not seen.  Which of Axis's qualities each is taken
 * as is Axis's choice, not a fact of the API.
 * CONFIRMED (Sonarr 4.0.20) for the five names its episode files held:
 * Bluray-1080p, Bluray-720p, HDTV-720p, WEBDL-1080p and SDTV, all in the
 * table. */
#define AXIS_ARR_QUALITY_TABLE(X) \
	X("WORKPRINT",    "workprint") \
	X("CAM",          "cam") \
	X("TELESYNC",     "telesync") \
	X("TELECINE",     "telecine") \
	X("DVDSCR",       "screener") \
	X("SDTV",         "sdtv") \
	X("DVD",          "dvd") \
	X("WEBDL-480p",   "web_480p") \
	X("WEBRip-480p",  "web_480p") \
	X("Bluray-480p",  "bluray_480p") \
	X("Bluray-576p",  "bluray_480p") \
	X("HDTV-720p",    "hdtv_720p") \
	X("WEBRip-720p",  "webrip_720p") \
	X("WEBDL-720p",   "webdl_720p") \
	X("Bluray-720p",  "bluray_720p") \
	X("HDTV-1080p",   "hdtv_1080p") \
	X("WEBRip-1080p", "webrip_1080p") \
	X("WEBDL-1080p",  "webdl_1080p") \
	X("Bluray-1080p", "bluray_1080p") \
	X("Remux-1080p",  "remux_1080p") \
	X("HDTV-2160p",   "hdtv_2160p") \
	X("WEBRip-2160p", "webrip_2160p") \
	X("WEBDL-2160p",  "webdl_2160p") \
	X("Bluray-2160p", "bluray_2160p") \
	X("Remux-2160p",  "remux_2160p") \
	X("BR-DISK",      "disc")

/* A9. GET queue is paged: the page (from 1) and its size go in the query,
 * and the answer is an object with the count of all records and this page's
 * records, each naming its movie (Radarr) by number.  The three "include"
 * members set to false keep a record small.  (Sonarr's half is below.)
 * PARTLY CONFIRMED (Radarr 6.4.4): the request with its three "include"
 * members is accepted, and the answer has "page", "pageSize",
 * "totalRecords" and "records".  STILL OPEN: a record's "movieId" (the
 * queue was empty).  A record without one is passed over, so a wrong name
 * here marks no movie as downloading and breaks nothing. */
#define AXIS_ARR_QUEUE_PATH          AXIS_ARR_API_PREFIX "/queue?page=%u&pageSize=%u" \
                                     "&includeMovie=false&includeSeries=false&includeEpisode=false"
#define AXIS_ARR_PAGE_TOTAL          "totalRecords"
#define AXIS_ARR_PAGE_RECORDS        "records"
#define AXIS_ARR_QUEUE_MOVIE         "movieId"

/* A9, Sonarr's half.  The same request without the member that is Radarr's,
 * and the same paged answer; a record names its series by number ("seriesId")
 * and its episode ("episodeId", not read: Axis notes only that Sonarr was
 * downloading something of the series).  A record without "seriesId" is
 * passed over, so a wrong name here marks no series as downloading.
 * PARTLY CONFIRMED (Sonarr 4.0.20): the request with "includeSeries" and
 * "includeEpisode" is accepted (200), and the answer has "page", "pageSize"
 * and "totalRecords" as numbers.  STILL OPEN (the queue was empty): a
 * record's "seriesId" as a number; whether a season pack is one record or
 * one for each episode (either reads the same here). */
#define AXIS_ARR_QUEUE_PATH_SONARR   AXIS_ARR_API_PREFIX "/queue?page=%u&pageSize=%u" \
                                     "&includeSeries=false&includeEpisode=false"
#define AXIS_ARR_QUEUE_SERIES        "seriesId"

/* A10. GET blocklist?page=N&pageSize=M answers the same paged shape, a
 * record with "movieId" or "seriesId", "sourceTitle", "date", "protocol"
 * ("usenet", "torrent"), "indexer", "message", and no torrent hash.
 * NOT USED: nothing asks for it, and nothing will.  Decided on 2026-11-21
 * (the proposal's question 5): no blocklist is brought
 * over (a person blocks a release in Axis as they need to), and the slices
 * that would have read it (R4, S4) were removed.  Kept here as what was
 * learned of the endpoint.
 * PARTLY CONFIRMED (Radarr 6.4.4): the paged shape; a record with "id" and
 * "movieId" numbers, "sourceTitle", "date", "protocol", "indexer" and
 * "message" strings (beside "languages", "customFormats", "quality" and
 * "movie"), and no hash on the record seen.  STILL OPEN: the two words of
 * "protocol", and whether a torrent's record has a hash. */
#define AXIS_ARR_BLOCKLIST_PATH      AXIS_ARR_API_PREFIX "/blocklist?page=%u&pageSize=%u"

/* A11. (Sonarr.)  GET series answers one array of every series, unpaged,
 * each an object with these members.  "tvdbId" is a number, 0 or absent for
 * none; "imdbId" a string or absent; "seriesType" one of the three words
 * below; "seasons" an array of objects, each with its number (0 is the
 * specials) and whether Sonarr monitors it.  ("seasonFolder", "statistics",
 * "images", "tags" and the rest are not read: Axis always keeps a series in
 * season folders, and maps no folder below the series' own.)
 * CONFIRMED (Sonarr 4.0.20): one unpaged array; "id", "tvdbId",
 * "qualityProfileId" and "year" numbers, "monitored" a boolean, "title",
 * "seriesType", "path", "rootFolderPath" (without a trailing slash) and
 * "imdbId" strings; every series carries "seasons", each entry with
 * "seasonNumber" (a number) and "monitored" (a boolean); "standard" and
 * "anime" seen as "seriesType".  (Sonarr 4 also gives a series a numeric
 * "tmdbId"; it is not read: design.md, "As built: slice S1", says why.)
 * STILL OPEN: what "tvdbId" is for a series Sonarr could not identify (none
 * was seen; 0, null and absent are all taken as none); the word "daily" (not
 * seen; no word is taken as "standard", and a word that is none of the three
 * is a kind Axis does not know, whose series is skipped); Sonarr 3.
 * "anime" is brought as any series and makes the series of the kind anime in
 * Axis (add-anime-support, A1); "daily" changes nothing. */
#define AXIS_ARR_SERIES_PATH         AXIS_ARR_API_PREFIX "/series"
#define AXIS_ARR_SERIES_ID           "id"
#define AXIS_ARR_SERIES_TITLE        "title"
#define AXIS_ARR_SERIES_YEAR         "year"
#define AXIS_ARR_SERIES_TVDB         "tvdbId"
#define AXIS_ARR_SERIES_IMDB         "imdbId"
#define AXIS_ARR_SERIES_MONITORED    "monitored"
#define AXIS_ARR_SERIES_TYPE         "seriesType"
#define AXIS_ARR_SERIES_PROFILE      "qualityProfileId"
#define AXIS_ARR_SERIES_PATH_MEMBER  "path"
#define AXIS_ARR_SERIES_ROOT         "rootFolderPath"
#define AXIS_ARR_SERIES_SEASONS      "seasons"
#define AXIS_ARR_SEASON_NUMBER       "seasonNumber"
#define AXIS_ARR_SEASON_MONITORED    "monitored"
#define AXIS_ARR_SERIES_TYPE_STANDARD "standard"
#define AXIS_ARR_SERIES_TYPE_DAILY   "daily"
#define AXIS_ARR_SERIES_TYPE_ANIME   "anime"

/* A12. (Sonarr.)  GET episode?seriesId=N answers one array of every episode
 * of that series, unpaged, each an object with these members.
 * "episodeFileId" is the number of the file that holds the episode, 0 or
 * absent for none ("hasFile" says the same and is not read); "airDate" is
 * the day it aired, "YYYY-MM-DD", absent for an episode without one;
 * "tvdbId" is 0 or absent for an episode TheTVDB gave no id for.
 * CONFIRMED (Sonarr 4.0.20): one unpaged array (a series of 1,491 episodes
 * came as one); "id", "seasonNumber", "episodeNumber", "episodeFileId" and
 * "tvdbId" numbers (beside "absoluteEpisodeNumber": A20), "monitored" a
 * boolean, "airDate" a string of that form and absent on some episodes;
 * "episodeFileId" is 0, never null, for an episode without a file; a series
 * number Sonarr does not have answers 200 with an empty array.  STILL OPEN:
 * the "tvdbId" of an episode TheTVDB has no id for (none was seen; 0, null
 * and absent are all taken as none). */
#define AXIS_ARR_EPISODES_PATH       AXIS_ARR_API_PREFIX "/episode?seriesId=%" G_GINT64_FORMAT
#define AXIS_ARR_EPISODE_ID          "id"
#define AXIS_ARR_EPISODE_TVDB        "tvdbId"
#define AXIS_ARR_EPISODE_SEASON      "seasonNumber"
#define AXIS_ARR_EPISODE_NUMBER      "episodeNumber"
#define AXIS_ARR_EPISODE_AIR_DATE    "airDate"
#define AXIS_ARR_EPISODE_MONITORED   "monitored"
#define AXIS_ARR_EPISODE_FILE        "episodeFileId"

/* A20. (Sonarr.)  Each entry of GET episode?seriesId=N may carry
 * "absoluteEpisodeNumber": Sonarr's own absolute number of the episode, a
 * whole number from 1; absent, null or 0 for none (a special, or a series
 * Sonarr numbers no episode of absolutely).  Axis reads it for one purpose:
 * to compare it with the number TheTVDB gives the same episode, and to report
 * where the two differ (add-anime-support, A1).  It is never used to match,
 * link, monitor or hold anything, so a value of another form (a string, a
 * fraction, a number outside 1 to 1,000,000) is no number and costs the entry
 * nothing else.
 * PARTLY CONFIRMED (Sonarr 4.0.20): the member was seen in the session of
 * 2026-11-21, a number on every episode looked at.  STILL OPEN: what an
 * episode without one carries (absent, null and 0 are all taken as none);
 * whether a standard series' episodes carry it too (it is read for every
 * series and compared only for one Sonarr has as anime); Sonarr 3. */
#define AXIS_ARR_EPISODE_ABSOLUTE    "absoluteEpisodeNumber"

/* A13. (Sonarr.)  Sonarr 4 gives a series this member, one of the two words;
 * Sonarr 3 has no such member, and a series whose highest season is monitored
 * is then taken to monitor new seasons (slice S2 decides that from the
 * seasons; the reading keeps only what was said).
 * PARTLY CONFIRMED (Sonarr 4.0.20): the member is a string, "all" on every
 * series seen.  STILL OPEN: the word "none" (not seen; any word but "all"
 * and "none" is kept as not said); Sonarr 3, that it is absent. */
#define AXIS_ARR_SERIES_MONITOR_NEW  "monitorNewItems"
#define AXIS_ARR_MONITOR_NEW_ALL     "all"
#define AXIS_ARR_MONITOR_NEW_NONE    "none"

/* A14. (Sonarr.)  GET episodefile?seriesId=N answers one array of every
 * episode file of that series, unpaged, each an object with its number, and
 * the members a movie's file has (A4: "path" absolute as Sonarr's own
 * container sees it, "size" in bytes, the quality's name two objects down,
 * "sceneName").  A file's number on several episodes (A12, "episodeFileId")
 * is a multi-episode file: it is one record here.
 * CONFIRMED (Sonarr 4.0.20): one unpaged array; "id" and "size" numbers
 * (beside "seriesId" and "seasonNumber"), "path" an absolute path (beside
 * "relativePath", not read), "quality" an object holding "quality" with
 * "name", "sceneName" a string; a file two episodes share is named by the
 * same "episodeFileId" on both.  (Whether "sceneName" is absent on some
 * files, as with Radarr, was not noted; an absent one is no release name.) */
#define AXIS_ARR_EPISODE_FILES_PATH  AXIS_ARR_API_PREFIX "/episodefile?seriesId=%" G_GINT64_FORMAT
#define AXIS_ARR_EPISODE_FILE_ID     "id"

/* A15. (Sonarr.)  Sonarr's season and episode numbers, and an episode's
 * "tvdbId", are TheTVDB's aired order and TheTVDB's episode id, the id Axis
 * stores as series_episode.provider_id under that order.  So a series from
 * Sonarr is matched by its "tvdbId" (A11) and would follow TheTVDB's aired
 * order in Axis (the source of numbering "tvdb").  Nothing in slice S1 reads
 * an episode's number against TheTVDB's: slice S2 does, when it makes the
 * episodes.
 * TO CONFIRM (not checked in the session of Sonarr 4.0.20: it needs a series
 * known to differ): for a series whose TMDb and TheTVDB numbering differ,
 * that Sonarr's seasons and numbers are TheTVDB's aired ones, and that an
 * episode's "tvdbId" is the id TheTVDB's own API gives that episode. */

/* A19. (Sonarr.)  The size of Sonarr's answers: a series' record in the list
 * is a few kilobytes (it carries its seasons with their statistics), so the
 * limit of the list (AXIS_ARR_LIST_MAX_BYTES) holds some tens of thousands of
 * series; an episode's record is about a kilobyte and an episode file's one
 * or two (it carries its media information), so one series' two lists stay
 * below AXIS_ARR_SERIES_MAX_BYTES (32 MB) even for a daily show of some ten
 * thousand episodes, and a library of 30,000 episode files is some tens of
 * megabytes in all, far below AXIS_ARR_SERIES_TOTAL_BYTES.
 * CONFIRMED (Sonarr 4.0.20): 207 series were a body of 1.1 MB (about 5 KB a
 * series, the largest record 16 KB, some 130 times below
 * AXIS_ARR_MAX_ELEMENT_BYTES), so the list's limit holds about 24,000
 * series; the episodes of a series of 1,491 were 755 KB (about half a
 * kilobyte an episode), 40 times below one list's limit.  STILL OPEN: the
 * size of an episode-file list (not measured). */

/* A16. A movie's path lies below its root folder's in the usual case, with
 * this separator on Linux and in containers.  An arr app on Windows gives
 * backslash paths with a drive letter, which are not absolute paths here:
 * the first version reports them as paths that cannot be mapped.
 * CONFIRMED (Radarr 6.4.4, in a container): absolute paths with "/", a root
 * folder's path without a trailing slash.  Windows was not seen. */
#define AXIS_ARR_PATH_SEPARATOR      '/'

/* A17. The size of the movie answer: a few kilobytes a movie, so Axis's
 * limit for it (AXIS_ARR_LIST_MAX_BYTES, 128 MB) holds about 20,000
 * movies; and one movie's record stays far below
 * AXIS_ARR_MAX_ELEMENT_BYTES.
 * CONFIRMED (Radarr 6.4.4): 2,483 movies were a body of 15.5 MB (about
 * 6.2 KB a movie), the largest record 10.8 KB.  At that rate 20,000 movies
 * are about 125 MB, just under the limit, which is reached at about
 * 20,000; a larger library is refused with a sentence saying it is too
 * large for this version.  One record is some 190 times below its own limit
 * of 2 MiB. */

/* A18. The apps do not limit the rate of API requests from one client; the
 * pace Axis keeps for Sonarr's two requests a series is a courtesy.
 * PARTLY CONFIRMED (Sonarr 4.0.20): twenty requests in a row were all
 * answered 200, in 220 ms together, without slowing.  STILL OPEN: Radarr;
 * thousands in a row. */
#define AXIS_ARR_REQUESTS_PER_SECOND 4

#endif /* AXIS_ARR_ASSUMED_H */

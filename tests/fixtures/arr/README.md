# Radarr answers, written by hand

These files are not recordings. Nobody ran Axis against a Radarr while they
were written: each has the shape `src/arr/axis-arr-assumed.h` assumes for the
version 3 API, with made-up titles, identifiers, paths and names. When a
session against a running Radarr corrects an assumption, correct the header
and these files together.

| File | Answer of | What it holds |
|---|---|---|
| `radarr-status.json` | `GET /api/v3/system/status` | The app's name and version |
| `radarr-qualityprofile.json` | `GET /api/v3/qualityprofile` | Two profiles, and three entries out of shape |
| `radarr-rootfolder.json` | `GET /api/v3/rootfolder` | Two root folders, and three that cannot be used |
| `radarr-queue.json` | `GET /api/v3/queue` | One page: one record with a movie, two without |
| `radarr-movies.json` | `GET /api/v3/movie` | Twelve entries, most of them odd on purpose |

`radarr-movies.json`, entry by entry:

1. "Paper Lanterns" (2011): monitored, with a file, and the members a real
   record carries that Axis never copies.
2. "The Quiet Orchard" (2004): not monitored, no file.
3. "Salt Meridian" (2019): `tmdbId` 0, so Radarr has no TMDb identifier for it.
4. "Glass Harbour" (1987): no `id`, so the record is incomplete.
5. "Nine Winters" (2022): `tmdbId` is a string, so the record is incomplete.
6. The number 42 where a movie should be.
7. `null` where a movie should be.
8. "Ash and Ember": a tab and a control character in the title, a year out of
   range, an availability word Axis does not know, and a file record with no
   path and a size that is not a number.
9. "Tin Sparrow" (2001): paths with a `..` segment.
10. "Low Tide Radio" (2015): Windows paths, and a quality name of no table.
11. An empty title, so the record is incomplete.
12. "Hollow Compass" (1999): `monitored` is a string, so the record is
    incomplete.

## Sonarr answers, written by hand

These are not recordings either, and nobody has yet run Axis against a
Sonarr: every member is as `src/arr/axis-arr-assumed.h` assumes it (A9, A11
to A14), each marked there as to confirm.

| File | Answer of | What it holds |
|---|---|---|
| `sonarr-status.json` | `GET /api/v3/system/status` | The app's name and version |
| `sonarr-queue.json` | `GET /api/v3/queue` | One page: one record with a series, two without |
| `sonarr-series.json` | `GET /api/v3/series` | Eleven entries, most of them odd on purpose |
| `sonarr-episode.json` | `GET /api/v3/episode?seriesId=1` | Nine entries: six episodes and three that cannot be read |
| `sonarr-episodefile.json` | `GET /api/v3/episodefile?seriesId=1` | Five entries: three files and two that cannot be read |

`sonarr-series.json`, entry by entry:

1. "Gullwing Ferry" (2025): monitored, the specials and season 2 not
   monitored, season 1 monitored, and the members a real record carries that
   Axis never copies.
2. "The Morning Tide Report" (2019): not monitored, a daily show, and
   `monitorNewItems` is `none`.
3. "Paper Crane Academy" (2014): `seriesType` is `anime`, and there is no
   `monitorNewItems` (as with Sonarr 3).
4. "Salt Meridian Nights" (2021): `tvdbId` 0, and a series type Axis does not
   know.
5. "Harbour Lights After Dark" (1998): `tvdbId` is `null`.
6. "Glass Harbour Diaries" (1987): no `id`, so the record is incomplete.
7. "Nine Winters On" (2022): `tvdbId` is a string, so the record is
   incomplete.
8. "Tin Sparrow Road" (2001): no `seasons`, so the record is incomplete.
9. "Low Tide Radio Hour" (2015): a season whose number is a string, so the
   record is incomplete.
10. "Hollow Compass Club" (1999): season 1 twice, so the record is incomplete.
11. The number 42 where a series should be.

`sonarr-episode.json` holds a double episode (two episodes name file 502), an
episode without a file, a special without a day or a TheTVDB id, a day that
is no day (February 31), an entry without a season, one whose number is a
string, and a string where an episode should be. Its entries also carry
`absoluteEpisodeNumber` in each form Axis must read as a number or as none:
1, 0, absent, the string `"4"`, `null` and 1000001 (out of range).
`sonarr-episodefile.json`
holds a file without a path whose size is a string, an entry without its
number, and `null`.

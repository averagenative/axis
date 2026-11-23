# Discover fixtures

These files stand in for TMDb's version 3 list answers in the Discover tests (`tests/test-discover-unit.c`, `tests/test-discover.c`, `tests/test-web-discover.c`). The fake TMDb in `tests/axis-test-discover.c` serves them by path: `/3/movie/popular` is `movie-popular.json`, its page 2 is `movie-popular-page2.json`, `/3/trending/tv/week` is `trending-tv-week.json`, and so on.

Every title, overview and person in them is made up. None was recorded from the real service: they are written by hand from TMDb's published API documentation, so the first run against TMDb may need corrections.

## What is believed, not verified

- A list answer is an object with `page`, `results`, `total_pages` and `total_results`. The "now playing" and "upcoming" answers also carry `dates`, which Axis does not read.
- A movie entry has `id` (a number), `title`, `original_title`, `release_date` (`YYYY-MM-DD`), `poster_path`, `backdrop_path`, `overview`, `original_language`, `genre_ids` (numbers), `vote_average` (a number from 0 to 10), `vote_count`, `popularity` and `adult`. A series entry has `name`, `original_name` and `first_air_date` in place of the movie's names and date.
- Trending answers name each entry's `media_type` (`movie`, `tv` or `person`); the other lists may or may not. Axis drops an entry whose `media_type` is not the kind asked for.
- A title nobody has rated has `vote_average` 0 and `vote_count` 0.
- `total_pages` can be far larger than Axis asks for; TMDb serves at most 500 pages.
- The genre answers (`/3/genre/movie/list`, `/3/genre/tv/list`) are an object with `genres`, a list of `id` and `name`.
- The movie lists take `region`, which decides which country's cinemas "now playing" and "upcoming" are about; trending and recommendations do not take it.
- TMDb has no list of upcoming series. `/3/tv/on_the_air` lists the series with an episode in the next seven days.
- A recommendations answer has the same form as the other lists.

The files `list-wrong-types.json` and `list-no-results.json` are not TMDb's at all: they hold fields of the wrong type and a missing list, for the parser's refusals.

# TheTVDB fixtures

Hand-written answers in the shape of TheTVDB's API version 4, as its public
documentation describes it and as
`src/meta/axis-meta-tvdb-assumed.h` assumes where the documentation is
silent. None was recorded from the real service: the add-metadata-tvdb change
had no key. Every title, identifier and date is made up: the series is
*Wobbly Lighthouse* (TheTVDB 81001, TMDb 7051, IMDb tt9000081), with a second
series (81002) for searches.

| File | Answer of |
|---|---|
| `login.json` | `POST /v4/login` |
| `search-series.json` | `GET /v4/search?query=...&type=series` |
| `series-81001.json` | `GET /v4/series/81001/extended?meta=translations&short=true` |
| `episodes-81001-official-0.json` to `-2.json` | the three pages of `GET /v4/series/81001/episodes/official/eng?page=N` |
| `remoteid-imdb.json` | `GET /v4/search/remoteid/tt9000081` |
| `seasons-types.json` | `GET /v4/seasons/types` |
| `failure.json` | an answer that says it failed |

The aired order holds what the motivating case needs: a season 2 of
half-hour episodes (24 minutes) where TMDb has none, and episodes dated
where TMDb has no date.

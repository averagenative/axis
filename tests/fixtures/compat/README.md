# Media server fixtures

Answers of a fake Plex Media Server, for the client compatibility tests.
Their *structure* (which objects, which fields, which types) is what a
Plex Media Server 1.43.5 returned to a read-only survey on 2026-11-12:
`GET /identity`, `/library/sections`, `/devices`, `/accounts`,
`/status/sessions` (empty, and with one session whose audio was converted
and whose video was passed through), `/status/sessions/history/all` and
`/library/metadata/<ratingKey>`. Every *value* is invented: no name,
title, address, identifier, path or token of the real server is kept in
this repository.

The fake server (tests/axis-test-plex.c) replaces `@AGE:<seconds>@` with
the Unix time that many seconds ago, so that the history is as old as a
test wants it whatever day it runs on (the placeholder sits inside a JSON
string, which the reader accepts as a number, so the files stay valid JSON).

| File | Stands for |
|---|---|
| `identity.json` | `GET /identity` |
| `sections.json` | `GET /library/sections`: a movie library, a show library with two folders, a music library |
| `devices.json` | `GET /devices` |
| `accounts.json` | `GET /accounts` |
| `history-1.json`, `history-2.json` | two pages of `GET /status/sessions/history/all` (the second is short, so it is the last; one play is by a device `/devices` does not list, one is older than the default retention) |
| `sessions-empty.json` | `GET /status/sessions` with nothing playing |
| `session-converted-audio.json` | the observed session: video passed through (`copy`), audio converted (`transcode`), over the wide-area network |
| `item-film.json` | `GET /library/metadata/<ratingKey>`: the item's file path |

The shapes that were **not** observed are in `constructed/`, with their own
README: they are unverified.

# Deluge fixtures: hand-written, not recorded

No Deluge was available to record these.  They were written by hand from
Deluge's published web JSON-RPC API (`<base>/json`, method names as in
Deluge's own web UI and its documentation) and from memory of Deluge 2.x.  The
first run of Axis against a real instance is expected to find corrections: see
the list of details that are believed but not verified below, in the openspec
change `add-downloader-deluge` and in `tests/test-download-deluge.c`.

Every answer is the envelope `{"result": ..., "error": null | {"message",
"code"}, "id": N}`.  The files all end in `"id": 1}`; the fake Deluge in the
test replaces that 1 with the id of the request it answers (the parser tests
read them as they are, with the id 1).

| file | what it stands for |
| --- | --- |
| `login-ok.json`, `login-wrong.json` | `auth.login` with the right and a wrong password (`true` / `false`) |
| `connected-true.json`, `connected-false.json` | `web.connected` |
| `hosts.json`, `hosts-empty.json`, `hosts-wrong-types.json` | `web.get_hosts`: a list of `[id, host, port, user]` |
| `plugins.json`, `plugins-no-label.json` | `core.get_enabled_plugins` with and without `Label` |
| `labels.json`, `labels-no-axis.json` | `label.get_labels` with and without Axis's label |
| `version.json` | `daemon.get_version` |
| `add-ok.json`, `add-upper.json` | `core.add_torrent_file` / `core.add_torrent_magnet`: the info-hash, lower and upper case |
| `add-null.json`, `add-not-a-hash.json` | an add that answers no hash, or something that is not one |
| `add-duplicate.json` | an add of a torrent Deluge already has (an error) |
| `remove-true.json`, `remove-false.json` | `core.remove_torrent` |
| `remove-not-in-session.json` | `core.remove_torrent` of a torrent Deluge does not have, in the shape seen from Deluge 2.2.0 (a made-up hash) |
| `label-options.json`, `label-options-off.json` | the Label plugin's `label.get_options` of a label that moves its finished torrents to a folder, and of one whose option is off |
| `torrents.json` | `core.get_torrents_status`: Downloading, Queued, Checking, Allocating, Paused, Moving (finished), Seeding, Error, Paused and finished, another label, no label, an upper-case hash key, finished with no location, finished with a name that is a path, a name with two spaces |
| `torrents-empty.json`, `torrents-not-an-object.json` | an empty answer; a result that is a list |
| `torrents-wrong-types.json` | fields of the wrong types, missing, out of range, bad hashes, one hash twice |
| `torrents-unknown-state.json` | a state word nobody knows, unfinished and finished |
| `error-not-authenticated.json` | any call without a session: error code 1 |
| `error-not-authenticated-words-only.json` | the same, with no code (the words alone say it) |
| `error-unknown-method.json`, `error-no-words.json` | other errors |
| `envelope-*.json` | envelopes that cannot be believed: another id, none, a string id, result and error together, neither, an error that is not an object |
| `not-an-object.json`, `truncated.json` | answers that are not one JSON object |

## Seen from a real Deluge 2.2.0 (fix-first-torrent)

The first real torrent run (2026-11-16) showed one answer:

- **A removal of a torrent Deluge does not have.** `core.remove_torrent` of
  a torrent that a plugin of the server's had already removed (leaving its
  files) was answered with an error whose message held
  `InvalidTorrentError` and `torrent_id <the 40-digit hash> not in
  session.` (the page showed it as `Failure: [...InvalidTorrentError...:
  torrent_id ... not in session. ]`).  The kind takes exactly that, with
  the hash of the torrent it asked about, as the torrent gone from Deluge
  (`AXIS_DOWNLOAD_ERROR_GONE`), not as a refusal.  What lies between the
  parts seen (`Failure instance: Traceback (failure with no frames): <class
  'deluge.error.InvalidTorrentError'>`, as twisted writes a Failure) and the
  error's `code` (4 in the fixture and the double) are believed, from
  memory of Deluge's `json_api.py` and `core/torrentmanager.py`; the kind
  reads neither.

## Believed, not verified

Each of these is what the kind assumes; a run against a real Deluge should
confirm or correct it.

1. The web interface takes a POST of JSON to `<base>/json` with the content
   type `application/json`, and answers `{"result", "error", "id"}` with the
   `id` it was sent, always in a 200 (an error of Deluge's is in the envelope,
   not in the status).
2. `auth.login` with `[password]` answers `true` and sets a cookie
   (`_session_id`), or answers `false`.  The kind checks that a cookie came.
3. A call without a valid session is answered with an error whose `code` is 1
   and whose message is "Not authenticated" (the kind takes the code, or the
   words, either).  `auth.check_session` is answered `true` or `false` and
   needs no session.
4. `web.connected` answers a boolean.  `web.get_hosts` answers a list of
   `[id, host, port, user]` (1.x and 2.x may differ in the last item).
   `web.connect` with `[host_id]` connects the web interface to that daemon;
   whatever it answers, the kind asks `web.connected` again.
5. `daemon.get_version` answers the daemon's version as a string; an older
   Deluge answers `daemon.info` instead.  Neither is needed: without one the
   test reports no version.
6. `core.get_enabled_plugins` answers a list of names that includes `Label`
   when the plugin is on; without it, `label.get_labels` is an unknown
   method.
7. `label.get_labels` answers a list of strings; `label.add` with `[label]`
   makes one and `label.set_torrent` with `[hash, label]` gives it to a
   torrent; both raise an error (not a false result) when they fail.  Labels
   are lower case, from `a-z`, `0-9`, `.`, `_` and `-`.
8. `core.add_torrent_file` with `[file name, base64 file, options]` and
   `core.add_torrent_magnet` with `[uri, options]` answer the torrent's
   info-hash as a string of 40 hexadecimal digits, and take `{}` as options.
   What they answer for a torrent Deluge already has (here: an error) is not
   known.  The hash of a v2 or hybrid torrent is not known to be the v1 hash
   Axis computes (then the add is treated as failed).
9. `core.get_torrents_status` with `[{"label": label}, [keys]]` answers an
   object keyed by hash; each value has the keys asked for: `name`, `state`
   (Downloading, Queued, Checking, Allocating, Paused, Moving, Seeding,
   Error), `progress` (0 to 100), `total_wanted` and `total_done` (bytes),
   `download_payload_rate` (bytes a second), `eta` (seconds, 0 for none),
   `download_location`, `is_finished`, `label` (a string, empty for none),
   `tracker_status`, `time_added`.  Whether the filter by label is applied by
   Deluge, and whether a torrent with no label has an empty one or none, is
   not known (the layer filters again, and an entry with no label is
   skipped).
10. The files of a torrent are at `download_location` joined with its `name`
    (true for torrents of one or many files, with no "move completed" folder
    configured; with one configured, `download_location` may be where the
    files were while downloading, not where they are).
11. `Moving` is reported while `is_finished` may already be true; the kind
    takes `Moving` as not yet there.  An `Error` torrent that is also
    `is_finished` is taken as completed, as the design says.
12. `core.remove_torrent` with `[hash, remove_data]` answers `true` when it
    removed it (with `remove_data` true, deleting the torrent's own files),
    and raises an error for a hash Deluge does not have (seen, above: that
    error is "not in session").
13. A torrent added and then labelled is, in between, unlabelled: a daemon
    that stops, or an answer that is lost, between `core.add_torrent_*` and
    `label.set_torrent` leaves a torrent no label filter lists (the kind
    cannot find it again).
14. The Label plugin's `label.get_options` with `[label]` answers the
    label's options as an object, among them `apply_move_completed`,
    `move_completed` (booleans) and `move_completed_path` (a string); when
    the first two are true, the plugin gives a torrent of that label that
    folder as its "move completed" folder once the label is set, so a
    finished torrent's files are moved there and its `download_location`
    becomes that folder.  A label Deluge does not have is an error (a
    `KeyError`).  Asked only for a torrent Deluge no longer has after its
    import.
15. `core.get_config_values` with `[[keys]]` answers an object of those keys
    alone; `download_location` and `move_completed_path` are the folders
    Deluge uses for every torrent (the kind refuses a label folder that is
    one of them or holds one).  Only those two keys are asked for, so
    nothing else of Deluge's settings is sent.

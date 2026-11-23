# SABnzbd fixtures: hand-written, not recorded

No SABnzbd was available to record these.  They were written by hand from
SABnzbd's published API format (https://sabnzbd.org/wiki/advanced/api) and
from memory of what its versions 3.x and 4.x answer.  The first run of Axis
against a real instance is expected to find corrections: see the list of
details that are believed but not verified in the openspec change
`add-downloader-sabnzbd` and in `tests/test-download-sabnzbd.c`.

Every answer here is what `output=json` gives, from a POST whose body carries
`mode`, `output=json` and `apikey`.

| file | what it stands for |
| --- | --- |
| `version.json` | `mode=version` |
| `get-cats.json`, `get-cats-no-axis.json` | `mode=get_cats` with and without Axis's category |
| `addfile-ok.json` | `mode=addfile` that made one job |
| `addfile-failed.json` | `mode=addfile` refused inside a 200 (`"status": false`) |
| `addfile-no-ids.json`, `addfile-two-ids.json` | a "successful" add with no job, or two |
| `wrong-key.json` | any call that needs the key, with a wrong one |
| `hostname-denied.txt` | the 403 of a host name outside `host_whitelist` (real, 5.1.3) |
| `queue.json` | `mode=queue`: Downloading, Queued, Paused, Fetching, Propagating, Grabbing, one job of another category |
| `queue-empty.json`, `queue-unknown-status.json` | an empty queue; a status word nobody knows |
| `history.json` | `mode=history`: Completed, Failed, every post-processing status, a job of another category, a Completed job with no folder |
| `history-empty.json`, `history-unknown-status.json` | an empty history; a status word nobody knows |
| `removed-queue.json`, `removed-none.json`, `removed-history.json` | the answers of `name=delete` |
| `get-config-categories.json` | `mode=get_config&section=categories`: the categories with their folders (relative, absolute, `*`, none, one that is not text) |
| `get-config-complete-dir.json` | `mode=get_config&section=misc&keyword=complete_dir` |
| `queue-wrong-types.json` | fields of the wrong types, missing, out of range |
| `queue-no-slots.json`, `history-no-slots.json` | the list is missing or is not a list |
| `not-an-object.json`, `truncated.json`, `plain-error.txt` | answers that are not one JSON object |

## Verified against SABnzbd 5.1.3 on 2026-11-16

The first run of Axis against a real SABnzbd 5.1.3 showed these (the
fixtures above are still hand-written; `hostname-denied.txt` is the one
answer here that is exactly what the real server said):

- **The host-name check.** A SABnzbd reached by a host name that is not in
  its `host_whitelist` (Config, Special) answers every request, whatever the
  key, `403` with the plain-text body `Access denied - Hostname verification
  failed: https://sabnzbd.org/hostname-check` (`hostname-denied.txt`).  It is
  not a refused key.  Axis recognises it (status 403 and a body, read up to
  512 bytes, that contains "Hostname verification failed") and says that
  SABnzbd refused the host name it used, naming the host; the state is
  "Failing", not "Secret refused".  Any other 403 stays a refused key.
- `mode=version` answers `{"version":"5.1.3"}` without a key, once the host
  name is accepted.
- `mode=get_config&section=categories` lists the categories as objects with
  `name` and `dir`.
- `mode=set_config&section=categories&name=<n>&dir=<d>` creates a category.
- A category Axis is configured with but SABnzbd lacks was reported
  correctly by Axis (the note on the client test).
- **What a history delete deletes.** `mode=history&name=delete&del_files=1`
  of a *completed* job removed its history entry and left its folder
  (`<category folder>/<job name>/`, with the film's file in it) where it
  was; `del_files` deletes the files of a job that failed only.  The double
  in `tests/test-download-sabnzbd.c` now does the same (a completed job's
  folder stays, a failed one's goes), and after an import Axis removes the
  completed job's folder itself, inside the category's folder only
  (`src/import/axis-import-folder.c`; change `fix-first-import`).
- **What `storage` names for a job of one file.** The history's `storage` of
  a completed job that held one file was that file,
  `<category folder>/<job name>/<job name>.mkv`, not the job's folder.  The
  import takes that file; after it Axis removes the job's folder that holds
  it (the first part beneath the category's folder).  That a job of several
  files is reported by its folder is still believed, not seen.

## Believed, not verified

Each of these is what the kind assumes; a run against a real SABnzbd should
confirm or correct it.

1. A POST whose body is `application/x-www-form-urlencoded` (or multipart for
   `addfile`) is read like the same parameters in a query, `apikey` included.
2. `mode=version` is answered without a key (so it cannot show a wrong key,
   and the test asks `mode=get_cats` next, which needs one).
3. A wrong or missing key is answered 200 `{"status": false, "error": "API Key
   Incorrect"}` (or "API Key Required"); an old SABnzbd answers the plain text
   `error: API Key Incorrect`.  A 401 or 403 is also taken as a refused key.
4. `mode=get_cats` answers `{"categories": ["*", ...]}`, names as configured.
5. `mode=addfile` takes the file in the multipart field `name` and the fields
   `cat`, `priority` (-1, 0, 1) and `nzbname`; it answers `{"status": true,
   "nzo_ids": ["SABnzbd_nzo_..."]}`, and a refusal as `status` false with an
   `error`.  Whether a file that is not an NZB is answered that way, or as
   status true with no identifier, is not known (both are refusals here).
6. `mode=queue` and `mode=history` take `start`, `limit` and a category filter
   under one of the names `cat` and `category` (the kind sends both; the layer
   filters again whatever comes).
7. A queue slot has `nzo_id`, `filename`, `cat`, `status`, `mb` and `mbleft`
   (decimal megabytes as strings), `percentage` (a string) and `timeleft`
   ("H:MM:SS", possibly "D:HH:MM:SS").  The category of a job with none is
   "*".  SABnzbd gives no rate per job (the kind reports none).
8. A history slot has `nzo_id`, `name`, `nzb_name`, `category`, `status`,
   `bytes` (a number), `storage` (the final folder of a job of several files,
   only once it is moved; for a job of one file, that file, as seen above)
   and `fail_message`.
9. The status words are as listed in the table above and in the design; in the
   history `Queued` and `Fetching` are taken as work after the download
   (this goes beyond the design's list), and the words are compared without
   regard to case.
10. `name=delete` on the queue answers `{"status": true, "nzo_ids": [the ones
    removed]}` and an empty list for an identifier it does not have; on the
    history it answers `{"status": true}` whether or not the job was there
    (so a history delete cannot tell "removed" from "was not there").
    `del_files=1` deletes a queued job's partial files, and on the history a
    failed job's files; a completed job's folder stays (seen, above).
    Whether a job deleted from the queue leaves an entry in the history is
    not known.
11. SABnzbd keeps the job name sent as `nzbname` as it is for ordinary release
    names; a name it changes would stop a lost answer being matched by name.
12. `mode=get_config&section=categories` answers `{"config": {"categories":
    [...]}}` (the objects with `name` and `dir` were seen; the `config`
    around them is believed), and `section=misc&keyword=complete_dir`
    answers `{"config": {"misc": {"complete_dir": "..."}}}` with only that
    setting.  A category's `dir` is relative to `complete_dir` unless it is
    absolute, an empty one means the category has no folder of its own, and
    a trailing `*` means the job's files go straight into the folder with no
    job folder (as SABnzbd's own post-processing works it out, from memory
    of its source, not run).

# Screenshots the guide needs

The guide has no images yet. Each place that needs one holds an HTML
comment of the form `<!-- screenshot: ... -->`. A later task takes the
images from a demo instance, with the default theme (Midnight), and saves
them under `docs/images/`.

Rules for every image:

- Use made-up titles, host names and paths only, such as the film "The
  Lantern Keepers", the series "Harbour Lights", the host `axis.example.com`
  and the disk `/media/disk1`. Never use a real library.
- Use generated posters, never artwork from a real title.
- Size the images for the web, and regenerate them with a script so that
  they do not go stale.

## The list

| Page | Image shows | Demo data it needs |
| --- | --- | --- |
| [01](01-install-and-look-read-only.md) | The **Set up Axis** page with the token, username and password filled in | A new instance with no account. A made-up token and the username `admin`, with the password masked. |
| [01](01-install-and-look-read-only.md) | The **Disk usage** page | Two disks with a few thousand generated files, in several codecs and resolutions, with a few duplicates. |
| [02](02-add-disks-and-library-folders.md) | The **Add a disk** confirmation page | A generated directory that is a mount point, empty, with no marker yet, so the page offers **Write the marker and add the disk**. |
| [03](03-connect-indexers-and-download-clients.md) | The **Indexers** page with the **Add an indexer** cards | No indexer added. |
| [03](03-connect-indexers-and-download-clients.md) | The **Download clients** list with one client **Working** | One test-double SABnzbd that answers the test and has the category `axis`. |
| [04](04-find-and-download-a-movie.md) | The title page of "The Lantern Keepers" with the **Add to Movies** form | A fake details provider answering with that title, a generated poster, a **Movies** library folder on an online disk, and two quality profiles. |
| [05](05-imports-leftovers-and-cleanup-jobs.md) | The **Imports** page with one import **Moved** and one **Blocked** | Two generated downloads, one imported and one blocked for a runtime that does not match. |
| [05](05-imports-leftovers-and-cleanup-jobs.md) | The **Leftovers in the download folders** page with two entries | A test-double client folder with one leftover file and one leftover folder, both made up. |
| [06](06-identify-movies-and-series-on-disk.md) | The **Identify** page for a **TV** library folder with likely and unknown entries | A generated **TV** library folder with about 20 series folders, some matched, some likely, some unknown. |
| [07](07-add-a-series.md) | A series page with its seasons and episode states | "Harbour Lights", three seasons, with episodes in each state (**In library**, **Wanted**, **Not aired**, **Downloading**). |
| [08](08-see-what-converting-would-save.md) | The **Space savings** overview with the three video profiles | The generated library of page 01, estimates refreshed, **Informed guess**. |
| [09](09-prove-a-profile-with-trials.md) | A file page with the **Convert this file** card | A generated video file, and at least one **Locked: trials only** profile version. |
| [09](09-prove-a-profile-with-trials.md) | A conversion page with the ten checks passed | A conversion record with ten **Passed** checks, made by the test double that stands in for a worker. |
| [10](10-convert-a-whole-library.md) | A conversion run list with series grouped and savings shown | A run over a generated library of about 40 series and 30 movies, in state **Ready to start**. |
| [11](11-run-conversions-on-a-worker.md) | The **Conversion workers** page with a connected worker | A test-double worker named `desk` that reports `hevc_nvenc` usable, with two disks it can reach, one with no file identity. |
| [12](12-themes-and-accounts.md) | The **Theme** card on the **Your account** page with **Midnight** selected | Any account. |
| [13](13-run-behind-a-reverse-proxy.md) | The **Reverse proxies** page with one resolved host name | `AXIS_TRUSTED_PROXIES=traefik` with a name that resolves in the demo network. |
| [19](19-see-what-needs-your-attention.md) | The **Needs attention** page with a notice in each of the three groups, and the bell in the header showing 3 | The demo's empty scene with three made-up notices: run `tests/tools/axis-demo-shots.sh` with `AXIS_DEMO_ATTENTION=3` in the environment. The entry is named `attention`. |

The README also wants screenshots of the Movies and TV lists, the Leftovers
in the download folders page and a conversion run, taken from the same demo
instance. They are not
placed in the guide yet.

## Take the images

The demo instance and the screenshot tool are in `tests/tools/`. They need
Podman (for the throwaway database) and Google Chrome, and they write
nothing into the repository.

1. Build the tests' tools:

   ```sh
   make build/gcc-release/tests/tools/axis-demo
   ```

2. From the top of the repository, run the demo and the screenshots into a
   directory outside the repository:

   ```sh
   tests/tools/axis-demo-shots.sh /tmp/axis-shots
   tests/tools/axis-demo-shots.sh /tmp/axis-shots-empty empty
   tests/tools/axis-demo-shots.sh /tmp/axis-shots-setup setup
   ```

   Each run starts a throwaway PostgreSQL, the demo on `127.0.0.1` at a
   random port, and headless Chrome. It captures every page the demo lists
   at 390 by 844 and at 1440 by 900 pixels, writes `report.txt` (pages wider
   than a phone, small tap targets, small text), and stops everything.
3. To take only some pages, add `--only` with their names, for example
   `--only guide-04-title-lantern,guide-07-series`. The names are in
   `demo-pages.txt` in the output directory. Add `--widths desktop` for the
   computer's width only.

The images of the list above, by the demo's page name:

| Page | Demo page | Scene | State |
| --- | --- | --- | --- |
| 01 | `guide-01-setup` | `setup` | Ready. The token is typed in by the tool and is made up. |
| 01 | `guide-01-disk-usage` | `full` | About 80 files, not a few thousand, and no duplicates yet. |
| 02 | (none) | | Not yet. The confirmation needs an empty mount point, which a demo without root cannot make. |
| 03 | `guide-03-indexers` | `empty` | Ready. |
| 03 | `guide-03-download-clients` | `full` | Ready. The clients are the tests' stand-ins, named SABnzbd and qBittorrent. |
| 04 | `guide-04-title-lantern` | `full` | Ready. |
| 05 | `guide-05-imports` | `full` | Ready. Three imports moved, two blocked. |
| 05 | `guide-05-leftovers` | `full` | Ready. One leftover folder and one file. |
| 06 | `guide-06-identify-tv` | `full` | Five series folders, not about 20: one likely, one unknown, the rest linked. |
| 07 | `guide-07-series` | `full` | Ready ("Harbour Lights", three seasons). |
| 08 | `guide-08-savings` | `full` | The estimates come from the few demo files. |
| 09, 10 | (none) | | Not yet. They need a conversion record and a run. |
| 11 | `guide-11-workers` | `full` | Not yet: no worker is connected. |
| 12 | `guide-12-account` | `full` | Ready. |
| 13 | `guide-13-proxies` | `full` | Not yet: no trusted proxy is set. |

# The Axis user guide

This guide shows you how to use Axis by what you want to do. Each page is a
worked example, from what you need first to what can go wrong. Each page
ends with a **Verified** note that says what has run on a real server and
what has only run in tests. Read that note before you rely on a page.

Axis is early software. Do not point it at a library you care about until
you have read the note on the page you are following.

To install Axis, and for container, Kubernetes and Podman detail, see
[the deployment notes](../deploy/README.md). This guide links to them and
does not repeat them.

## Start here

| I want to | Page |
| --- | --- |
| Install Axis beside my existing stack and look at my library without changing it | [Install and look read-only](01-install-and-look-read-only.md) |
| Tell Axis about my disks and my movie and TV folders | [Add disks and library folders](02-add-disks-and-library-folders.md) |
| Connect my indexers and download clients, and understand each test result | [Connect indexers and download clients](03-connect-indexers-and-download-clients.md) |

## Find, download and import

| I want to | Page |
| --- | --- |
| Add a movie, and read why Axis chose a release | [Find and download a movie](04-find-and-download-a-movie.md) |
| Understand how an import moves files, clear leftovers, and use my own cleanup job | [Imports, leftovers and your own cleanup jobs](05-imports-leftovers-and-cleanup-jobs.md) |
| Clean up a movie or an episode that has more than one file | [Clean up duplicates](05-imports-leftovers-and-cleanup-jobs.md#clean-up-duplicates) |
| Have Axis recognize what is already on my disks, and review its doubts | [Recognize movies and series on disk](06-identify-movies-and-series-on-disk.md) |
| Add a series, and use season packs and the calendar | [Add a series](07-add-a-series.md) |
| Add a movie or a series from my phone, and check on it later | [Use Axis from a phone](15-use-axis-from-a-phone.md) |
| Migrate my movies from Radarr and my series from Sonarr, with their profiles, folders and files | [Migrate from Radarr and Sonarr](16-migrate-from-radarr-and-sonarr.md) |
| Download one video from YouTube into a library folder of its own | [Download a video from YouTube](17-download-from-youtube.md) |
| Fetch the definitions of other torrent sites from Jackett's repository, update them, and put an earlier version back | [Fetch site definitions from Jackett](18-fetch-site-definitions-from-jackett.md) |

## Save space

| I want to | Page |
| --- | --- |
| See what converting would save | [See what converting would save](08-see-what-converting-would-save.md) |
| Try a conversion and prove a profile | [Prove a profile with trials](09-prove-a-profile-with-trials.md) |
| Convert a whole library, one file at a time | [Convert a whole library](10-convert-a-whole-library.md) |
| Do the conversions on a faster machine with a GPU | [Run conversions on a worker](11-run-conversions-on-a-worker.md) |

## Run Axis

| I want to | Page |
| --- | --- |
| Find out from any page that something waits for me, and dismiss what I have seen | [See what needs your attention](19-see-what-needs-your-attention.md) |
| Pick a theme and manage accounts | [Themes and accounts](12-themes-and-accounts.md) |
| Let people who are not administrators ask for a movie or a series, and approve or decline what they ask for | [Let other people ask for titles](20-let-other-people-ask-for-titles.md) |
| Put Axis behind a reverse proxy | [Run behind a reverse proxy](13-run-behind-a-reverse-proxy.md) |
| Back up and restore the database, check data checksums, and find out what is wrong when the database is the problem | [Back up, restore, and repair the database](14-back-up-restore-and-repair-the-database.md) |
| Choose how long Axis keeps the history of movies, series, downloads, and imports | [Choose how long Axis keeps history](14-back-up-restore-and-repair-the-database.md#choose-how-long-axis-keeps-history) |

## Contribute

| I want to | Page |
| --- | --- |
| Repair a site definition when a torrent site changes its pages, with a test | [How to repair an indexer definition](CONTRIBUTING-definitions.md) |

## Not written yet

These use cases are planned and have no page:

- Tell Plex or Jellyfin about changes, and map their paths.
- Bind Axis's outbound traffic to a VPN interface. The deployment notes
  describe it in [Sending Axis's traffic through a VPN](../deploy/README.md#sending-axiss-traffic-through-a-vpn).
- Troubleshooting by symptom.

## How the pages are written

- Every example title, host name and path is made up. Example hosts use
  `example.com`.
- A label in **bold** is the text Axis shows on the page.
- Screenshots are not made yet. [`SCREENSHOTS.md`](SCREENSHOTS.md) lists
  the images each page needs, and the demo data for each.

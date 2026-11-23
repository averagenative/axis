# Connect indexers and download clients

This page shows you how to connect Axis to the places it searches for
releases (indexers) and to the programs that download them (download
clients), and how to read the result of each test. Allow about 30 minutes.

Axis downloads nothing itself. It searches your indexers, picks a release,
hands it to your download client, and follows the download to the end.

## Before you begin

You need:

- An administrator account, and a disk with a library folder
  ([Add disks and library folders](02-add-disks-and-library-folders.md)).
- A TMDb token, so that Axis can look up titles. The Setup guide step
  **Details from TMDb** walks you through it: sign in at TMDb, open the API
  page of your account settings, and copy the **API Read Access Token**.
  This is the long one that starts with `eyJ`. Axis refuses the short API
  Key. Paste the token in the step and select **Save the token**, and then
  **Check the token**.
- For Usenet: an indexer account and its API address and key, and a
  running SABnzbd with its API key.
- For torrents: a running Deluge or qBittorrent with its web interface on,
  and either a Torznab feed (from Jackett or Prowlarr) or one of the public
  sites Axis knows (YTS, EZTV, The Pirate Bay, TorrentProject, LimeTorrents,
  TheRARBG, Torrents-CSV).
- Optional: Prowlarr, if you already keep your indexers in it.

For the best result, give the download client and the library the same
disk, mounted once, at the same path in every container. Then an import is a
rename and not a copy. See
[the one layout rule](../deploy/README.md#the-one-layout-rule).

## Add an indexer

1. Open **System**, then **Indexers**.
2. In the **Add an indexer** card, pick the kind that fits:

   | Card | Use it for |
   | --- | --- |
   | **A Usenet indexer (Newznab)** | Any Usenet indexer. Give its API address and key. |
   | **A torrent indexer, through a Torznab feed** | One tracker of Jackett's, or one indexer of Prowlarr's. |
   | **YTS, for movies**, **EZTV, for TV**, **The Pirate Bay, for movies and TV** | Public sites that need no account and no key. |
   | **TorrentProject**, **LimeTorrents**, **TheRARBG**, **Torrents-CSV**, each **for movies and TV** | Public sites that Axis reads with a site definition of its own. They need no account, no key and no file to install. See [Add a built-in torrent site](#add-a-built-in-torrent-site). |
   | **Prowlarr, for every indexer in it** | Prowlarr, which searches all its indexers with one request. |

   <!-- screenshot: the Indexers page with the Add an indexer cards -->

3. Fill in the form:

   - **Name**: the name Axis shows on its pages.
   - **Address**: its address, such as `https://api.example.com`. A path
     goes in **Base path (optional)**, and only when the service is served
     under one.
   - **API key**: shown once. Axis never shows a stored key again and never
     writes it to a log.
   - **Newznab categories for movies** and **Newznab categories for TV**: leave them empty
     for 2000 (all movies) and 5000 (all TV).
   - **Follow download redirects to the indexers**: on by default. Leave it
     on if you use Prowlarr and an indexer has its Redirect option on.

4. Select **Add the indexer**. Axis shows "The indexer has been added.
   Test it now: an indexer's test reads what it can search by and where ...".
5. Select **Test**. Read the **State** column.

## Add a built-in torrent site

Axis comes with a site definition for four public torrent sites. It
maintains and tests these definitions itself, so you install nothing. Axis
asks a site nothing until you add it.

1. Open **System**, then **Indexers**.
2. In **Add an indexer**, open the card of the site, for example
   **LimeTorrents, for movies and TV**. The card says in one sentence what
   the site is and how Axis searches it.
3. Leave the **Address** as it is, unless the site has moved. If the site
   has moved, or you run your own copy of Torrents-CSV, enter its address in
   **Or another address (optional)**.
4. Select **Add the indexer**, and then **Test**.

What each site can search by:

| Site | Movies | TV | By IMDb identifier | Notes |
| --- | --- | --- | --- | --- |
| The Pirate Bay (built in, no definition) | Identifier first, then title and year | Title and SxxEyy, or Sxx for a season | Movies | Its video categories only. VIP and trusted uploaders show as a flag. |
| TorrentProject | Title and year | Title and SxxEyy, or Sxx | No | Axis reads a release's info-hash from the release's own page only when you grab it. |
| LimeTorrents | Title and year | Title and SxxEyy, or Sxx | No | Axis reads the search feed: one request gives every result's name, category, size, seeders and info-hash. |
| TheRARBG | Title and year, in Movies | Title and SxxEyy, in TV | No | Keeps only results whose name has every word you searched for. |
| Torrents-CSV | Title and year | Title and SxxEyy, or Sxx | No | The first 25 results, the best seeded first. |

Axis leaves out every result that a site files as software, music, books or
games.

A note on RARBG: the original RARBG site closed in May 2023. TheRARBG is the
site that took up its name, with an index of its own. The address
`rarbg.torrentsbay.org` is a proxy of TheRARBG that answers programs with a
browser challenge, which Axis cannot pass. If you choose that address, the
indexer shows as **Failing** with a message that says so. Keep the default
address, `https://therarbg.com/`.

## When a site changes its pages

Sites without an API change their pages from time to time. When a site's
answer no longer looks like the pages its definition reads, Axis does not
report "no releases". Instead, it does the following:

- The indexer's **State** is **Failing**, with a message such as
  "LimeTorrents's pages no longer look as expected (the part every answer of
  the site has is missing)".
- Searches pass the indexer over for an hour, and its row shows
  **Passed over**. A test or a check still asks the site.
- The dashboard lists the indexer under **Needs attention**.

A site can also ask Axis to wait, for example after many searches. The
row then shows **Asked to wait**, and the **Problem** column says until
when. Axis asks the site nothing before then, not for a test or a check
either.

To find out what changed, open the indexer and select **Check this
indexer**. Enter a title the site has, and select **Check it**. The page
shows each request Axis made, the status and size of each answer, how many
results it found, and what each field read for the first five results. To
see the page itself, select **Show an excerpt of each row read**.

To repair a built-in definition without waiting for a new version of Axis,
paste a newer version of it on the definition's page, in **Repairing it:
your own version**. Axis checks it before it uses it: the definition must
keep its id and say a later version. When a later version of Axis ships a
definition that is as new as yours or newer, Axis uses its own again, and
the page says that your version is no longer used. The steps to write a
repair are in
[How to repair an indexer definition](CONTRIBUTING-definitions.md).

## Read an indexer's test result

| State | What it means | What to do |
| --- | --- | --- |
| **Not checked yet** | Axis has not asked it anything. | Select **Test**. |
| **Working** | It answered, and Axis read what it can search by. | Nothing. |
| **Access refused** | The message starts "The API key was refused". | Copy the key again from the indexer's site. For Prowlarr, the API Key is in **Settings**, **General**, under **Security**. |
| **Not answering** | The message starts "Not answering". | Check the address, the port and the network between the containers. |
| **Failing** | The message starts "No usable answer". It answered, but not in a form Axis can use. | Read the message. Check the base path, and that the address is the API address and not the website. For a site, see [When a site changes its pages](#when-a-site-changes-its-pages). |

The **Problem** column adds what else Axis knows: "No API key is set: add it under Settings for this indexer.", a
request limit the indexer reported ("not asked until ..."), "Its daily
allowance of files is used up.", or "It serves its files from another address:
accept it on its page." An address that starts with `http` shows a
**Not encrypted** badge: the key crosses the network in the clear.

To try a search, open **Indexers**, then **Search every indexer**. In **Look
for**, pick **A movie**, enter a title in **Title or words**, and select
**Search every indexer**. The page lists each release
with its indexer, size and age, and what each indexer answered.

## Add a download client

1. Open **System**, then **Download clients**.
2. In **Add a download client**, fill in the form:

   - **Name**: for example `Torrents`.
   - **Kind**: **SABnzbd**, **Deluge** or **qBittorrent**.
   - **Address**: where Axis can reach the client, such as
     `http://sabnzbd:8080`, `http://deluge:8112` or
     `http://qbittorrent:8080` between containers on one network.
   - **User name (qBittorrent only)**: leave it empty for the others. The
     qBittorrent default is `admin`.
   - **API key or password**: for SABnzbd, the API Key (in SABnzbd, open
     **Config**, then **General**, under **Security**; not the NZB Key).
     For Deluge, the password of its web interface; its Label plugin must
     be on. For qBittorrent, the web interface password.
   - **Category or label**: leave `axis`. Axis gives every download it adds
     this category and looks at nothing else in the client. Keep it apart
     from what Radarr and Sonarr use.

3. Select **Add the download client**. Axis shows "The download client has been
   added. Test it, then map its download folders to disks under Path
   mappings."
4. Select **Test**, and read the **State** column.

   <!-- screenshot: the Download clients list with one client Working -->

5. Map the client's folders to your disks. Open the client. In **Path
   mappings**, enter the **Client's path** (the folder as the client names
   it, inside its container), choose the **Disk**, enter the **Path on the
   disk** (empty when the client's path is the disk's top), and select
   **Add the mapping**. Axis never guesses a mapping: a finished download
   whose path no mapping covers waits, and the page says why.

## Read a download client's test result

| State | What it means | What to do |
| --- | --- | --- |
| **Not checked yet** | Axis has not asked it anything. | Select **Test**. |
| **Working** | It answered and accepted the login. The note says more. | Read the note. |
| **Access refused** | "The API key or password was refused". | Retype the key or password. For qBittorrent, a few wrong logins make it ban Axis's address for an hour, and Axis waits instead of trying again. |
| **Not answering** | "Not answering". | Check the address and the network. |
| **Failing** | "No usable answer". | Read the message. For example, "this may not be SABnzbd" means the address answers but is not that program. |

The note under **Working** can say:

- SABnzbd: "SABnzbd has no category called "axis". A job sent with an
  unknown category lands in SABnzbd's default one, which Axis does not look
  at; add the category in SABnzbd's settings first."
- Deluge: "The label "axis" did not exist in Deluge; it was made." Deluge
  also needs its web interface connected to a daemon. If it is not, the test
  connects it and says so.
- qBittorrent: "The category "axis" did not exist in qBittorrent; it was
  made."
- Any client: "No download of this client has finished yet, so its path
  mappings could not be checked." After a first download finishes, test
  again. The note then says whether the latest finished path maps to a
  disk.

## What can go wrong

- A download finishes and sits waiting: its path has no mapping. Add a
  mapping, as in step 5 above.
- The client's folder and the library are on different disks. Axis copies
  every import, and the Setup guide step shows "Not the disk of ...: every import
  from here is a copy." Mount the disk once for both containers.
- A Prowlarr that shows "None read yet. Test Prowlarr to read them." has
  not been tested. Select **Test**.

## Verified

- Indexers, checked against real services: Prowlarr, two real Usenet
  indexers (Newznab), and the real YTS, EZTV and The Pirate Bay sites.
- Not yet run against a real service: Torznab feeds (test doubles only) and
  the community definition files, which are written from hand-made
  fixtures. Those definition files are not covered on this page: see
  [Fetch site definitions from Jackett](18-fetch-site-definitions-from-jackett.md).
- The built-in sites (TorrentProject, LimeTorrents, TheRARBG, Torrents-CSV)
  and The Pirate Bay's search by IMDb identifier: written from the answers
  each site gave on 2026-11-18, and checked with `axisd --check-definition`
  against those saved answers. The daemon has not yet searched the live
  sites with them.
- Download clients: SABnzbd has been checked with a real download, and
  Deluge has answered for real. qBittorrent has not run against a real
  client: its page is written from the code and test doubles only, so
  expect corrections.

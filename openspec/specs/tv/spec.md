# tv Specification

## Purpose
Series Axis manages: adding one with a monitoring choice, each episode's state, searching for and grabbing episodes and season packs without ever guessing which episode a release holds, and their history.
## Requirements
### Requirement: Series are added with a monitoring choice
An administrator SHALL be able to add a series from its title page with a quality profile, a library folder and a choice of which episodes to monitor, and SHALL be able to change monitoring per season and per episode.

#### Scenario: Future only
- **WHEN** a series is added monitoring future episodes only
- **THEN** episodes already aired are unmonitored and later ones are monitored

#### Scenario: New episode announced
- **WHEN** metadata gains an episode in a monitored season
- **THEN** the episode appears, monitored

### Requirement: Each episode shows its state
Each episode of a managed series SHALL show one state derived from the facts, and the series SHALL show how many of its aired monitored episodes are in the library.

#### Scenario: Not aired
- **WHEN** an episode's air date has not passed
- **THEN** its state is not aired and it is not searched for

### Requirement: Episodes are searched for when they air and until found
A monitored episode SHALL be searched for after it airs, and on a paced schedule while it is still wanted. A season with several wanted episodes SHALL be sought as a whole season first.

#### Scenario: Airs
- **WHEN** a monitored episode's air time plus the delay has passed
- **THEN** it is searched for

#### Scenario: Season pack
- **WHEN** a finished season is wholly wanted and an acceptable pack exists
- **THEN** the pack is grabbed rather than each episode

### Requirement: A release is matched to the episodes it holds, never by guessing
A release SHALL be grabbed only for episodes of the series that its name states and that exist in the series' data. A release whose numbers do not exist for the series SHALL be rejected with that reason. A release that states only an absolute number SHALL be rejected with that reason, unless the series is of the kind anime and its absolute numbers are usable, in which case it SHALL be matched through them and through nothing else.

#### Scenario: Double episode
- **WHEN** a release holds episodes 2 and 3
- **THEN** it satisfies both

#### Scenario: Numbering does not fit
- **WHEN** a release is numbered for an episode the series' data does not have
- **THEN** it is rejected, and the series page counts such releases

#### Scenario: An absolute number for a standard series
- **WHEN** a release of a series that is not marked as anime states only an absolute episode number
- **THEN** it is rejected with that reason

### Requirement: An episode is never grabbed twice at once
A release SHALL NOT be grabbed while any episode it holds has a grab in flight.

#### Scenario: Overlap
- **WHEN** a season pack is in flight and a single episode of that season is found
- **THEN** the single episode is not grabbed

### Requirement: Acting on series is for administrators
Any logged-in user SHALL be able to see series, episodes and the calendar. Adding, monitoring, searching, grabbing and removing SHALL be for administrators and recorded in the series' history.

#### Scenario: Viewer
- **WHEN** a non-administrator opens a series
- **THEN** it is shown and no action is offered or accepted

### Requirement: Every series on disk is listed
The TV list SHALL list, beside the managed series, every series that identification has linked to a present file in a library folder for TV and that Axis does not manage. Each SHALL be shown once, as In library and not managed by Axis, with its poster, its year, how many of its aired episodes are on disk, and its size, in every view, with the filters, the sorts and the tab counts. The list SHALL say how many series folders identification holds for a person, with a link to the Identify page, or that Identify is switched off.

#### Scenario: A series on disk
- **WHEN** identification has linked a series' folder and Axis does not manage the series
- **THEN** the list shows it with "On disk, not managed by Axis", "5 of 6 aired episodes on disk" and its size, and its link leads to its page

#### Scenario: Counts, filters and sorts
- **WHEN** the list is filtered by name or by state, or sorted by title
- **THEN** series on disk are counted in In library, kept or dropped by the filter, and placed in the order like any other series

#### Scenario: Held for a person
- **WHEN** identification holds a series folder for review
- **THEN** the list says that one series folder or file on disk is not listed yet, with a link to the Identify page

#### Scenario: A TV library folder, nothing listed
- **WHEN** a library folder for TV exists and nothing is listed
- **THEN** the list names the library folder and says what lists its series (Identify, or that Identify is switched off), not that there is no TV

#### Scenario: No TV library folder
- **WHEN** no library folder for TV exists and no series is known
- **THEN** the TV list says no series yet and how to add one, and nothing about disks

### Requirement: A series on disk has a page
A series on disk SHALL have a page showing each season and episode of the series' data as on disk (with its file, quality, size and copies), missing, or not aired; every file of the series on no episode, with the reason; and its folders. An administrator SHALL be offered to manage it; anyone else SHALL be told that an administrator can.

#### Scenario: The page
- **WHEN** the page of a series on disk is opened
- **THEN** it shows its episodes on disk with their files and quality, two files of one episode as two copies, the missing and not aired episodes, and the files on no episode with their reasons

#### Scenario: Viewer
- **WHEN** a non-administrator opens it
- **THEN** no manage form is offered, and a line says that an administrator can have Axis manage it

### Requirement: Nothing is asked of a series on disk
Axis SHALL NOT search for, grab, upgrade, rename, move or import anything for a series on disk it does not manage, SHALL NOT follow its metadata on the TV schedule, and showing it SHALL write nothing.

#### Scenario: Scheduler
- **WHEN** the TV scheduler and follow-up run with series on disk and none managed
- **THEN** no series is considered, no job is queued, and no row or file changes

#### Scenario: Looking
- **WHEN** the list and the pages of series on disk are opened
- **THEN** no series, series file or job is made, and every file on disk is as it was

### Requirement: A disk away never takes a series out of the list
A series on disk SHALL stay listed while a disk that holds its files is offline, and SHALL say which disk is offline.

#### Scenario: Disk offline
- **WHEN** the only disk of a series on disk goes offline
- **THEN** the series stays listed with "a disk offline", and its page names the disk and marks its files "Disk offline"

### Requirement: A series on disk can be managed
An administrator SHALL be able to manage a series on disk from its page with a quality profile, a destination and a choice of what to monitor (future episodes by default, every episode, or nothing). Managing SHALL search for nothing. Its episodes on disk SHALL count as in the library, and only the missing episodes it monitors SHALL be wanted. Upgrades of the episodes on disk SHALL follow the quality profile. Removing the series SHALL touch no file, and the series SHALL be listed again as a series on disk.

#### Scenario: Manage
- **WHEN** an administrator manages a series on disk monitoring future episodes
- **THEN** no search is made, its episodes on disk are in the library, and the page says Axis manages it now

#### Scenario: Remove
- **WHEN** an administrator removes a managed series whose files are on disk
- **THEN** no file is touched, the page says Axis no longer manages it, and it is listed as a series on disk

#### Scenario: Every identified series
- **WHEN** an administrator uses "Manage every identified series" on a TV root
- **THEN** each identified series is managed with its files, monitoring future episodes or nothing, and none is searched for

### Requirement: A series added through search sees its episodes on disk
When a series is added and identification has linked its folders, its episodes on disk SHALL count as in the library from the add, and SHALL NOT be searched for as missing.

#### Scenario: Added with every episode monitored
- **WHEN** a series whose folder is identified is added monitoring every episode
- **THEN** its episodes on disk are in the library or wanted only as upgrades, its missing episodes are wanted, and the added event says how many episode files were on disk already

### Requirement: The calendar and Wanted list what is still missing
The calendar SHALL list only episodes of managed series that are not on disk. Wanted SHALL list the missing episodes of managed series, and the episodes on disk below the quality profile's cutoff SHALL be under their own tab, Upgrade wanted. Neither SHALL list anything of a series on disk that Axis does not manage.

#### Scenario: Calendar
- **WHEN** a managed series has episodes on disk and episodes still to air
- **THEN** the calendar lists the ones still to air and none on disk

#### Scenario: Wanted
- **WHEN** a managed series has an episode missing and an episode on disk below its cutoff
- **THEN** Wanted lists the missing one, and its Upgrade wanted tab lists the other

#### Scenario: Not managed
- **WHEN** a series is on disk and not managed
- **THEN** the calendar and Wanted list nothing of it

### Requirement: The TV list works for a large library
A page of the TV list SHALL be read in a number of statements that does not grow with the number of series, episode files or posters. The series on disk SHALL be read in one pass, and the counts of a page's series on disk SHALL be read for that page's series alone.

#### Scenario: Thousands of series on disk
- **WHEN** 1,500 series and 60,000 episode files are on disk
- **THEN** every page costs the same statements in every view, paging reaches every series once, and the page's counts read only its own series' episode files

### Requirement: A series can be added in one step from Discover
On a Discover card of a series that is neither managed nor on disk, the TV module SHALL offer an administrator a one-step "Add to TV" with the quality profile, library folder and monitoring that the bar above the list holds (the person's own choice, else the defaults its title page's form selects: the default quality profile, else the first usable one; the first usable library folder; the default monitoring), no specials, and a search queued at once (the title page form's "Search now" box, under "Options"), and an "Add with options" link to the title page; without a usable profile or library folder it SHALL offer only the link. For a series it manages or has on disk it SHALL show its state mark. The add SHALL come back to the page it was made from (`back`, when it is a safe local path) with `done=added`.

#### Scenario: Adding from Discover
- **WHEN** an administrator presses "Add to TV" on a Discover card of a series
- **THEN** the series is added with the profile, library folder and monitoring of the bar above the list, and the browser is sent back to the Discover page, where the card shows its new state

#### Scenario: A series on disk
- **WHEN** a Discover card is of a series identified on disk
- **THEN** it shows the In library mark and no add button

### Requirement: What needs attention says why and where
Every statement that a series or an episode needs attention (the series' title page, its own page, the TV list in every view, Wanted, the dashboard) SHALL say what is wrong in words, the reason its episode's row gives (the most common reasons first, at most three, saying how many episodes are left out with a link to them), and SHALL link to the place where it is put right: the episode's releases for a search's outcome, the import that needs a person, else the episode's row on the series page (every season and episode row has an anchor). A count SHALL NOT be shown without a link. Links to pages only an administrator may open SHALL be named, not linked, for anyone else.

#### Scenario: A profile rejected what was found
- **WHEN** an episode's last search found one release and the series' profile rejected it as too large
- **THEN** the series' title page says "S01E01 needs attention." with the search's words, the episode linked to its releases, and "See what was found and what you can do"

#### Scenario: The lists
- **WHEN** the TV list or Wanted shows that series
- **THEN** its reason is in words ("S01E01 needs attention: 1 release was found ...") and links to the same releases

### Requirement: The page that states a problem offers what resolves it
The releases page of an episode or a season SHALL offer, next to what the last search ended in, what resolves it by its kind: for releases the profile rejected, Grab it anyway (the grab by hand, with its guards), a form "Use another quality profile for this series" that lists every usable profile marked by what the search's own decision would make of those releases under it (accepted or not, and why not) and saves through the series' edit, which searches again and comes back with a notice naming the new profile, and a link to the profile's own page; for nothing found, when the next automatic search is due and Search again now; for releases on the blocklist, Clear the blocklist and Search again now; for every other kind an action or a link to where it is resolved. An administrator SHALL get the actions; anyone else SHALL get the words and no form.

#### Scenario: Another profile accepts the release
- **WHEN** an administrator opens the releases of an episode whose only release Small 1080p rejected as too large and Balanced 1080p accepts
- **THEN** the page offers Grab it anyway, marks Balanced 1080p as accepting this release and Small 1080p and 4K as rejecting it, and links to Small 1080p's page

#### Scenario: Choosing it
- **WHEN** the administrator chooses Balanced 1080p there
- **THEN** the series' profile is Balanced 1080p, a search of the episode is queued with the cause "profile", the browser comes back to the releases with "The quality profile is Balanced 1080p now.", and that search accepts and grabs the release

#### Scenario: A viewer
- **WHEN** someone who is not an administrator opens those releases
- **THEN** the reason and "An administrator can grab a release anyway ..." are shown, with no grab button and no profile form, and a post of the form is refused

#### Scenario: Nothing found
- **WHEN** an episode's last search found no release
- **THEN** its releases say when Axis searches again by itself and offer Search again now, and its row says when the next automatic search is

### Requirement: Episodes with no air date can be searched for by hand
An episode with no air date SHALL say that Axis searches for an episode by itself only once its air date has passed and that a person may search for it. An administrator SHALL be offered Search anyway on its row and, for a season with more than one, a search of all its monitored missing episodes with no air date at once, each searched for by itself, recorded in the history; asked again while they are queued, nothing more SHALL be queued.

#### Scenario: The season's
- **WHEN** an administrator uses "Search all 2 with no air date" on a season
- **THEN** one search of each of the two episodes is queued and the history says "A search of the 2 episodes of season 1 with no air date asked for by root: 2 searches queued."

### Requirement: A new quality profile searches again
When an administrator changes a series' quality profile, from any form, every episode the series monitors that is wanted under the new profile (missing and aired, or in the library below its cutoff) SHALL be searched for again at once, grouped as the scheduler groups searches, with the cause "profile", their waits set back; the history and the notice SHALL say so. A change of destination alone SHALL search for nothing, and a series that monitors nothing wanted SHALL search for nothing.

#### Scenario: A destination alone
- **WHEN** an administrator changes only the destination
- **THEN** no search is queued and the notice says the settings were saved

#### Scenario: Nothing monitored
- **WHEN** the profile of a series that monitors nothing is changed
- **THEN** no search is queued, the notice says nothing was searched for, and the history says why

### Requirement: The series' settings card
The series page SHALL have a card "Settings for this series" (anchor `settings`) that shows the quality profile and the destination once (as the fields an administrator changes, else as values), the other facts with plain labels ("Details last updated"), and the actions in one row with Remove this series last and set apart; its head SHALL say "Quality profile: <name>" with a link to the card.

#### Scenario: An administrator
- **WHEN** an administrator opens a series page
- **THEN** it has "Settings for this series", the profile as a field and not again as a value, "Update details now" and "Remove this series", and no "This series" or "Follow the metadata now"

### Requirement: A series, a season or an episode can be searched by hand
An administrator SHALL be offered Manual search for a whole series, for each season and for each episode. A manual search SHALL ask every enabled indexer now and store every release it found with its verdict and reasons, and SHALL NOT grab anything by itself. It SHALL run whatever the automatic rules say is wanted now (episodes with no air date, not monitored, in the library), and only a series that was removed SHALL stop it. It SHALL NOT change any episode's last search, next search or wait, nor the state an episode shows, and while it runs the episodes SHALL NOT show as Searching. Asked again while one of the same level is queued or running, nothing more SHALL be queued. The history SHALL say who asked for it and what it found.

#### Scenario: A season none of whose episodes is wanted now
- **WHEN** an administrator asks for a manual search of a season whose monitored episodes all have no air date
- **THEN** a search of that season is made with the cause "list", its releases are stored, nothing is grabbed, and no episode's next automatic search changes

### Requirement: The manual search page lists everything, grouped
The page of a level's manual search SHALL list every release the search found, in four groups in this order, each with its count: acceptable under the profile, rejected by the profile, not what this search asked for (single episodes on a season search, a pack on an episode search, an episode not wanted now, a release numbered beyond the series' data), ruled out (another series, no episode identifiable, a sample, on the blocklist). Each row SHALL keep its reasons in words and say which episodes it holds. The list SHALL be sorted by size, age, quality, indexer or seeders, and filtered by indexer or by kind, through links that reload the page. While the search is queued or running the page SHALL say so and follow it (htmx), and SHALL have a plain reload link; once it has finished the list SHALL be shown without polling.

#### Scenario: A season search that found single episodes
- **WHEN** a season's manual search found one release of each of its two episodes and no season pack
- **THEN** both are listed under "Not what this search asked for", each with "A season search takes the whole season", and an administrator has "Grab this one" on each

#### Scenario: Sorted and filtered
- **WHEN** the page is opened with `sort=size` and `indexer=NewsOwl`
- **THEN** only that indexer's releases are listed, the largest first in each group, and the page says how many the filter hides

### Requirement: An administrator can grab any listed release of this series by hand
An administrator SHALL be able to take any listed release that is episodes of this series, whatever the profile and the automatic rules about wanted episodes, whole seasons and air dates say, after a confirmation that says in one sentence which episodes it fills, which files in the library it replaces and whether that is an upgrade or a downgrade, and that the quality profile is set aside by hand. Replacing a file the release is not better than SHALL need a second, explicit tick; without it nothing SHALL be sent. A release a fixed rule rules out, one on the blocklist and one holding an episode in flight SHALL NOT be taken through this confirmation, and its row or its confirmation SHALL say why; a release ruled out only by fixed rules about its name SHALL be offered "Grab it by force" instead. The grab SHALL be recorded in the history and the audit with the person's name and "by hand". Someone who is not an administrator SHALL see the list and no grab.

#### Scenario: An episode with no air date filled by hand
- **WHEN** an administrator grabs, by hand, a release of an episode that has no air date and is not monitored
- **THEN** that episode is claimed and downloading, its monitoring is unchanged, and the grab says it was chosen by hand with the profile set aside

#### Scenario: Another series
- **WHEN** a listed release is of another series of the same name
- **THEN** it is under "Ruled out" with why, has no "Grab this one", and a post to grab it through the ordinary form is refused

#### Scenario: A downgrade
- **WHEN** an administrator confirms a release that would replace an episode's better file without ticking the box
- **THEN** it is refused, nothing is sent, and with the box ticked it is sent and recorded as replacing a better file

#### Scenario: A viewer
- **WHEN** someone who is not an administrator opens the manual search page
- **THEN** every release is listed with its reasons, there is no "Grab this one", and a post to grab one is refused

### Requirement: A hand grab claims exactly the episodes the release holds
A grab by hand SHALL claim every episode the release holds that the series' data has, with or without an air date, monitored or not, and no other; those episodes SHALL be in flight for that grab and their monitoring SHALL NOT change. A release that also names episodes the series' data does not have SHALL be listed as not what was asked for (never grabbed by a search, and never offered the one-click Grab it anyway) and MAY be grabbed by hand, through the confirmation only, for the episodes the data has. The confirmation SHALL warn that the release numbers its episodes differently from the series' data, that files are placed by number and may land on the wrong episodes, and SHALL say what the import will do with a file that serves a known and an unknown number (placed and named for the known episode, the other number noted); taking it SHALL need a second tick. A release none of whose numbers the data has SHALL stay ruled out for "Grab this one" and SHALL be taken only by a grab by force, for the episodes the person names. A grab by hand that fails SHALL NOT be followed by an automatic search; the history SHALL say to search by hand again or wait for the scheduled search.

#### Scenario: Numbered beyond the data
- **WHEN** a release named for episodes 5 and 6 of a season whose data has an episode 5 and no episode 6 is grabbed by hand with the numbering tick
- **THEN** episode 5 alone is claimed, the confirmation warned about the numbering, and its one file is imported for episode 5 with S01E06 noted

#### Scenario: Without the numbering tick
- **WHEN** such a release is confirmed without the tick, or posted to the one-click grab
- **THEN** it is refused and nothing is sent

#### Scenario: A failed grab by hand
- **WHEN** the download of a release grabbed by hand from a manual search of the whole series fails
- **THEN** no search is queued, and the history says to use Manual search again or wait for the scheduled search

### Requirement: Adding a series from a phone
A series' add form SHALL keep its fields (quality profile, destination, what to monitor, specials, search now) in a disclosure followed by the add button. On a screen at most 40rem wide the disclosure SHALL be closed and its summary SHALL name the quality profile and the destination the form holds, so that adding with the defaults is one tap on the button; wider, the fields SHALL show in the form as before.

#### Scenario: One tap with the defaults
- **WHEN** an administrator opens the page of a series that is not added
- **THEN** the form's fields are inside a disclosure whose summary names the default quality profile and destination, and the button "Add to TV" follows the disclosure

### Requirement: A series' episodes on a phone
On a screen at most 40rem wide each episode of a series' page SHALL be one stacked row: its number, its title and its state on its first line, then its details after their headings, then its buttons; no part of it SHALL be off the screen.

#### Scenario: Harbour Lights on a phone
- **WHEN** a series' page is shown at 390 pixels
- **THEN** every episode's state is visible beside its number and title without scrolling sideways

### Requirement: A person's search is answered by the work in hand
A search an administrator asks for (an episode's Search for S01E01, Search for this season, Search for every wanted episode, Search all with no air date, a Manual search of any level) SHALL make nothing new only while that very search (the same scope, item and family) is queued or running, or ended less than a minute ago. A wider search in hand SHALL hold nothing back (a season's search takes whole-season packs only, the series' packs only). Automatic searches and manual searches SHALL be two families, neither holding the other. A click on a search the scheduler queued at its lower priority SHALL raise it to a person's priority. The answer SHALL say where that search stands (running since when, or waiting and how many ahead), or what it found and from when one can be asked again. Search for every wanted episode SHALL plan as the scheduler does, leave out each wanted episode searched a moment ago, raise the series' queued searches, say so when nothing new is left, and write the history's line only when it queued something.

#### Scenario: Five clicks on an episode's Search
- **WHEN** an administrator clicks Search on S01E01 five times quickly
- **THEN** one search is queued, and each later answer comes back to the episode's row saying that a search of S01E01 is waiting for its turn

#### Scenario: A wider search in hand
- **WHEN** the search of season 1 is queued and an administrator clicks Search on S01E02
- **THEN** a search of S01E02 is queued as well

#### Scenario: A search the scheduler queued
- **WHEN** the scheduler queued S01E01's search behind its backlog and an administrator clicks Search on S01E01
- **THEN** nothing new is queued, and that search is raised to a person's priority

### Requirement: A season's search that finds no pack falls back to its episodes
A season's (or the series') search that finds no acceptable whole pack SHALL queue the searches of its wanted episodes (of its seasons) one by one, though its own job is still running while it does.

#### Scenario: No pack for an airing season
- **WHEN** a season's search, run by its job, finds only single episodes and rejects them as not the season
- **THEN** each wanted episode of the season is queued for a search of its own

#### Scenario: Search all wanted again
- **WHEN** an administrator clicks Search for every wanted episode five times quickly
- **THEN** what the scheduler would queue is queued once, the others say that every wanted episode is in a search already, and the history has one line

#### Scenario: Searched a moment ago
- **WHEN** the search of season 1 ended a few seconds ago and an administrator clicks Search for this season
- **THEN** nothing is queued, the answer says what it found and from when it can be asked again, and a minute after it ended the click queues a search

### Requirement: The search buttons say when their work is in hand
While a button's very search is queued or running, or ended a moment ago, a series' page SHALL draw that search button as one that says so ("Searching", "Waiting for its turn", "Searched a moment ago") and cannot be pressed, by the same rule the server answers with; a Manual search button SHALL lead to the manual search's page instead. Search all with no air date, decided episode by episode, SHALL never be drawn so. This SHALL be drawn by the server from the jobs in hand, so a plain reload shows it, and an open page SHALL ask again once the minute is over.

#### Scenario: Reload while a search waits
- **WHEN** an administrator reloads a series' page while its episode's search is queued
- **THEN** the episode's row has a disabled "Waiting for its turn" button and no Search button

### Requirement: A series' page says what is happening
A series' page SHALL show, for the series, each season and each episode, one line of what is happening to it: its own search in hand (running since when and what it says, or waiting for its turn and how many are ahead), the download of an episode at its client (how far, how much), else for fifteen minutes what its last search found (found and sent one to a client, found nothing, none acceptable, did not finish and why), else a wider search in hand, for information. The lines SHALL follow the hub: the page follows its series' search topic and its title's topic, and fetches its lines out of band in one request after the last of a burst of changes; while anything is in hand also every 30 seconds; without the stream the page SHALL be right as loaded. Anyone logged in SHALL see the lines; only an administrator the buttons.

#### Scenario: A download at its client
- **WHEN** someone opens the page of a series whose S01E03 downloads at 40%
- **THEN** S01E03's line says it downloads at its client, with how far and how much

#### Scenario: The live fragment
- **WHEN** the page's probe asks for its lines
- **THEN** the answer holds every line of the page (and for an administrator every search button) marked to replace its namesake, and no page around them

### Requirement: Actions come back to the row or card
Every action a series' page or its releases page posts SHALL come back to the row or card it was posted from (an episode's row, a season, the settings card, the releases page's place), by the redirect's anchor, with the notice drawn inside that row or card. An episode's Monitor and Stop monitoring (for example Stop monitoring S02E03) SHALL come back with that episode's season open.

#### Scenario: Monitor an episode
- **WHEN** an administrator clicks Stop monitoring S02E03
- **THEN** the browser comes back to S02E03's row with season 2 open, and the notice is in that row

### Requirement: An administrator can block a release
An administrator SHALL be able to put a release on a series' blocklist by hand. Each such entry SHALL record who made it and when, and the series' blocklist SHALL say of every entry how it got there: a failure with its reason, a person's block, or a download a person gave up on. No search, grab by hand or grab by force SHALL take a release that is on the blocklist, for any episode of the series. A person's entry SHALL match the release by its info-hash when that is known, and by its folded name from any indexer; a failure's entry SHALL keep matching only the indexer it came from. Every block SHALL be written to the series' history with the user, and every action here SHALL be an administrator's, checked for the form's token.

"Block this release" SHALL be offered on each row of a manual search (of the series, a season or an episode) that could be grabbed. It SHALL be one post that changes nothing but the blocklist and the history, and the page SHALL come back at that row, which SHALL say "On the blocklist (blocked by NAME)" from then on and link to the blocklist. A manual search's rows SHALL show the blocklist as it is now.

"Block and search again" SHALL be offered for a download Axis grabbed for a series, on the Downloads page while it is in progress or finished, and on a file's row of the series' Files and links page where the file's release is known. It SHALL lead to a confirmation that writes nothing. When the grab is open, confirming SHALL block the release and give up on the download through the same path a failed download takes: the grab closed and its episodes free again, the client asked to remove the download with its files, and the next best release looked for under the rules of that path. When the grab is closed, confirming SHALL only block the release: no library file, link to an episode, import, download or job SHALL be changed, and the page SHALL say so. This action SHALL NOT delete or unlink a file of the library.

One entry SHALL be removable from the blocklist by itself ("Unblock").

#### Scenario: From a manual search
- **WHEN** an administrator selects Block this release on a row of an episode's manual search
- **THEN** the release is on the series' blocklist with their name and the time, the page shows that row again saying who blocked it, nothing was grabbed, and the next scheduled search of the episode takes another release

#### Scenario: The same name from another indexer
- **WHEN** a person blocked a release and another indexer lists a release of the same name later
- **THEN** a search does not take it, and its reason says who blocked it

#### Scenario: A download that is not wanted
- **WHEN** an administrator opens Block and search again for an episode's download in progress
- **THEN** the page says what it would do and writes nothing; once confirmed, the release is on the blocklist by that person, the grab is closed as given up on with no episode held by it, the client is asked to remove the download, and one search for the next best is queued

#### Scenario: A release that is in the library already
- **WHEN** an administrator blocks the release an episode's present file came from
- **THEN** the release is on the blocklist for the future, and the file on disk, its link to the episode, its import, its grab and the jobs are as they were, which the page says

#### Scenario: One release taken off
- **WHEN** an administrator selects Unblock on one entry of a blocklist that has two
- **THEN** that entry is gone and the other stays

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks for any of these
- **THEN** it is refused and nothing changes

### Requirement: A series follows one source of episode numbering
Each managed series SHALL follow one source of numbering at a time: TMDb, or TheTVDB's aired order. Its seasons, episodes, air dates, runtimes, monitoring, wanted state, searches, the numbers sent to indexers, release matching, pack decisions, size checks and imports SHALL use that source alone, and its pages SHALL name it. A series added from its title page or from Discover SHALL follow its record provider (TMDb); a series SHALL follow TheTVDB's aired order only when that is chosen for it, and never by itself. The source SHALL NOT make a release that states only an absolute number, or numbering across seasons, acceptable for a series that is not marked as anime.

#### Scenario: A release numbered as TheTVDB numbers it
- **WHEN** a series follows TheTVDB's aired order, whose season 2 holds half-hour episodes, and a release named S02E03 of a half-hour's size is found
- **THEN** it is matched to that season 2 episode 3 and judged against its runtime, not refused by size

#### Scenario: A date the other source lacks
- **WHEN** an episode has no air date at TMDb, the series follows TheTVDB's aired order, and TheTVDB gives it a date that has passed
- **THEN** the episode is wanted and searched for automatically

#### Scenario: An absolute number
- **WHEN** a series that is not marked as anime follows TheTVDB and a release states only an absolute episode number
- **THEN** it is rejected with that reason, as before

### Requirement: Changing a series' numbering is confirmed first
An administrator SHALL be able to change a series' source from its settings card, to TheTVDB's aired order while TheTVDB has a key and the series has a TheTVDB identifier, and back to TMDb at any time, through a page that shows, before anything changes, a table of each episode with a file or a grab (its numbering now, its numbering after, its file, how it was matched), the files whose episode has no counterpart, the files whose name and new episode disagree, and the episodes that would be new with how many would become monitored and searched for. Opening the page SHALL change nothing. A confirmation SHALL be refused, and the page shown again, when the series or anything the mapping read changed after the page was made.

#### Scenario: The table
- **WHEN** an administrator opens the page to change a series from TMDb to TheTVDB's aired order
- **THEN** each episode with a file is listed with its numbering now and after, its file's path and how it was matched

#### Scenario: Nothing before the confirmation
- **WHEN** the page is opened and left without confirming
- **THEN** the series, its episodes, files, grabs and history are as they were

#### Scenario: Changed since
- **WHEN** a file of the series is imported after the page was made and the administrator then confirms
- **THEN** nothing changes, and the page is shown again with the new file

### Requirement: A change of numbering keeps every file and its history
Each episode SHALL be mapped to at most one episode of the new source, in this order: an identity it held there before; the same air date and title; the only episode of that date on each side with both running times known and within half of each other (by date only); a title unique on both sides (by title only). A pairing SHALL be taken only when each side is the other's one candidate, so a tie stays unmapped, and the last two steps SHALL be flagged on the page. Titles SHALL be compared as letters and digits, case folded, with a stand-in title such as "Episode 4" counting as none. A file linked to an episode SHALL stay on that episode's row whatever its name says, unless the administrator ticks the box that places files by their names in the same change; a file no link held SHALL be placed by its own name. A mapped episode SHALL keep its files, grabs, history, search pacing and monitoring. An unmapped episode SHALL be kept, marked gone, with its files linked and listed on the series' page. An episode new in the source SHALL take its season's monitoring, and the episodes of a season the series did not have SHALL be left unmonitored unless the administrator clears the box that says so, which is ticked by default. No file SHALL be deleted, moved or renamed. The blocklist SHALL be unchanged, the series' stored search results SHALL be cleared, and the series' history SHALL record the change, who made it and its counts.

#### Scenario: Matched by air date and title
- **WHEN** an episode's air date and title are the same in both sources
- **THEN** after the change it has the new number and keeps its file, its grabs and its history

#### Scenario: A file with no counterpart
- **WHEN** an episode with a file has no counterpart in the new source
- **THEN** the file stays on disk where it was, linked to the episode kept as gone, and the series' page lists it as on an episode the new numbering lacks

#### Scenario: Nothing renamed
- **WHEN** a series with 40 files changes its numbering
- **THEN** every file has the same path, name, size and inode as before

#### Scenario: A new season
- **WHEN** the new source has a season the series did not have and the administrator confirms without clearing the box
- **THEN** that season's episodes are added unmonitored, and the page said beforehand how many new episodes would be monitored and searched for

#### Scenario: Recorded
- **WHEN** a change of numbering is confirmed
- **THEN** the history names the administrator, the old and new sources and how many episodes were matched, kept without a counterpart and new

### Requirement: A change of numbering waits for work in flight
A change of a series' numbering SHALL be refused while any grab or import of the series is open, while a search of the series runs, and while a season of the new source that lists episodes has not been fetched; the page SHALL say what it waits for. Each search, listing and grab SHALL carry the numbering it was made under: a release found under the earlier numbering SHALL be refused when it is claimed, and a download grabbed under it SHALL be held for a person instead of being imported under the new numbering.

#### Scenario: A pack downloading
- **WHEN** a season pack of the series is downloading and an administrator confirms a change of numbering
- **THEN** nothing changes, and the page names the pack and says the change can be made once it is imported or dismissed

#### Scenario: A listing made before the change
- **WHEN** a manual search of the series was listed under TMDb's numbering, the series then changes to TheTVDB's aired order, and an administrator grabs a release from that listing
- **THEN** the release is not grabbed, and no grab is recorded

### Requirement: A change of numbering can be undone
Changing a series back to the source it followed before SHALL map every episode that was mapped by the first change to the episode it was, and episodes kept as gone by the first change SHALL return with their files and history.

#### Scenario: There and back
- **WHEN** a series is changed from TMDb to TheTVDB's aired order and then back to TMDb
- **THEN** every episode has its first number, files, grabs and history again, and no file was touched

### Requirement: A file whose name says another episode is placed by its name only by a person
A series' page SHALL list, for an administrator, the files linked to an episode other than the one their name gives under the numbering the series follows, and SHALL offer "Place by its name" for one file and "Place all by their names", each after a page that shows what changes (the episode left without a file, and whether it becomes wanted). Placing SHALL move only the file's link, never the file, SHALL record it in both episodes' history, and SHALL be refused onto an episode that has a file, outside the numbering the series follows, and while a grab or a manual import of the series is open. Files SHALL be placed one at a time, each against what the ones before left, so that two files named for one episode never both land on it. "Place all" SHALL be refused, and its page shown again, when what can be placed changed after the page was made. A file an Axis import linked, or whose name was read under another numbering than the series follows, SHALL be listed apart and placed only one at a time, each after a page that says its name is probably its old number. The page that changes a series' numbering SHALL offer a box, clear by default, that places by their names, in the same change, the files whose names and new episodes disagree; the files listed apart SHALL never be placed by "Place all" or by that box.

#### Scenario: Placed by its name
- **WHEN** after a change of numbering a file named S01E01 is linked to the episode now numbered S2023E01, and an administrator places it by its name
- **THEN** the file is linked to S01E01, S2023E01 is left without a file and is wanted when it is monitored and aired, the file's path is unchanged, and both episodes' history say that it was placed by its name

#### Scenario: Never by itself
- **WHEN** a file's name says another episode and no administrator places it
- **THEN** its link stays where it is, and identifying the folder again does not move it

### Requirement: A series keeps working when TheTVDB cannot be asked
A series that follows TheTVDB SHALL keep its stored seasons and episodes, and everything that reads them, while TheTVDB has no key, refuses the key or does not answer. Its page SHALL say that its numbering is not being refreshed and why, and SHALL offer the change back to TMDb. TheTVDB's aired order SHALL NOT be offered to another series while TheTVDB has no key.

#### Scenario: The key removed
- **WHEN** TheTVDB's key is cleared while a series follows TheTVDB's aired order
- **THEN** its episodes, wanted state and files are as before, its page says its numbering is not refreshed because TheTVDB has no key, and it can still change back to TMDb

### Requirement: A series from Sonarr is matched by its TheTVDB identifier and follows TheTVDB's aired order
A series migrated from Sonarr SHALL be matched by its TheTVDB identifier and by nothing else: to the stored TMDb title that holds that identifier, and then in TheTVDB's aired order. It SHALL follow TheTVDB's aired order from the moment it is added, whatever the default for new series is, and SHALL NOT be added under another numbering and switched. A series whose TheTVDB identifier TMDb does not know SHALL be skipped as not matched, also when TheTVDB knows it, and a series for which TheTVDB's aired order lists nothing SHALL be skipped with that reason. While TheTVDB has no key in Axis, a Sonarr SHALL NOT be read, no series SHALL be migrated, and the page SHALL say that a key is needed and where it is set; a plan read before the key was removed SHALL skip every series as "No TheTVDB key in Axis". A series whose type in Sonarr is not standard, daily or anime SHALL be skipped with that reason.

#### Scenario: By its identifier
- **WHEN** Sonarr holds a series named *Wobbly Lighthouse* with a TheTVDB identifier, and two stored series share that name
- **THEN** the series with that identifier is the one migrated, and it follows TheTVDB's aired order

#### Scenario: No key
- **WHEN** TheTVDB has no key in Axis and an administrator opens a Sonarr connection
- **THEN** the page does not offer the reading, says that Sonarr numbers its series as TheTVDB does and that a key is needed, and no series is migrated

### Requirement: A migrated series keeps Sonarr's monitoring of each season and episode
A migrated series SHALL have each season and each episode monitored or not as Sonarr has it, the specials included, an episode taken by its season and number in the numbering the series follows. An episode of Axis's at a place Sonarr does not list SHALL take its season's monitoring, and a season Sonarr does not list SHALL be monitored as later seasons are, the specials never. Whether seasons announced later are monitored SHALL follow Sonarr's `monitorNewItems`; when Sonarr gives none, later seasons SHALL be monitored when the series' highest season is. An episode Sonarr has that the numbering followed does not SHALL be counted in the list of what was migrated and SHALL make nothing. A series that is not monitored in Sonarr, migrated because the option was ticked, SHALL have nothing monitored.

#### Scenario: One season switched off
- **WHEN** Sonarr monitors seasons 1 and 3 of a series and not season 2
- **THEN** in Axis seasons 1 and 3 are monitored and season 2 is not

#### Scenario: One episode switched off
- **WHEN** Sonarr monitors a season and not its episode 4
- **THEN** in Axis that season's episode 4 is not monitored and its other episodes are

#### Scenario: An episode the numbering lacks
- **WHEN** Sonarr lists an episode that TheTVDB's aired order as Axis has it does not
- **THEN** the list of what was migrated counts it, and no episode is made for it

### Requirement: Episode files are linked to the episodes Sonarr names, and kept
An episode file SHALL be linked only when every episode Sonarr names for it matches: the series' episode at the same season and number in the numbering the series follows and, where Sonarr gives the episode's TheTVDB identifier and Axis holds one, the same identifier. A place Sonarr lists twice SHALL match nothing. A linked file SHALL be on every episode Sonarr names for it and SHALL be shown as migrated from Sonarr, with the administrator who confirmed the run and the date. A file that only identification linked to the series by its name, on other episodes than Sonarr names, SHALL be moved to the episodes Sonarr names and marked as migrated, when every episode matches and the file has the size Sonarr recorded; a file a person or an import linked SHALL stay where it is and be counted. A migrated file SHALL stay on the episodes Sonarr had it on whatever its name states: identification and placing files by their names SHALL NOT move it. It SHALL be kept: no search SHALL take a release that holds an episode with such a file, a season pack included, and an import SHALL NOT replace it unless an administrator grabbed the release by hand and confirmed that it replaces the file, until an administrator lets it go, for the one episode on the series' page or for every such file of a connection on that connection's page. Letting Axis manage such a file again on the links page, where its name gives its episodes, SHALL place it on those episodes, SHALL end its keeping and SHALL leave its mark as migrated, so that a later run does not keep it again. A later run SHALL link no file of a migrated series that no longer follows TheTVDB's numbering in Axis, and the preview SHALL say how many files that leaves out.

#### Scenario: A double episode
- **WHEN** Sonarr names one file for episodes 1 and 2 of a season, and the file is in the index at its mapped path
- **THEN** the file is linked to both episodes

#### Scenario: Shown
- **WHEN** an administrator opens the files of a migrated series
- **THEN** each file linked by the run says "Migrated from Sonarr", with who ran it and when

#### Scenario: Linked by its name first
- **WHEN** identification linked, by its name, a file of a series to episode 4 of season 1, and Sonarr has that file on episode 2
- **THEN** the run moves the link to episode 2, marks it as migrated, and counts it in the list of what was migrated

#### Scenario: Not upgraded by itself
- **WHEN** a migrated episode file, the file of a double episode included, is below its profile's cutoff and the series is no longer held
- **THEN** its episodes are not wanted, and no search takes a release that would replace the file

### Requirement: A migrated series is not searched for until a person starts it
A series migrated from Sonarr SHALL be held: no scheduled search, no search for an upgrade, no search for an episode that airs later, no search after a change of its quality profile and no follow-up after a failed grab SHALL run for it until an administrator starts searching for the connection's series or for that series, or the run's tick starts it. An episode at a place where Sonarr has a file and Axis has none SHALL be held apart, and SHALL stay held when its series is started, until a file on it is kept or was linked by a person or an import (a later run that links the file included), an administrator uses "Search for S01E01" on that episode, or an administrator starts the series with the tick, not pre-ticked, that includes these episodes. A search an administrator starts SHALL run while the series is held: "Search for S01E01" for one episode or "Search for this season" for one season SHALL search only that and leave the series held, "Search for every wanted episode" for the whole series SHALL end the series' hold, and a manual search SHALL list releases and end no hold. While a file that identification linked after the run is not kept, the connection's page SHALL say so and that a later run keeps it. A held episode that is wanted SHALL say that Axis does not search for it until searching is started.

#### Scenario: A new episode of a held series
- **WHEN** an episode of a held series airs and the scheduler runs
- **THEN** it is not searched for, and the series' page says Axis does not search for it until searching is started

#### Scenario: One episode, by a person
- **WHEN** an administrator uses "Search for S01E01" for one episode of a held series
- **THEN** that episode is searched, the series stays held, and the scheduler searches nothing else of it

#### Scenario: Linked by identification after the run
- **WHEN** identification links a file to an episode held apart after the run
- **THEN** the episode stays held, the connection's page says the file is not kept yet, and a later run keeps the file and ends the hold

#### Scenario: Started
- **WHEN** an administrator starts searching for the migrated series
- **THEN** their wanted episodes are searched at the scheduler's ordinary pace, except the episodes whose files Axis does not see

### Requirement: A release a rule about its name refuses can be grabbed by force for the episodes a person names
On a series' manual search, an administrator SHALL be offered "Grab it by force" on a release whose every fixed rule is about its name (not this series, more than one title fits, another year, no readable title, no episode readable, numbered for no episode of the series' data). Its confirmation SHALL state each rule being set aside in plain words and SHALL have the person name what the release is for: the episodes its name gives when they are in the series' data, which the person may change, else the episodes the person ticks, or a whole season. The files listed and the digest SHALL always be of what is named. When several episodes are named, the confirmation SHALL offer a tick that one video file of the download holds them all. It SHALL say what the import does, and SHALL list each file in the library that would be replaced with its quality; each SHALL need a tick of its own, whether or not the release is better, and the grab SHALL need a tick that the person has checked the release. The grab SHALL claim exactly the episodes named (every episode of the season in the series' data, for a season), SHALL be refused when one of them is in flight and while an import of the series is open, and SHALL be recorded as forced with the rules set aside and the person's name, in the history and the audit. The page SHALL carry a digest that covers, among the rest, the numbering the series follows, what is named, its monitoring and whether each replaced file is kept by hand, and a post made after what it showed changed SHALL send nothing and show the page again. A release with a fixed rule that protects the system, and one on the blocklist, SHALL NOT be offered it. Someone who is not an administrator SHALL NOT be offered it.

#### Scenario: The name gives no episode
- **WHEN** an administrator forces a release whose name states no season or episode, ticking S01E03 of Lantern Bay
- **THEN** S01E03 alone is claimed and downloading, and the grab says it was forced for S01E03 by that person

#### Scenario: Numbered for no episode of the data
- **WHEN** a release is named S02E01 and the series' data has no season 2, and the administrator forces it for S01E09
- **THEN** S01E09 is claimed, and the confirmation said the release's own number is set aside

#### Scenario: A whole season
- **WHEN** an administrator forces a pack whose name is not the series' and chooses "a whole season", season 1
- **THEN** every episode of season 1 in the series' data is claimed

#### Scenario: Nothing named
- **WHEN** the confirmation is posted with no episode and no season chosen
- **THEN** it is refused and nothing is sent

#### Scenario: The numbering changed meanwhile
- **WHEN** the series' numbering is changed after the confirmation was shown and the administrator then confirms
- **THEN** nothing is sent and the page is shown again

#### Scenario: An import of the series is open
- **WHEN** an import of the series is blocked and an administrator confirms "Grab it by force"
- **THEN** it is refused and nothing is sent

### Requirement: An episode's file kept by hand is not upgraded by itself
An episode file that came from a forced grab, from an assignment by hand or from the links page SHALL be marked as made by a person, with who and how, wherever the file is shown, and SHALL be kept by hand: no automatic search and no search by hand SHALL grab a replacement for it, a pack that would replace it SHALL be rejected for an automatic grab with that reason, and the episode's row SHALL say it is kept and by whom. An administrator SHALL be able to clear and to set "kept by hand" on a file's link. A grab from a manual search (Grab this one, or Grab it by force) SHALL replace it only with a tick that names that file, whether or not the release is better, and no one-click grab SHALL replace it. The import of a grab that did not tick the kept file SHALL be blocked for it, and SHALL replace it only when a person chooses "import anyway".

#### Scenario: Below the cutoff
- **WHEN** an episode's file from a forced grab is below the profile's cutoff and the scheduled search runs
- **THEN** nothing is grabbed for that episode

#### Scenario: Let go
- **WHEN** an administrator clears "kept by hand" on that file
- **THEN** the episode is wanted for an upgrade by the ordinary rules

### Requirement: A series' files can be linked to episodes by hand
An administrator SHALL be able, on a series' links page, to link a video file in a TV library folder to one or several episodes of the series, to move a file's link to other episodes, and to unlink a file, for several files in one step, after a review of the whole change. The review SHALL show each episode's files before and after, each episode left without a file and whether it is wanted again, and each file whose name gives another episode than the one it is given. Nothing on disk SHALL be renamed, moved or deleted. When an episode that is given a file holds a file that is not part of the change, both SHALL stay linked unless the person ticks unlinking the other. The change SHALL be applied as one, and SHALL be refused while an import of the series is open, while a grab claims an episode it touches, and when the review is out of date. Each episode it touches SHALL get a history line with the person's name. A link made this way SHALL be marked as made by a person, kept by hand, and stamped with the numbering the series follows. A file unlinked this way SHALL stay unlinked until a person links it or lets Axis manage it again, and SHALL be listed as unlinked by hand. The page SHALL reach only the series' own files: those linked to it, those in a folder that holds a linked file of it, those identification linked to it, and those unlinked by hand from it; no other file SHALL be listed or accepted in a plan, whatever the address or the post names, and the files with no link SHALL be listed a page at a time. A file linked to another series SHALL NOT be taken over: the page SHALL say to unlink it there first. The files the "unlink the other" ticks touch SHALL be part of the review, each with its before and after, and of its digest; a ticked file that keeps another episode SHALL have its remaining link recorded as set by hand, so that nothing adds the removed episode again, and the review SHALL say so in its row. A change SHALL be refused, with the page shown again, when a file it changes was linked to another series after the review. An administrator SHALL be able to let Axis manage one such file again, after a review that says what it does: the file is handed back to identification, which places it by its name, so its episodes are replaced by what the name gives, never added to; the review SHALL list the episodes it will be on, those left without a file and whether each is wanted. When identification would give the file no episode, its link SHALL stay as it is, still marked by hand, and nothing SHALL be confirmed. For a link an import made, only the person's marks SHALL go. For a file unlinked by hand, the mark SHALL go and the review SHALL say where the next run places the file. The mark of a hand unlink SHALL be the file's alone (any series that can reach the file can clear it), and SHALL be cleared when the series it was made for is removed, or when an import by Axis links a file at that path.

#### Scenario: Two files swapped
- **WHEN** an administrator gives the file on S01E03 to S01E04 and the file on S01E04 to S01E03 in one change
- **THEN** each episode has the other's file, both links say they were made by hand, both episodes' history says so, and neither file on disk was touched

#### Scenario: A link on an episode the numbering lacks
- **WHEN** a change of numbering left a file linked to an episode kept as gone, and an administrator moves its link to S01E07
- **THEN** S01E07 has the file, and the file is no longer listed as on an episode the numbering lacks

#### Scenario: A file with no link
- **WHEN** an administrator links a video file in the series' folder that has no link to S01E02 and S01E03
- **THEN** both episodes have that file

#### Scenario: The episode has a file already
- **WHEN** S01E05 has a file that is not part of the change and another file is given S01E05 without the unlink tick
- **THEN** S01E05 has both files, and the review said so

#### Scenario: A grab in flight
- **WHEN** a grab claims S01E04 and a change that touches S01E04 is applied
- **THEN** nothing changes, and the page says to wait for the grab to be imported or dismissed

#### Scenario: Out of date
- **WHEN** a file of the series is imported after the review was shown and the administrator then applies it
- **THEN** nothing changes and the review is shown again

#### Scenario: Handed back to identification
- **WHEN** the file named S01E01 was moved by hand to S01E03 and an administrator lets Axis manage it again
- **THEN** the review says it will be on S01E01 and S01E03 will be left without a file (and whether that is wanted), and after the confirmation the file is on S01E01 alone, also after the next run of identification

#### Scenario: Identification gives nothing
- **WHEN** an administrator lets Axis manage a file linked by hand whose name gives no episode of the series
- **THEN** the page says so, offers nothing to confirm, and the link stays as it is, still marked by hand

#### Scenario: The ticks are part of the review
- **WHEN** an administrator ticks unlinking the other file of an episode and shows the review again
- **THEN** every file the ticks touch is listed with its before and after, a ticked file that keeps another episode is marked as linked by hand and kept, and a confirmation that does not post the ticks the review showed changes nothing

### Requirement: A link made by a person is not moved or removed without being shown
A link marked as made by a person SHALL NOT be listed or moved by "place by its name", by Place all or by the numbering page's tick box. A change of numbering SHALL keep it on its episode as it keeps every link, and its confirmation SHALL list the links made by hand in a group of their own, with the episode each is on before and after.

#### Scenario: Its name says another episode
- **WHEN** a file linked by hand to S01E04 is named for S01E03
- **THEN** the series' page does not list it among the files whose names say another episode, and Place all does not move it

#### Scenario: A change of numbering
- **WHEN** a series with a file linked by hand changes its numbering
- **THEN** the confirmation lists that file with the episode it is on before and after, and after the change it is on that episode

### Requirement: A file's row says what placing that file alone would do
On a series' page, each file listed as named for another episode SHALL show what placing that one file by its name would do, judged alone against the series as it is. What Place all would do with the files together SHALL be shown on Place all's confirmation.

#### Scenario: Two files named for each other's place in a chain
- **WHEN** file A is on S01E02 and named for S01E03, and file B is on S01E03 and named for S01E04, which has no file
- **THEN** A's row says S01E03 has a file of its own, B's row offers placing it, and Place all's confirmation says both are placed

### Requirement: A partly imported download counts as an open import
An import of a series that ended partly imported SHALL count as an open import of that series until it is retried to the end or dismissed: placing files by their names, changing links by hand, a grab by force for the series and a change of numbering SHALL wait for it, and SHALL say that it is partly imported and can be retried or dismissed.

#### Scenario: Placing while a pack is partly imported
- **WHEN** a pack's import ended partly imported and an administrator places a file of the series by its name
- **THEN** nothing is placed, and the page names the import and says to retry or dismiss it

### Requirement: A series has a kind, and Anime is set only by a person or by Sonarr
Each managed series SHALL have a kind, standard or anime, and every series SHALL be standard until an administrator marks it as anime, or until it is migrated from a Sonarr that has it as anime. An administrator SHALL be able to mark a series as anime with a box on the add form of its title page, on the form that manages a series on disk, and on the series' settings card, and the box SHALL never be ticked by itself. The one-step Add of a Discover card or a search result SHALL add a standard series. Axis SHALL NOT mark a series as anime by itself; the add form MAY say beside the box that the stored details describe animation whose original language is Japanese. A change of the kind SHALL be refused for anyone who is not an administrator, SHALL write one line in the series' history, SHALL write nothing when the kind is already the one asked for, and SHALL NOT move a file, change a link, or change the numbering the series follows. For a series that is not marked as anime, matching, importing and searching SHALL be as they are when no series is marked.

#### Scenario: Marked on the Add form
- **WHEN** an administrator adds *Tenkuu Rangers* with the box "Anime" ticked
- **THEN** the series is of the kind anime, and its page shows the card "Anime numbering"

#### Scenario: Never by itself
- **WHEN** a series whose stored details say animation with Japanese as its original language is added without the box ticked
- **THEN** the series is standard

#### Scenario: A viewer
- **WHEN** a viewer posts a change of a series' kind
- **THEN** it is refused, and the kind is unchanged

### Requirement: An anime series' absolute numbers are TheTVDB's, shown, and used only when they can be trusted
An episode's absolute number SHALL be the one TheTVDB states for that episode in the numbering the series follows, copied onto the episode at each refresh and at each change of numbering, and SHALL NOT be counted, inferred, or taken from a file's name, a release, another numbering or another application; under TMDb's numbering an episode has none. The page of an anime series SHALL show each episode's absolute number in a column of its episode table, and SHALL say in a card "Anime numbering" which of four states the series' numbers are in, computed each time the page is shown: no source (the series does not follow TheTVDB's aired order, with a link to the page that changes its numbering, or, without a TheTVDB key, that a key is needed first); none given; usable; or not usable, naming the episodes at fault. The numbers SHALL be usable only when the series follows TheTVDB's aired order, each number is on exactly one episode of seasons 1 and up, and the numbers rise with season and episode. A special SHALL never be matched by an absolute number. When the numbers are usable, the card SHALL say what a search asks for and how a batch is taken; otherwise it SHALL say that the series is searched by season and episode alone.

#### Scenario: Another numbering
- **WHEN** an anime series follows TMDb's numbering
- **THEN** its page says that absolute numbers come from TheTVDB's aired order and links to the page that changes the numbering, and no name is matched by an absolute number

#### Scenario: A number on two episodes
- **WHEN** TheTVDB gives two episodes of an anime series the same absolute number
- **THEN** the page names both episodes, and no name is matched by an absolute number for that series

#### Scenario: Numbers out of order
- **WHEN** the absolute number of S03E03 is lower than that of S03E02
- **THEN** the series' numbers are not usable, and the page says which episodes are out of order

#### Scenario: Usable
- **WHEN** every numbered episode of seasons 1 and up has a number of its own and the numbers rise
- **THEN** the page says for how many episodes TheTVDB gives a number, and whether the numbers equal a plain count of the episodes in aired order or from which episode they differ

### Requirement: An anime series added by hand follows the numbering that carries absolute numbers
An anime series that an administrator adds from its title page SHALL follow TheTVDB's aired order from the start when Axis has a TheTVDB key and TheTVDB lists the series, and the add form SHALL say so. Otherwise, and always for a series whose files identification has linked on disk, it SHALL be added under the numbering any new series gets, and the form SHALL say so. When that numbering is not TheTVDB's aired order, the series' page SHALL say that its absolute numbers have no source.

#### Scenario: With a key
- **WHEN** Axis has a TheTVDB key and an administrator adds a series marked as anime that TheTVDB lists
- **THEN** the series follows TheTVDB's aired order and its episodes carry TheTVDB's absolute numbers

#### Scenario: Without a key
- **WHEN** Axis has no TheTVDB key and an administrator adds a series marked as anime
- **THEN** the series follows TMDb's numbering, and its page says a TheTVDB key is needed for absolute numbers

#### Scenario: Not listed at TheTVDB
- **WHEN** Axis has a TheTVDB key and an administrator adds a series marked as anime that TheTVDB does not list
- **THEN** the series follows the numbering any new series gets

#### Scenario: A series on disk
- **WHEN** an administrator manages a series on disk with the box "Anime" ticked while Axis has a TheTVDB key
- **THEN** the series is of the kind anime and keeps the numbering any new series gets, and the form said so before it was sent

### Requirement: An anime series is migrated from Sonarr like any other
A series that Sonarr has as anime SHALL be migrated as any series is: matched by its TheTVDB identifier, following TheTVDB's aired order, its episodes matched and monitored as any migrated series' are, its files linked to the episodes Sonarr names and shown as migrated from Sonarr, and held until a person starts searching. It SHALL be of the kind anime in Axis, and the preview's row SHALL say so. A file SHALL be linked where Sonarr has it whatever number its name states. Sonarr's own absolute number for each episode of seasons 1 and up SHALL be compared with TheTVDB's, on the preview and by the run, and the run SHALL keep what it found on the series. The preview, the run's report, the series' history and the series' page SHALL say for how many episodes the numbers agree, differ or are missing on one side, and SHALL name the episodes that differ with both numbers, the first 50 of them. A difference SHALL change nothing that is migrated, and a comparison that cannot be made SHALL be said as not compared and SHALL NOT stop the migration. A series of a kind Axis does not know SHALL be skipped.

#### Scenario: Migrated with its files
- **WHEN** Sonarr holds *Hoshi no Kagami* as anime, with two seasons and a file for every episode that Axis sees at the mapped paths
- **THEN** the series is in Axis as anime, every file is linked to the episode Sonarr has it on and says "Migrated from Sonarr", and nothing is searched for

#### Scenario: A file named by its absolute number
- **WHEN** Sonarr has the file `[KumoSubs] Hoshi no Kagami - 14 [1080p].mkv` on S02E02
- **THEN** it is linked to S02E02

#### Scenario: The numbers differ
- **WHEN** Sonarr gives S03E02 the absolute number 29 and TheTVDB, as Axis holds it, gives it 30
- **THEN** the series is migrated as it would be otherwise, and the report says the absolute numbers differ, naming that episode and both numbers

#### Scenario: Listed to migrate
- **WHEN** an administrator reads a Sonarr that holds a series of the type anime
- **THEN** the preview lists it to be migrated, says that it is numbered by season and marked as anime in Axis, and shows the comparison of its absolute numbers

### Requirement: A name numbered absolutely is matched for an anime series through TheTVDB's numbers
For a series of the kind anime whose absolute numbers are usable, a release's or a file's name that states an absolute episode number SHALL be matched to the one episode that has that number, and what it holds SHALL be said in the series' season and episode with the number it came from. For a series that is not marked as anime, such a name SHALL be rejected because it states only an absolute episode number, in the words used for any series; for an anime series whose numbers are not usable, it SHALL be rejected with the state of the numbers. Every number a name states SHALL map, or the name SHALL be rejected whole. A name SHALL be rejected, with a reason a person can read, and never resolved by a guess, when: it states a season together with a bare number; it states a season and an episode that season does not have; it states both numberings and they are not the same episode; a number is on no episode; a number is on an episode that has not aired or has no air date; a number is not a whole number from 1 to 9999; or it names a special by a word such as OVA. A four-digit year SHALL never be read as an absolute number. A name that states a season and an episode SHALL be matched by them, under any of the series' names; when it also states an absolute number, both SHALL be the same episode. The mapping SHALL be a lookup of the number among the series' episodes as they are when the name is read, never arithmetic and never the nearest number. A number written with three or four digits directly after the series' name, with no hyphen and no "E" before it, SHALL be rejected as ambiguous when the series also has the season and episode those digits spell (`101`: S01E01). A range of absolute numbers that is exactly every episode of one or more whole seasons SHALL be taken as their pack. Any other range is a batch: a search judging a release SHALL take it as the episodes it holds (see the requirement on searching and batches), and it SHALL be rejected as the name of a file, of a file on disk or of a release a person grabs by force, so that no file is placed on a range of episodes by its name. A release matched through an absolute number SHALL leave a line in the series' history, when it is grabbed by a search or by a person, that names both numbers; a line that cannot be written SHALL NOT fail the grab.

An absolute number SHALL be read only under the series' own name. Under a name that holds one of the series' names as whole words with more words before it, after it or both, and is not the series' title or original title (a season's or a sequel's name that the provider lists among the series' other names, such as `Tenkuu Rangers 2nd Season`, `Tenkuu Rangers II`, `Tenkuu Rangers Rising` or `Zoku Tenkuu Rangers`), the number counts that part's episodes: the name SHALL be rejected with a reason of its own. A second number after the one read (`- 05 - 06`, `- 05 [06]`, `- 05 & 06`, `- 05 [1080p][06]`), a season stated with a bare number and no hyphen (`S2 05`, `2nd Season 05`, `S2 #05`), a word right after the number that says it is not one episode (`Movie`, `BD`, `PV`, `Complete`, `Vol`, `Part 2`, `of 12`), a year in the name more than one off the series' year, and a name that reads as two different numbers under two of the series' names (`Tenkuu Rangers - 3 - 05` when "Tenkuu Rangers 3" is one of them) SHALL be rejected.

Under a name that is not the series' title or original title, a search SHALL take an absolute number by itself only when the series has one season, specials apart, or the number (a range's first) is higher than the episode count of every season after the first; otherwise the release SHALL be listed with what it holds and left to a person's own grab, since the name may be a part's and the number that part's own count. A name whose title key is the title's but that has punctuation the title does not have (`Tenkuu Rangers!!`) SHALL count as not the title for this; a hyphen, a colon, a comma, a dot and an underscore SHALL NOT count as such punctuation.

A release whose number or range is read only because it follows the series' name, with nothing in the name that marks it as an episode's (`Tenkuu Rangers 137 [1080p]`), SHALL be listed with what it holds and SHALL NOT be grabbed by any search, scheduled or started by a person: only a person's own grab of that release takes it, whichever request of a search returned it. Such a name SHALL NOT be read at all when it states a year more than one off the series', or when another title the search compared could own it: one whose name the release's name also begins with (the same name, or a longer one that takes the number in), or one whose name is the series' name followed by that very number (`Tenkuu Rangers 2` for `Tenkuu Rangers 02`).

The page of a series that is not marked as anime SHALL count the releases its kept searches found that state only an absolute episode number, and SHALL say to mark the series as anime if it is one. The page of an anime series SHALL count the releases its kept searches found that state a season with a bare number.

#### Scenario: Mapped
- **WHEN** absolute 137 of *Tenkuu Rangers* is S06E12 and a release is named `[Tsuki] Tenkuu Rangers - 137 [1080p]`
- **THEN** it holds S06E12, and the page says it came from the absolute number 137

#### Scenario: Not an anime series
- **WHEN** the same release is found for a series that is not marked as anime
- **THEN** it is rejected because it states only an absolute episode number, and the series' page counts it and says to mark the series as anime if it is one

#### Scenario: Both numberings disagree
- **WHEN** a release is named `[NekoFansub] Kumo Academy - S01E05 - 006` and absolute 6 is S01E06
- **THEN** it is rejected because its two numberings name different episodes

#### Scenario: A season and a bare number
- **WHEN** a release is named `[Tsuki] Tenkuu Rangers S2 - 05 [1080p]`
- **THEN** it is rejected because it states a season and an absolute episode number, and it is not read as S02E05

#### Scenario: Not aired
- **WHEN** absolute 140 is an episode whose air date is ahead
- **THEN** a release numbered 140 is rejected, and the reason names the episode and says it has not aired

#### Scenario: A special
- **WHEN** a release of an anime series is named `[Tsuki] Tenkuu Rangers - OVA 2 [1080p]`
- **THEN** it is rejected as a special to assign by hand

#### Scenario: A bare number that is also a season and an episode
- **WHEN** a release is named `[Tsuki] Tenkuu Rangers 101 [1080p]`, and the series has both S01E01 and an episode with the absolute number 101
- **THEN** it is rejected as ambiguous, while `[Tsuki] Tenkuu Rangers - 101 [1080p]` is the episode with the absolute number 101

#### Scenario: A number above the highest
- **WHEN** a release is numbered 300 and the highest absolute number TheTVDB gives the series is 291
- **THEN** it is rejected, the reason names both numbers, and no episode is chosen for it

#### Scenario: A batch of part of a season
- **WHEN** a file of a download is named `[Tsuki] Tenkuu Rangers - 01-12 [1080p].mkv` and season 1 has 24 episodes
- **THEN** it is rejected as a batch of part of a season, the reason says which episodes it would hold and that only a search takes a batch, and the file is placed on nothing

#### Scenario: Numbers that are not usable
- **WHEN** TheTVDB gives two episodes of an anime series the same absolute number
- **THEN** no name is matched through an absolute number for that series, and each such release says that the numbers cannot be used

#### Scenario: A name of part of the series
- **WHEN** the other names of *Tenkuu Rangers* include "Tenkuu Rangers 2nd Season", and a release is named `[Tsuki] Tenkuu Rangers 2nd Season - 05 [1080p]`
- **THEN** it is rejected, and the reason says it is numbered under a name of a part of the series; it is not read as absolute 5

#### Scenario: A word before the series' name
- **WHEN** the other names of *Tenkuu Rangers* include "Zoku Tenkuu Rangers", and a release is named `[Tsuki] Zoku Tenkuu Rangers - 05 [1080p]`
- **THEN** it is rejected as numbered under a name of a part of the series

#### Scenario: Two readings that disagree
- **WHEN** the other names include "Tenkuu Rangers 3", and a release is named `[Tsuki] Tenkuu Rangers - 3 - 05 [1080p]`
- **THEN** it is rejected, and the reason gives both readings

#### Scenario: A low number under another name
- **WHEN** *Tenkuu Rangers* has three seasons of twelve, "Sora no Senshi" is one of its other names, and a search finds `[Tsuki] Sora no Senshi - 05 [1080p]` and `[Tsuki] Sora no Senshi - 17 [1080p]`
- **THEN** the first is listed as holding S01E05 and no search grabs it, a person's "Grab this one" takes it, and the second is a search's to grab for S02E05

#### Scenario: A bare number is a person's to grab
- **WHEN** S06E12 is wanted and the only release a search finds is `[Tsuki] Tenkuu Rangers 137 [1080p]`, which the profile accepts
- **THEN** the search grabs nothing, the release is listed as holding S06E12 with the reason that no search takes it, and a person's "Grab this one" takes it for S06E12

#### Scenario: A year that is not the series'
- **WHEN** *Tenkuu Rangers* is of 2023 and a release is named `[Tsuki] Tenkuu Rangers 05 (2011) [1080p]`
- **THEN** it is not this series' release, and the reason names both years

#### Scenario: Another title could own the name
- **WHEN** the search also looked up a title named "Tenkuu Rangers 2", and a release is named `[Tsuki] Tenkuu Rangers 02 [1080p]`
- **THEN** it is not read as absolute 2, and the reason names the other title

#### Scenario: Two numbers after the hyphen
- **WHEN** a release is named `[Tsuki] Tenkuu Rangers - 05 - 06 [1080p]`
- **THEN** it is rejected, because two numbers follow the series' name

#### Scenario: A season and a bare number with no hyphen
- **WHEN** a release is named `[Tsuki] Tenkuu Rangers S2 05 [1080p]`
- **THEN** it is rejected as a season with a bare number, and is not taken as the whole of season 2

### Requirement: A file placed by its absolute number is placed only where its grab claimed
An import SHALL place a video file whose episodes were read from an absolute number only on episodes its grab claimed, reading the file's own name through the numbers as they are at the import, SHALL leave any other such file in the download and list it to be assigned by hand, and SHALL record in the episode's history the number the file was read by. When any video file of a download is numbered absolutely and cannot be placed (its number is on no episode, on an episode not aired, or on an episode the grab did not claim, or its name is in a doubtful form), the import SHALL place none of the download's files that it read by an absolute number, and each SHALL name the file at fault, since one number that does not fit puts the others in doubt. A file that states a season and an episode, a special named by a word, a file of another title and a file whose name states no episode SHALL NOT stand in the others' way, except as the requirement on searching and batches says for a download grabbed by absolute numbers. For a grab by force, a file named by an absolute number SHALL be placed, file by file, on an episode the person named when its number maps to one of them. A file placed this way SHALL be linked as any import's file, not by hand and not kept, SHALL follow the ordinary rules for the file its episode already has, and a file kept by hand SHALL stay. "Assign by hand" SHALL propose the episode a file's absolute number maps to when the file could be imported as it is, and the series' links page SHALL say which episode a file's name states by its absolute number. "Place by its name" SHALL read an anime series' files through the same numbers, so that a file whose name states a number that is now on another episode is listed, and SHALL never move it by itself.

#### Scenario: Imported
- **WHEN** a grab claimed S06E12 and its download holds `[Tsuki] Tenkuu Rangers - 137 [1080p].mkv`
- **THEN** the file is placed on S06E12, named by season and episode, and the history says it was read by its absolute number 137

#### Scenario: Outside the claim
- **WHEN** the same download also holds a file numbered 138, which the grab did not claim
- **THEN** that file is left in the download and listed for assigning by hand, and so is the file numbered 137: a download whose files are numbered absolutely is placed whole or not at all

#### Scenario: A file whose number is not read
- **WHEN** a grab claimed S06E12 and S06E13 and its download holds `[Tsuki] Tenkuu Rangers - 137 [1080p].mkv` and `[Tsuki] Tenkuu Rangers 2 - 138 [1080p].mkv`
- **THEN** the second file is left because two numbers follow the series' name, and the first is left too, with a reason that names the second

#### Scenario: Several files, each on its episode
- **WHEN** a grab claimed S01E01 and S01E02 and its download holds files numbered 1 and 2, and an opening named `NCOP 01`
- **THEN** the two files are placed on S01E01 and S01E02, and the opening is left with its reason

#### Scenario: TheTVDB moves a number
- **WHEN** a file named by absolute number 30 is on S03E02, and a later refresh gives 30 to S03E03
- **THEN** "Place by its name" lists the file apart as saying another episode than the one it is on, placing every file by its name leaves it where it is, and it is moved only when a person places it

### Requirement: An anime series is searched by absolute number as well, and a batch is taken as the episodes it holds
A search for an episode of an anime series whose absolute numbers are usable SHALL ask by the episode's absolute number as well as by its season and episode; when the numbers are not usable or the episode has none, the search's record SHALL say why it did not ask by an absolute number. A search of a season of an anime series SHALL also ask by the series' title alone, which is how a batch is found, and a search of the whole series SHALL ask by the title alone. Every request of an anime series' search SHALL ask in the TV and the anime categories, and the search's record (the series' history, the list of its searches, the manual search's page) SHALL name each request before what each indexer did with it. Asking in more ways SHALL change nothing of what is taken: every release returned SHALL be matched and judged once by the rules for names numbered absolutely, and a release those rules leave to a person's own grab SHALL stay one.

A release that holds episodes by a range of absolute numbers that is not whole seasons (a batch) SHALL be judged by a search as exactly the episodes the range maps to. It SHALL fit an episode search when it holds the episode, a season search when it holds a wanted episode of the season, and a search of the whole series always. It SHALL be taken only when at least one episode it holds is wanted, every monitored episode it holds is wanted or an upgrade, none is in flight and none has a file kept by hand, and its size SHALL be judged against the sum of its episodes' runtimes. Its grab SHALL claim exactly those episodes, so that the import, which places a file read by an absolute number only on an episode its grab claimed, places no file of it on any other episode. A range one number of which maps to no episode, or to an episode that has not aired, SHALL be rejected, naming the number.

A download grabbed by absolute numbers (an anime series' grab, other than a grab by force, whose release name states absolute numbers and no season and episode, decided by what the name states and not by what its numbers map to at the import) SHALL have every file held to that claim, however the file's own name states its episode. When one of its files states an episode outside the claim, or when the names of its files do not account for every episode it was grabbed for while one video file does not say which episode it is, the import SHALL place none of its files and SHALL wait for a person, with the grab still open. When such a download is imported and an episode it was grabbed for got no file, the grab's record and the series' history SHALL say so, and while that episode has no file no search SHALL grab a release of that exact name again; a person's own grab MAY take it. A file placed from such a grab SHALL NOT be kept by hand and SHALL be upgraded by the ordinary rules.

For one number and for a range alike, a season or a part of the series stated after the number, in a bracket or bare, as a number, an ordinal or a word (`[2nd Season]`, `Second Season`, `[Season 1+2]`, `(Part 2)`, `Cour 2`, `[S2]`), a word anywhere after the series' name for something that is no episode (a soundtrack, a manga, a novel, an artbook, a trailer, a sample, a recap, a chapter), and a range in a form that is read only in part (`01-12v2`, `E01-E12`, `01-12, 14`) SHALL be rejected.

#### Scenario: Found by its number
- **WHEN** the only release of S06E12 an indexer has is named `[Tsuki] Tenkuu Rangers - 137 [1080p]`, and the indexer returns it only for the words `Tenkuu Rangers 137`
- **THEN** an automatic search of S06E12 finds it and grabs it for S06E12

#### Scenario: Not asked by number
- **WHEN** an episode of an anime series whose absolute numbers are not usable is searched for
- **THEN** it is asked for by season and episode alone, in the TV and the anime categories, and the search's record says why no absolute number was asked

#### Scenario: A batch across seasons
- **WHEN** `[Hikari-Raws] Sword of the Silver Moon - 01-28 [BD 1080p]` maps to S01E01 to S02E04, all monitored and wanted
- **THEN** it is grabbed for those 28 episodes, and each file is imported into its own season under its season and episode

#### Scenario: A batch that holds an episode not wanted
- **WHEN** a batch of absolute 1 to 12 holds S01E03, which is monitored and in the library at its profile's cutoff
- **THEN** the batch is not taken, and the reason names S01E03

#### Scenario: A number that does not map
- **WHEN** a batch is numbered 280 to 300 and the series has 291 numbered episodes
- **THEN** it is rejected, and the reason names 292, the first number no episode has

#### Scenario: A batch a person must take
- **WHEN** a season search finds `[Tsuki] Tenkuu Rangers 01-12 [1080p]`, whose range is read only because it follows the series' name
- **THEN** it is listed as holding those twelve episodes, no search grabs it, and a person's "Grab this one" takes it

#### Scenario: A file of a batch that does not say which episode it is
- **WHEN** a batch was grabbed for S01E03 to S02E02 and its download holds files numbered 3, 4 and 6 and a file named `05.mkv`
- **THEN** no file is placed, each says that `05.mkv` does not say which episode it is and that the names account for 3 of the 4 episodes, and the import waits for a person with the grab still open

#### Scenario: A batch that lacks a file
- **WHEN** the same batch's download holds files numbered 3, 4 and 6 and no other video
- **THEN** the three files are placed, each in its own season's folder, the history says that S02E01 got no file, and no search grabs that release again while S02E01 has no file

#### Scenario: A file of a batch named for an episode it was not grabbed for
- **WHEN** a release was grabbed by absolute numbers for S01E03 and S01E04 and its download holds files named for S02E01 and S02E02
- **THEN** neither file is placed on any episode, and the import waits for a person

#### Scenario: A season after the number
- **WHEN** a search finds `[Tsuki] Tenkuu Rangers - 01-12 [2nd Season] [1080p]` or `[Tsuki] Tenkuu Rangers - 05 (Part 2) [1080p]`
- **THEN** each is rejected as stating a season or a part beside an absolute number, and no search grabs it

### Requirement: An administrator can replace a series' download in flight
When an administrator asks to grab a release by hand or by force for a series and the only obstacle is one open grab that holds an episode the release holds and whose download is queued, downloading or paused at its client, has not completed and has no import, the confirmation SHALL say which release is downloading, which episodes it holds, at which client and how far it is, and SHALL offer "Replace the download in flight with this one" behind a tick. These rules of a movie's replace SHALL hold for a series: the confirmation writes nothing; it is an administrator's with the form's token; the other rules of a grab by hand or by force stand; a grab in the way that cannot be replaced (arrived, being sent, still being removed, failed and not yet dealt with, the very release chosen, the same torrent) gets no offer and the page says why; the checks that can be made first are made first; the old grab is closed with the person's name and its download removed; the release replaced is not blocked; no search is queued; nothing is sent while anything of the old download is left at its client; a send that fails afterwards leaves the episodes wanted again, said on the page; a download that has completed or has an import is not replaced and the page links to the import, or to the Downloads page while no import exists yet; a post whose grab or digest is no longer what the confirmation showed changes nothing and shows the confirmation again.

The grab in the way SHALL be ended whole: no part of a download is removed and another part kept. When it holds episodes the chosen release does not hold, the confirmation SHALL name them and say that they stop downloading and go back to ordinary searching, and replacing SHALL need a tick of its own for them. After the replace those episodes SHALL be wanted again under the ordinary rules.

When more than one grab stands in the way, no replace SHALL be offered: the confirmation SHALL name each grab with its episodes and link to the Downloads page.

What a movie's replace never has deleted SHALL hold for a series: a torrent's client is asked where the torrent is before it is asked to remove it with its data, each time, and is not asked to delete when the place is the top of a disk, a library folder, in one or holding one, or cannot be judged. A client that cannot be asked where the torrent is before the grab is ended SHALL leave everything as it was: nothing ended, nothing sent. A replaced download that finishes at its client where the client is not asked to delete SHALL be left there, its removal ended as kept and said on the Downloads page and in the series' history. When the claim of the chosen release fails with an error after the old download was removed, the error shown SHALL say that the old download was removed, that the new release was not sent, and why. When a search claims the episodes in the moment between the removal and the person's claim, the person's release SHALL NOT be sent, and the page SHALL say that a search grabbed another release, without naming it.

Giving up on a series' torrent, blocking it and searching again, or its failure SHALL follow the movie's rule: a torrent that its client says is in a library folder is removed from the client without its data and its files are left; a place that cannot be judged is removed with its data, as before.

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks for the confirmation or the replace
- **THEN** it is refused and nothing changes

#### Scenario: It completes while the person confirms
- **WHEN** the download in the way completes after the confirmation was shown and before the post
- **THEN** the post ends nothing and sends nothing, and the confirmation is shown again

#### Scenario: An episode in place of an episode
- **WHEN** an administrator replaces the download of S01E03 with another release of S01E03
- **THEN** the old grab is closed as replaced by that person, its download is removed with its data, the chosen release is sent, and S01E03 has one open grab

#### Scenario: A pack in flight
- **WHEN** a season pack of S01 is downloading and an administrator replaces it with a release of S01E03, with both ticks
- **THEN** the pack's download is removed whole, the release of S01E03 is sent, and the other episodes the pack held are wanted again and named on the page

#### Scenario: Without the tick for the others
- **WHEN** the grab in the way holds episodes the chosen release does not, and the form is posted without their tick
- **THEN** nothing is ended and nothing is sent

#### Scenario: Two grabs in the way
- **WHEN** a pack is chosen while two of its episodes are downloading as two grabs
- **THEN** no replace is offered, the page names both grabs and links to the Downloads page, and a post that asks for a replace is refused

#### Scenario: It has completed
- **WHEN** the download in the way has completed, or an import of it exists
- **THEN** no replace is offered, and the page links to the import


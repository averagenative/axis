# movies Specification

## Purpose
Films Axis manages: adding one, its state and why, searching for and grabbing the best acceptable release once, handling failures, and its history.
## Requirements
### Requirement: Films are added from their title page
An administrator SHALL be able to add a film from its title page with a quality profile and a library folder, and a film SHALL be managed at most once.

#### Scenario: Add
- **WHEN** an administrator adds a film
- **THEN** it appears among the managed films with its state

#### Scenario: Twice
- **WHEN** a film already managed is added again
- **THEN** it is refused

### Requirement: A film's state is shown and explained
Each managed film SHALL show one state derived from the facts (not available, wanted, searching, downloading, to import, in library, upgrade wanted, needs attention), and a film that needs attention SHALL say why.

#### Scenario: Before release
- **WHEN** a film's "Search from" setting has not been reached
- **THEN** its state is not available and it is not searched for

#### Scenario: Nothing acceptable
- **WHEN** a search finds releases and the profile rejects them all
- **THEN** the film needs attention and the reasons are shown

### Requirement: The best acceptable release is grabbed, once
Searching for a film SHALL read each release found, reject those that do not match the film, are blocklisted or are rejected by its profile, and send the highest-ranked remaining release to a download client. A film SHALL NOT have two grabs in flight.

#### Scenario: Grab
- **WHEN** a wanted film is searched and acceptable releases exist
- **THEN** the highest-ranked is sent to a download client and the film is downloading

#### Scenario: Wrong film
- **WHEN** a release found does not match the film, or matches ambiguously
- **THEN** it is not grabbed

#### Scenario: Searched twice at once
- **WHEN** two searches for one film run at the same time
- **THEN** one release is grabbed

### Requirement: Every candidate keeps its verdict
For each search the daemon SHALL keep every release considered with its verdict and reasons, and SHALL show them on the film's page.

#### Scenario: Why not
- **WHEN** an administrator opens a film after a search
- **THEN** each release is listed as accepted or rejected with its reasons

### Requirement: A failed release is not tried again
When a film's download fails, stalls beyond the limit or disappears, the release SHALL be blocklisted for that film and the next best acceptable release SHALL be sought.

#### Scenario: Failure
- **WHEN** a film's download fails
- **THEN** that release is blocklisted and another search follows

#### Scenario: None left
- **WHEN** no acceptable release remains
- **THEN** the film needs attention

### Requirement: Wanted films are searched for without hammering indexers
Wanted films SHALL be searched automatically when they become available and on a schedule, least recently searched first, with the time between searches of one film growing while nothing is found.

#### Scenario: Pacing
- **WHEN** many films are wanted
- **THEN** only a limited number are searched in each run

### Requirement: A finished download waits for import
When a film's download completes the film SHALL be marked to import and the files SHALL be left where they are.

#### Scenario: Completed
- **WHEN** the download client reports the film's download complete
- **THEN** the film's state is to import and nothing has been moved

### Requirement: Acting on films is for administrators
Any logged-in user SHALL be able to see managed films and their states. Adding, searching, grabbing and removing SHALL be for administrators, and each SHALL be recorded in the film's history.

#### Scenario: Viewer
- **WHEN** a non-administrator opens the Films tab
- **THEN** the films are shown and no action is offered or accepted

### Requirement: A film's finished download is imported
When a managed film's download completes, the daemon SHALL import it, and on success the film SHALL be in the library with its file linked to it.

#### Scenario: Imported
- **WHEN** a film's download completes and its import succeeds
- **THEN** the film's state is in library

#### Scenario: Blocked
- **WHEN** the import is blocked
- **THEN** the film needs attention and links to the blocked import

### Requirement: Films already on disk are in the library
A managed film with an identified file SHALL be in the library and SHALL NOT be searched for, and a film added after its file was identified SHALL be in the library at once.

#### Scenario: Already there
- **WHEN** a film whose file is identified is added
- **THEN** its state is in library and nothing is searched

### Requirement: A search judges releases against the file on disk
A search for a film that has a file SHALL judge each release against the best of its files present on disk, and SHALL NOT grab a release that is not wanted over that file.

#### Scenario: At the cutoff
- **WHEN** a film's file meets its profile's cutoff and a search finds acceptable releases
- **THEN** none is grabbed, and the search says none is wanted over the file in the library

#### Scenario: Better than the file
- **WHEN** a search finds a release of a better quality than the file on disk
- **THEN** that release is grabbed, and a release of the file's own quality is not

### Requirement: Identified films can be added together
An administrator SHALL be able to have Axis manage every identified film of a root in one action, with a quality profile and a monitoring choice; each keeps its file where it is, and none is searched for.

#### Scenario: Bulk add
- **WHEN** an administrator manages all identified films of a root as unmonitored
- **THEN** each is managed, in library, and none is searched for

### Requirement: The films list works for a large collection
The movies list SHALL remain usable with thousands of entries, managed films and movies on disk alike, reading only what a page needs: a page SHALL cost a number of statements that does not grow with the number of entries or with the number of posters it shows, and SHALL read the facts of its own entries alone.

#### Scenario: A page
- **WHEN** a page of the films list is requested
- **THEN** only that page's films and the counts are read

#### Scenario: Thousands on disk
- **WHEN** five thousand films are managed and three thousand movies are on disk
- **THEN** every page, in every order and in the in-library tab, costs at most five statements, lists its entries once and in order, and paging forwards and back reaches every entry once

### Requirement: "Not out" is told from "Wanted" at a glance
On the movies lists, the "Not available yet" state mark SHALL differ from "Wanted" in form (a dashed outline and slanted words), using only the themes' existing variables.

#### Scenario: A list with both
- **WHEN** a list shows a movie not out yet and a wanted one
- **THEN** their marks are visibly different in every theme

### Requirement: The whole movie library is listed
The movies list SHALL hold every film Axis manages and every movie that identification has linked to a file present in a root for movies, one entry per title whatever the number of its files, folders and disks. A movie on disk that Axis does not manage SHALL be in library, with its poster, year, quality and size, in the same views, filters, sorts and state counts as a managed film, and SHALL say that Axis does not manage it. An entry with more than one copy on disk SHALL say how many, and its page SHALL list every copy.

#### Scenario: Movie on disk
- **WHEN** identification has linked a folder on disk to a movie nobody added
- **THEN** the movies list shows it as in library and not managed, with its quality and size, and the in-library and all tabs count it

#### Scenario: Two copies
- **WHEN** a movie has an identified folder on each of two disks
- **THEN** it is one entry that says 2 copies, and its page lists both files

#### Scenario: Name and state filters
- **WHEN** the list is filtered by name or by the in-library state
- **THEN** the movies on disk that match are listed with the managed films, and a filter by another state lists none of them

### Requirement: Nothing is asked of a movie on disk
A movie on disk that Axis does not manage SHALL NOT be monitored, and nothing SHALL be searched for, grabbed, upgraded, renamed, moved or written for it until an administrator has Axis manage it. Showing it SHALL write nothing and SHALL change nothing on disk, and the scheduler SHALL NOT consider it.

#### Scenario: Looking writes nothing
- **WHEN** the list, the title page and the dashboard are opened with movies on disk
- **THEN** no film row, file link or job is made, and every file and name on disk is as it was

#### Scenario: Scheduler
- **WHEN** the scheduler runs with movies on disk and one wanted film
- **THEN** it considers the wanted film alone and queues no search for any movie on disk

#### Scenario: Wanted
- **WHEN** the Wanted page is opened
- **THEN** no movie on disk is on it

### Requirement: A movie on disk can be managed from its page
An administrator SHALL be able to have Axis manage a movie on disk from its title page, choosing a quality profile, a destination and whether it is monitored for upgrades (not monitored by default). It SHALL then be a managed film with its files, in library, and nothing SHALL be searched for. Anyone else SHALL see that it is not managed and no form.

#### Scenario: Manage
- **WHEN** an administrator manages a movie on disk without monitoring
- **THEN** it is a managed film, not monitored, in library with its identified file, no search is queued, and its page says Axis manages it now

#### Scenario: Viewer
- **WHEN** someone who is not an administrator opens a movie on disk
- **THEN** its state and files are shown, with no form, and it says an administrator can have Axis manage it

### Requirement: Removing a film never deletes its files
Removing a managed film SHALL NOT delete, move or change a file. When identification links its file on disk, the movie SHALL stay in the list as in library and not managed, and the administrator SHALL be taken to its page, which says so; its history SHALL be shown there. The confirmation SHALL say beforehand that it stays.

#### Scenario: Removed with its file on disk
- **WHEN** an administrator removes a managed film whose identified file is on disk
- **THEN** the file is still there, the movie is listed as in library and not managed, and its page says Axis no longer manages it, with its history

#### Scenario: Removed before identification looked at it
- **WHEN** an administrator removes a managed film whose file an import linked and identification has not looked at yet
- **THEN** no file is touched, the list is shown, and nothing says the movie stays listed

### Requirement: A disk that is away never takes a movie out of the list
A movie whose files are on a disk that is offline SHALL stay in the list and in its counts, and it and its file SHALL say that the disk is offline. A movie on disk whose files a scan of an online disk found gone SHALL no longer be listed.

#### Scenario: Disk offline
- **WHEN** the disk holding a movie on disk goes offline
- **THEN** the movie is still listed and counted as in library, its page says its disk is offline and that this never means it is gone, and its file is marked "Disk offline"

#### Scenario: One copy away
- **WHEN** one of the two disks holding copies of a movie goes offline
- **THEN** the movie is one entry with its two copies, and says one of its disks is offline

#### Scenario: File gone
- **WHEN** a scan of an online disk finds a movie on disk's only file gone
- **THEN** the movie is no longer listed

### Requirement: What identification holds back is said on the list
The movies list SHALL say how many folders and files identification holds for a person, and how many wait for the metadata provider, and SHALL link to the Identify page, or say that Identify is switched off when it is.

#### Scenario: Held for review
- **WHEN** identification holds two folders for a person and waits for the provider about one
- **THEN** the list says that two movie folders and files are not listed yet and that one more waits for the provider, with a link to review them

#### Scenario: Identify off
- **WHEN** the Identify module is switched off
- **THEN** the list says so instead of linking to it

### Requirement: The dashboard counts the whole library
The dashboard SHALL count every entry of the movies list, saying how many are in the library, and its list of movies added lately SHALL be the films a person added.

#### Scenario: Movies on disk on the dashboard
- **WHEN** four movies are on disk and one wanted film was added
- **THEN** the dashboard says 5 movies, 4 in the library and 1 wanted, and lists the added film under movies added lately

### Requirement: A movie can be added in one step from Discover
On a Discover card of a movie that is neither managed nor on disk, the Movies module SHALL offer an administrator a one-step "Add to Movies" with the quality profile and library folder that the bar above the list holds (the person's own choice, else the defaults its title page's form selects: the default quality profile, else the first usable one; the first usable library folder, which is the online one with the most free space), the default availability, monitored, and a search queued at once (the title page form's "Search now" box, under "Options"), and an "Add with options" link to the title page; without a usable profile or library folder it SHALL offer only the link. For a movie it manages or has on disk it SHALL show its state mark. The add SHALL come back to the page it was made from (`back`, when it is a safe local path) with `done=added`.

#### Scenario: Adding from Discover
- **WHEN** an administrator presses "Add to Movies" on a Discover card
- **THEN** the movie is added with the profile and library folder of the bar above the list, its search is queued, and the browser is sent back to the Discover page, where the card shows its new state

#### Scenario: Nowhere to put it
- **WHEN** no library folder for movies is on a disk that is online
- **THEN** the card offers only the "Add with options" link to the title page, which says why it cannot be added

#### Scenario: Not a local path
- **WHEN** an add is posted with a `back` that is not a path on this site
- **THEN** the browser is sent to the title page, as without one

### Requirement: A film that needs attention says why and where
A film that needs attention SHALL say why in the words of its state, and its title page SHALL link that statement to what puts it right on the page (the box of what can be done, the settings card, the blocked import, the Downloads page or the Disks page, by the reason). The Movies list, in every view, and Wanted SHALL link the reason (and the list the state's mark) there; the dashboard SHALL say the most common reason after the count.

#### Scenario: The list
- **WHEN** a film's last search found one release and its profile rejected it
- **THEN** the Movies list's reason and Wanted's link to the box of what can be done on its page

### Requirement: What resolves a film's problem is next to it
Next to a film's state, its page SHALL offer what resolves the reason it needs attention: for releases the profile rejected, Grab it anyway, a form "Use another quality profile for this movie" that lists every usable profile marked by what the search's own decision would make of those releases (saved through the film's edit, which searches again) and a link to the profile's page; for every other reason an action or a link. An administrator SHALL get the actions; anyone else SHALL get the words and no form.

#### Scenario: Another profile accepts the release
- **WHEN** an administrator opens a film whose only release Small 1080p rejected as too large and Balanced 1080p accepts
- **THEN** its page marks Balanced 1080p as accepting it, and choosing it makes Balanced 1080p the film's profile, queues a search with the cause "profile", and that search grabs the release

#### Scenario: A viewer
- **WHEN** someone who is not an administrator opens that film
- **THEN** the reason and that an administrator can grab a release anyway or use another profile are shown, with no form and no grab button

### Requirement: A new quality profile searches a film again
When an administrator changes a film's quality profile, the film SHALL be searched for again at once with the cause "profile" when it is monitored and wanted under the new profile (missing, below its cutoff, or its last search needing attention) and may be searched for now; the history and the notice SHALL say so, or say why not. A change of destination alone SHALL search for nothing; a film that is not monitored SHALL NOT be searched for.

#### Scenario: Not monitored
- **WHEN** the profile of a film that is not monitored is changed
- **THEN** no search is queued, and the notice says the movie is not monitored and that Search for this movie searches for it

### Requirement: The film's settings card
A managed film's page SHALL have a card "Settings for this movie" (anchor `settings`) with its quality profile, destination, availability and monitoring once (fields for an administrator, values for anyone else), and Remove this movie last in its own row, set apart.

#### Scenario: An administrator
- **WHEN** an administrator opens a managed film
- **THEN** it has "Settings for this movie" with "Remove this movie", and no "Options" card

### Requirement: A film can be searched by hand
An administrator SHALL be offered Manual search for a managed film, beside Search for this movie. A manual search SHALL ask every enabled indexer now and store every release it found with its verdict and reasons, and SHALL NOT grab anything by itself. It SHALL run whether or not the film is wanted now (not out yet, in the library at its cutoff, not monitored); only a film that was removed SHALL stop it. It SHALL NOT change the film's last search, next search or wait, nor the state the film shows. The history SHALL say who asked for it and what it found.

#### Scenario: Not out yet
- **WHEN** an administrator asks for a manual search of a film that is not out yet
- **THEN** it is searched for with the cause "list", its releases are stored, nothing is grabbed, and its next automatic search does not change

### Requirement: The film's manual search page lists everything, grouped
The film's manual search page SHALL list every release the search found, in the groups acceptable under the profile, rejected by the profile, not what this search asked for, and ruled out (another film, not certainly this film, a sample, on the blocklist), each row with its reasons; sorted and filtered by links as a series' is; and while the search runs it SHALL say so and follow it.

#### Scenario: Ruled out
- **WHEN** a listed release's indexer says it is another film
- **THEN** it is under "Ruled out" with why, and has no "Grab this one"

### Requirement: An administrator can grab any listed release of this film by hand
An administrator SHALL be able to take any listed release that is this film, whatever the profile and the automatic rules say, after a confirmation that says in one sentence that it fills the film, which file in the library it replaces and whether that is an upgrade or a downgrade, and that the quality profile is set aside by hand; a downgrade SHALL need a second, explicit tick. A release a fixed rule rules out and one on the blocklist SHALL NOT be taken through this confirmation; a release ruled out only by fixed rules about its name SHALL be offered "Grab it by force" instead. The grab SHALL be recorded with the person's name and "by hand". Someone who is not an administrator SHALL see the list and no grab.

#### Scenario: The profile rejected it
- **WHEN** an administrator confirms "Grab this one" on a release the film's profile rejected as too large
- **THEN** it is sent, and the history says it was grabbed by hand with the profile set aside

#### Scenario: A fixed rule, through the ordinary form
- **WHEN** a post to "Grab this one" names a release that could be more than one title
- **THEN** it is refused and nothing is sent

### Requirement: Adding a movie from a phone
A movie's add form SHALL keep its fields (quality profile, destination, Search from, monitored, search now) in a disclosure followed by the add button. On a screen at most 40rem wide the disclosure SHALL be closed and its summary SHALL name the quality profile and the destination the form holds, so that adding with the defaults is one tap on the button; wider, the fields SHALL show in the form as before. After adding, the title's page SHALL say the movie was added and show its state and what happens next.

#### Scenario: One tap with the defaults
- **WHEN** an administrator opens the page of a movie that is not added
- **THEN** the form's fields are inside a disclosure whose summary names the default quality profile and destination, and the button "Add to Movies" follows the disclosure

#### Scenario: The page that confirms it
- **WHEN** a movie has been added with a search asked for
- **THEN** its page says "The movie was added." and shows the state Searching with "A search is queued."

### Requirement: A film's search is answered by the work in hand
Search for this movie and Manual search on a film's page SHALL make nothing new while a search of their kind of the film is queued or running, or ended less than a minute ago; the answer SHALL say where the one in hand stands, or what the one that ended found and from when one can be asked again. While that lasts the buttons SHALL say so and not be pressed (Manual search leads to its page), drawn by the server.

#### Scenario: Five clicks on Search now
- **WHEN** an administrator clicks Search for this movie five times quickly
- **THEN** one search is queued, each later answer comes back to the film's panel saying that a search of the movie is waiting for its turn, and the button says "Waiting for its turn"

#### Scenario: Searched a moment ago
- **WHEN** the film's search ended a few seconds ago and an administrator clicks Search for this movie
- **THEN** nothing is queued and the answer says what it found and from when it can be asked again

### Requirement: A film's page says what is happening
A managed film's state block SHALL say what is happening to it: its download at its client (how far, how much) by the state's own line and bar, and in a line of its own its search in hand (running since when, or waiting and how many ahead), else for fifteen minutes what its last search found. The block SHALL be fetched again, with an administrator's buttons out of band, when the hub says one of the film's searches or downloads changed; without the stream the page SHALL be right as loaded.

#### Scenario: The state block followed
- **WHEN** the film's page is open and its search starts
- **THEN** the block's line and the buttons change without a reload

### Requirement: A film's actions come back to their card
Every action of a film's page SHALL come back to the card it was posted from (the film's panel, the releases, the blocklist, the settings card), with the notice drawn inside it.

#### Scenario: Save the settings
- **WHEN** an administrator saves Settings for this movie
- **THEN** the browser comes back to that card, and the notice is in it

### Requirement: An administrator can block a release
An administrator SHALL be able to put a release on a movie's blocklist by hand. Each such entry SHALL record who made it and when, and the movie's blocklist SHALL say of every entry how it got there: a failure with its reason, a person's block, or a download a person gave up on. No search, grab by hand or grab by force SHALL take a release that is on the blocklist. A person's entry SHALL match the release by its info-hash when that is known, and by its folded name from any indexer; a failure's entry SHALL keep matching only the indexer it came from. Every block SHALL be written to the movie's history with the user, and every action here SHALL be an administrator's, checked for the form's token.

"Block this release" SHALL be offered on each row of a manual search that could be grabbed. It SHALL be one post that changes nothing but the blocklist and the history, and the page SHALL come back at that row, which SHALL say "On the blocklist (blocked by NAME)" from then on and link to the blocklist. A manual search's rows SHALL show the blocklist as it is now.

"Block and search again" SHALL be offered for a download Axis grabbed for a movie, on the Downloads page while it is in progress or finished, and on a file's row of the movie's page where the file's release is known. It SHALL lead to a confirmation that writes nothing. When the grab is open, confirming SHALL block the release and give up on the download through the same path a failed download takes: the grab closed, the client asked to remove the download with its files, and the next best release looked for under the rules of that path. When the grab is closed, confirming SHALL only block the release: no library file, link, import, download or job SHALL be changed, and the page SHALL say so. This action SHALL NOT delete or unlink a file of the library.

One entry SHALL be removable from the blocklist by itself ("Unblock").

#### Scenario: From a manual search
- **WHEN** an administrator selects Block this release on a row of a movie's manual search
- **THEN** the release is on the movie's blocklist with their name and the time, the page shows that row again saying who blocked it, nothing was grabbed, and the next scheduled search takes another release

#### Scenario: The same name from another indexer
- **WHEN** a person blocked a release and another indexer lists a release of the same name later
- **THEN** a search does not take it, and its reason says who blocked it

#### Scenario: A download that is not wanted
- **WHEN** an administrator opens Block and search again for a movie's download in progress
- **THEN** the page says what it would do and writes nothing; once confirmed, the release is on the blocklist by that person, the grab is closed as given up on, the client is asked to remove the download, and one search for the next best is queued

#### Scenario: A release that is in the library already
- **WHEN** an administrator blocks the release a movie's present file came from
- **THEN** the release is on the blocklist for the future, and the file on disk, its link to the movie, its import, its download and the jobs are as they were, which the page says

#### Scenario: One release taken off
- **WHEN** an administrator selects Unblock on one entry of a blocklist that has two
- **THEN** that entry is gone, the other stays, and a manual search's row of the first can be grabbed again

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks for any of these
- **THEN** it is refused and nothing changes; a viewer still sees that a release is on the blocklist and who blocked it

### Requirement: A migrated movie is not searched for until a person starts it
A movie migrated from Radarr SHALL be held: no scheduled search, no search for an upgrade, no search after a change of its quality profile and no follow-up after a failed grab SHALL run for it until an administrator starts searching for the connection's movies or for that movie ("Let Axis search for it"), or the run's tick starts it. A held movie that would read as wanted SHALL be marked "Held" in the movies list and on its page, and its page SHALL say that Axis does not search for it until searching is started; a held movie whose migrated file is kept SHALL NOT say so, since starting changes nothing for it. "Search for this movie" and Manual search by an administrator SHALL end the movie's hold. A movie that Radarr has a file for which Axis does not see SHALL stay held when the others are started, until a later run finds its file or an administrator starts that movie.

#### Scenario: Held through the scheduler
- **WHEN** a monitored movie without a file was migrated and the scheduler runs three times
- **THEN** it was not searched for, and its page says Axis does not search for it until searching is started

#### Scenario: One movie, by a person
- **WHEN** an administrator presses "Search for this movie" on a held movie that has no file
- **THEN** it is searched, and it is held no longer

#### Scenario: A file Axis does not see
- **WHEN** Radarr has a file for a movie, Axis does not see it, and searching is started for the migrated movies
- **THEN** that movie stays held and its page says why

### Requirement: A movie's file that was migrated is marked and kept
A file linked by migrating SHALL be shown as migrated, with the app, the administrator who confirmed the run and the date. It SHALL be kept, whatever the movie's quality profile says and whether or not the movie is held: no automatic search and no "Search for this movie" SHALL look for an upgrade of it, a one-click grab SHALL be refused, and an import SHALL NOT replace it unless an administrator grabbed the release by Manual search and confirmed there that it replaces the file. Its pages SHALL say "Migrated from Radarr; Axis does not replace it" and SHALL NOT name a person as keeping it. An administrator SHALL be able to let it go: one file with "Let Axis upgrade it again" on the movie's page, or every file the connection keeps this way with "Let Axis upgrade these files" on the connection's page. That release SHALL be confirmed on a page that writes nothing, SHALL let go of no file a person kept or linked by hand, and SHALL search nothing at once. Letting Axis manage such a file again on the links page SHALL end its keeping and SHALL leave its mark as migrated, so that a later run does not keep it again.

#### Scenario: Shown
- **WHEN** an administrator opens the files of a movie whose file was migrated
- **THEN** the file says "Migrated from Radarr", with the administrator's name and the date, and that Axis does not replace it

#### Scenario: Not upgraded by itself
- **WHEN** a migrated file is below its profile's cutoff and the movie is no longer held
- **THEN** the movie wants no upgrade and no automatic search runs for one

#### Scenario: Let go
- **WHEN** an administrator confirms "Let Axis upgrade these files" for the connection, and a movie that is not held has a migrated file below its profile's cutoff
- **THEN** the file is no longer kept, the movie wants an upgrade and is searched for one at the ordinary pace, and a file a person kept or linked by hand is as it was

### Requirement: A release a rule about its name refuses can be grabbed by force for this movie
On a movie's manual search, an administrator SHALL be offered "Grab it by force" on a release whose every fixed rule is about its name (not the title asked for, more than one title fits, another year, another title's identifier at the indexer, no readable title). Its confirmation SHALL state each rule being set aside in plain words, that the grab is for this movie and no other, what its import does, and each file in the library it would replace with its quality. The grab SHALL need a tick that the person has checked the release, and a tick of its own for each file it would replace, whether or not the release is better; without them nothing SHALL be sent. The page SHALL carry a digest of what it shows, among it the movie's title and quality profile, and a post made after any of it changed SHALL send nothing and show the page again. The grab SHALL be recorded as forced with the rules set aside, in the history and the audit, with the person's name. A release with a fixed rule that protects the system (a sample, password-protected, an archive's or a program's extension, one part of an upload) and one on the blocklist SHALL NOT be offered it, and a post to force one SHALL be refused. Someone who is not an administrator SHALL NOT be offered it.

#### Scenario: Several stored titles share the name
- **WHEN** a manual search of Copper Heron (1995) lists a release named without a year while Copper Heron (2011) is also stored, and an administrator confirms "Grab it by force" with the checked tick
- **THEN** it is sent for Copper Heron (1995), the grab records the rule "it could be more than one title" as set aside, and the history says it was grabbed by force by that person

#### Scenario: A file would be replaced
- **WHEN** the movie has a file and the administrator confirms without ticking that file
- **THEN** it is refused and nothing is sent, and with the tick it is sent and the file is recorded as the one to replace

#### Scenario: The page is out of date
- **WHEN** a file of the movie is imported after the confirmation was shown and the administrator then confirms
- **THEN** nothing is sent, and the confirmation is shown again with the new file

#### Scenario: Password-protected
- **WHEN** a listed release is password-protected
- **THEN** it has no "Grab it by force", and a post to force it is refused

#### Scenario: A viewer
- **WHEN** someone who is not an administrator opens the manual search page
- **THEN** no release has "Grab it by force", and a post to force one is refused

### Requirement: A movie's file kept by hand is not upgraded by itself
A movie's file that came from a forced grab, from an assignment by hand or from the links page SHALL be marked as made by a person, with who and how, wherever the file is shown, and SHALL be kept by hand: no automatic search and no Search for this movie SHALL grab a replacement for it, and the movie's page SHALL say that it is kept, by whom, and how to let it go. An administrator SHALL be able to clear and to set "kept by hand" on a file's link. A grab from a manual search (Grab this one, or Grab it by force) SHALL replace a kept file only with a tick that names that file, whether or not the release is better. No one-click grab (Grab, Grab it anyway) SHALL be offered for a movie whose file is kept, and a post to one SHALL send nothing. The import of a grab that did not tick the kept file SHALL be blocked for it, and SHALL replace it only when a person chooses "import anyway". A forced grab that fails SHALL NOT be followed by an automatic search.

#### Scenario: Below the cutoff
- **WHEN** a movie's file from a forced grab is below its profile's cutoff and the scheduled search runs
- **THEN** nothing is grabbed, and the movie's page says the file is kept by hand

#### Scenario: Let go
- **WHEN** an administrator clears "kept by hand" on that file
- **THEN** the movie is wanted for an upgrade by the ordinary rules

#### Scenario: No single click
- **WHEN** a better release is listed for a movie whose file is kept by hand
- **THEN** the movie's page offers no "Grab it anyway", a post to the grab route sends nothing, and the confirmation of "Grab this one" asks for the tick that names the kept file

### Requirement: A movie's files can be linked by hand
An administrator SHALL be able to link a video file that is in a movies library folder to a managed movie, to take a file's link from another movie to this one, and to unlink a file, on the movie's links page, after a review of the whole change. The page SHALL list the movie's linked files with how each link was made, the files with no link in a folder that holds a linked file of the movie, the files identification holds as the movie, the files unlinked by hand from it, and any other present video file of a movies library folder that is asked for by its number. An administrator SHALL be able to start from a video file's page in the library, or from a unit on the Identify page that is not linked, search the managed movies by name, and open the chosen movie's links page with that file; that entry SHALL write nothing. Nothing on disk SHALL be renamed, moved or deleted. When the movie already has a file, the review SHALL say so and both SHALL stay linked unless the person ticks unlinking the other; each present file SHALL have a tick of its own, and the ticks SHALL be part of the review and of its digest. When the file is another movie's, the review SHALL name that movie and say whether it is then left without a file and searched for again. The change SHALL be refused while a grab or an import of a movie it touches is open, when a file it changes was linked elsewhere after the review, and when the review is out of date. Each change SHALL be recorded in the history of every movie it touches with the person's name. A link made this way SHALL be marked as made by a person, and kept by hand. A file unlinked this way SHALL stay unlinked until a person links it or lets Axis manage it again, and SHALL be listed as unlinked by hand. An administrator SHALL be able to let Axis manage one such file again, after a review with a digest, and that SHALL change no link: for a linked file, the person's marks SHALL be taken off, and the link SHALL become identification's when identification holds the file as this movie; when identification reads the file as another title or as none, nothing SHALL be confirmed and the link SHALL stay marked by hand; for a file unlinked by hand, the mark SHALL go and the next identification run SHALL link the file as it reads it. Removing a movie SHALL clear the marks made for it.

#### Scenario: A file identification does not match
- **WHEN** an administrator links a video file that identification left for review to Copper Heron (1995)
- **THEN** the movie is in the library with that file, the link says it was made by hand by that person, and the file on disk is unchanged

#### Scenario: The movie already has a file
- **WHEN** the movie has a file and the administrator applies the link without the unlink tick
- **THEN** both files are linked, and the review said the movie would have two

#### Scenario: Unlinked
- **WHEN** an administrator unlinks a movie's only file
- **THEN** the file is on disk as before, the movie is no longer in the library, and a later identification run does not link the file again

#### Scenario: From the file's page
- **WHEN** an administrator opens "Link to a movie or episode by hand" for a video file of a movies library folder and searches for a managed movie by part of its name
- **THEN** the results link to that movie's links page with the file, and that page lists the file

#### Scenario: Identification reads it otherwise
- **WHEN** an administrator lets Axis manage again a file linked by hand that identification reads as another movie
- **THEN** nothing can be confirmed, and the file stays linked by hand

### Requirement: An administrator can replace a movie's download in flight
When an administrator asks to grab a release by hand or by force for a movie and the only obstacle is one open grab of the movie whose download is queued, downloading or paused at its client, has not completed and has no import, the confirmation SHALL say which release is downloading, at which client and how far it is (as of the last poll of its client), and SHALL offer "Replace the download in flight with this one" behind a tick. The confirmation SHALL write nothing and SHALL ask the client nothing. Replacing SHALL be an administrator's, the post SHALL be checked for the form's token, and on the force confirmation the force digest SHALL be checked as well as the replace's. Every other rule of a grab by hand or by force SHALL stand: a release that a fixed rule or the blocklist refuses SHALL get no offer.

When a grab stands in the way and cannot be replaced, the confirmation SHALL say why and offer nothing, in each of these cases: its download has arrived and waits for its import (with a link to the import, or to the Downloads page while no import exists yet); a release is being sent to the client at that moment; Axis is still removing an earlier download from the client; the download has failed or has left its client and the follow-up has not dealt with it yet; the release chosen is the very release in flight, or the same torrent (the same info hash) from another indexer; another download of the movie that no grab follows is still at a download client, which the page names, with a link to the Downloads page.

Once confirmed, every check of the new grab that can be made before the client is asked SHALL be made first, and a refusal there SHALL leave the download in flight untouched. Then, under the movie's lock and only while its download has not completed and has no import, the grab in flight SHALL be closed with a reason that names the person and the release that replaces it, the client SHALL be asked to remove its download under the place rule below, and the movie's history SHALL say so. The release replaced SHALL NOT be put on the blocklist. Replacing SHALL queue no search. When the grab in flight is no longer what the confirmation showed (another grab, another digest, completed, an import), the post SHALL change nothing and show the confirmation again.

The chosen release SHALL be sent only when nothing of the replaced download is left at its client. When the client fails to remove it after Axis has looked at where it is, the old grab SHALL stay closed with its removal still to be made, nothing SHALL be sent, no search SHALL grab anything for the movie while the download is unfinished at its client, the follow-up SHALL ask for the removal again, and the page SHALL say all of this. Two downloads of one movie SHALL never be sent by a replace.

When the chosen release cannot be sent after the download in flight was removed, the movie SHALL be wanted again under the ordinary rules, and the page SHALL say that the download in flight was removed, that nothing is downloading for the movie now, and that a scheduled search takes it up again only if it is monitored. When the claim of the chosen release fails with an error after the download in flight was removed, the error shown SHALL begin by saying the same. When a scheduled search claims a release for the movie in the moment between the removal and the person's claim, the person's release SHALL NOT be sent, and the page SHALL say which release the search grabbed.

A replace SHALL NOT have anything deleted outside the download's own place. Before a torrent's client is asked to remove a torrent with its data for a replace, the first time and every time the removal is asked again, the client SHALL be asked where the torrent's files are now. The client SHALL NOT be asked to remove the torrent with its data when it names no place, a place that is not a plain full path, a place that none of the client's path mappings reaches, the top of a disk, a library folder, a place inside a library folder (a downloads folder aside) or a place that holds one. When that is so before the grab in flight is ended, nothing SHALL be ended and nothing sent; for a place that no path mapping reaches, the page SHALL say to add a path mapping for that folder. When the client cannot be asked where the torrent is before the grab in flight is ended (it does not answer, or it does not list the torrent while part of its list cannot be read), nothing SHALL be ended and nothing sent, and the page SHALL say to try again. A torrent that its client no longer lists, in a list that was read whole, SHALL be asked for without its data. A Usenet job is not judged by place: its unfinished files are in its client's own folder. The place is judged as text through the client's path mappings, and a torrent moved in its client in the few milliseconds between the last look and the removal is not seen.

When the download of a replaced grab finishes at its client while its removal is still to be made, the follow-up SHALL still judge its place and ask for its removal. Where the place is one the client is not asked to delete at, the removal SHALL end as kept: the client SHALL NOT be asked again, the download SHALL stay at its client and SHALL NOT be imported, the Downloads page and the movie's history SHALL say that it was to be replaced, could not be removed and is still at its client, and the movie SHALL no longer count as in flight. While the torrent is still unfinished at such a place, the removal SHALL stay failed and the movie SHALL stay in flight.

#### Scenario: The client cannot be asked
- **WHEN** the download in flight is a torrent and its client does not answer when an administrator confirms the replace
- **THEN** nothing is ended, nothing is sent, the download stays in flight as it was, and the page says that the client could not be asked and to try again

#### Scenario: A list that cannot be read whole
- **WHEN** the client does not list the torrent and lists an entry that Axis cannot read
- **THEN** the torrent is not taken for gone, nothing is ended and nothing is sent

#### Scenario: It finishes where it may not be deleted
- **WHEN** the removal of a replaced download is still to be made, the torrent is moved into a library folder, and it finishes downloading there
- **THEN** the client is not asked to remove it, the file in the library folder stays, the removal ends as kept with the reason on the Downloads page and in the movie's history, and the movie is not in flight any more

#### Scenario: A second download of the movie at a client
- **WHEN** one grab of the movie is downloading and another download of the movie, which no grab follows, is still at a download client
- **THEN** no replace is offered, the page names the other download and links to the Downloads page, and a post that asks for the replace ends nothing

#### Scenario: The same torrent from another indexer
- **WHEN** the release chosen has the info hash of the torrent in flight
- **THEN** no replace is offered, and the page says that it is the same torrent

#### Scenario: A torrent moved into a library folder
- **WHEN** the download in flight is a torrent that someone moved, in its client, into a library folder, and an administrator confirms the replace
- **THEN** the client is not asked to remove it, the file of that name in the library folder stays, nothing is ended, nothing is sent, and the page says where the client says the torrent is

#### Scenario: A place no path mapping reaches
- **WHEN** the client says the torrent's files are at a place that none of its path mappings reaches
- **THEN** the replace is refused in the same way, because Axis cannot tell that the place is not a library folder, and the page says to add a path mapping for that folder

#### Scenario: Moved while the removal waits
- **WHEN** the client stopped answering after the look at where the torrent is and before its removal, so that the old grab is closed with its removal still to be made, and the torrent is moved into a library folder before the follow-up asks again
- **THEN** the follow-up does not ask the client to remove it, the file in the library folder stays, the grab says why, and nothing is grabbed for the movie while the torrent is unfinished at its client

#### Scenario: A check of the new grab fails first
- **WHEN** the release chosen would replace a file in the library that it is not better than, and that tick is not given
- **THEN** nothing is ended and nothing is sent

#### Scenario: The page is out of date or forged
- **WHEN** a post asks for a replace of a grab that is not the one in the way, or carries a digest that is not the one of the confirmation of this release
- **THEN** nothing is ended, nothing is sent, and the confirmation is shown again

#### Scenario: Two posts of one replace
- **WHEN** a second post of the same replace arrives after the first has ended the download in flight
- **THEN** the second ends nothing and sends nothing, and the movie has one open grab and one download

#### Scenario: The release in flight itself
- **WHEN** the release chosen is the one that is downloading
- **THEN** no replace is offered, and the page says so

#### Scenario: Replaced
- **WHEN** an administrator ticks the box and confirms, for a movie whose download is half done
- **THEN** the old grab is closed as replaced by that person, the client has removed the old download with its data, the chosen release is sent, and the movie has one open grab

#### Scenario: Without the tick
- **WHEN** the form is posted without the tick
- **THEN** nothing is ended and nothing is sent

#### Scenario: The client cannot remove it
- **WHEN** the client refuses or stops answering when asked to remove the old download, after Axis has looked at where it is
- **THEN** the old grab is closed with its removal still to be made, nothing is sent, the page says so and why, no search grabs anything for the movie while the download is unfinished at the client, and the client is asked again at the next follow-up

#### Scenario: The new release cannot be sent
- **WHEN** the old download was removed and the chosen release cannot be fetched from its indexer, or its client refuses it
- **THEN** the page says that the download in flight was removed, that this release could not be sent and that nothing is downloading for the movie now, and the movie is wanted again: searched for by the next scheduled search when it is monitored, and by the person otherwise

#### Scenario: It has completed
- **WHEN** the download in flight has completed, or an import of it exists
- **THEN** no replace is offered, and the page links to the import, or to the Downloads page while no import exists yet

#### Scenario: It completes while the person confirms
- **WHEN** the download completes after the confirmation was shown and before the post
- **THEN** the post ends nothing and sends nothing, and the page points to the import

#### Scenario: The release replaced is not blocked
- **WHEN** a download was replaced
- **THEN** its release is not on the movie's blocklist, and a later search may take it

#### Scenario: No search in between
- **WHEN** a download was replaced
- **THEN** no search of the movie was queued by the replace

#### Scenario: A search claimed first
- **WHEN** a scheduled search grabs a release for the movie in the moment between the removal and the person's grab
- **THEN** the person's release is not sent, the movie has one open grab and one download, and the page says which release was grabbed

#### Scenario: Another obstacle
- **WHEN** the release is also on the blocklist or ruled out by a rule that protects the system
- **THEN** no replace is offered and the page says that obstacle

#### Scenario: A viewer
- **WHEN** a user who is not an administrator, or a post without the form's token, asks to replace
- **THEN** it is refused and nothing changes

### Requirement: Giving up on a torrent leaves its files when its client places them in a library folder
When Axis asks a torrent's client to remove a download with its data because the download failed or stalled, or because an administrator chose "Give up on this download" or "Block and search again", it SHALL first ask the client where the torrent's files are now. When the client names a place that maps to a known volume and is the top of a disk, a library folder, inside a library folder (a downloads folder aside) or holds one, Axis SHALL ask the client to remove the torrent without its data, SHALL leave the files where they are, and SHALL record on the grab and on the download that they were left and why, which the Downloads page shows. When the client cannot be asked, names no place, names a place that is not a plain full path, or names a place that none of its path mappings reaches, the removal SHALL be with its data, as before, so that a failed download in the client's own folder is still deleted. Remove on the Downloads page does not go through this rule.

#### Scenario: Give up on a torrent moved into a library folder
- **WHEN** an administrator gives up on a torrent that someone moved, in its client, into a library folder
- **THEN** the torrent is removed from the client without its data, the file of that name in the library folder stays, and the Downloads page says that its files were left and why

#### Scenario: A failed torrent in a library folder
- **WHEN** a torrent that its client says is in a library folder fails, and the follow-up deals with it
- **THEN** the release is blocked and the torrent removed from the client without its data, and nothing in the library folder is deleted

#### Scenario: A place no path mapping reaches
- **WHEN** an administrator gives up on a torrent whose client says it is at a place that none of its path mappings reaches
- **THEN** the client is asked to remove it with its data, as before


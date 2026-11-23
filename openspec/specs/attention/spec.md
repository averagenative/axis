# attention Specification

## Purpose
Define the notices that tell an administrator what needs a person: worked out from the state that exists and never stored, in three levels, shown by a bell in the header, on the "Needs attention" page and on the dashboard, and dismissed by each user until the thing happens again.
## Requirements
### Requirement: Notices are worked out from the state that exists
A notice SHALL be worked out from the state Axis already keeps for the thing it is about (a disk's state, an import's state, a client's state), and Axis SHALL NOT store notices. A notice SHALL be gone, with no action on the notice itself, once the state behind it has ended, and SHALL be listed on the page "Needs attention" (`/attention`) while it lasts.

#### Scenario: Dealt with on its own page
- **WHEN** an administrator decides the one blocked import on the Imports page
- **THEN** the notice about blocked imports is gone from `/attention` and from the count without anyone touching the notice

#### Scenario: Mends itself
- **WHEN** a disk that had a notice for being offline is mounted again and its next check finds it
- **THEN** its notice is gone

#### Scenario: Nothing waits
- **WHEN** no state has a notice
- **THEN** `/attention` says "Nothing needs attention." and lists nothing

### Requirement: A notice says what waits, why, and where to deal with it
Each notice SHALL be one sentence in plain words that says what is waiting and why, SHALL say since when where that is known, and SHALL link to the page where a person deals with it, naming that page. Many things of one kind MAY be one notice with their number. A count of one SHALL read in the singular.

#### Scenario: A disk
- **WHEN** the disk named Attic has been offline for 25 minutes because its path is not mounted
- **THEN** its notice says the disk Attic has been offline for 25 minutes and that its path is not mounted, and links to that disk's page

#### Scenario: Several of a kind
- **WHEN** three imports are blocked
- **THEN** one notice says 3 imports are blocked and links to the Imports page

#### Scenario: One of a kind
- **WHEN** one import is blocked
- **THEN** the notice reads "1 import is blocked" with the rest of its sentence in the singular

### Requirement: Three levels, the most serious first
Every notice SHALL have one of three levels, named on the page in plain words: "Not working" (something is broken and media cannot arrive or be stored until it is mended), "Waiting for you" (Axis stopped where only a person can decide), and "Worth a look" (something is degraded and the rest still works). The page SHALL list the notices by level in that order, and within a level the newest first.

#### Scenario: Order
- **WHEN** a download client is not working, two imports are blocked and one of three indexers is failing
- **THEN** the page lists the client under "Not working", then the imports under "Waiting for you", then the indexer under "Worth a look"

#### Scenario: A level can rise
- **WHEN** the only switched-on indexer that still worked fails as well
- **THEN** the indexers' notices are listed under "Not working"

### Requirement: No notice for what mends itself or is only progress
A state SHALL become a notice only when a person can do something about it, when Axis is not about to mend it, and when its cause has no notice of its own. A state that only a person ends SHALL be a notice at once. A state Axis keeps retrying SHALL be a notice only after its wait: a disk offline for 10 minutes; a download client not answering or failing for 30 minutes; an indexer not answering or failing for 30 minutes and on at least a second contact. A wait SHALL count from when the thing stopped working, not from the last change of why (a client that stops answering and then answers with errors is one outage); it SHALL begin again when a client or an indexer is switched on; and a disk SHALL be judged only once the running daemon has checked it. A state that ends by a timer of its own (an indexer asked to wait, an allowance used up, a back-off, a stalled download) SHALL NOT be a notice; files that were not found are the one exception (see "The further notices"). Work that is running, queued or done SHALL NOT be a notice.

#### Scenario: A short outage
- **WHEN** a disk is offline for 3 minutes and then found again
- **THEN** no notice was shown for it and the count did not change

#### Scenario: The reason changes
- **WHEN** a download client has not answered for 25 minutes and then answers with errors for 10 more
- **THEN** its notice is shown, and says 35 minutes

#### Scenario: A disk the daemon has not checked
- **WHEN** the daemon starts and a disk's record says it has been offline for three hours
- **THEN** there is no notice about it until the daemon's own check has found it offline

#### Scenario: Switched on again
- **WHEN** an indexer that was failing is switched off for a day and switched on
- **THEN** it has no notice until it has been failing for 30 minutes from then, on a contact made since

#### Scenario: A refused key
- **WHEN** a download client refuses the password Axis has
- **THEN** its notice is shown at the next reading, without a wait

#### Scenario: An allowance used up
- **WHEN** an indexer's daily allowance is used up
- **THEN** there is no notice: the allowance comes back by itself

#### Scenario: A consequence
- **WHEN** ten movies cannot be searched for because no indexer works, and the indexer has its notice
- **THEN** those ten movies are not counted in the notice about movies that need attention

### Requirement: The first notices
The page SHALL have notices for: a disk offline or not recognised at its path; imports blocked or partly done; movies that need attention and series that need attention, counted for the reasons a person must act on (every acceptable release failed, the profile rejected what was found, no destination, a profile that cannot be used, a download left at its client, a record that could not be read); a download client not working; an indexer not working; a details provider that refused its key; and movies or series migrated from Radarr or Sonarr that Axis would search for once searching is started: a monitored movie without a file, a series with a monitored episode without a file. A migrated title that has its file SHALL NOT be counted. A notice of a module that is switched off SHALL NOT be shown.

#### Scenario: A migration that waits
- **WHEN** a migration from Radarr brought 12 monitored movies that have no file and "Start searching" was not selected
- **THEN** a notice under "Waiting for you" says 12 movies migrated from Radarr have no file and that Axis does not search for them until searching is started, and links to that migration's page

#### Scenario: Migrated titles that have their files
- **WHEN** a migration from Radarr brought movies that all have their files, which are kept, and "Start searching" was not selected
- **THEN** there is no notice about the migration: starting searching would change nothing for them

#### Scenario: A release only a person can grab
- **WHEN** the only release found for an episode is one no search takes by itself, so the episode needs attention because the profile rejected what was found
- **THEN** its series is counted in the notice about releases that wait for a person (see "Releases that wait for a person have their own notice"), which links to TV's Wanted page, and not among the series that need attention

#### Scenario: A module switched off
- **WHEN** the TV module is switched off
- **THEN** no notice about series is shown or counted

### Requirement: The further notices
Beside the first notices, the page SHALL have notices for: a disk with less free space than the setting says (a share of its size; 0 for never), a disk mounted read-only while Axis may write, and a disk whose marker could not be written; imports that wait because Read-only storage is on; finished downloads that cannot be imported, whatever the cause; files left in the download folders and never imported; an indexer Axis holds because its pages changed or it answers with a browser challenge; movies and series folders found in the library that wait for a person to say which title they are, and links Axis made by itself and is no longer sure of; files Axis could not read, files that were not found, and library folders of which a scan has ended and none ever ended complete; a media server whose connection checks fail; workers paused after a crash, refused for their version, or not heard from; conversion runs that are paused, or ready and not started; jobs of work that failed for good and are still kept as failed; YouTube downloads that failed; and movies and episodes with more than one file. Each SHALL follow the rule against noise: at once when only a person ends it; after its wait when Axis keeps trying or the state usually ends by itself (a media server unreached for 30 minutes and on a second failed check; a worker not heard from for 10 minutes beyond the Workers page's own rule, and for no more than a day; a finished download that cannot be imported, 10 minutes after it finished; a conversion run that is ready, 10 minutes after its list was finished); and not at all for what is only a consequence of another notice. Files that were not found SHALL be a notice at once, although the index drops them by itself after its keeping time, so that a person can act while they are still listed. The imports that wait for Read-only storage SHALL be one thing however many wait, so that a dismissal of it holds while more arrive. The notice about finished downloads SHALL name no cause and SHALL link to the page that shows each download's reason. The notice about failed jobs SHALL count exactly the jobs its link lists. A notice about an indexer Axis holds SHALL last until a contact with the indexer works, and SHALL be "Not working" when no other switched-on indexer can be searched.

#### Scenario: A disk that is almost full
- **WHEN** the setting is 5 and the disk Attic has less than 5 percent of its size free
- **THEN** a notice under "Worth a look" says the disk Attic is almost full, with its free space and its size, and links to that disk's page

#### Scenario: Never told
- **WHEN** the setting is 0
- **THEN** no disk has a notice for being almost full

#### Scenario: Imports that wait for Read-only storage
- **WHEN** Read-only storage is on and two imports wait for it
- **THEN** one notice under "Waiting for you" says 2 imports wait because Read-only storage is on, and links to the storage settings

#### Scenario: Dismissed while more arrive
- **WHEN** an administrator dismisses that notice and a third import waits for Read-only storage
- **THEN** the notice stays dismissed

#### Scenario: Finished downloads that cannot be imported
- **WHEN** a download finished at the client Harbor 11 minutes ago and Axis cannot find it where the client says it is
- **THEN** a notice under "Waiting for you" says 1 finished download at Harbor cannot be imported yet, names no title, no path and no cause, and links to the Downloads page

#### Scenario: A client still moving the file
- **WHEN** a download finished at the client 3 minutes ago and cannot be found yet
- **THEN** it has no notice

#### Scenario: A disk is away
- **WHEN** such a download has a notice, an administrator dismissed it, and a disk goes offline and comes back
- **THEN** the notice is as it was throughout, and is still dismissed when the disk is back

#### Scenario: An indexer Axis holds
- **WHEN** the pages of an indexer run from a site definition no longer look as the definition expects, and another indexer answers
- **THEN** a notice under "Worth a look" says so at the next reading, without a wait or a second contact, and the indexer has no notice for failing beside it

#### Scenario: Nothing else can be searched
- **WHEN** an indexer is held and every other switched-on indexer is held, not answering, refused or failing
- **THEN** its notice is under "Not working" and says nothing can be searched for

#### Scenario: The hold's time has passed
- **WHEN** an indexer was held for a browser challenge, its notice was dismissed, and the hold's hour passes before the next search asks the indexer and holds it again
- **THEN** the notice is there throughout, and stays dismissed

#### Scenario: A contact works
- **WHEN** a held indexer answers a search properly
- **THEN** its notice is gone at the next reading

#### Scenario: A worker that restarts
- **WHEN** a worker is not heard from for two minutes and then registers again
- **THEN** no notice was shown for it

#### Scenario: A silent worker
- **WHEN** the worker garage was last heard from 36 minutes ago and Axis has been running for more than 10 minutes
- **THEN** a notice under "Worth a look" says the worker garage has not been heard from for 36 minutes

#### Scenario: Axis has just started
- **WHEN** Axis started less than 10 minutes ago and a worker was last heard from before the start
- **THEN** the worker has no notice yet, and a notice about silent workers that was dismissed before the start is still dismissed

#### Scenario: A worker switched off
- **WHEN** a worker was last heard from two days ago
- **THEN** it has no notice

#### Scenario: A refused worker
- **WHEN** a worker is refused for its version and was last heard from 36 minutes ago
- **THEN** it has the notice about refused workers and is not counted among the silent ones

#### Scenario: A run just planned
- **WHEN** a conversion run's list was finished 2 minutes ago and the run was not started
- **THEN** it has no notice; 10 minutes after its list was finished it has one under "Worth a look"

#### Scenario: Old failures
- **WHEN** three jobs of work are failed for good, one of them for nine days
- **THEN** the notice says 3 jobs, and the list its link opens has those 3 jobs

#### Scenario: A folder Axis is only now reading
- **WHEN** a library folder was added and its first scan has not ended
- **THEN** it has no notice

#### Scenario: A folder on a disk that is away
- **WHEN** a library folder was never completely scanned and its disk is offline
- **THEN** the folder has no notice of its own

#### Scenario: Scanned again
- **WHEN** a library folder has the notice, an administrator dismissed it, and a scan of the folder runs and ends incomplete again
- **THEN** the notice is as it was throughout, and is still dismissed

### Requirement: Releases that wait for a person have their own notice
A release that no search takes by itself and that a person may take with "Grab this one" (numbered beyond the series' data in part, in the place of a file kept by hand, an anime release whose absolute number is read too weakly, or one that was downloaded before without a file for an episode) SHALL be counted where its search is stored, unless it was left first because it does not hold what was searched for (another episode, nothing that is wanted): a person who took it would not fill the episode. The series that have an episode needing attention because its last search found such a release SHALL have a notice of their own under "Waiting for you", which says what to do, and such an episode SHALL NOT also count its series among the series that need attention. An episode's last search SHALL be the last finished search of that episode alone: a release found by a search of a season or of the whole series does not bring its series into this notice. The notice SHALL link to a list of exactly those episodes. The notice SHALL end when the episode is grabbed, imported or no longer wanted; when a person's grab of the release fails, the episode SHALL stay in the notice until it is searched again.

#### Scenario: A release for another episode
- **WHEN** a search for S01E03 finds only a release that holds S01E04 and an episode the series' data does not have
- **THEN** the release is not counted as one that waits for a person, and S01E03's series is not in that notice for it

#### Scenario: The list behind the notice
- **WHEN** one series has an episode that waits for "Grab this one" and another series' episode needs attention for another reason
- **THEN** the notice's link opens TV's Wanted page listing the first episode and not the second

#### Scenario: Only a person can take it
- **WHEN** the last search of an episode found one release, which holds the place of a file kept by hand, so that no search grabs it
- **THEN** a notice says 1 series has an episode that only a person can take, and the series is not counted in the notice about series that need attention

#### Scenario: Beside the others
- **WHEN** one series has such an episode and another series' releases were all rejected by its profile
- **THEN** there are two notices, one for each, and the count is 2

#### Scenario: Grabbed
- **WHEN** a person grabs the release
- **THEN** the notice about releases only a person can take is gone at the next count of the series

### Requirement: Who sees a notice
Every notice SHALL be an administrator's, because each one is ended by an action only an administrator has, and an administrator SHALL see every notice. A user who is not an administrator SHALL see no notice and no count of them: the page `/attention`, the control's own request (`/attention/bell`) and the posts that dismiss a notice or show it again SHALL refuse them with 403, and the control's own request SHALL answer them with an empty body, so that a page left open by a user whose role was taken away loses its control and shows no error in its place.

#### Scenario: An ordinary user
- **WHEN** a disk is offline, imports are blocked, and a user who is not an administrator opens any page and then `/attention`
- **THEN** the header shows them no attention control, `/attention` refuses them (403), and nothing they are sent names a disk, an import, a title or a path

#### Scenario: The control's own request
- **WHEN** a user who is not an administrator requests `/attention/bell`
- **THEN** the answer is 403 with nothing in it

#### Scenario: An administrator
- **WHEN** an administrator opens the same pages
- **THEN** they see both notices and a count of 2

#### Scenario: A role taken away
- **WHEN** an administrator is made an ordinary user
- **THEN** their next page shows no attention control

### Requirement: A person may dismiss a notice
A person SHALL be able to dismiss a notice without ending its cause. A dismissal SHALL belong to the user who made it and SHALL apply to what that user was shown: the notice SHALL stay out of their count and their list while its level is no higher, its number is no higher, and nothing of it happened again since. It SHALL come back when the thing happens again, when there are more of them, or when its level rises, and SHALL NOT come back only because time passed. A dismissal SHALL last only while its notice lasts: when a whole reading finds the notice gone (its cause has ended), every user's dismissal of it SHALL end, so that the next time the thing happens it is shown whatever its number is then; a number that drops without reaching nothing SHALL keep the dismissal. A reading that cannot tell whether a notice still holds (a disk Axis has not checked since it started, a count that is not finished, a download whose disk is away, a folder that is being scanned) SHALL NOT count as finding it gone: the notice SHALL stay as it was last known, and every dismissal of it SHALL be kept, across a restart of Axis as well. A details provider's refused key is known only to the running daemon, so after a restart of Axis its notice SHALL count as happening again and SHALL be shown once more while the key is still refused. Dismissed notices SHALL stay listed on the page apart from the others, each with a way to show it again. A dismissal SHALL change nothing but the notice's place for that user.

#### Scenario: Dismissed for one user
- **WHEN** one of two administrators dismisses the notice about a disk that is offline
- **THEN** it leaves that administrator's count and moves to their dismissed list, the other administrator still sees and counts it, and the disk's state, checks and page are unchanged

#### Scenario: It happens again
- **WHEN** the disk comes back, its notice ends, and the disk later goes offline again for 10 minutes
- **THEN** the administrator who dismissed the first is shown the new notice and it is counted

#### Scenario: More of them
- **WHEN** an administrator dismisses "3 imports are blocked" and a fourth import is blocked
- **THEN** the notice is back in their list and count

#### Scenario: Fewer of them
- **WHEN** an administrator dismisses "3 imports are blocked" and one of the three is decided
- **THEN** the notice stays dismissed

#### Scenario: Other things of the same kind, later
- **WHEN** an administrator dismisses "12 movies need attention", all twelve are mended so that the notice is gone, and later twelve other movies need attention
- **THEN** the notice is shown and counted again, to that administrator and to everyone

#### Scenario: Dismissed before a restart
- **WHEN** an administrator dismissed the notice about a disk that is offline, Axis is restarted, and the disk is still offline at Axis's first check of it
- **THEN** the notice is still dismissed for that administrator

#### Scenario: A count that is not finished
- **WHEN** an administrator dismissed "3,000 series need attention", Axis is restarted, and its first count of the series takes several readings
- **THEN** the dismissal is kept while the count runs, and the notice is dismissed when the count is done

#### Scenario: Show again
- **WHEN** the administrator selects "Show again" on a dismissed notice
- **THEN** it is back in their list and count

#### Scenario: A forged form
- **WHEN** a dismissal is posted without the session's CSRF token
- **THEN** it is refused and nothing is dismissed

### Requirement: The count costs a page view nothing
Drawing the attention control for a page, a fragment or the control's own request SHALL run no database statement. The notices SHALL be read in the background: again within 10 seconds of something that can change them, no more often than once in 10 seconds, and once a minute whatever happened. A reading SHALL be bounded: each source's statement SHALL be under a deadline, none SHALL read the index of files as a whole (a count over a partial index that holds only the rows counted, such as the files whose probe failed, is not that), and a source that cannot be read SHALL keep the notices it last gave. A number that is too much to work out in a reading (the duplicates) SHALL be left by a job and read from where it was left, and its notice SHALL say so. The counts of movies and of series SHALL be made only after a job that can change them ended (a search, an import), never sooner than 5 minutes after the count before, and every 10 minutes when none ended; a job that runs every minute whatever happened SHALL NOT cause a count. A stop of Axis SHALL NOT wait for the database to answer a reading: a reading in flight SHALL be called off, and what it gave SHALL be thrown away.

#### Scenario: Many page views
- **WHEN** a hundred pages are requested while nothing changes
- **THEN** no statement was run for the control on any of them

#### Scenario: A burst of changes
- **WHEN** fifty jobs end within a second
- **THEN** the notices are read once for them

#### Scenario: The job that follows the downloads
- **WHEN** the job that follows the movies' downloads ends, as it does every minute
- **THEN** the movies are not counted again for it

#### Scenario: A source that fails
- **WHEN** the statement of one source fails during a reading
- **THEN** the other sources' notices are fresh, that source's last notices are still shown, and the page says since when they are unchanged

#### Scenario: A stop while the database does not answer
- **WHEN** Axis is asked to stop while a reading waits on a database that answers nothing
- **THEN** the reading is called off and Axis stops within a second for it, whatever deadline the statement had


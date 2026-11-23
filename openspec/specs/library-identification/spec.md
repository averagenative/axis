# library-identification Specification

## Purpose
Recognising what is already in the library: matching film folders and files to titles with stated evidence and confidence, linking only what is sure, and leaving the rest for a person, without changing anything on disk.
## Requirements
### Requirement: Films in the library are matched to titles with stated evidence
The daemon SHALL match each film folder or file in a films root to a metadata title, recording the evidence and a confidence of sure, likely or unknown. The titles a name could mean SHALL be every title of the provider's answers that has the name as its title, original title or other name, each judged by its full record: its years SHALL be the full record's, never the search's answer's alone: the year of its first release, and the year of its theatrical release in the region when that is within two years of the first (a release on disc or by streaming is no year of it), and its runtime the full record's. A search answer's year SHALL NOT rule out a title of the name: at most five titles of the name SHALL be read in full for one folder, those whose stored year is within three years of the names' year first, then those further off, the nearest first. A title whose stored year is its full record's and more than three years off SHALL NOT be read again. A title further off whose full record is not answered SHALL be waited for only when no title of the name is within three years; otherwise it SHALL be judged as stored. When more titles within three years have the name than are read, the match SHALL be at most likely. When the names give a year, the title SHALL be sure only when its year is the names' year exactly and its runtime fits the main video's, no other title of the name has the year exactly with a runtime that fits or is not known, and every title of the name a year off has a runtime known that does not fit by the tolerance (not the wider band). A year that is only a title's theatrical year in the region SHALL count as no better than a year off when another title of the name was first released within a year of the names' year. A title whose nearest year is two off the names', when no title of the name has the year, is a year off or has no year known, SHALL be likely, the reasons naming both years; it SHALL NOT stand in the way of a nearer title. A title with no year known SHALL NOT stand in the way. A title that has the name only as another name, found among the first answers read for their other names while others within reach were not read, SHALL be at most likely. Titles that the year and the runtime do not tell apart SHALL NOT be chosen between, and the reason SHALL name each with its years and its runtime. The provider's order of its answers or a title's popularity SHALL NOT decide.

#### Scenario: Identifier in the name
- **WHEN** a folder name carries a provider identifier
- **THEN** it is matched to that title as sure, with that evidence

#### Scenario: Title, year and runtime
- **WHEN** the names give a title and year that match one title and the file's runtime agrees
- **THEN** it is matched as sure

#### Scenario: No year
- **WHEN** the names give a title without a year
- **THEN** the match is at most likely

#### Scenario: Identifier in an .nfo alone
- **WHEN** an `.nfo` beside the film names a title and no folder or file name reads as that title or one of its other names
- **THEN** the match is at most likely, with the `.nfo`'s title as a candidate

#### Scenario: Two of one name, a year apart, the runtime telling them apart
- **WHEN** two films have the folder's name, one of the folder's year and one a year later, the file runs as the first, and the second's runtime is known and does not fit
- **THEN** it is matched to the first as sure, and the reasons say why the other is not it

#### Scenario: Two of one name, a year apart, both of a fitting length
- **WHEN** a documentary of 2012 running 95 minutes and a drama of 2013 running 100 minutes have one name, and the drama's folder is named with 2012 and its file runs 100 minutes
- **THEN** it is held for a person with both as candidates, and the reasons give each one's year and runtime

#### Scenario: Two of one name and year that both fit
- **WHEN** two films of the folder's name and year both run as the file does, or one of them gives no runtime
- **THEN** it is held for a person with both as candidates, and the reasons give each one's year and runtime

#### Scenario: Only the year tells them apart
- **WHEN** two films have the folder's name, one of its year and one a year off, and the file is not probed yet
- **THEN** it is likely, the one of its year first, and it is looked at again once the file is probed

#### Scenario: The year of its release in the region
- **WHEN** a folder is named with the year a film came out in cinemas in the region, a year after its first release, no other film of the name came out within a year of it, and the runtime fits
- **THEN** it is matched as sure, and the reasons say which year that is

#### Scenario: The year in the region beside a film of that year
- **WHEN** a folder is named with the year a film came out in cinemas in the region, and another film of the name was first released that year
- **THEN** it is held for a person

#### Scenario: A disc decades later
- **WHEN** a film of 1975 came out on disc in the region in 2009, another film of its name is of 2009, and a folder named with 2009 holds a file that runs as the 1975 film
- **THEN** it is not matched to the 1975 film, and nothing is linked as sure

#### Scenario: A film first released abroad
- **WHEN** a folder is named with the year a film was first released abroad, the search's answer gives the year it came out in the region, many years later, and the full record gives the first release and a runtime that fits
- **THEN** the full record is read, and the folder is matched as sure

#### Scenario: The only title far off, its full record not answered
- **WHEN** the one title of the folder's name is many years off by its search's answer, and its full record is not answered
- **THEN** the folder waits for it, and is decided once it is answered

#### Scenario: A namesake many years off
- **WHEN** a title of the folder's name is decades off its year by its search's answer, another title of the name is of the folder's year, and the far one's full record is not answered
- **THEN** it is asked for once, nothing waits for it, and the folder is decided without it

#### Scenario: Seven of one name, all far off
- **WHEN** seven films have the folder's name, every one many years off its year
- **THEN** the five nearest are read in full, the other two are not, and nothing is matched

#### Scenario: Two years off
- **WHEN** the one film of the folder's name was first released two years after the folder's year
- **THEN** it is likely, and the reasons name both years

#### Scenario: Two years off beside a film of the year
- **WHEN** one film of the folder's name is of its year, with a runtime that fits, and another is two years off
- **THEN** it is matched to the one of its year as sure

#### Scenario: The search gives another year
- **WHEN** the search's answer gives a film one year and its full record another, and the folder is named with the full record's
- **THEN** it is matched as sure, with no year held against it

#### Scenario: Named by another name
- **WHEN** a folder is named by one of a film's other names, no answer of the search has that name as its title, and every answer within reach of the folder's year is read in full for its other names
- **THEN** the film is matched by the films' rules

#### Scenario: Another name among answers not all read
- **WHEN** a folder is named by one of a film's other names, found among the first answers read in full, while more answers of that year were not read
- **THEN** the match is at most likely

#### Scenario: The second page of a search
- **WHEN** no answer on the first page of a search has the folder's name, the page is full, and the film is on the second page under another name
- **THEN** the second page is asked for and the film is matched; with a first page that is not full, the second is not asked for

#### Scenario: One video, one name
- **WHEN** a folder "Title (Year)" holds one video whose own name reads as no title, the year matches and the runtime fits
- **THEN** it is matched as sure; a folder holding several such videos is held

#### Scenario: Another cut
- **WHEN** a folder and its one film's file agree on a title and the year of its first release, no other title of that name is among the search's answers, and the file runs a fifth longer than it, or two fifths longer with "Extended" or "Director's Cut" in its name
- **THEN** it is matched as sure, and the reasons say the runtime is within the wider band

#### Scenario: Far off the runtime
- **WHEN** the file runs further from the film's runtime than the wider band, another title has its name (of any year), or its name says only "Collection", "Version", "Cut" or "Edition"
- **THEN** it is held, and the reasons give both runtimes

### Requirement: Only sure matches are linked without a person
A match SHALL be linked automatically only when it is sure. Likely and unknown units SHALL wait on a review page with their candidates and the reasons.

#### Scenario: Likely
- **WHEN** a unit's match is likely
- **THEN** it is not linked until an administrator confirms it

#### Scenario: Undo
- **WHEN** an administrator undoes a link
- **THEN** the unit returns to review and the film no longer counts that file

### Requirement: Identification changes nothing on disk
Identification, review and bulk adding SHALL NOT rename, move, delete or modify any file.

#### Scenario: Tree unchanged
- **WHEN** a root is identified and its review actions are used
- **THEN** every file and name on disk is as it was

### Requirement: Identification is paced
Lookups SHALL go through the metadata layer within its limits, a bounded number of units SHALL be handled per run, and a rescan SHALL consider only new or changed units. A root whose last run left units or seasons to look at, and that is not waiting for the provider, SHALL be queued again within a minute.

#### Scenario: Rescan
- **WHEN** a root is scanned again with nothing changed
- **THEN** no unit is looked up again

#### Scenario: Quiet root
- **WHEN** a root has not been scanned or probed since its last run, and that run finished with nothing left and no provider failure
- **THEN** the scheduler does not queue it again

#### Scenario: Work left
- **WHEN** a run of a root reaches its limit of units with more left
- **THEN** the minute's schedule queues its next run, and not twice while one is queued

#### Scenario: Provider waiting
- **WHEN** a root's last run stopped because the provider did not answer
- **THEN** the minute's schedule leaves it to the wait, and the ten minutes' schedule queues it after the wait

### Requirement: An identified film's files are shown once on its page
A title's page SHALL show the files identification linked to it once: in the movies module's own card while that module is on, whether or not the film is managed, and in identification's card when it is off.

#### Scenario: Movies on
- **WHEN** the page of an identified film that is not managed is opened with the movies module on
- **THEN** its files are in the movies' card with a way to manage it, and identification's card is not shown

#### Scenario: Movies off
- **WHEN** the same page is opened with the movies module off
- **THEN** identification's card shows its files

### Requirement: The bulk action says what it does
The Identify page's action for a root SHALL be named for managing every identified movie, and SHALL say that each is listed in Movies already, how many Axis manages, and that each keeps its file and nothing is searched for.

#### Scenario: A root
- **WHEN** an administrator opens a root with identified movies Axis does not manage
- **THEN** the action reads "Manage every identified movie" and says each is listed in Movies as in library

### Requirement: Series in the library are matched to titles with stated evidence
The daemon SHALL match each folder at the top of a root for TV, and each video standing alone there, to a series by the rules films use, recording the evidence and a confidence of sure, likely or unknown. A series folder SHALL be one unit whatever season folders it holds. The episode files' own names SHALL stand in for a film's file name, and the runtime checked SHALL be that of the episode the main video holds. A series folder whose names give no year SHALL be decided by its episodes instead of the year (see "A series folder without a year is linked by its episodes"). A series folder whose name gives a title and a year SHALL be read by its name alone when its episode files name no series.

#### Scenario: Identifier in the name
- **WHEN** a series folder's name carries a TMDb, TVDB or IMDb identifier (`{tmdb-}`, `{tvdb-}`, `[imdbid-]`)
- **THEN** it is matched to that series as sure, with that evidence

#### Scenario: Name, year and the episode's runtime
- **WHEN** a folder's name gives a title and year that match one series and the main video's length agrees with its episode's runtime
- **THEN** it is matched as sure, and the reason names the episode's runtime

#### Scenario: Episode files that name no series
- **WHEN** a folder "Title (Year)" holds episode files named only by their numbers and episode titles, one series has the title and year, and the main video's length agrees with its episode's runtime
- **THEN** it is matched as sure, as the same folder without its year is by its episodes

#### Scenario: No year, one episode on disk
- **WHEN** a folder's name gives a title without a year, two series have that name, and one numbered episode is on disk
- **THEN** both are candidates, the match is at most likely, and the folder waits for a person

#### Scenario: Episodes in the series folder itself
- **WHEN** a series folder holds episode files with no season folder
- **THEN** it is matched, and each file is placed by the season its own name gives

#### Scenario: A video at the top of the root
- **WHEN** a video named for a series' episode stands alone at the top of a TV root
- **THEN** it is its own unit, matched to that series

#### Scenario: A series split across roots
- **WHEN** two roots for TV each hold a folder of the same series
- **THEN** both are matched to that series, which is listed once

#### Scenario: A film for a series folder
- **WHEN** an administrator chooses a film for a series folder
- **THEN** the choice is refused and the folder stays as it was

### Requirement: Episode files are read by their own names
Each video of a series unit SHALL be read for what its own name says it holds, by the same reading the TV module's release matcher uses: episodes of one season, or a daily show's air date. A file whose name holds no episode SHALL be kept with the reason, and SHALL NOT be placed by a guess. Subtitles, samples and extras SHALL NOT be read as episodes.

#### Scenario: Episode forms
- **WHEN** files are named `S01E02`, `S01E02E03`, `S01E02-E04`, `1x02` or `S00E01` in a `Specials` folder
- **THEN** each holds those episodes of that season

#### Scenario: A daily show
- **WHEN** a file's name gives an air date and no season
- **THEN** it holds the episode that aired that day, when exactly one did

#### Scenario: Held, with the reason
- **WHEN** a file's name gives no season and episode, only an absolute number, a whole season, episodes across seasons, or a season other than its season folder's
- **THEN** it holds no episode and keeps that reason

#### Scenario: Another series' name
- **WHEN** a file in a series' folder is named for a title that is not one of that series' names
- **THEN** it is not placed on the series' episodes

#### Scenario: Not episodes
- **WHEN** a series folder holds a subtitle and an `Extras` folder
- **THEN** neither is read as an episode file

### Requirement: Each season a series needs is fetched once
A run of a root for TV SHALL fetch, through the metadata layer and within its limits, each season its linked series need whose episodes were never fetched, at most a bounded number per run, and SHALL leave the rest for the next run. A person's link of a series folder SHALL ask for a run of its root.

#### Scenario: Nothing changed
- **WHEN** a TV root is run again with nothing changed and every season fetched
- **THEN** no unit is looked at again and the provider is asked for no season

#### Scenario: A person's link
- **WHEN** an administrator links a held series folder to a series
- **THEN** a run of its root is queued, and after it the folder's files are placed on that series' episodes

### Requirement: A series folder without a year is linked by its episodes
A series folder whose names give no year SHALL be linked as sure when all of these hold: the folder's name reads as a title (its episode files agreeing, or naming no series); the provider's series with that title, original title or other name, all of them compared together, are at most four; exactly one of them fits the episodes on disk; and none of the others is one the episodes cannot be checked against. The series compared SHALL be those whose title or original title is the name and those of the first three other answers whose full records have it as another name. A series fits when at least three numbered episodes of regular seasons are on disk in at least three files, none is in a season it does not have, at least 90 percent of them are within its seasons' episode counts, and the main video's runtime does not contradict its episode's runtime. A series the provider lists no seasons or no episode counts for SHALL be one that cannot be checked: it SHALL NOT count as not fitting. A series that has the name only as another name SHALL be at most likely when not every other answer was read for its other names. The main video SHALL be probed. Two series that both fit SHALL never be chosen between. Every other case SHALL be held for a person, with the reason in words. Every verdict of a series folder SHALL end with what its files hold.

#### Scenario: One series of the name, the episodes fit
- **WHEN** a folder named without a year holds four numbered episodes that are episodes of the one series of its name
- **THEN** it is linked as sure, and the reasons say that the episodes fit it and no year is needed

#### Scenario: A season the series does not have
- **WHEN** a folder's files name a season that the one series of its name does not have
- **THEN** it is held, and the reasons name the missing season

#### Scenario: Two of one name, one fits
- **WHEN** two series have the folder's name and the episodes on disk are episodes of only one of them
- **THEN** it is linked to that one as sure, and the reasons say why the other does not fit

#### Scenario: Two of one name that both fit
- **WHEN** two series have the folder's name and the episodes on disk fit both
- **THEN** it is held with both as candidates, and the reasons say that both fit

#### Scenario: Another series known by the name
- **WHEN** one series has the folder's name as its title, another has it as an other name, and the episodes on disk fit both
- **THEN** both are compared, and the folder is held with both as candidates

#### Scenario: A series that cannot be checked
- **WHEN** two series have the folder's name, the episodes fit one, and the provider lists no seasons or no episode counts for the other
- **THEN** it is held, the one that fits first among the candidates, and the reasons name the one that cannot be checked

#### Scenario: The runtime contradicts
- **WHEN** the episodes fit the one series of the name but the main video runs far longer or shorter than its episode
- **THEN** it is held, and the reasons give both runtimes

#### Scenario: No numbered files
- **WHEN** a folder's files give no season and episode numbers
- **THEN** it is held, and the reasons say that too few numbered episodes are on disk

#### Scenario: One of the series' other names
- **WHEN** the folder's name is one of a series' other names, not its title
- **THEN** that series is the candidate, and it is linked as sure when the episodes fit

#### Scenario: A name that matches only loosely
- **WHEN** no series' title or other name has the folder's name exactly
- **THEN** nothing is linked

#### Scenario: No runtime given
- **WHEN** the provider gives no runtime for the series and the episodes fit it
- **THEN** it is linked as sure, and the reasons say the runtime was not checked

#### Scenario: A library named without years
- **WHEN** 1,500 series folders, none with a year in its name, each hold the episodes of the one series of its name
- **THEN** every one is linked as sure, and none waits for a person

### Requirement: Folders held by the earlier rule are looked at again
A unit of a root of movies or for TV whose verdict was made by an earlier version of the rules, and that no person decided, SHALL be looked at again once after the upgrade, with nothing changed on disk and nothing pressed, in the runs' bounded batches: one held for review (unknown or likely), and a series' folder that was linked by itself by its episodes under the rules before the series' comparison was corrected (version 2). A unit linked otherwise SHALL NOT be looked at again for a version. A look again SHALL never remove or change a link (see "Only a person changes a link"). A unit a person decided SHALL stay as the person left it.

#### Scenario: Held for its missing year
- **WHEN** a series folder was held for review because its names gave no year, and its episodes fit the one series of its name
- **THEN** the scheduler queues its root and the run links it as sure

#### Scenario: A film held by the earlier rules
- **WHEN** a film folder was held for review by the earlier rules (two films of its name within a year, its year one off by the search's answer, or no film fitting by the search's answer's year) and the new rules make it sure
- **THEN** the scheduler queues its root and the run links it as sure

#### Scenario: A series linked by the earlier comparison
- **WHEN** a series folder without a year was linked by itself to the one series whose title is its name under the rules before version 2, and another series known by that name fits its episodes too
- **THEN** it is looked at again, its link stands, and it is listed for a person as linked under earlier rules and now uncertain, with both series as choices

#### Scenario: A link of version 2
- **WHEN** a folder was linked by itself under version 2 of the rules, by its episodes or by its name
- **THEN** it is not looked at again for version 3, and its link stands

#### Scenario: A person's decision
- **WHEN** a person chose a title for a folder, marked it as not a movie, or undid a folder's link
- **THEN** the folder keeps that decision

#### Scenario: Once
- **WHEN** a held folder has been looked at again
- **THEN** it is not looked at again until something in it changes

### Requirement: A held folder says what would settle it
The Identify page SHALL show, for every unit held for review, beside its reasons, one sentence saying what would settle it: what to choose on the page, or what in the names would let Axis link it by itself.

#### Scenario: Too few episodes
- **WHEN** a series folder is held because too few numbered episodes are on disk
- **THEN** the page says to confirm the series, and that Axis links it by itself once at least three numbered episodes are on disk or the folder's name gives its year

### Requirement: A full record not answered is told later
When a full record a unit's decision needs is not answered by the provider (an error, a timeout, a missing title), the unit SHALL NOT be decided on the search's answer: nothing final SHALL be stored for it, and it SHALL be looked at again by itself once its wait is over, the wait doubling with each try from fifteen minutes up to a day. A unit that is not linked SHALL wait, its reasons saying that the provider did not answer for which title and why, with the candidates known so far; a linked unit SHALL keep its link and what was said of it. When the provider holds Axis off (it told Axis to wait, or it is backed off after failures in a row), the run SHALL stop after the folder that met it, and the root SHALL wait. "Identify new and changed files now" SHALL end every unit's wait of its root.

#### Scenario: The full record fails
- **WHEN** the search finds a film of a folder's name and year, and its full record is answered with an error
- **THEN** the folder waits, its reason saying the provider did not answer for that title, its candidates the titles of its name known so far, and Movies counts it as waiting for the provider

#### Scenario: A linked folder looked at again
- **WHEN** a folder linked by itself is looked at again and a full record its decision needs is not answered
- **THEN** its link stands, with its reasons as they were, and it is looked at again after the wait

#### Scenario: The provider answers again
- **WHEN** the wait of a waiting folder is over and the provider answers
- **THEN** the scheduler queues its root and the folder is decided by the rules, linked as sure when they say so

#### Scenario: It fails again
- **WHEN** a waiting folder's full record fails once more
- **THEN** it waits twice as long as before

#### Scenario: Told to wait
- **WHEN** the provider answers a full record with "too many requests", or fails twice in a row
- **THEN** that folder waits, the run stops after it, the root waits, and the folders after it are looked at once the root's wait is over

### Requirement: Only a person changes a link
A run SHALL NOT remove or change a link it finds, whatever made it (by itself, an import, a person). When a run looks again at a folder linked by itself or by an import and the evidence no longer makes that link sure, or makes another title sure, the link SHALL stand, the new reasons and candidates SHALL be kept, and the folder SHALL be listed for a person as linked and now uncertain, saying why; a person SHALL be able to keep the link, choose another title or undo it there. A later look that makes the same link sure again SHALL take it off that list.

#### Scenario: A film's file runs far off when looked at again
- **WHEN** a film folder linked by itself is looked at again and its file now runs far from the film's runtime
- **THEN** the link stands, and the Identify page lists the folder as linked and now uncertain, with the reasons

#### Scenario: A person keeps it
- **WHEN** an administrator keeps the link of a folder listed as uncertain
- **THEN** it is linked as the person's choice, and it is no longer listed

### Requirement: A folder with more videos than are read says so
A film folder with more than 64 videos, or a series folder with more than 5,000, SHALL say on each of its units, in their reasons, how many of its videos were not read and are in no unit, and the folder's own unit SHALL be at most likely; the log SHALL name the folder and the counts.

#### Scenario: Sixty-six videos
- **WHEN** a film folder holds 66 videos of one film
- **THEN** its unit is held, its reasons say that 2 videos are in no unit, and the log names the folder

### Requirement: A video without a year joins its title's film only when the folder says so
A video whose name gives a title without a year SHALL be one unit with the videos of that title that give a year only when the folder's own name gives that title with that year (one year off allowed). Otherwise it SHALL be a unit of its own.

#### Scenario: A folder without a year
- **WHEN** a folder named by a title without a year holds "Title.mkv" and "Title (1984).mkv"
- **THEN** they are two units

#### Scenario: A folder of that year
- **WHEN** a folder "Title (1985)" holds "Title.mkv" and "Title (1984).mkv"
- **THEN** they are one unit, the folder's

### Requirement: A candidate shows what tells it apart
Each candidate on the Identify pages SHALL show, beside its poster and title, its year, and where its stored record has them its running time and, for a series, its number of seasons (specials not counted) and the length of an episode.

#### Scenario: Two of one name
- **WHEN** a folder's candidates are two movies of one name, of 2004 running 104 minutes and of 2005 running 88 minutes
- **THEN** each shows its year and "Runs 1 h 44 min" or "Runs 1 h 28 min"

#### Scenario: A series
- **WHEN** a candidate is a series whose record lists seasons 0 to 3 and episodes of 45 minutes
- **THEN** it shows "3 seasons, 45 min an episode"

### Requirement: An action returns to the place in its list
After an action taken on a card of a root's list on the Identify pages (Confirm this movie or Confirm this series, Choose this movie or Choose this series, Mark as not a movie or Mark as not a series, Keep this link, Undo), whether on the root's page, on the list's own page or on the unit's page opened from that list, the browser SHALL be sent back to the same page and list (with its page of results), to the card that followed the one acted on; to the one before it when it was the last; to the list's heading when the list is now empty, by the address's fragment, without script. The notice of the action SHALL name the folder and what was done, SHALL offer Undo where the action can be undone, and SHALL be shown directly before that card, or under the list's heading, not at the top of the page. Undo from the notice SHALL return to the unit's own card.

#### Scenario: Confirm in the middle of a list
- **WHEN** an administrator confirms the second of three likely folders on the root's page
- **THEN** the redirect ends in the third one's anchor, and the page shows the notice "Linked" with the folder and its title directly before that card

#### Scenario: The last card
- **WHEN** the card acted on was the last of its list
- **THEN** the redirect ends in the anchor of the card before it

#### Scenario: The list is now empty
- **WHEN** the card acted on was the only one of its list
- **THEN** the redirect ends in the list's own anchor, and the notice is under the list's heading

#### Scenario: The unknown list on its own page
- **WHEN** an administrator selects Choose this movie on a card of the unknown list's own page
- **THEN** the browser comes back to the unknown list's page, not the root's page, at the next card, with the notice there

#### Scenario: From a unit's page opened from a list
- **WHEN** an administrator opens a unit from the unknown list and chooses its title there
- **THEN** the browser comes back to the unknown list at the card that followed that unit

#### Scenario: Keep the last uncertain link
- **WHEN** an administrator keeps the only link listed as linked and now uncertain
- **THEN** the browser comes back to that list's heading on the root's page, which says nothing is uncertain now, with the notice under it

#### Scenario: Undo in Linked lately
- **WHEN** an administrator undoes the latest link in Linked lately
- **THEN** the browser comes to the row after it, with the notice in a row of its own directly before it

#### Scenario: Undo from the notice
- **WHEN** an administrator selects Undo in the notice of a confirmation
- **THEN** the unit is back in review and the browser comes to its own card, with the notice that it is back in review

### Requirement: A folder's "Name (Year)" names its title, whatever the name is
A folder's or a video's name that is exactly "<something> (<year>)" SHALL be read as the title "<something>" and the year, whatever the something is made of (digits, one or two characters, letters that look like a tag or read as a Roman numeral, a quality word), when it holds a letter or a digit. A short number with a year beside it in a release's name SHALL be a title too. A number alone with no year ("01.mkv") SHALL stay a name that reads as no title.

#### Scenario: A number
- **WHEN** a folder "412 (2011)" holds "412 (2011).mkv"
- **THEN** the title read is "412", of 2011, and it is searched for

#### Scenario: A number that looks like a year
- **WHEN** a folder is named "1983 (1961)"
- **THEN** the title read is "1983" and the year 1961

#### Scenario: A quality word
- **WHEN** a folder is named "4K Ghosts (2015)"
- **THEN** the title read is "4K Ghosts", of 2015

#### Scenario: A release named by a number
- **WHEN** a video is named "412.2011.1080p.BluRay.x264-GROUP.mkv"
- **THEN** the title read is "412", of 2011

#### Scenario: A number alone
- **WHEN** a video is named "01.mkv"
- **THEN** it reads as no title

### Requirement: A film's name that looks like part of a series says so
When the title a film's names read as holds, after its first word, an episode's or a special's number ("S05M01", "S05E01", "5x01") and nothing fits it, the unknown verdict's first reason SHALL say that the name looks like part of a series, naming the number, and the Identify page's search SHALL start from the words after the number. Nothing SHALL be linked differently.

#### Scenario: A series' special filed as a film
- **WHEN** a file "Copper Lantern Show S05M01 Night Harbour (2007 360p re-rip).mp4" is in a films root and no film fits it
- **THEN** it is unknown, its first reason says the name looks like part of a series and names S05M01, the settle line says to search for its own title, and the page's search box holds "Night Harbour"

### Requirement: Identification leaves a link read under another numbering as it is
Each link of a file to an episode of a managed series SHALL record the numbering the file's name was read under. When identification looks at a series folder again, it SHALL NOT move or remove a link whose name was read under another numbering than the one the series follows, and SHALL place by its name, under the numbering the series follows, only a file that no link holds. A disk that is offline while a series' numbering changes SHALL leave every link of its files as it was.

#### Scenario: Looked at again after a change
- **WHEN** a series' numbering changed from TMDb to TheTVDB's aired order, and a new file in its folder makes identification look at the folder again
- **THEN** the links of the files read under TMDb's numbering are as they were, no episode becomes wanted, and only the new file is placed by its name

#### Scenario: A disk offline
- **WHEN** the disk of a series' files is offline while its numbering changes
- **THEN** every file keeps its link, and no episode becomes wanted

### Requirement: A link that was migrated is not changed by a run
A run SHALL NOT replace, move or remove a file's link to a movie or to episodes that was made by migrating from Radarr or Sonarr, whether the run is sure of the folder or a person confirms what the run read. Such a link SHALL NOT decide the title of its folder: the run SHALL read the folder by its own names, and SHALL NOT take the folder's other files to the movie the migrated file is linked to.

#### Scenario: The run reads the folder otherwise
- **WHEN** a file migrated as the movie *Marrow Lane (1990)* lies in the folder *Tin Sparrow (2001)*, another movie Axis manages, and a person confirms the folder as *Tin Sparrow (2001)* after a run
- **THEN** the migrated file is still linked to *Marrow Lane (1990)*, as migrated, and the folder's other file is linked to *Tin Sparrow (2001)*

#### Scenario: A migrated series' folder
- **WHEN** identification runs over the folder of a series migrated from Sonarr, and a person confirms what it read
- **THEN** every migrated episode file stays linked to the episodes Sonarr had it on, as migrated

### Requirement: What a person linked or unlinked by hand is not changed by a run
A run SHALL NOT replace, move or remove a file's link to a movie or to episodes that is marked as made by a person (a forced grab, an assignment at import, the links page), and SHALL NOT link a file a person unlinked by hand. Adding every identified movie, or linking the identified files of one, SHALL leave such files alone too. A file SHALL stay unlinked by hand until a person links it by hand again or lets Axis manage it again, until the title it was unlinked from is removed, or until an import by Axis links a file at its path.

#### Scenario: The run reads the name otherwise
- **WHEN** a file linked by hand to S01E04 is named for S01E03 and its folder is looked at again
- **THEN** the file is still linked to S01E04, by hand

#### Scenario: A folder linked to another title afterwards
- **WHEN** a person chooses another title for the folder of a file that is linked by hand to a movie
- **THEN** that file's link by hand stands

#### Scenario: Unlinked by hand
- **WHEN** a file a person unlinked is in a folder a run links to its movie as a sure match
- **THEN** the file gets no link

#### Scenario: Axis manages it again
- **WHEN** an administrator lets Axis manage again a file that was unlinked by hand from a movie, and identification runs
- **THEN** the run links the file to the movie it reads, and the file is no longer listed as unlinked by hand


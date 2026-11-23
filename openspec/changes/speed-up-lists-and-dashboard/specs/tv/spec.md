## MODIFIED Requirements

### Requirement: The calendar and Wanted list what is still missing
The calendar SHALL list only episodes of managed series that are not on disk. Wanted SHALL list the missing episodes of managed series, and the episodes on disk below the quality profile's cutoff SHALL be under their own tab, Upgrade wanted. Neither SHALL list anything of a series on disk that Axis does not manage. Wanted SHALL list its episodes in pages of 100, ordered by series, season and episode, SHALL say how many episodes there are in all and how many the page lists, and SHALL link to the page before and the page after. Every tab SHALL state the number of all its episodes, not of the page's.

#### Scenario: Calendar
- **WHEN** a managed series has episodes on disk and episodes still to air
- **THEN** the calendar lists the ones still to air and none on disk

#### Scenario: Wanted
- **WHEN** a managed series has an episode missing and an episode on disk below its cutoff
- **THEN** Wanted lists the missing one, and its Upgrade wanted tab lists the other

#### Scenario: Not managed
- **WHEN** a series is on disk and not managed
- **THEN** the calendar and Wanted list nothing of it

#### Scenario: More wanted episodes than a page
- **WHEN** 3,713 episodes are wanted
- **THEN** Wanted lists 100 of them, says "100 of 3,713 episodes on this page", and its pages reach every one of them once

### Requirement: The TV list works for a large library
A page of the TV list SHALL be read in a number of statements that does not grow with the number of series, episode files or posters. The series on disk SHALL be read in one pass, and the counts of a page's series on disk SHALL be read for that page's series alone. The TV list, Wanted, the calendar and the dashboard's figures about series SHALL NOT read the facts of every episode of the library: the states and counts of the series SHALL be counted by the database, and only the episodes that a page shows, or that tell what is wrong with a series on the page, SHALL be read. The states, counts and sentences SHALL be the same as the ones a series' own page gives each of its episodes.

#### Scenario: Thousands of series on disk
- **WHEN** 1,500 series and 60,000 episode files are on disk
- **THEN** every page costs the same statements in every view, paging reaches every series once, and the page's counts read only its own series' episode files

#### Scenario: Thousands of managed series
- **WHEN** 1,500 series with 36,000 episodes, their files, searches and grabs are managed, and the job table holds 160,000 finished jobs
- **THEN** a page of the TV list and a page of Wanted read no more episodes than the page shows or needs to explain, in a number of statements that does not grow with the library

#### Scenario: The counts agree
- **WHEN** series have episodes that are wanted, searched for, downloading, waiting for import, in the library, wanted for an upgrade, needing attention, not aired, not monitored and gone
- **THEN** the TV list and Wanted state for every series the same counts, state and sentence as are derived from every episode's facts

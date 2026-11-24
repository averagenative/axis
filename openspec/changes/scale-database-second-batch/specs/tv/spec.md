## ADDED Requirements

### Requirement: The scheduler reads only the series that can have something due
A run of the scheduler of automatic searches SHALL ask the database which series have an episode that can be due: one that is listed, monitored and aired, whose next search time has come, and that has no file or has a file that its series' quality profile might replace. It SHALL read the episodes of those series only, a bounded number of series at a time, and SHALL keep of what it plans no more than the run can queue. The database SHALL be told what a profile might replace by the profile itself (the qualities it does not upgrade from), not by a second statement of the rule; a file it cannot settle (of no known quality, or made by a conversion) SHALL leave its series to be read. What a run queues SHALL be what it queued when it read every episode: the same searches, in the same order, under the same limits.

#### Scenario: Most of the library is settled
- **WHEN** 5,000 series of 60 episodes are managed, and the episodes of 250 of them have an episode whose next search time has come
- **THEN** the run reads the episodes of those 250 series and of no other

#### Scenario: Everything is due
- **WHEN** every series has an episode that is due
- **THEN** the run reads every series' episodes, 50 series at a time, and holds one such page in memory at a time

#### Scenario: A page boundary changes nothing
- **WHEN** the same library is scheduled with one series a page and with fifty
- **THEN** the same searches are queued

#### Scenario: More is planned than a run queues
- **WHEN** sixty series each have a season to search for and a run queues two searches
- **THEN** the two queued are those of the series searched longest ago, as when every plan was kept

### Requirement: Every series follows the metadata within six hours
Each run of the scheduler SHALL sync as many of the series that are behind as go round every series once in six hours at the run's interval: the number of series times the interval over six hours, rounded up, never fewer than 5 and never more than 500. The series longest behind SHALL be taken first. A run SHALL begin no further sync once it has spent two minutes on them. After a start or a long stop, when every series is behind, they SHALL be taken at that same pace, not all at once.

#### Scenario: A small library
- **WHEN** 100 series are managed
- **THEN** a run syncs 5 of those that are behind

#### Scenario: A large library
- **WHEN** 5,000 series are managed and a run comes every 15 minutes
- **THEN** a run syncs 209, and every series is reached within six hours

#### Scenario: After a start
- **WHEN** the daemon starts and all of 12 series are behind
- **THEN** the first run syncs the 5 longest behind, the second the next 5, the third the last 2

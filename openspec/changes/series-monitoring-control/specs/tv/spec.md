## MODIFIED Requirements

### Requirement: Each episode shows its state
Each episode of a managed series SHALL show one state derived from the facts, and the series SHALL show how many of its aired monitored episodes are in the library. The series SHALL show one state of its own: the worst state of its episodes. When no episode of it is wanted, in flight, not aired or in need of attention, the series SHALL read "Not monitored" only when nothing of it is monitored: none of its seasons, none of its episodes, and its new seasons are not monitored either. Otherwise such a series SHALL read "In library" when an episode of it is in the library, and "Not aired" when none is. Episodes that are not monitored SHALL NOT make a series read as not monitored while anything of it is monitored. The series' own page, the TV list in every view and the series' card on a title's page SHALL give the same state and sentence for a series, whether the episodes are counted by the database or read one by one. The sentence of a series whose monitored, aired episodes are all in the library SHALL say so, and SHALL say that new seasons and episodes are monitored when they are.

#### Scenario: Not aired
- **WHEN** an episode's air date has not passed
- **THEN** its state is not aired and it is not searched for

#### Scenario: The specials are not monitored
- **WHEN** every season of a series is monitored and every aired episode of those seasons is in the library, the specials are not monitored and have no file, and new seasons are monitored
- **THEN** the series reads "In library" on its own page and in the TV list, and its sentence says that every monitored episode that has aired is in the library and that new seasons and episodes are monitored as they are announced

#### Scenario: One season left out
- **WHEN** one season of a series is not monitored and every aired episode of the other seasons is in the library
- **THEN** the series reads "In library"

#### Scenario: Nothing is monitored
- **WHEN** no season and no episode of a series is monitored and its new seasons are not monitored
- **THEN** the series reads "Not monitored", and its sentence says that Axis searches for nothing of it and how many of its episodes are in the library

#### Scenario: Only new seasons are monitored
- **WHEN** no season and no episode of a series is monitored, none of its episodes is in the library, and its new seasons are monitored
- **THEN** the series reads "Not aired", and its sentence says that new seasons and episodes are monitored as they are announced

#### Scenario: Counted and derived agree
- **WHEN** series have only episodes that are in the library or not monitored
- **THEN** the TV list states for each the state and sentence that are derived from every episode's facts

## ADDED Requirements

### Requirement: The monitoring of a whole series is changed in one place
A series' page SHALL state, at its top, how the series is monitored in words: how many of its seasons are monitored, whether its specials are, and whether new seasons are monitored when they are announced. An administrator SHALL be able to change, with one form there, whether new seasons and episodes are monitored as they are announced, and what to do with the seasons: leave them as they are, monitor every season, monitor the latest season only, or stop monitoring every season. Monitoring every season SHALL leave the specials as they are unless the administrator asks for them too. Monitoring the latest season only and stopping every season SHALL stop monitoring the specials. A season SHALL be changed with all its episodes. The change SHALL write only what is monitored: it SHALL NOT delete, move or unlink a file, and an episode that has a file SHALL keep it whether it is monitored or not. The change SHALL NOT queue a search: an episode that becomes wanted SHALL be taken by the next automatic search. The change SHALL be one transaction and SHALL be recorded in the series' history in one line with the user, only when something changed. A choice for the seasons SHALL become the series' choice of what to monitor; a migrated series SHALL keep "As in Sonarr" until such a choice is made. The change SHALL be for administrators, and a form without the session's token SHALL be refused.

#### Scenario: New seasons from now on
- **WHEN** an administrator ticks "Monitor new seasons and episodes as they are announced" and leaves the seasons as they are
- **THEN** new seasons are monitored from then on, no season or episode changes, the series' choice of what to monitor stays, and the history says who changed it

#### Scenario: Every season
- **WHEN** an administrator monitors every season of a series whose specials are not monitored, without asking for the specials
- **THEN** every season and its episodes are monitored, the specials are not, and the series' choice is "Every episode"

#### Scenario: Every season and the specials
- **WHEN** an administrator monitors every season and ticks the specials' box
- **THEN** the specials and their episodes are monitored too, an aired special without a file is wanted, and no search is queued

#### Scenario: The latest season only
- **WHEN** an administrator monitors the latest season only
- **THEN** that season and its episodes are monitored, and no other season and no special is

#### Scenario: Stop
- **WHEN** an administrator stops monitoring every season of a series whose episodes are in the library, and new seasons are not monitored
- **THEN** nothing of it is monitored, it reads "Not monitored", and every file is still linked to its episode

#### Scenario: A migrated series
- **WHEN** an administrator makes a choice for the seasons of a series whose choice is "As in Sonarr"
- **THEN** the series' choice becomes the one made, and the history says what it was

#### Scenario: Nothing to change
- **WHEN** the form asks for what the series already has
- **THEN** nothing is written, no history line is added, and the page says that nothing changed

#### Scenario: Not an administrator
- **WHEN** a non-administrator, or a form without the session's token, posts the change
- **THEN** it is refused and nothing changes

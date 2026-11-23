## ADDED Requirements

### Requirement: The dashboard opens at once from what was last read
Once the daemon has started, a view of the dashboard SHALL run no statement and SHALL NOT wait for any figure: every card SHALL show the figures that were last read, whatever the size of the library. The figures SHALL be read again in the background when something happened (a job ended, an import, a person's action), when a dashboard is opened, and at least every minute, and no sooner than a few seconds after the reading before. A card whose figures have not been read yet SHALL say "Counting..." in place of them. A figure that could not be read SHALL stay as it was last read. A part that only administrators see SHALL be shown to administrators only. Stopping the daemon SHALL NOT wait on the database for a reading in progress.

#### Scenario: Opened while figures are being read
- **WHEN** the dashboard is opened while its figures are being read for the first time
- **THEN** the page is answered at once, and the card of each part not read yet says "Counting..."

#### Scenario: A large library
- **WHEN** the library has 1,500 series and 36,000 episodes and the dashboard is opened
- **THEN** the page is answered from memory with no statement sent to the database

#### Scenario: Something happened
- **WHEN** a job ends after the figures were read
- **THEN** the figures are read again in the background, and the next view shows the new ones

#### Scenario: Not an administrator
- **WHEN** a user who is not an administrator opens the dashboard
- **THEN** nothing of a part that only administrators see is shown

### Requirement: An open dashboard updates by itself
After each reading of the figures, a dashboard that is open SHALL be told through the live event stream and SHALL replace its cards with the new ones without a reload, by asking for the cards alone. That request SHALL be answered from memory, SHALL NOT cause a reading, and SHALL be answered with no content when the page already shows the same cards.

#### Scenario: New figures
- **WHEN** the figures change while a dashboard is open
- **THEN** the page's cards show the new figures without a reload

#### Scenario: Nothing changed
- **WHEN** an open dashboard asks for its cards and they are the ones it shows
- **THEN** the answer has no content and the page is left as it is

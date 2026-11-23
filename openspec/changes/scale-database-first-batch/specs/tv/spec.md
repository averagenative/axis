## ADDED Requirements

### Requirement: Following the metadata writes only what changed
When a series' episodes follow the metadata, the row of an episode whose number, title, air date, runtime and provider id are what the metadata says, and that is not marked gone, SHALL NOT be written. A sync that finds nothing new SHALL leave every episode's row, and the identity recorded for it, as they were.

#### Scenario: Nothing changed
- **WHEN** a series is synced and the metadata says of every episode what its row already holds
- **THEN** no row of the series' episodes is written, and no change is reported

#### Scenario: Something changed
- **WHEN** the metadata numbers an episode anew or gives it another title
- **THEN** that episode's row takes what the metadata says, as before

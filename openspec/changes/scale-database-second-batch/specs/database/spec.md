## ADDED Requirements

### Requirement: A search's results are stored in a bounded number of statements
The releases a search considered SHALL be stored in the search's transaction with one statement for up to 1,000 of them, and what a module keeps of each release with a bounded number of statements more, not with statements for each release. A release that cannot be stored SHALL fail the statement and the transaction, as a single row's statement did. The rows stored SHALL be the rows stored before.

#### Scenario: A thousand releases
- **WHEN** a search of a series considered 1,000 releases
- **THEN** they are stored by one statement, and what the series keeps of each (the episodes it holds) by one more

### Requirement: Old searches are deleted in bounded batches
The searches that ended more than a week ago and are not among their owner's latest five (the manual ones counted apart) SHALL be deleted, with the releases they stored, no more often than once an hour and at the first follow-up after a start: at most 1,000 searches a statement, each statement its own transaction, at most ten statements a time. What a time leaves SHALL wait for the next. The question for the searches that never finished, asked every minute, SHALL be answered from an index of those searches.

#### Scenario: Nothing is old enough
- **WHEN** the follow-up runs every minute and no search is old enough to go
- **THEN** the prune is sent once an hour, not every minute

#### Scenario: A backlog after a long stop
- **WHEN** 135,000 searches are old enough to go
- **THEN** one time deletes 10,000 of them in ten statements, and the next times the rest

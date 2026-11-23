## ADDED Requirements

### Requirement: Deleting a row does not read a large table for each row deleted
A foreign key whose referencing table grows with the library or its history SHALL have an index on its referencing column, so that deleting a referenced row looks its references up by index. Where most referencing rows hold NULL the index MAY hold only the rows that do not. This covers the grabs that name a stored search result, a search or an episode; the import files that name a library file; the history events that name an episode or a title; the imports and downloads that name a title.

#### Scenario: A thousand stored results deleted
- **WHEN** 1,000 stored search results are deleted while 250,000 grabs are kept
- **THEN** each grab that names one of them is found through an index, and the grabs are not read once for each result

#### Scenario: A thousand library files deleted
- **WHEN** 1,000 library files are deleted while 3,000,000 import files are kept
- **THEN** each import file that names one of them is found through an index

### Requirement: Pooled sessions do not compile their statements
Every session of the daemon's connection pool SHALL run with PostgreSQL's just-in-time compilation off. The daemon's statements are short; compiling one costs more than running it wherever the planner's estimate passes the server's threshold.

#### Scenario: A statement the planner prices high
- **WHEN** a list's statement is priced above the server's threshold for compilation
- **THEN** it runs without being compiled, in every pooled session, with no setting made by the statement's own code

### Requirement: The statements are measured on a database at the design scale
A test program SHALL make a database at the design scale (5,000 series, 300,000 episodes, 20,000 movies, 400,000 library files, 1,000,000 stored search results, 250,000 grabs, 300,000 imports with 3,000,000 files, 3,000,000 history events, 2,000,000 jobs) or a stated share of it, call the daemon's own functions on it, and explain the statements they sent with the values they were sent with. It SHALL run at a tenth of the scale with the other tests and hold bounds on the statements this requirement and its neighbours cover.

#### Scenario: Run with the other tests
- **WHEN** the tests run
- **THEN** the program makes the database at a tenth of the design scale, and fails if the header's job count reads the whole job table or a covered delete takes longer than its bound

#### Scenario: Run by hand at the design scale
- **WHEN** the program is run with the scale set to 100
- **THEN** it prints, for every measured statement, the time before and after the indexes and the lines of its plan that say where the time went

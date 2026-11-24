## ADDED Requirements

### Requirement: Counting the films by state looks nothing up once a film
The statement that works out every film's state for the list's counts, for a list filtered by state and for the count of films that need attention SHALL read each kind of fact once and join it: the open grabs, their imports, the imports under way, the files, the last searches, the downloads still at a client and the searches queued or running. It SHALL NOT run a look-up for each film. The state it gives each film SHALL be the state it gave before. The statement that reads the films' facts SHALL read the searches queued or running once, not once a film.

#### Scenario: Twenty thousand films
- **WHEN** 20,000 films are managed and the list's counts are read
- **THEN** no part of the statement runs once for each film

#### Scenario: The same states
- **WHEN** a film is downloading, waits for its import, is searched for, needs attention or is in the library
- **THEN** the list counts it under the state its own page shows

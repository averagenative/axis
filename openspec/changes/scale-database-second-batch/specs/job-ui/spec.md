## ADDED Requirements

### Requirement: A page of the Jobs list does not read the jobs it does not show
A page of the Jobs list, whatever its filters (a state, a queue, work or upkeep) and wherever its cursor stands, SHALL be read from an index in which the jobs of one kind, queue and state lie together in the order of their ids. Its cost SHALL follow the number of such groups and the page's size, not the number of jobs kept. This SHALL hold for a filter that few jobs or none pass. The names of the queues that the page's filter offers SHALL be read the same way. The page's content, its order (the jobs that wait or run first, then the newest) and its cursor SHALL be what they were.

#### Scenario: Two million jobs
- **WHEN** the job table holds 2,000,000 jobs and the first page of the work is asked for
- **THEN** the page's statement reads the newest jobs of each kind, queue and state through the index, and no statement of the page reads the table as a whole

#### Scenario: A filter that nothing passes
- **WHEN** a state and a queue are chosen that no job has
- **THEN** the page is empty, and was found so without reading the jobs

#### Scenario: The page is what it was
- **WHEN** any page is read with any filter
- **THEN** it lists the same jobs in the same order as the statement that sorted every job

# module-management Specification

## Purpose
Switching modules on and off: what an administrator can switch, what a disabled module stops doing, and the promise that switching off removes nothing.
## Requirements
### Requirement: Modules can be switched on and off
An administrator SHALL be able to enable or disable any module not marked required, from a Modules page, and the state SHALL persist across restarts.

#### Scenario: State persists
- **WHEN** a module is disabled and the daemon restarts
- **THEN** the module is still disabled

#### Scenario: Required module
- **WHEN** a request tries to disable a required module
- **THEN** it is refused and the module stays enabled

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page or a switch
- **THEN** the response is 403 and nothing changes

### Requirement: Disabling hides a module and stops its work
While a module is disabled, the daemon SHALL omit its navigation entry, answer 404 for its routes, not fire its schedules, and not start its queued jobs. The change SHALL take effect without a restart.

#### Scenario: Routes and navigation
- **WHEN** a module is disabled
- **THEN** its navigation entry is absent from every page and its routes answer 404

#### Scenario: Queued jobs wait
- **WHEN** a module is disabled while jobs of its kinds are queued
- **THEN** those jobs stay queued and are not started

#### Scenario: Running job finishes
- **WHEN** a module is disabled while one of its jobs is running
- **THEN** that job is allowed to finish

#### Scenario: Schedules silent
- **WHEN** a disabled module's schedule comes due
- **THEN** no job is created

### Requirement: Disabling removes nothing
Disabling a module SHALL NOT delete or alter its stored data, settings or queued jobs, and re-enabling SHALL resume from the same state.

#### Scenario: Re-enable resumes
- **WHEN** a module is disabled and later enabled
- **THEN** its data and settings are as before and its queued jobs start

### Requirement: Dependencies are enforced
A module SHALL NOT be enabled while a module it depends on is disabled, and SHALL NOT be disabled while an enabled module depends on it.

#### Scenario: Enable blocked
- **WHEN** an administrator enables a module whose dependency is disabled
- **THEN** the request is refused and the page names the dependency

#### Scenario: Disable blocked
- **WHEN** an administrator disables a module that an enabled module depends on
- **THEN** the request is refused and the page names the dependent module

#### Scenario: Unknown or cyclic dependency
- **WHEN** a module declares a dependency that is not registered, or dependencies form a cycle
- **THEN** the daemon refuses to start and names the modules


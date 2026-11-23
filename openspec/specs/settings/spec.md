# settings Specification

## Purpose
Everything configurable after start-up: settings declared and typed by the modules that use them, validated, stored in the database, edited from one page, with secrets that can be written but never read back.
## Requirements
### Requirement: Declared, typed settings
Each module SHALL declare its settings with a key, type, default and limits. The daemon SHALL accept a value only if it satisfies the declaration, and SHALL return the default for a setting that has no stored value.

#### Scenario: Default when unset
- **WHEN** a declared setting has no stored value
- **THEN** reading it returns the declared default

#### Scenario: Invalid value rejected
- **WHEN** a value outside the declared limits or of the wrong type is written
- **THEN** the write fails with a message naming the setting, and the stored value is unchanged

#### Scenario: Duplicate key refused
- **WHEN** two modules declare the same setting key
- **THEN** the daemon refuses to start and names the key

### Requirement: Stored values survive and are checked on read
The daemon SHALL keep stored rows it has no declaration for, and SHALL NOT use a stored value that fails the current declaration.

#### Scenario: Undeclared row kept
- **WHEN** the daemon starts with a stored setting it does not declare
- **THEN** the row is left unchanged and a warning names the key

#### Scenario: Stored value no longer valid
- **WHEN** a stored value fails the current declaration
- **THEN** reading the setting returns the default and a warning names the key but not the value

### Requirement: Changes apply without restart
A successful write SHALL be visible to subsequent reads from any thread and SHALL notify interested code, unless the setting is declared as needing a restart.

#### Scenario: Change visible
- **WHEN** a setting is changed
- **THEN** the next read from any thread returns the new value, and subscribers to that key are notified

#### Scenario: Restart-required setting
- **WHEN** a setting declared as needing a restart is changed
- **THEN** the page states that the change takes effect after a restart

### Requirement: Secret settings are write-only
A setting declared secret SHALL never be rendered in a page, written to a log, or included in an error message or notification.

#### Scenario: Secret not shown
- **WHEN** an administrator opens the Settings page and a secret setting has a value
- **THEN** the page shows that it is set, and the value appears nowhere in the response

#### Scenario: Secret not logged
- **WHEN** a secret setting is changed or fails validation
- **THEN** no log line contains the submitted or stored value

#### Scenario: Secret cleared
- **WHEN** an administrator clears a secret setting
- **THEN** the stored value is removed and the page shows it as not set

### Requirement: Settings page
The daemon SHALL provide a page, for administrators only, listing every declared setting of every enabled module with its label, help text and current value, and accepting changes.

#### Scenario: Field error
- **WHEN** an administrator submits an invalid value
- **THEN** the page shows the error beside that field, keeps the other submitted values, and stores nothing from that submission

#### Scenario: Non-administrator refused
- **WHEN** a non-administrator requests the page or submits a change
- **THEN** the response is 403 and nothing changes

#### Scenario: Change recorded
- **WHEN** a setting is changed
- **THEN** a log line records the user and the key

### Requirement: Default theme setting
An administrator SHALL be able to set which built-in theme is used for users who have chosen none and for logged-out pages.

#### Scenario: Changing the default
- **WHEN** an administrator changes the default theme
- **THEN** the login page and users without a choice use it, and users who chose a theme keep theirs


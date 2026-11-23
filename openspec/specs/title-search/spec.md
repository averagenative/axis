# title-search Specification

## Purpose
Finding films and series by name from any page: the search box, the results page and the page for each title.
## Requirements
### Requirement: Search from every page
Every page SHALL offer a search box in the top bar that finds films and series by name, and SHALL work without script.

#### Scenario: Search
- **WHEN** a logged-in user submits a name
- **THEN** a results page shows matching films and series with poster, title and year

#### Scenario: Without script
- **WHEN** the search form is submitted by a browser without script
- **THEN** the same results page is returned

#### Scenario: Nothing found
- **WHEN** nothing matches
- **THEN** the page says so

### Requirement: Results do not wait for images
The list of results SHALL be shown without waiting for artwork, and a result without artwork SHALL show a placeholder.

#### Scenario: Slow image host
- **WHEN** artwork cannot be fetched
- **THEN** the results are shown with placeholders

### Requirement: Results survive the provider being unreachable
When the details provider is unreachable, saved results SHALL be shown with their age; when no provider is configured, the page SHALL say what is needed, naming the TMDb token and where it is set.

#### Scenario: Provider down
- **WHEN** a repeated search is made while the details provider is unreachable
- **THEN** saved results are shown with the time they were fetched

#### Scenario: No provider
- **WHEN** no TMDb token is stored
- **THEN** the results page says Axis has no TMDb token, and links an administrator to where it is added with "Add a TMDb token in Settings"

### Requirement: Title page
Each title SHALL have a page showing its artwork, overview, year, status, runtime, genres and external identifiers, and for a series its seasons and episodes with air dates.

#### Scenario: Series
- **WHEN** a series' page is opened
- **THEN** its seasons are listed and each shows its episodes

### Requirement: Provider text is never markup
Text from a provider SHALL be shown as text.

#### Scenario: Markup in a title
- **WHEN** a title or overview contains markup
- **THEN** it is displayed literally

### Requirement: Modules supply the actions
The results and title pages SHALL offer a place where an enabled media module shows its actions for a title, absent when no module supplies one.

#### Scenario: No module
- **WHEN** no media module is enabled
- **THEN** no action is shown

### Requirement: Searching is limited per user
Searches SHALL require a session and SHALL be limited per user.

#### Scenario: Too many
- **WHEN** a user searches faster than the limit
- **THEN** further searches are refused for a time with an explanation

### Requirement: The kind can be chosen with the search
The search box SHALL offer a choice of all, films or series, and the choice SHALL be kept on the results page.

#### Scenario: Series only
- **WHEN** a search is submitted with series chosen
- **THEN** only series are shown and the choice remains selected

### Requirement: Results are a list on a phone
On a screen at most 40rem wide the search results SHALL be a list of rows, each a small poster beside the title, its state when a module gives one, its kind and year, and the first lines of its overview; the whole row SHALL open the title's page.

#### Scenario: Searching on a phone
- **WHEN** a name is searched for at 390 pixels
- **THEN** the results are one row each, at least three to the screen, each with its poster, title, kind and year and the start of its overview

### Requirement: A title's page on a phone
On a screen at most 40rem wide a title's page SHALL show its poster beside its name and facts, with the overview and what can be done with the title below them at the full width, so that the add form's button is on the first screen.

#### Scenario: A title not added
- **WHEN** the page of a movie that is not added is opened at 390 by 844 pixels
- **THEN** its poster is beside its name, year and runtime, and the button that adds it is on the first screen without scrolling

### Requirement: A series' title page names the numbering it shows
A series' title page SHALL show its seasons and episodes as the series' source numbers them (the source a managed series follows, else its record provider's own) and SHALL name that source. While TheTVDB has a key and knows the series, the page SHALL offer an administrator to show the seasons in the other numbering, changing nothing; showing TheTVDB's aired order for the first time SHALL fetch it. A user who is not an administrator SHALL see only the numbering the series follows, and opening the page SHALL send no request to TheTVDB for them.

#### Scenario: A managed series following TheTVDB
- **WHEN** an administrator opens the title page of a series that follows TheTVDB's aired order
- **THEN** its seasons are TheTVDB's aired order, the page says so, and it offers to show TMDb's seasons

#### Scenario: Looking changes nothing
- **WHEN** an administrator shows the other numbering on a series' title page
- **THEN** the series still follows the source it did, and nothing about it changed


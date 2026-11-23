# web-shell Specification

## Purpose
The frame every web page lives in: the shared layout and navigation, static assets compiled into the binary and served under versioned URLs, the security headers sent with every response, and the error pages.
## Requirements
### Requirement: Common page layout
Every HTML page SHALL be rendered inside one shared layout containing the application name, the navigation, and the page's own content and title.

#### Scenario: Full page for a normal request
- **WHEN** a browser requests a page without htmx headers
- **THEN** the response is a complete HTML document with a doctype, the page title, the navigation, and the page content

#### Scenario: Content only for an htmx request
- **WHEN** a page is requested with the `HX-Request` header
- **THEN** the response contains the page content and an updated title, without the surrounding document or navigation

#### Scenario: History restore gets the full page
- **WHEN** a page is requested with both `HX-Request` and `HX-History-Restore-Request`
- **THEN** the response is the complete HTML document

### Requirement: Output is escaped
Text placed into a page SHALL be HTML-escaped, so that data can never be interpreted as markup.

#### Scenario: Markup in a title or content string
- **WHEN** a page is rendered with a title or text containing `<script>` and quote characters
- **THEN** the response contains those characters escaped and no script element from that text

### Requirement: Module navigation
The navigation SHALL list the entries contributed by registered modules, ordered by each entry's declared order and then its label, and SHALL mark the entry matching the current page.

#### Scenario: Module contributes an entry
- **WHEN** a module registering a navigation entry is present
- **THEN** every page's navigation includes that entry linking to its path

#### Scenario: Absent module contributes nothing
- **WHEN** a module is not registered
- **THEN** none of its navigation entries appear

#### Scenario: Current page is marked
- **WHEN** a page whose path matches a navigation entry is rendered
- **THEN** that entry is marked as the current page

### Requirement: Static assets
The daemon SHALL serve its bundled assets from the binary, with a content type matching the file and caching headers that let browsers keep them indefinitely, under a URL that changes whenever any asset changes.

#### Scenario: Asset is served
- **WHEN** a client requests the current URL of the bundled htmx script
- **THEN** the response is 200 with a JavaScript content type, an ETag, and a long-lived immutable cache policy

#### Scenario: Conditional request
- **WHEN** a client requests an asset with `If-None-Match` equal to its ETag
- **THEN** the response is 304 with no body

#### Scenario: Stale asset URL
- **WHEN** a client requests an asset under an outdated version segment
- **THEN** it is redirected to the current URL for that asset

#### Scenario: Unknown asset and path traversal
- **WHEN** a client requests an asset that does not exist, or a path containing `..` segments in any encoding
- **THEN** the response is 404 and nothing outside the bundled assets is ever served

### Requirement: Security headers
Every response SHALL carry a content security policy that allows resources only from the daemon itself and forbids inline script and style, plus headers forbidding framing and content-type sniffing; non-static responses SHALL NOT be cacheable.

#### Scenario: Headers on a page
- **WHEN** any page is requested
- **THEN** the response has the content security policy, `X-Content-Type-Options: nosniff`, a same-origin referrer policy, and `Cache-Control: no-store`

#### Scenario: Headers on errors and assets
- **WHEN** an error page or a static asset is requested
- **THEN** the same security headers are present (assets keep their own cache policy)

#### Scenario: Pages need no inline script or style
- **WHEN** any shell page is rendered
- **THEN** it contains no inline script, no inline style element, and no `style` attribute

### Requirement: Error pages
The daemon SHALL answer unknown paths with 404, known paths with an unsupported method with 405 and an `Allow` header, and handler failures with 500, each rendered in the common layout without internal details.

#### Scenario: Unknown path
- **WHEN** a client requests a path no route matches
- **THEN** the response is 404 rendered in the common layout

#### Scenario: Wrong method
- **WHEN** a client sends POST to a path that only supports GET
- **THEN** the response is 405 and the `Allow` header lists GET

#### Scenario: Handler failure hides details
- **WHEN** a page handler reports an internal error
- **THEN** the response is 500 with a generic message, and the error detail appears in the log but not in the response

### Requirement: Dashboard
The daemon SHALL serve a dashboard at `/` showing the daemon version and the registered modules.

#### Scenario: Dashboard lists modules
- **WHEN** a logged-in user requests `/`
- **THEN** the page shows the version and the name of each registered module

### Requirement: Built-in themes with a dark default
The daemon SHALL provide several built-in colour themes, including more than one dark theme, a light theme, and one that follows the device's light or dark setting. The default theme SHALL be Midnight, which is dark.

#### Scenario: Dark by default
- **WHEN** a page is rendered for a user who has chosen no theme, or for no user
- **THEN** the page uses the default theme, which is Midnight unless an administrator changed the default

#### Scenario: Follows the device
- **WHEN** a user chooses the theme that matches the device
- **THEN** the page is light on a device set to light and dark on a device set to dark

#### Scenario: Unknown theme
- **WHEN** a user's stored theme is not one this version provides
- **THEN** the page uses the default theme

#### Scenario: No theme attribute
- **WHEN** a page reaches the stylesheet with no theme attribute on it
- **THEN** it gets Midnight's colours

### Requirement: Themes are readable
Every built-in theme SHALL meet WCAG AA contrast for body text, muted text, links, button labels and status labels against their backgrounds.

#### Scenario: Contrast is checked
- **WHEN** the themes are tested
- **THEN** each required pair of colours in each theme meets its contrast level

### Requirement: Themes need no script or inline style
The theme SHALL be selected by an attribute the server writes on the page, with every colour defined in the stylesheet.

#### Scenario: No inline style
- **WHEN** any page is rendered in any theme
- **THEN** it contains no inline style and no script written by Axis

### Requirement: Logo and tab icon
Every page SHALL show the Axis mark in its header and declare a tab icon, and the login and setup pages SHALL show the mark with the name. The mark SHALL follow the theme's colours.

#### Scenario: Mark follows the theme
- **WHEN** a page is rendered in any theme
- **THEN** the mark in its header is drawn in that theme's text and accent colours

#### Scenario: Tab icon
- **WHEN** a browser requests the tab icon a page declares
- **THEN** it is served with an image content type

### Requirement: Icons for browsers, home screens and tiles
Every page SHALL declare a vector and a bitmap tab icon, a touch icon for iOS home screens and Safari tiles, a pinned-tab icon, a web app manifest with 192 and 512 pixel icons including a maskable one, and a theme colour matching the page's theme. The daemon SHALL answer `/favicon.ico`, `/apple-touch-icon.png` and `/apple-touch-icon-precomposed.png` without a session.

#### Scenario: Icon requested without a session
- **WHEN** a client with no session requests `/favicon.ico` or `/apple-touch-icon.png`
- **THEN** the icon is returned with status 200 and an image content type

#### Scenario: Declared icons exist
- **WHEN** a page's head and the manifest are read
- **THEN** every icon they name is served, with the pixel size they state

#### Scenario: Touch icon is opaque
- **WHEN** the touch icon is inspected
- **THEN** it is 180 by 180 pixels and has no transparent pixels

#### Scenario: Nothing else became public
- **WHEN** a client with no session requests any other non-static path
- **THEN** it is redirected to log in as before

### Requirement: Two-level navigation, media first
The navigation SHALL have a row of primary tabs in this order: the dashboard, Media, activity, Library, system; and beneath it a second row listing the pages of the tab the current page belongs to. Inside Media the second row SHALL list the media types that are enabled (Movies, TV, and later others), in the order of their pages, and a third row, in the same thin style, the pages of the open type. A tab with no visible page SHALL NOT be shown, and a tab with a single page SHALL show no second row; a media type with a single page SHALL show no third row. Within a tab pages SHALL be ordered as their modules state. The current tab, the current media type and the current page SHALL each be marked. The Media tab SHALL lead to the first enabled type's first page.

#### Scenario: Order of tabs
- **WHEN** a page is rendered with pages in every tab
- **THEN** the tabs appear in the order dashboard, Media, activity, Library, system

#### Scenario: Second row
- **WHEN** a page under Library is open
- **THEN** the second row lists the Library pages, with the open one marked, and Library is marked as the current tab

#### Scenario: Media types and their pages
- **WHEN** a page of TV is open with the movies and TV modules enabled
- **THEN** the second row reads Movies, TV with TV marked, and the third row lists TV's pages with the open one marked

#### Scenario: Empty tab
- **WHEN** no enabled module has a page in a tab
- **THEN** that tab is absent

#### Scenario: Module switched off
- **WHEN** a module is disabled
- **THEN** its pages are absent from the navigation, and its media type from Media's second row

#### Scenario: Only enabled types
- **WHEN** the movies and TV modules are enabled and no other media module is
- **THEN** Media's second row reads Movies, TV and nothing else

### Requirement: Pages a user cannot use are not shown
A page marked for administrators SHALL be shown to administrators only, and a tab whose pages are all hidden from a user SHALL NOT be shown to that user.

#### Scenario: Non-administrator
- **WHEN** a page is rendered for a user who is not an administrator
- **THEN** administrator-only pages are absent, and so is a tab that has only such pages

### Requirement: The top row is one line
The top row SHALL hold the brand, the primary tabs, the title search's box, the attention control to the right of that box, the running-jobs indicator and the user's own controls. Adding a page to an existing tab SHALL NOT change the top row, a notice coming or going SHALL NOT change the row's width, and the row SHALL NOT wrap at desktop widths (wider than 77rem). Narrower than that and wider than a phone, the primary tabs SHALL take a line of their own under the rest of the row, which SHALL stay one line.

#### Scenario: More pages
- **WHEN** a module adds a page to an existing tab
- **THEN** the top row's contents are unchanged

#### Scenario: Does not wrap
- **WHEN** a page is rendered at 1280 pixels wide for an administrator with every module enabled
- **THEN** the top row occupies one line

#### Scenario: Narrower than the row
- **WHEN** a page is rendered at 1024 pixels wide for an administrator with every module enabled
- **THEN** the primary tabs are on a line of their own, and the brand, the search box, the attention control, the running-jobs indicator and the user's corner are on one line with nothing overlapping

#### Scenario: A count arrives
- **WHEN** the number in the attention control goes from none to 12
- **THEN** nothing else in the top row moves

### Requirement: Usable on a small screen without script
On a narrow screen each navigation row SHALL stay on one line and scroll sideways, without any script, and the content SHALL take the full width. On a phone (a screen at most 40rem wide) the header SHALL be laid out for a thumb, without any script: a top line with the brand, the header's indicators and the user's corner; the rows of pages under it as one strip that scrolls sideways (in Media the media types, then the open type's pages); then the search box with the attention control to its right in the same line, which SHALL stay at the top of the screen when the page is scrolled down; and the primary tabs SHALL be a bar fixed at the foot of the screen, every tab in it at once, each one tap. In that line the search box SHALL take the room the control leaves, the control SHALL be at least 44 CSS pixels wide and high, and neither SHALL overlap the other or be pushed out of the screen at 360 pixels. The bars SHALL keep their contents inside the screen's safe area. On a phone every link, box and button of every page SHALL be at least 44 CSS pixels high and wide.

#### Scenario: Narrow screen
- **WHEN** a page is shown at a tablet's width, wider than a phone and narrower than the top row
- **THEN** both navigation rows are reachable by sideways scrolling and nothing wraps

#### Scenario: A phone
- **WHEN** a page of TV is shown at 390 pixels
- **THEN** Dashboard, Media, Activity, Library and System are all in a bar at the foot of the screen, Movies, TV, Discover and the open type's pages are one strip under the top line, and the page does not scroll sideways

#### Scenario: Search in reach
- **WHEN** a long page is scrolled down on a phone
- **THEN** the search box and the attention control beside it stay at the top of the screen and the lines above them have scrolled away

#### Scenario: The narrowest phone
- **WHEN** a page is shown at 360 pixels to an administrator with a count of 99+
- **THEN** the search box's kind and text field and the attention control are on one line, none overlaps another, the text field is at least 9rem wide, the control is at least 44 pixels wide and high, and the page does not scroll sideways

#### Scenario: Safe area
- **WHEN** a page is rendered
- **THEN** its viewport reaches the screen's edges (`viewport-fit=cover`) and the bars pad themselves by the screen's safe-area insets

#### Scenario: Tap targets
- **WHEN** any page is shown at 390 pixels
- **THEN** no link, box or button on it is smaller than 44 pixels in width or height

### Requirement: A dashboard worth landing on
The dashboard SHALL show, in this order where each exists: what is coming in (downloads, recent imports, wanted titles), the space on each volume with the collection's size, what is running, and, to administrators, what needs attention; each summary linking to the page behind it and stating when its figures were taken. A section with nothing to show for lack of the feature SHALL be absent. The dashboard SHALL NOT list the daemon's internal modules.

#### Scenario: Space
- **WHEN** the dashboard is opened
- **THEN** each volume is shown with how full it is and its free space, and a volume that is offline or mismatched is called out

#### Scenario: Acquisition not present
- **WHEN** no acquisition module is enabled
- **THEN** the dashboard has no section for what is coming in

#### Scenario: Figures agree
- **WHEN** the dashboard shows a figure
- **THEN** the page it links to shows the same figure

#### Scenario: Nothing needs attention
- **WHEN** nothing is failed, missing, offline or waiting
- **THEN** the attention summary says so and lists nothing

#### Scenario: Something needs attention
- **WHEN** a volume is offline
- **THEN** the dashboard says so and links to it

### Requirement: A varied set of themes
The built-in themes SHALL include at least five dark themes whose backgrounds are visibly different colours from one another, at least two light themes, and a high-contrast theme. The default SHALL remain dark.

#### Scenario: Distinct backgrounds
- **WHEN** the built-in themes are compared
- **THEN** every theme's page background differs from every other theme's by at least the stated colour distance

#### Scenario: Two light themes
- **WHEN** the theme picker is opened
- **THEN** it offers at least two light themes

#### Scenario: Previews show the difference
- **WHEN** the theme picker is opened
- **THEN** each theme's preview shows its page colour, a card, its accent and the logo mark in that theme's colours

### Requirement: The brand leads the top bar
The logo mark and the name SHALL be the largest elements of the top bar and SHALL be vertically centred with its other contents.

#### Scenario: Brand size
- **WHEN** a page is rendered
- **THEN** the name is set larger than any navigation label and the mark is at least as tall as the name

### Requirement: Tables of one kind line up
Every data table SHALL declare its kind, and tables of the same kind SHALL have identical column positions regardless of their contents, on one page and across pages. Counts and sizes SHALL be right-aligned in figures of equal width, with their headings aligned to them. Actions SHALL be in a right-aligned column of their own.

#### Scenario: Two tables of one kind
- **WHEN** a page shows two tables of the same kind with different contents
- **THEN** each column starts at the same horizontal position in both

#### Scenario: Numbers
- **WHEN** a column holds counts or sizes
- **THEN** its cells and its heading are right-aligned

#### Scenario: Every table has a kind
- **WHEN** any page with a data table is rendered
- **THEN** the table declares a kind and a column for each role

### Requirement: Form controls line up
Text fields, drop-downs and buttons SHALL have the same height and box, and controls placed in one row SHALL share a bottom edge, with labels above their own control and help text below the row.

#### Scenario: Button beside a labelled field
- **WHEN** a button is placed in a row with a labelled text field or drop-down
- **THEN** the button's box and the field's box have the same height and the same bottom edge

#### Scenario: Filter bar
- **WHEN** a filter bar with several fields and an action is rendered
- **THEN** every control in a line has the same height and bottom edge

### Requirement: The dashboard shows downloads
When a download client is configured the dashboard SHALL show active downloads and the most recent arrivals, linking to the Downloads page.

#### Scenario: No client
- **WHEN** no download client is configured
- **THEN** the dashboard has no downloads section

#### Scenario: Active download
- **WHEN** a download is in progress
- **THEN** the dashboard shows it with its progress

### Requirement: Films have a tab
When the movies module is enabled the navigation SHALL show Movies among Media's types, listing the whole movie library (the films Axis manages and the movies on disk it does not) as posters with their states; its pages (All, Wanted, History) form its third row, and its list is headed "All movies". The Wanted and History pages SHALL be headed, and titled in the browser, "Wanted movies" and "Movie history".

#### Scenario: Films tab
- **WHEN** the movies module is enabled
- **THEN** the Media tab is present between the dashboard and Activity, and Movies is the first of its types

#### Scenario: Module off
- **WHEN** the movies module is disabled
- **THEN** Movies and its pages are absent, and so is the Media tab when no other type is enabled

#### Scenario: Headings that name the list
- **WHEN** the Wanted page under Movies is opened
- **THEN** its heading and its browser title say "Wanted movies", not "Wanted" alone

### Requirement: Sizes are written one way
Every page SHALL write sizes in binary units with the same formatter, and a whole number SHALL have no fraction.

#### Scenario: One unit
- **WHEN** a page shows a release's size and a volume's free space
- **THEN** both are in binary units

#### Scenario: Whole number
- **WHEN** a size is exactly 25 GiB
- **THEN** it is written "25 GiB"

### Requirement: Long names break at their separators
Release names, file names and paths SHALL be able to wrap after their separators and SHALL NOT be broken inside a word unless a single word is longer than the space, and their text SHALL be unchanged.

#### Scenario: Release name in a narrow column
- **WHEN** a release name does not fit its column
- **THEN** it wraps after a dot, hyphen or underscore

#### Scenario: Copying
- **WHEN** the name is copied from the page
- **THEN** the copied text is the name exactly

### Requirement: Row actions look like actions
An action in a table row that changes something SHALL be shown as a button.

#### Scenario: Override
- **WHEN** a release can be grabbed against the profile's verdict
- **THEN** the action, "Grab it anyway", is a button

### Requirement: Names and details have room on a phone
On a screen at most 40rem wide, a data table's rows SHALL be stacked: each row a block with its first cell and its state on its first line (a short first cell keeps the name beside it), a progress bar next when it has one, then every other cell after its column's heading, then its buttons; no table SHALL scroll sideways, and no table SHALL be laid out any differently on a wider screen. For this every body cell of a column with a heading SHALL carry the heading as its label.

#### Scenario: A phone
- **WHEN** the Downloads page is shown at 390 pixels
- **THEN** each download shows its name and state on its first line, its progress next, and its client, size, speed and time left each after its heading, and no part of it is off the screen

#### Scenario: Labels
- **WHEN** any page with a data table is rendered
- **THEN** every body cell of a column with a heading carries the heading as its label, and a cell across columns or under an empty heading carries none, also in a row rendered on its own as a live update

#### Scenario: A desktop
- **WHEN** the same table is shown at 1280 or 1440 pixels
- **THEN** its columns are where they were

### Requirement: State marks hold in every theme
Every film state's mark SHALL have its words at 4.5:1 against the mark's background and its edge at 3:1 against the card, in every theme.

#### Scenario: Not available yet
- **WHEN** a film is not out yet
- **THEN** its mark, which reads "Not available yet", has its words and edge meet those ratios in every theme

### Requirement: Counts read right in the singular
A count in a sentence SHALL use the singular for one.

#### Scenario: One release
- **WHEN** a search found one release
- **THEN** the page says "1 release found"

### Requirement: A reason is stated once
A page SHALL NOT repeat the same explanatory sentence in two places.

#### Scenario: Film needing attention
- **WHEN** a film needs attention
- **THEN** its reason appears once on its page

### Requirement: Series have a tab and a calendar
When the TV module is enabled the navigation SHALL show TV among Media's types, with pages for the series library (the managed series and the series on disk) and for a calendar of the episodes still to come by air date, in its third row; its list is headed "All series". The calendar, Wanted and History pages SHALL be headed, and titled in the browser, "TV calendar", "Wanted episodes" and "TV history".

#### Scenario: Series tab
- **WHEN** the TV module is enabled
- **THEN** TV is present in Media's second row after Movies

#### Scenario: Calendar
- **WHEN** the calendar is opened
- **THEN** the episodes of managed series that are not on disk are listed by the day they air, with their states, under the heading "TV calendar"

#### Scenario: Wanted and History are told apart from the movies' pages
- **WHEN** the Wanted page and the History page under TV are opened
- **THEN** their headings and browser titles say "Wanted episodes" and "TV history"

### Requirement: Lists of titles scale and can be filtered
Lists of titles SHALL be paged, SHALL be filterable by name and state, and SHALL be sortable, with the choices carried in the address. A page of a list SHALL be read in a number of statements that does not grow with the number of titles or of posters on it.

#### Scenario: Thousands of films
- **WHEN** five thousand films are managed
- **THEN** the films list shows one page of them with links to the rest and a total count

#### Scenario: Name filter
- **WHEN** part of a title is typed in the filter
- **THEN** only films whose title or other names contain it are listed

#### Scenario: Posters
- **WHEN** a page of the movies list shows sixty posters, and another a table of a hundred
- **THEN** both cost the same number of statements, and no statement is made per poster

### Requirement: Lists of titles have three views
A list of titles SHALL be offered as posters, as a list of rows and as a table, and a user's last choice for each list SHALL be remembered. The Movies and TV lists SHALL label the three views Posters, List and Table.

#### Scenario: Switching
- **WHEN** a user chooses the table view
- **THEN** the same titles are shown as a table, and the list opens as a table next time

#### Scenario: The labels of the views
- **WHEN** the Movies list is opened
- **THEN** its views are offered as Posters, List and Table

### Requirement: Library folders can be found and show what they are
Folder lists in the library SHALL have a name filter, and a folder identified as a title SHALL show that title and its artwork.

#### Scenario: Identified folder
- **WHEN** a film folder is identified
- **THEN** its row shows the film's poster and title

#### Scenario: Not identified
- **WHEN** a film folder is not identified
- **THEN** its row says so and links to where it can be identified

### Requirement: What is needed to download is shown until it is all there
While any of a TMDb token, an indexer source, a download client, a usable destination or writable storage is missing, the dashboard SHALL list what is needed with the state of each in its card "Still needed before Axis can download", and a wanted title's page SHALL say what stands in the way.

#### Scenario: Nothing configured
- **WHEN** a film is added with no indexer source and no download client
- **THEN** its page states that nothing can search for or download it yet and names what is missing

#### Scenario: Everything configured
- **WHEN** nothing is missing
- **THEN** the checklist is not shown

### Requirement: Table columns hold their contents in any font
A table column of fixed width SHALL be wide enough for its role's longest value in any common sans-serif or monospace face, and a value that does not fit SHALL be clipped within its cell.

#### Scenario: A wider font
- **WHEN** the browser draws a table in a wider face than the one the widths were measured with
- **THEN** a cell's content never overlaps the next column, whatever font the browser uses

### Requirement: Only work a person would watch counts as running
The running indicator and the dashboard's summary of what is running SHALL count only jobs that are work (such as a scan, a search, an import or a conversion), not background upkeep (such as checking download clients or schedules). Every job SHALL be named in plain words wherever it is shown.

#### Scenario: Only upkeep is running
- **WHEN** the only running job is the periodic check of the download clients
- **THEN** the running indicator shows none and the dashboard says nothing is running

#### Scenario: Plain names
- **WHEN** a job is listed
- **THEN** it is named in plain words, with its technical kind as a detail

### Requirement: Media management pages say what they are
The pages of the Library tab SHALL be named for what they do (Files, Identify, Disks, Quality profiles, then Disk usage, Space savings, Conversion profiles, Player compatibility, Conversion workers, Conversions, Conversion runs), in two runs with a thin divider between looking after what is there and converting it, and every page of the Library tab and of System SHALL open, directly under its heading, with one or two plain sentences saying what it is for and why someone would care. A page's heading SHALL begin with its navigation label and MAY go on to say what the page holds ("Identify movies and series on disk", "Disks and library folders").

#### Scenario: Names and runs
- **WHEN** an administrator opens a page of the Library tab with every module enabled
- **THEN** the second row reads Files, Identify, Disks, Quality profiles, then after a divider Disk usage, Space savings, Conversion profiles, Player compatibility, Conversion workers, Conversions, Conversion runs, and each page's heading begins with its label

#### Scenario: What a page is for
- **WHEN** any page of the Library tab or of System is opened from its row
- **THEN** a muted sentence or two under its heading says what it is for

#### Scenario: Conversion workers
- **WHEN** the Conversion workers page is opened
- **THEN** it says that a worker is a separate program, on this machine or another with a better CPU or GPU, that does the heavy re-encoding so the server does not have to, that none is needed until conversions are used, and what the page lets you do; and each worker's jobs are named in plain words

### Requirement: The media types are named Movies and TV
Wherever the interface names the two media types (navigation, page headings, tab labels, the search's kind and its results' tabs and headings, the dashboard and its checklist of what is still needed, form labels) it SHALL call them Movies and TV; running sentences SHALL use plain nouns such as "movie", "series" and "episode", and addresses, code and job kinds SHALL be unchanged. A heading MAY add the noun of what it lists ("Wanted movies", "Wanted episodes", "Discover movies").

#### Scenario: Labels and sentences
- **WHEN** a movie is added with no library folder for movies
- **THEN** the dashboard's card "Still needed before Axis can download" has the item "A library folder for Movies", and its sentence says that no library folder for movies is set up

### Requirement: No page is wider than a phone
At 390 pixels no page SHALL scroll sideways; only a strip of tabs or links SHALL be wider than the screen, and it SHALL scroll inside its own box.

#### Scenario: Every page family
- **WHEN** every page the demo instance lists is shown at 390 pixels
- **THEN** none is wider than the screen

### Requirement: Controls fit a thumb on a phone
On a screen at most 40rem wide, every field, drop-down, button, tab, page link, list link and disclosure SHALL be at least 44 CSS pixels high, and the text in a field SHALL be at least 16 pixels so that the phone does not zoom in when it is focused. Links inside running text are exempt.

#### Scenario: Searching and adding
- **WHEN** the title page of a movie that is not added is shown at 390 pixels
- **THEN** the search box, its kind, the add form's disclosure and its button are each at least 44 pixels high, and the search box's text is 16 pixels

### Requirement: Every field follows the theme
Number and time fields SHALL have the same box and colours as text fields, in every theme and at every width.

#### Scenario: Discover's years
- **WHEN** Discover's filters are shown in a dark theme
- **THEN** the year fields are drawn with the theme's field background and border, not the browser's light default

### Requirement: The pages can be looked at on a phone
The repository SHALL keep a demo instance, filled with made-up titles in every state a page shows, and a tool that captures its pages at 390 by 844 and 1440 by 900 pixels with a report of sideways scrolling, tap targets under 44 pixels and small text, run by one command under the throwaway database of the tests. It SHALL NOT be part of `make check`, SHALL listen on loopback only, SHALL stop everything it started, and SHALL contain no real title, overview, poster, person, host or path.

#### Scenario: Taking the screenshots
- **WHEN** `tests/tools/axis-demo-shots.sh DIR` is run with Chrome installed
- **THEN** DIR holds the screenshots of every page the demo lists at both sizes and a report naming any page wider than the phone, and no daemon, browser or database container of the run is left

#### Scenario: Made up
- **WHEN** the demo instance's pages are read
- **THEN** every title, overview and poster on them comes from the demo's own catalogue of made-up titles, and nothing is fetched from outside the machine

### Requirement: A form comes back to where the person was
Every form that leaves the person on its page (or on the page it was posted from) SHALL come back to the row or card it acted on, by an anchor in the redirect ("?done=<word>&at=<id>#<id>", or HX-Redirect for htmx), and the page SHALL draw its notice inside that row or card, not at the top; when that row or card is not drawn any more, the notice SHALL be at the top. Every row and card a form acts on SHALL have a stable id. The Identify pages, which do this already, are not changed.

#### Scenario: Save on a settings card
- **WHEN** an administrator saves a card of Settings
- **THEN** the browser lands on that card and the notice is inside it

#### Scenario: The row is gone
- **WHEN** a form's row is not on the page it comes back to
- **THEN** the notice is drawn at the top of the page

### Requirement: A list of titles keeps its place
After Add, Hide from Discover, or the Add of a title page opened from it, a Discover list or the search results SHALL come back to the same list (tab, filters, page, query) at the card acted on, or at the next card when that one left the list; the notice there SHALL name the title ("Added Night Kite (2020) to Movies."), and the card SHALL show its new state. A title page opened from a list SHALL know it (`?from=`): its back link and its Add SHALL come back to the list at the card. The search results' cards SHALL have the one-step Add of Discover's.

#### Scenario: Add from a filtered Discover list
- **WHEN** an administrator adds a film from the second page of Popular filtered by a genre
- **THEN** the browser lands on that page, with that filter, at the film's card, and the notice in it names the film

#### Scenario: Not interested
- **WHEN** an administrator marks a card Hide from Discover
- **THEN** the browser lands at the card that followed it

### Requirement: Where a person's Adds from a list go
Above a Discover list and the search results, an administrator SHALL have a bar that says where Add puts a title of each kind shown ("New movies go to [folder] with [profile]"; for series also which episodes to monitor), with the same choices as a title's own Options; a folder on a disk that is offline or mounted read-only SHALL be shown, with why, and SHALL NOT be choosable, in the bar or in a title's own Options, and an Add naming one (a card drawn before, a hand-made post) SHALL be refused on the server with the folder and why. A choice that became such a folder, or was removed, SHALL be no choice (the installation's defaults), and the bar SHALL say so. The choice SHALL apply at once to every Add on the page (their forms carry it, their closed Options say it), SHALL be that person's own, kept with their account (beside the theme) across sessions; the first time it SHALL show the installation's defaults. A title's own Options SHALL still choose for that title. Without htmx the bar SHALL be a form with a "Save as my defaults" button; with htmx a change SHALL post by itself and swap the bar and every card's Add out of band, the page staying where it is. A viewer SHALL NOT see the bar.

#### Scenario: Choose another folder
- **WHEN** an administrator chooses another movie folder in the bar
- **THEN** every card's Add on the page uses it, and so do the cards in a later session

#### Scenario: A folder away
- **WHEN** a movie folder's disk is away
- **THEN** the bar lists it as one that cannot be written to now, and choosing it is refused

#### Scenario: A disk mounted read-only after the choice
- **WHEN** the folder a person chose is on a disk that is then mounted read-only, and a card drawn before it is added
- **THEN** the Add is refused, naming the folder and that its disk is mounted read-only; nothing is added; the bar says the choice cannot be used now, and the cards' Adds use the default folder

### Requirement: The attention control is in the header of every page
The header of every page SHALL show, to an administrator (every notice is an administrator's), a control to the right of the title search's box: a bell with the number of notices that user has not dismissed. The control SHALL be a link to the page "Needs attention" (`/attention`) and SHALL work without script. Its accessible name SHALL say the number in words a screen reader can read whole ("3 things need attention", "1 thing needs attention", "Nothing needs attention") and SHALL say when any of them is "Not working", so that colour is never the only sign. The control SHALL keep one width whatever the number, a number above 99 SHALL read "99+", and with nothing to show the bell SHALL be drawn muted with no number. When the title search is off, the control SHALL take the box's place. A user who is not an administrator SHALL be shown no control.

#### Scenario: Something waits
- **WHEN** an administrator with three notices, one of them "Not working", opens any page
- **THEN** the header shows the bell with 3 directly to the right of the search box, named "3 things need attention, 1 not working", linking to `/attention`

#### Scenario: Nothing waits
- **WHEN** an administrator with no notice opens any page
- **THEN** the bell is there, muted, with no number, named "Nothing needs attention"

#### Scenario: Without script
- **WHEN** the control is selected in a browser that runs no script
- **THEN** the page "Needs attention" opens

#### Scenario: A large number
- **WHEN** there are 140 notices
- **THEN** the control reads "99+" and is as wide as with 3

#### Scenario: The page is in no tab
- **WHEN** `/attention` is open
- **THEN** no primary tab is marked as open, and the control is marked as the page that is open

### Requirement: The dashboard's card is the head of the same list
The dashboard's card "Needs attention" SHALL be drawn from the same notices as the page `/attention`, for the user who looks: the notices that user has not dismissed, the most serious first, at most five of them, each with its level in words and its sentence as the link to the page where it is dealt with, and a link to the page `/attention` that says how many there are when there are more. The card SHALL follow the same waits and dismissals as the page, so that the card, the bell and the page drawn at the same moment never differ; drawing it SHALL run no database statement; and no part of the dashboard SHALL add a line to it by a computation of its own. The card SHALL be as fresh as the view of the dashboard: it SHALL NOT follow the event stream, while the bell above it does. A user who is not an administrator SHALL be shown no such card. A disk that is away SHALL still be called out at once on the dashboard's Disk space card.

#### Scenario: The same list
- **WHEN** an administrator has three notices, one "Not working", one "Waiting for you" and one "Worth a look"
- **THEN** the card lists them in that order, each with its level in words, each linking to where it is dealt with, and ends with a link to `/attention`

#### Scenario: The same waits
- **WHEN** a disk has been offline for three minutes
- **THEN** the card "Needs attention" does not list it, and the Disk space card marks the disk offline

#### Scenario: Dismissed
- **WHEN** an administrator dismisses a notice on `/attention`
- **THEN** it is gone from that administrator's card and still on another administrator's

#### Scenario: More than the card shows
- **WHEN** an administrator has seven notices
- **THEN** the card lists the five most serious and links to "All 7 on the Needs attention page"

#### Scenario: An ordinary user
- **WHEN** a user who is not an administrator opens the dashboard
- **THEN** it has no "Needs attention" card and names no notice

### Requirement: Labels say what they hold
Every heading, card title, column header, link, button, count, empty state and notice SHALL say what it holds or does in words that a person who has never seen the page understands without the rest of the page. A card SHALL be titled by what it holds, not by the object the page is about; a link SHALL name where it goes and SHALL NOT be a pronoun or a relative word alone; a button SHALL name what it acts on when the page holds more than one such thing; a count SHALL carry its noun and its whole; an empty state SHALL say what to do next; a notice that names a setting SHALL name it by its label, with its key at most as a detail beside it. A state that movies and series share SHALL have the same word for both, indexers and download clients SHALL call refused credentials "Access refused", and Settings SHALL show each choice of a choice setting by its label, with the stored value at most as a detail.

#### Scenario: A card names its contents
- **WHEN** an administrator opens an indexer's page
- **THEN** its cards are titled "State of this indexer" and "Settings for this indexer", and no card is titled "State" or "Settings" alone

#### Scenario: A link read alone
- **WHEN** the page of a managed series is opened
- **THEN** its link to the series' title page reads "Series overview", and no link on the page reads "Its title page"

#### Scenario: A count with its noun and its whole
- **WHEN** a managed series has 71 aired monitored episodes and none of them is on disk
- **THEN** its title page and its row in the TV list say "0 of 71 aired episodes in the library"

#### Scenario: One state word for movies and TV
- **WHEN** a movie and an episode both need attention
- **THEN** the mark of each reads "Needs attention", and no mark reads "Attention" alone

#### Scenario: A button names what it acts on
- **WHEN** an administrator opens a download client's page
- **THEN** its buttons read "Test this download client now" and "Remove this download client"

#### Scenario: An empty state says what to do
- **WHEN** no download client is set up and an administrator opens the Downloads page
- **THEN** the page says that no download client is set up yet and links to the Download clients page with "Add a download client"

#### Scenario: A setting named by its label
- **WHEN** Read-only storage is on and an import waits because of it
- **THEN** the import's page names the setting "Read-only storage" and where it is set, and shows `storage.read_only` at most in parentheses after it

#### Scenario: One word for the TMDb credential
- **WHEN** no TMDb token is stored
- **THEN** the dashboard's checklist, the search page and the Setup guide all call what is missing a TMDb token, and none calls it a metadata key


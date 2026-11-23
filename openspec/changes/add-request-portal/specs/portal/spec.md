# portal Specification (delta)

## ADDED Requirements

### Requirement: The portal API is answered on a listener of its own, which is off by default
The daemon SHALL answer the portal API (every path under `/api/v1/portal/`) only on a second listener, on the port `server.portal_listen_port`, which SHALL be 0 (no listener) by default. That listener SHALL answer nothing but the portal API: no page, no static file, no login form, no worker API, no probe and no metrics. The daemon's main listener SHALL answer every path under `/api/v1/portal` as a path that does not exist, with or without a credential.

#### Scenario: Off by default
- **WHEN** the daemon starts with no portal port configured
- **THEN** it listens on its main port only, and a request to `/api/v1/portal/hello` there, with a valid portal token, is answered 404

#### Scenario: Only the API on the portal listener
- **WHEN** the portal listener is on and requests for `/`, `/login`, `/static/axis.css`, `/admin/users`, `/api/v1/worker/claim`, `/healthz` and `/metrics` arrive on it
- **THEN** each is answered 404 in JSON, with no redirect, no cookie and no page

#### Scenario: Not on the main listener
- **WHEN** the portal listener is on and a portal operation is sent to the main listener with a valid portal token
- **THEN** it is answered 404 and nothing is done

### Requirement: Portal tokens
A portal SHALL authenticate with a token that an administrator creates on the page "Request portal", which is shown once and stored only as a hash. A portal token SHALL be valid only for the portal API, SHALL be revocable with immediate effect, and SHALL NOT appear in any page, log line or error. At most four portal tokens SHALL be active at once, so that a token can be replaced without an interruption. A token SHALL be accepted only in the `Authorization` header: a request to the portal API that has a query string SHALL be refused, whatever it carries.

#### Scenario: A valid token
- **WHEN** a portal presents its token to `hello`
- **THEN** the request is accepted

#### Scenario: A revoked token
- **WHEN** an administrator revokes a token and it is presented again
- **THEN** the request is refused with 401

#### Scenario: Shown once, stored as a hash
- **WHEN** an administrator creates a portal token
- **THEN** it is shown on that page once, cannot be retrieved again, and the database holds only its hash

#### Scenario: Replacing a token
- **WHEN** a second token is created, the portal is restarted with it, and the first is revoked
- **THEN** the portal works throughout and the first token is refused afterwards

#### Scenario: A worker's token
- **WHEN** a worker's token is presented to the portal API
- **THEN** it is refused with 401, and counts as a wrong token

#### Scenario: A portal token elsewhere
- **WHEN** a portal token is presented to the worker API or to a page of the engine's interface
- **THEN** it grants nothing

#### Scenario: A session on the portal API
- **WHEN** a request to the portal API carries an administrator's logged-in session and no portal token
- **THEN** it is refused

#### Scenario: A token in the address
- **WHEN** a request to the portal API carries a valid token in its query string, with or without the header
- **THEN** it is refused with 400 and nothing is done

#### Scenario: Wrong tokens are throttled
- **WHEN** wrong tokens are presented repeatedly from one address
- **THEN** further requests from that address are answered 429 for a time, without their token being looked at

### Requirement: The scope is a closed list of operations
The portal API SHALL consist of exactly these operations, each a POST of one JSON object: `hello`, `login`, `logout`, `whoami`, `search`, `browse`, `title`, `art`, `request`, `requests` and `withdraw`. A portal token SHALL open nothing else in the daemon. The daemon SHALL refuse, on the portal API, any method but POST (405), any body that is not `application/json` (415) or is larger than 16 KiB (413), any request that carries what a browser sends (`Origin`, `Sec-Fetch-Site`, `Sec-Fetch-Mode` or `Cookie`: 403), any path that is not one of the operations (404), and any body with a member that the operation does not define, or a member of the wrong type or over its length (422). Every refusal SHALL be JSON, and none SHALL redirect or set a cookie.

#### Scenario: Not an operation
- **WHEN** a valid token posts to `/api/v1/portal/approve`, `/api/v1/portal/users` or `/api/v1/portal/requests/all`
- **THEN** each is answered 404 and nothing is done

#### Scenario: Every other route of the daemon
- **WHEN** a test presents a valid portal token, with no session, to every route in the daemon's route table, on both listeners
- **THEN** none answers as it would to a session or a worker, and the only routes that do anything are the eleven operations on the portal listener

#### Scenario: A browser
- **WHEN** a request with a valid token also carries `Origin` or `Cookie`
- **THEN** it is refused with 403

#### Scenario: Methods and bodies
- **WHEN** an operation is sent as GET, with a form body, or with a body of 17 KiB
- **THEN** it is refused with 405, 415 and 413

#### Scenario: A member that is not defined
- **WHEN** a `request` body carries `profile`, `folder`, `requester`, `user_id` or `approve`
- **THEN** it is refused with 422 and no request exists

### Requirement: Login through the API is for requesters only, and its answers are uniform
The `login` operation SHALL take a username and a password, SHALL have the daemon check the password against its argon2id hash, and SHALL answer with a session proof for an enabled account whose role is requester. It SHALL answer an unknown username, a disabled account, a wrong password and an account of any other role with the same status, the same body and the same work (one password hash is computed in each case), and SHALL NOT say which it was. No operation SHALL return a password hash, and none SHALL say whether an account exists. A requester's password SHALL have at least 12 characters and SHALL NOT contain the username; an administrator sets it.

#### Scenario: A requester logs in
- **WHEN** `login` carries the right password of the enabled requester `robin`
- **THEN** the answer carries a session proof, `robin`'s name and when the proof expires

#### Scenario: Uniform failures
- **WHEN** `login` carries an unknown username, a disabled requester's right password, a wrong password, or the right password of an administrator or of an ordinary user
- **THEN** each answer has the same status and the same body, and each took one password hash

#### Scenario: An administrator's password opens nothing here
- **WHEN** `login` carries an administrator's right username and password
- **THEN** no session proof is made, for the portal API or for the engine's interface

#### Scenario: Password rules for a requester
- **WHEN** an administrator sets a requester's password to 11 characters, or to one that contains the username
- **THEN** it is refused with a message that states the rule

### Requirement: A portal token alone acts as no one
Every operation except `hello` and `login` SHALL require, beside the portal token, the session proof of the person it is for: a random value that the daemon issued at that person's login and stores only as a hash. The daemon SHALL take the person from the proof and from nothing else in the request, so that no operation can name a person to read or to act for. A proof SHALL stop working at once when the person logs out, when their account is disabled or deleted, when their password or their role changes, 30 days after its last use and 90 days after it was issued. A proof that is missing, unknown or ended SHALL be answered 401 with the same body in each case. A session proof SHALL NOT be accepted as a session of the engine's interface, and a session of the engine's interface SHALL NOT be accepted as a proof.

#### Scenario: The token without a proof
- **WHEN** a valid portal token calls `requests`, `request`, `search`, `browse`, `title`, `art`, `whoami` or `withdraw` with no proof
- **THEN** each is answered 401 and nothing is read or done

#### Scenario: Naming a person does nothing
- **WHEN** `requests` is called with `robin`'s proof and a member that names `sam`
- **THEN** it is refused with 422, and with the member removed the answer holds `robin`'s requests only

#### Scenario: Another person's request
- **WHEN** `withdraw` is called with `robin`'s proof and the identifier of a request that `sam` made and `robin` does not follow
- **THEN** the answer is 404, the same as for an identifier that was never issued, and the request is unchanged

#### Scenario: A made-up proof
- **WHEN** an operation carries a well-formed proof that the daemon did not issue
- **THEN** it is answered 401

#### Scenario: Disabled at once
- **WHEN** an administrator disables `robin` and `robin`'s proof is presented in the next request
- **THEN** it is answered 401

#### Scenario: A password or role change ends proofs
- **WHEN** an administrator changes `robin`'s password, or makes `robin` an ordinary user
- **THEN** every proof issued to `robin` before is answered 401

#### Scenario: Logout
- **WHEN** `logout` is called with a proof and the proof is presented again
- **THEN** it is answered 401

#### Scenario: Not a session of the engine
- **WHEN** a session proof is sent to the engine's interface as its session cookie, and an engine session's cookie value is sent to the portal API as a proof
- **THEN** neither is accepted

#### Scenario: Stored as a hash
- **WHEN** the session table is read from the database
- **THEN** it holds no value that can be used as a proof

### Requirement: Answers carry a fixed set of fields
Each operation SHALL answer with the members listed here and no others. A title SHALL be `id`, `kind`, `name`, `year`, `overview` (at most 600 characters), `has_poster`, `state` (one of `available`, `partly_available`, `downloading`, `approved`, `requested`, `declined`, `closed`, `open`), `asked_by` (a name, present only while `requests.show_who_asked` is on) and, from `title` for a series, `seasons` (each `number`, `episodes`, `aired` and `state`). A request SHALL be `id`, the title's `id`, `kind`, `name` and `year`, `seasons`, `state` (one of `waiting`, `approved`, `downloading`, `partly_available`, `available`, `declined`, `withdrawn`, `failed`), `have` and `of` (episode counts, for a series), `cause` (one of the fixed causes, when failed), `reason` (when declined), `mine` (made or followed), `asked_at` and `changed_at`. `whoami` SHALL be the person's name and, for each kind, the limit, what is left and when the window ends. No answer SHALL carry a file name, a path, a release name, an indexer, a download client, a quality profile, a library folder, a disk, a size on disk, a job, a setting, the daemon's version, an account's role or an account other than as `asked_by`.

#### Scenario: A search result's members
- **WHEN** `search` answers
- **THEN** each result has exactly the members of a title, and the answer's other members are `page`, `more` and `stale`

#### Scenario: Nothing of the library's inner workings
- **WHEN** a library holds files, releases, grabs and imports for the titles in an answer of `search`, `browse`, `title` or `requests`
- **THEN** the answer's text holds none of their file names, paths, release names, indexer names, client names, profile names or folder names

#### Scenario: Who asked follows the setting
- **WHEN** `requests.show_who_asked` is off
- **THEN** no answer of any operation carries `asked_by`, and none carries a name other than the caller's own

#### Scenario: Only the caller's requests
- **WHEN** `requests` is called with `robin`'s proof
- **THEN** it lists the requests `robin` made and follows, and no other

### Requirement: Posters come from the engine's own artwork by title, never by address
The `art` operation SHALL take a title's `id` and SHALL answer with that title's poster from the artwork the daemon already holds, as an image of at most 4 MiB with its type, or 404 when the daemon holds none. It SHALL take no address, no file name and no artwork identifier, and SHALL fetch nothing from anywhere on the caller's account. The portal SHALL serve a poster only at `/art/<title id>`, to a logged-in person, and SHALL fetch nothing from any address but the daemon's.

#### Scenario: A poster by title
- **WHEN** `art` is called for a title whose poster the daemon holds
- **THEN** the answer is that image

#### Scenario: No address is taken
- **WHEN** `art` is called with a member that holds an address, a path or a hash
- **THEN** it is refused with 422, and the daemon made no outbound request

#### Scenario: Not held
- **WHEN** `art` is called for a title whose poster was never downloaded
- **THEN** the answer is 404, no outbound request is made for it, and the portal shows the title without a picture

### Requirement: Searching for a title to request is bounded
The `search` operation SHALL pass the query, of at most 100 characters, to the configured details provider as a search text and to nothing else, SHALL apply the same adult filter as the engine's own search, and SHALL answer from the daemon's stored results, marked `stale`, when the provider cannot be reached. The `title` operation SHALL refuse a title that the adult filter excludes. `browse` SHALL answer from the stored Discover lists, without the titles marked "Not interested", and SHALL never ask the provider. The same query for the same kind within 10 minutes SHALL be answered without asking the provider again.

#### Scenario: A search
- **WHEN** `search` is called with "lantern orchard"
- **THEN** the provider is asked once and the results carry each title's state

#### Scenario: Asked once
- **WHEN** the same search is made five times within 10 minutes, by any callers
- **THEN** the provider was asked once

#### Scenario: The provider is unreachable
- **WHEN** the provider does not answer
- **THEN** `search` answers from stored results with `stale` set, or with none and `stale` set

#### Scenario: Too long
- **WHEN** the query has 101 characters, or holds a control character
- **THEN** it is refused with 422 and the provider is not asked

#### Scenario: Browse asks nobody
- **WHEN** `browse` is called
- **THEN** no request goes to the provider

### Requirement: The daemon's limits are the authority
The daemon SHALL enforce, on the portal API and whatever the portal does itself: the login throttle for each username, with the delays of the engine's own login; a ceiling of 30 failed logins an hour through all portal tokens together, after which a login is admitted once a minute until the hour has passed; a search budget of 30 searches a minute through all portal tokens together and 10 a minute for each person; each person's request quota; and a ceiling of `requests.portal_per_day` (30 by default) on what all requests made through the portal API count in one day. A refusal by a limit SHALL be 429 with the time to wait and SHALL do nothing. The client address that `login` reports SHALL be recorded as reported by the portal, and SHALL only ever add a refusal: no limit SHALL be loosened or cleared by what the portal reports.

#### Scenario: Guessing one password
- **WHEN** `login` fails many times for `robin`
- **THEN** further attempts for `robin` are refused with a time to wait, even with the right password, as at the engine's own login

#### Scenario: Guessing across names
- **WHEN** 30 logins fail within an hour, each for another username and each reported from another address
- **THEN** the next login is refused with a time to wait, and at most one a minute is admitted until the hour has passed

#### Scenario: Reported addresses clear nothing
- **WHEN** `robin` is delayed and a `login` for `robin` reports a new client address
- **THEN** it is refused with the same time to wait

#### Scenario: The search budget
- **WHEN** 31 different searches arrive within a minute
- **THEN** the 31st is refused with 429 and the provider was asked at most 30 times

#### Scenario: The portal's daily ceiling
- **WHEN** requests made through the portal API today count 30 in all, and another person with quota left asks for a movie
- **THEN** it is refused with 429, saying that the portal's limit for today is reached, and no request exists

#### Scenario: Quota does not depend on the client
- **WHEN** a person is at their limit and `request` is called for them again and again
- **THEN** each call is refused and no request exists

### Requirement: The portal is a separate program that holds nothing but its token
The project SHALL provide a program, `axis-portal`, that serves the request pages and talks to the daemon only through the portal API. It SHALL NOT be linked with the database client library, SHALL have no setting for a database, a media path, an indexer, a download client or a details provider, and SHALL refuse a configuration key it does not know. It SHALL take its token from a file or the environment, never from the command line. It SHALL verify the daemon's certificate by default and SHALL refuse to send its token over unencrypted HTTP to an address that is not loopback unless explicitly configured to. It SHALL write no file: it SHALL run with a read-only root filesystem, no volume and no writable directory.

#### Scenario: Not linked with the database client
- **WHEN** the built `axis-portal` is examined
- **THEN** it does not depend on libpq and holds none of the daemon's database, storage, indexer or download code

#### Scenario: An unknown key
- **WHEN** `axis-portal` is given a configuration with `database_url`
- **THEN** it refuses to start and names the key

#### Scenario: Plain HTTP refused
- **WHEN** `axis-portal` is configured with an unencrypted address that is not loopback and no override
- **THEN** it refuses to start and says why

#### Scenario: Nothing writable
- **WHEN** `axis-portal` runs with a read-only root filesystem, no volume and no tmpfs, and people log in, search and ask
- **THEN** everything works and no file was created

#### Scenario: The daemon is away
- **WHEN** the daemon does not answer
- **THEN** the portal's pages say that requests are not available now, with 503, and the portal keeps running

### Requirement: The portal's pages
`axis-portal` SHALL serve these pages and no others: `/login`, `/logout`, `/` (the lists to browse), `/search`, `/title/<id>`, `/requests` (my requests), the posts that ask for a title and that withdraw a request, `/art/<title id>` and its static files. Every page and post except `/login` and the static files SHALL require a session, and a request without one SHALL be sent to `/login`; a health probe SHALL be answered only to a loopback address and SHALL say nothing but that the program runs. The pages SHALL be rendered on the server, SHALL send the same security headers and content security policy as the engine's interface, and SHALL load nothing from a third party. Every title SHALL show its state, a request form SHALL offer for a series the whole series, the first season, the latest season or picked seasons, and the page SHALL show what is left of the quota. Text from the provider SHALL never be markup.

#### Scenario: Without a session
- **WHEN** a client without a session requests `/`, `/search`, `/title/7`, `/requests` or `/art/7`, or posts a request
- **THEN** each is sent to `/login` (an htmx request is answered 401) and the daemon is not asked anything

#### Scenario: The probe
- **WHEN** `/healthz` is requested from an address that is not loopback
- **THEN** it is answered 404

#### Scenario: Headers
- **WHEN** any page of the portal is served
- **THEN** it carries the engine interface's content security policy and security headers, and refers to no address but the portal's own

#### Scenario: A title's state and the form
- **WHEN** a logged-in requester opens a series that is partly available
- **THEN** the page shows the state of the series and of each season, offers only the seasons that can be requested, and shows what is left of the quota for seasons

#### Scenario: Provider text
- **WHEN** a title's overview holds `<script>` and a quotation mark
- **THEN** the page shows them as text

#### Scenario: A failure in plain words
- **WHEN** a request reads failed with the cause that a download failed
- **THEN** "My requests" says "A download failed. Axis is looking again."

### Requirement: The portal is not a way to the engine's pages
`axis-portal` SHALL answer a path it does not serve with its own 404 and SHALL NOT pass any request on. Its only requests to the daemon SHALL be the eleven operations, to the one configured address, built by the portal from fields it has checked; no part of a browser's request SHALL choose the address, the path or a header of a request to the daemon.

#### Scenario: The engine's paths
- **WHEN** a logged-in requester requests `/admin/users`, `/films`, `/attention`, `/api/v1/worker/claim` or `/api/v1/portal/requests` from the portal
- **THEN** each is answered 404 by the portal and the daemon received no request

#### Scenario: Headers are not passed on
- **WHEN** a browser's request carries `Authorization`, `X-Forwarded-For`, `Host` and `Cookie` values of its own choosing
- **THEN** no request to the daemon carries any of them, and the daemon's address is the configured one

### Requirement: The portal's sessions, cookies and forms
After a login the portal SHALL set a session cookie that holds the session proof, with `HttpOnly`, `Secure`, `SameSite=Lax`, `Path=/` and the `__Host-` prefix, and SHALL never put the proof, or any secret, in an address. It SHALL reject any request other than GET and HEAD that does not carry the CSRF token derived from that cookie, or that the browser reports as cross-site; the login form SHALL carry a token bound to a short-lived cookie set before login. A `next` value that is not a path of the portal SHALL be ignored. The portal SHALL serve the login form and accept a login only over HTTPS, as its own listener or a listed trusted proxy reports it, unless explicitly configured otherwise for a test.

#### Scenario: The cookie
- **WHEN** a login succeeds
- **THEN** the cookie is named with the `__Host-` prefix and has `HttpOnly`, `Secure`, `SameSite=Lax` and `Path=/`

#### Scenario: A post without the token
- **WHEN** a logged-in person's request post arrives without the CSRF token, with another session's token, or with an `Origin` of another site
- **THEN** the response is 403 and the daemon is not asked to make a request

#### Scenario: Nothing secret in an address
- **WHEN** the pages a person walks through after login are read
- **THEN** no link, form action or redirect holds the proof, the CSRF token or the portal token

#### Scenario: An open redirect
- **WHEN** a login succeeds with a `next` value that is an absolute address
- **THEN** the person is sent to `/`

#### Scenario: Plain HTTP
- **WHEN** the login form is requested over plain HTTP and no override is set
- **THEN** no form is served, the page says that the portal needs HTTPS, and a posted login is refused before the daemon is asked

#### Scenario: Forwarded headers from elsewhere
- **WHEN** a request that does not come from a listed trusted proxy carries `X-Forwarded-Proto: https` and `X-Forwarded-For`
- **THEN** both are ignored

### Requirement: The portal keeps nothing, and a change at the daemon takes effect at once
`axis-portal` SHALL keep no session and no account of its own: every page that shows or changes something of a person SHALL present that person's proof to the daemon, and the daemon's answer SHALL decide. The portal MAY keep in memory, for at most 30 seconds, what `whoami` said, to draw a page's header, and SHALL drop it at a 401. A restart of the portal SHALL log nobody out. What the portal holds in memory SHALL be only: its token, its throttle counts, the key of its pre-login forms, answers of `whoami` for at most 30 seconds, and posters up to a fixed size.

#### Scenario: A restart
- **WHEN** a person is logged in and the portal restarts
- **THEN** their next page is served without logging in again

#### Scenario: Disabled while logged in
- **WHEN** an administrator disables a requester who has a page of the portal open
- **THEN** the requester's next request, page or post, is sent to `/login`, and no request can be made in between

#### Scenario: A role change
- **WHEN** a requester is made an ordinary user
- **THEN** their next request to the portal is sent to `/login`

#### Scenario: Logout
- **WHEN** a person logs out and their old cookie is sent again
- **THEN** it is treated as no session, by the portal and by the daemon

### Requirement: The portal refuses cheaply before it asks the daemon
`axis-portal` SHALL keep limits of its own, in memory, so that a flood is refused without a request to the daemon: failed logins for each client address (an IPv6 address by its `/64`) with the delays of the engine's throttle; at most 10 searches a minute and 10 request posts a minute for each session; and the engine interface's limits on connections, request heads and bodies, with a body of at most 64 KiB. A refusal SHALL be 429 with the time to wait. These limits SHALL add to the daemon's and SHALL NOT replace them.

#### Scenario: A flood of logins from one address
- **WHEN** a hundred wrong logins arrive from one address
- **THEN** the daemon was asked for at most the free failures and one more, and the others were answered 429 by the portal

#### Scenario: A flood of searches
- **WHEN** a session sends 50 searches within a minute
- **THEN** the daemon was asked for at most 10

#### Scenario: A large body
- **WHEN** a post of 65 KiB arrives
- **THEN** it is answered 413 and not read

### Requirement: A remembered browser is not locked out by a stranger
A successful `login` SHALL answer with a device value, signed by the daemon as the engine interface's device cookie is, that the portal sets as a cookie (`HttpOnly`, `Secure`, `SameSite=Strict`) and sends back with that browser's later logins. A login that presents a valid device value of the account SHALL be throttled on that device's own count and on the count shared by the account's devices, as at the engine's own login, and not on the username's count. The portal SHALL treat the value as opaque.

#### Scenario: The owner still gets in
- **WHEN** `robin` logged in from a browser once, and a stranger then fails for `robin` until the username is delayed
- **THEN** the stranger is refused with a time to wait even with the right password, and `robin` logs in from the remembered browser at once

#### Scenario: An altered device value
- **WHEN** the username is delayed and a login presents a device value that was changed, or that belongs to another account
- **THEN** it is refused with the same time to wait

### Requirement: What the portal logs
`axis-portal` SHALL log to standard error, optionally as JSON: its start with its settings and without its token; each successful login with the username and the client address; each failed login with the client address and without the username that was typed; each request made or withdrawn with the username and the title's identifier; each refusal by a limit; and each failure to reach the daemon. No log line, at any level, SHALL hold a password, the portal token, a session proof, a device value, a CSRF token or a cookie.

#### Scenario: Secrets absent from the log
- **WHEN** people log in, fail to log in, search and ask with debug logging on
- **THEN** no log line holds a submitted password, the token, a proof, a device value or a cookie value

#### Scenario: A password typed as a username
- **WHEN** a login fails with a username field that holds what looks like a password
- **THEN** the log line for it holds the client address and not that text

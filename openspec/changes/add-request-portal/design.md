# Design: a request portal

## Context

What exists today, read from the code:

- **Accounts.** `app_user` holds accounts with two roles, `admin` and `user` (`data/migrations/0003_web_auth.sql`). Passwords are argon2id hashes. An account has an epoch that a password change or a disable advances, and a session is valid only under the current epoch.
- **What an ordinary user can do.** Almost nothing but look. Every route needs a session by default (`AXIS_WEB_ROUTE_DEFAULT`), and every route that changes something is registered with `AXIS_WEB_ROUTE_ADMIN`: adding a movie or a series, searching, grabbing, editing, removing, every settings page. An ordinary user can read Discover, the title search, a title's page, the Movies and TV lists with their wanted and history pages, a series' releases, the Downloads list, the library browser and the Jobs page, and can change their own password, theme and sessions. So an ordinary user sees file names, release names and download state, and can ask for nothing.
- **Sessions.** Stored in PostgreSQL as hashes, loaded into memory at start, 30 days idle and 90 days in all, revocable. The CSRF token is derived from the session cookie's value and is stored nowhere.
- **Login throttling.** In memory, per username and per client address, with a doubling delay. A browser that logged in before carries a signed device cookie and is throttled on a count of its own, so that a stranger cannot lock the owner out.
- **Password hashing.** Two hashes at once, each up to 256 MiB, with at most 32 logins waiting (`src/web/axis-web-auth.h`). Beyond that a login is answered 503.
- **The worker API.** A third class of route beside public and session routes: every path under `/api/v1/worker/`, POST of one JSON object, a bearer token stored as a hash, a gate that refuses whatever a browser sends, a throttle for wrong tokens by address (`worker_gate()` in `src/web/axis-web-auth.c`). `axis-worker` is a separate binary that is linked without libpq, which `tests/check-worker-link.sh` checks.
- **One listener.** `axisd` listens on one port for its interface and the worker API (`src/axisd/main.c`).
- **Adding a title.** `axis_movies_add()` and `axis_tv_add()` take a library folder, a quality profile and, for a series, what to monitor. `axis_tv_set_season_monitored()` monitors one season. The one-step Add on a Discover card uses the person's stored `add_defaults`.

There is no command-line client. `axis-portal` is the second client binary, beside `axis-worker`.

## Goals and non-goals

Goals:

- A person who is not an administrator can ask for a movie or for seasons of a series, and can see what happened.
- An administrator approves, or a person is approved automatically, within limits.
- The internet-facing piece is sandboxed from the engine. If it is taken over, the damage is bounded, and this document says by what.
- An installation without a portal gains no new surface.

Non-goals are listed in the proposal under "What stays out".

## Decisions

Each decision has its reason. Alternatives that were weighed follow in the sections below.

| # | Decision | Reason |
|---|---|---|
| 1 | The portal is a separate process, `axis-portal`, in its own container. | A flaw in the code that faces the internet then yields a process with no database, no media and no secret of the engine. |
| 2 | Requester accounts live in the engine's database, in `app_user`. | The portal then stores nothing, and one set of code hashes passwords, counts failures, disables accounts and advances epochs. |
| 3 | A new role, requester, below the ordinary user, that cannot use the engine's interface at all. | An ordinary user reads file names, release names and downloads today. A person who only asks should see none of it, and a password typed into the internet-facing portal should open nothing else. |
| 4 | The portal's login accepts requesters only. | A password of an administrator must never be useful to whoever holds the portal. |
| 5 | A session lives in the daemon, as a hashed row. The browser's cookie holds the session proof, and the portal holds no session. | Revocation is then the daemon's and immediate, a restart of the portal logs nobody out, and the portal has nothing to persist. |
| 6 | The portal token alone acts as no one. Every operation for a person needs that person's session proof, and the daemon takes the person from the proof only. | There is then no key that can act as any user. Whoever holds the token can act only for people who log in while they hold it. |
| 7 | The portal API has a listener of its own, off by default. | A network rule can then allow the portal one port, and an installation without a portal has no portal API. |
| 8 | A closed list of eleven operations, each with a fixed set of members in and out, and unknown members refused. | A scope that is a list can be tested as a list. |
| 9 | A request carries no free text. | Nothing a requester types then reaches an administrator's page, which removes a stored-markup and a persuasion channel. |
| 10 | Series are requested by season. Shortcuts become a season list when the request is made. No episodes. | Seasons are what the engine monitors and what a person means. A stored list means that a later change of the title's record does not silently widen a request. |
| 11 | A request's later states are derived from the engine's own records, not stored. | The same reason as for notices: nothing can go stale and nothing needs a sync. |
| 12 | Approval is required by default. Automatic approval is two switches for each person. | The default bounds what a stolen requester password can fetch to nothing. |
| 13 | Limits are enforced on both sides, and the daemon is the authority. | The portal's limits are cheap refusals. The daemon's limits hold when the portal is the attacker. |
| 14 | Posters are served by title identifier from the engine's own artwork. | There is then no operation that takes an address, so no fetch can be aimed. |
| 15 | No document says that the portal may be exposed until an independent review has passed. | The project's rule for security changes, applied to the piece that needs it most. |

## Architecture

```
 internet
    |
 reverse proxy (TLS)                       private network
    |                                    +------------------+
 axis-portal  ---- portal API (JSON) --> | axisd  :7801     |  portal listener: the API only
 (pages, no state)   token + proof       |        :7800     |  main listener: interface, worker API
                                         |   |              |
                                         |   PostgreSQL     |
                                         |   media, indexers, download clients
                                         +------------------+
```

The portal builds every request to the daemon itself, from fields it has checked. Nothing of a browser's request is passed on: not a path, not a header, not a body. It is a client of an API, not a proxy.

The shared code is small. The portal reuses, from the tree: the security headers and the CSRF and cookie helpers of `src/web/`, the login throttle, the trusted-proxy list of `src/core/`, the connection limits, and the HTTP client. Each is used today without the database or can be split so. Where a file of `src/web/` pulls in the database pool, the slice that builds the portal moves the part the portal needs into a file of its own, and the link check proves the result.

## Threat model

### Assets

| Asset | Where it is | Why it matters |
|---|---|---|
| The media library | The daemon's mounts | Deleting or replacing files is the worst outcome. |
| The database | PostgreSQL, on the private network | Holds accounts, indexer keys, download-client secrets and provider keys, unencrypted. |
| Administrator accounts and sessions | The daemon | An administrator can do everything. |
| Indexer allowances and accounts | The daemon | Searches spend them. A flood of approved requests could exhaust them or draw attention. |
| The details provider's key and rate budget | The daemon | Searches for titles spend it. |
| Disk space and bandwidth | The server | Each approved request fetches something. |
| Requesters' passwords and what they asked for | The portal in transit, the daemon at rest | Private to a household. |
| The portal token | The portal's container | Opens the portal API. |

### Attackers

- **A stranger on the internet**, with no account. Can reach the portal's login page and static files, and nothing else of the portal.
- **A requester**, or someone with a requester's password. Can ask, within that person's limits.
- **Someone who has taken over the portal's process or container**, through a flaw in the portal, in libsoup, in GLib or in the image. Holds the portal token, sees every password and session proof that passes through from then on, and can send the daemon anything from the portal's network address.
- **Someone on the network between the proxy and the portal, or between the portal and the daemon.** Out of scope beyond what the deployment section says: those networks are private container networks, and the examples say so.
- **A hostile details provider.** Already in the engine's threat model: provider text is never markup and is read by bounded readers. The portal renders the same text and holds to the same rule.

### Trust boundaries

1. **Internet to proxy to portal.** Everything is untrusted. Forwarded headers are believed only from a listed proxy.
2. **Portal to daemon.** The daemon trusts nothing the portal says. It validates every member, takes the person from the session proof, counts every limit itself, and treats the reported client address as a hint that can only add a refusal.
3. **Daemon to database, media, indexers and clients.** Unchanged. The portal has no part in it.

### What each side can and cannot do

| | The portal (`axis-portal`) | The daemon (`axisd`) |
|---|---|---|
| Holds | Its token. In memory: throttle counts, the key of its pre-login forms, `whoami` answers for 30 seconds, posters. | Everything else. |
| Reaches | The daemon's portal listener. | The database, the media, the indexers, the download clients, the providers. |
| Checks passwords | No. Passes them to `login` and forgets them. | Yes, against argon2id hashes. |
| Decides who a request is from | No. Presents the browser's session proof. | Yes, from the proof alone. |
| Decides limits | Cheap refusals of its own. | Every limit, from its own records. |
| Approves, places, adds, searches, grabs | No, and no operation exists for it. | Yes, by an administrator or by a person's switch. |
| Sees files, releases, indexers, clients, profiles, folders | No, and no answer carries them. | Yes. |
| Serves the engine's pages | No. | On the main listener only. |
| Writes to disk | Never. | Its data directory and the media. |

### If the portal is taken over

An attacker who holds the portal's container and its token **can**:

- See the username and password of every requester who logs in from then on, and the session proof of every requester who uses the portal from then on. It can act as those people, within their limits, until their password is changed or their account is disabled.
- Try passwords for requester accounts, at the daemon's rate: five free failures for a username and then a doubling delay, and 30 failures an hour over all names, then one a minute.
- For a person whose proof it holds: read that person's requests, make requests up to that person's quota and up to the portal's daily ceiling (30 by default), and withdraw that person's waiting requests. Every such request waits for an administrator, unless that person is approved automatically for that kind, and then it is bounded by that person's quota and by the season cap.
- Search and browse titles with a proof it holds, up to 30 searches a minute, which spends the provider's budget up to that rate. Read each title's state, and who asked for it while `requests.show_who_asked` is on.
- Show requesters false pages, and learn their addresses and browsers.
- Stop the portal from working.
- Where the platform cannot limit the portal to one port (Compose and Quadlet networks cannot), reach the daemon's main listener as any host on a private network can: its login page, its static files and its probes. A login from the portal's address is refused there (see "The main listener and the portal's network").

The attacker **cannot**:

- Read the database, the media, the configuration, any indexer key, any download-client secret or any provider key. None of them is in the container, and no operation returns them.
- Act as an administrator or an ordinary user, or reach any page of the engine through the portal API. `login` does not accept their roles, and no operation leads there.
- Act as a requester who does not use the portal while it is held. The token alone acts as no one, and a session proof cannot be made up.
- Read a request of a person whose proof it does not hold, except what a title's public state says.
- Approve or decline, choose a quality profile or a library folder, or change what an approval fetches.
- Cause any download that an administrator has not approved, while no person is approved automatically. That is the default.
- Create, change or delete an account, or change a password. No operation exists for it.
- Delete or change a file, a title, a download or a setting.
- Make the daemon fetch an address of its choosing. The only text that travels onward is a search query of at most 100 characters, to the configured provider.
- Put markup or text of its choosing in front of an administrator. A request carries no free text, and a username is the administrator's own choice.
- Keep administrators from logging in by flooding `login`. Portal logins have one of the two hashing places and a queue of their own.

What is honestly not prevented: while the portal is held, it is the requesters' browser's only counterpart, so it can impersonate every requester who logs in or returns in that time. Nothing in a design where the portal renders the pages can prevent that. What bounds it is that requesters can do little: the default of approval, the quotas, the portal's daily ceiling and the notices that those ceilings raise. Changing the token and the requesters' passwords ends it.

## 1. Accounts and login

**Where accounts live.** In `app_user`, with a third role, `requester`. The alternative, a table of its own for requesters, gives a structural guarantee that a requester is never a `user_id` of the engine's interface, but it doubles the code for hashing, epochs, disabling and the Users page, and requests need one kind of owner whether an ordinary user or a requester asked. Instead the separation is enforced in three places, each with a test: the engine's login refuses a requester with the answer of a wrong password; the session lookup refuses a session whose account is a requester; and the middleware refuses a requester on every route of the main listener that is not public.

**Requester or ordinary user.** A new, weaker role is recommended, for the reason in decision 3. An ordinary user keeps what they have and gains Request in the engine's interface (slice P1). The portal accepts requesters only. Whether an ordinary user should also be able to log in at the portal is an open question at the end; the recommendation is no.

**Passwords.** Hashing stays in the daemon. The portal passes the password to `login` and keeps nothing. A requester's password has at least 12 characters and does not contain the username; the engine's own minimum stays 10. An administrator sets it and gives it to the person. A password change through the portal is not in these slices, because it would be the first operation that writes to an account; it is an open question.

**Uniform answers.** `login` answers an unknown name, a disabled account, a wrong password and a role that is not requester with one status and one body, and computes one hash in each case so that the time says nothing. No operation says whether an account exists. A later password reset must hold to the same rule.

**Sessions.** A login makes a row in `web_session` with the kind `portal`: the hash of a random proof, the account, its epoch and the times. The proof goes to the browser as the portal's session cookie. On each page, the portal presents the proof with its own token, and the daemon decides. The portal keeps no session table. It may keep what `whoami` said for 30 seconds to draw a page's header, and every operation that shows or changes anything of a person is checked by the daemon at that moment, so a disable, a password change or a role change takes effect at the person's next request, not at their next login.

The alternative is a session table in the portal's memory, with the daemon asked only at login. A restart would then log everyone out, and a disabled account would keep working until the portal asked again. It was rejected for those two reasons. The cost of the choice is that the browser holds a value the daemon issued. It is worth nothing without the portal token, and it is not a session of the engine's interface: the kinds do not cross.

**What a restart does.** Of the portal: nothing is lost but the throttle counts, the poster cache and the key of the pre-login forms, so a login form that was open must be loaded again. Of the daemon: portal sessions survive as the engine's own do; the in-memory throttles and budgets start empty, as the engine's login throttle does today.

**Lockout and rate limiting.** Both sides, the daemon as the authority:

| Limit | The portal | The daemon |
|---|---|---|
| Failed logins for one address | Refuses after the free failures, with the engine's delays. | Uses the reported address only to add a refusal. |
| Failed logins for one username | Does not count. | Five free failures, then a doubling delay, as at the engine's login. |
| Failed logins in all | Does not count. | 30 an hour over all portal tokens, then one a minute. |
| A remembered browser | Carries the daemon's signed device value as a cookie and sends it back with a login. | Throttles a remembered browser on its own count, as at the engine's login. |
| Searches | 10 a minute for a session. | 10 a minute for a person, 30 a minute in all, and a 10-minute cache. |
| Request posts | 10 a minute for a session. | Each person's quota, and 30 a day in all through the portal. |
| Wrong tokens | Not applicable. | Throttled by address, as for workers. |

The username throttle lets a stranger delay a requester's login. The device value is the existing answer to that, and the portal carries it without reading it.

## 2. The API scope

Every operation is `POST /api/v1/portal/<name>` with one JSON object, on the portal listener, with `Authorization: Bearer <portal token>`. Except for `hello` and `login`, the object carries `proof`. The gate is the worker API's, in this order: the address is not throttled; a portal token; nothing a browser sends; no query string; POST; a JSON body of at most 16 KiB; an operation that exists. Then the operation checks the proof, then every member.

| Operation | Takes | Answers |
|---|---|---|
| `hello` | Nothing. | The protocol number, the kinds that can be requested, the names of the lists to browse, whether names are shown. |
| `login` | `username`, `password`, `client_address`, `user_agent`, optionally `device`. | `proof`, `name`, `expires_in`, `device`. Or the uniform refusal, or 429 with the wait. |
| `logout` | `proof`. | Nothing. |
| `whoami` | `proof`. | `name` and, for each kind, the limit, what is left and when the window ends. |
| `search` | `proof`, `query` (100 characters), optionally `kind` and `page`. | Titles, `page`, `more`, `stale`. |
| `browse` | `proof`, `list`, `kind`, optionally `page`. | Titles, `page`, `more`. |
| `title` | `proof`, `id`. | One title and, for a series, its seasons with their states. |
| `art` | `proof`, `id` (a title). | The poster, as an image. |
| `request` | `proof`, `id`, `kind` and, for a series, `seasons`: `all`, `first`, `latest` or a list of numbers. | The request, what was left out and why, what is followed, and what is left of the quota. |
| `requests` | `proof`, optionally `page`. | The caller's requests, made and followed. |
| `withdraw` | `proof`, `request`. | Nothing. |

The members of a title and of a request are fixed in the `portal` specification, with the list of what no answer carries. A request's identifier outside the daemon is a random value of 22 characters, not the row's number, so identifiers cannot be guessed or counted. A title's identifier is the daemon's number for the title record; it says roughly how many titles the daemon has seen, which is accepted.

**How everything else is rejected.** Three independent rules, each with a test:

1. The portal listener has a router that holds the eleven operations and nothing else. A path that is not one of them has no route.
2. The main listener answers every path under `/api/v1/portal` as not found, before any credential is looked at.
3. A portal token is looked at only by the portal gate. Every other route requires a session or a worker token and never reads it. A test walks the route table with a valid portal token and no session, and expects nothing to open.

**Object-level checks.** No operation takes a person. `requests` lists the caller's; `withdraw` and the request shown by `title` are checked against the caller; another person's request is answered as one that does not exist. A member that names a person is an unknown member and is refused.

**The token.** An administrator creates a token on the page "Request portal" under System. It is shown once and stored as a SHA-256 hash in `portal_token`, as a worker's is. The deployment gives it to the portal as a secret file. To replace it: create a second token, restart the portal with it, revoke the first. Up to four can be active. Revoking takes effect at the next request. The page also shows when each token was last used and from where, the day's counts against each ceiling, and whether the listener is on.

## 3. Requests

**Data model** (migration `0083_requests.sql`):

- `request`: an identifier, a public identifier, the title, the kind, who asked, when, the decision (`waiting`, `approved`, `declined`, `withdrawn`, `replaced`), who decided and when, how it was approved (by an administrator, automatically, or without waiting because Axis was looking for the title already), the decline's reason, the library folder and quality profile used, and the movie or series row it added. A partial unique index allows one open request (waiting or approved) for a movie.
- `request_season`: a request and a season number. One open request for a series' season, by a partial unique index.
- `request_follower`: a request and a person.
- `request_closed_title`: a title and a kind that an administrator closed to requests, with who and when.
- On `app_user`: the two switches and the two limits of a person (null for the installation's).

As built in P1, the rows also hold: the name of the account that asked (kept so that a request still says who asked when the account is deleted; the account's own column is then empty); the account that made the row, which a hand-over does not change (the daily ceiling counts it); when the request was counted against that person's quota (when it was made, or when a follower took it over); a sixth decision, `removed`, which a trigger sets when the title an approved request added is removed from Axis; why a request waits although the person's switch is on; the engine's own reason when an approved title could not be added, and when it was added; and whether it came through the engine's interface or the portal API, which P2's daily ceiling counts. `request_season` repeats the title and carries an "open" flag that a trigger keeps equal to its request's decision, so that the unique index can be on the season itself. A trigger moves a title's requests to the record that is kept when two records of a title merge, as the movies' and the series' own triggers do.

**Seasons.** The person's choice (whole series, first, latest, picked) is expanded by the daemon when the request is made, from the seasons the title's record has then. The specials are in a request only when picked by number. A season that the provider announces later is not part of an earlier "whole series" request; it is asked for when it exists. That is the price of a list that never widens by itself, and it is an open question whether a follow-on for new seasons is wanted later.

**Approval.** One code path, used by the administrator's button and by a person's switch: in one transaction, lock the request, check that it still waits, add the title (or find it), monitor the request's seasons, record the decision, and queue the search. Approval of a series that Axis has only widens its monitoring.

**Automatic approval.** Two switches for each person, movies and series, off at creation. It is bounded by the person's quota, by `requests.auto_max_seasons` (3) for a series, and by the defaults: without a default folder and profile for the kind, the request waits and the page says why.

**A cap on what one approval may fetch.** A movie is one movie under the default quality profile, whose size limits apply as to any grab. A series approved automatically is at most three seasons. An administrator's own approval has no cap, and the Requests page states how many aired episodes it would look for before the button is pressed.

**Quotas.** For each person and each kind, over a calendar window that the administrator chooses for the installation: a day, a week from Monday, or a month, in the server's local time. A movie counts one and each season counts one. The count is made from the request rows when it is needed, so there is no counter to drift: a withdrawn request no longer counts, a declined one does, a followed one never did. The portal's daily ceiling is counted the same way, from the rows that were made through the portal API.

**Duplicates.** A second person becomes a follower, is not refused and is not charged. For a series the comparison is by season: seasons that are available or already in an open request are left out, and the person follows the requests that hold them. A title that Axis already looks for, added by an administrator, gives the person a request that is approved without waiting, because approving it fetches nothing more.

**Declined.** A decline shows its reason to the person who asked and to followers. The title can be asked for again; the new request takes the declined one's place, and the quota is what limits repeats. To stop them for good, an administrator closes the title to requests, on the decline ("Do not ask again") or later, and can open it again.

**Withdrawing.** A person may withdraw their own request while it waits, and the count goes back. With followers, the earliest follower takes it over if they have quota left. Nothing else can be changed after a request is made.

**States.** The daemon stores decisions only. Downloading, partly available, available and failed are read from the engine's grab, import and file records when a request or a title is shown. Failed carries one of four fixed causes in plain words. "Available" means that the file is in the library folder. Whether the media server has scanned it is outside what Axis knows for certain, so the page says "In the library".

**How the requester learns.** From "My requests" in the portal. Notification outside Axis (e-mail, push or a webhook) is on the roadmap for notices in general and is not designed here. The later slice below adds an in-portal list of what changed.

**Placement.** The library folder and the quality profile come from `requests.movies_root`, `requests.movies_profile`, `requests.series_root` and `requests.series_profile`, or from the approving administrator. The requester never chooses, and a request that carries such a member is refused.

### As built in P1

Slice P1 is built (`src/requests/`, `src/web/axis-web-requests.c`, migration `0083_requests.sql`). Where it differs from the text above, and what it had to settle:

- **Approval is two steps, not one transaction.** The decision is one transaction: the title's lock, the request's row locked, the check that it still waits, the record. The title is placed after that commit, because `axis_movies_add()` and `axis_tv_add()` take connections of their own and are called unchanged. Placing can be run again: it finds what an earlier attempt did. A daemon that stops between the two steps leaves an approved request with no title and no reason; it reads as "could not be added" once two minutes have passed, is counted by the notice, and "Try again" completes it. A database error in the middle of a placing is recorded as the reason at once when it can still be written.
- **A placing never removes what it added.** A series whose season could not be monitored stays, with the seasons monitored so far; the request reads "could not be added" with the reason, and "Try again" monitors the rest. So "nothing in part" holds for a movie and not for a series: a failed placing of a series can leave it added with some seasons monitored. Removing it again was the earlier design; it could remove a series that another request's placing, or an administrator, had started to use in the meantime.
- **Two placings of one title wait for each other.** A placing holds a session advisory lock of the title for its whole length, on a connection of the pool that does nothing else, and lets go of it on every path. The lock is of a class of its own, not the title's lock that asks and decisions take: a placing calls the movies and TV modules, which may store the title's record, and a merge of two records of the title takes the title's lock in its trigger on the metadata store's connection, which the placing would wait for while holding the lock the trigger waits for, a deadlock the database cannot see. The cost is one connection of the pool for each placing in flight, beside the modules' own.
- **"Try again" is for a title that was never added.** It is refused for a request whose title was added and is still in Axis, so a crafted post cannot add again a title that an administrator changed on purpose. A folder of the other kind is refused on a first approval and on "Try again" alike.
- **A title removed from Axis closes its request.** A trigger on `request` sets the decision `removed` when an approved request that was added loses its movie or series (the row's `ON DELETE SET NULL` when an administrator removes the title). `removed` is not open: it leaves the partial unique indexes and the season mirror, the title can be asked for again, the person who asked reads "Removed from Axis" among their requests, and the quota it used stays used. A movie that is unmonitored and whose file is deleted is not handled (see the known limits).
- **How races are prevented.** Every change to a title's requests takes a transaction advisory lock of the title first, then, where a quota is counted, one of the person. The locks have two keys, a class (title, person, placing) and the id, so titles and people never share one. The quota's count, the daily ceiling's count and the insert they guard are in that transaction, so two requests at the same moment cannot both pass a limit with room for one. The account's row is read `FOR SHARE`, so a disable waits for a request in flight and the withdrawal that follows it finds the request. The movie or series that the facts read before the transaction named is locked `FOR KEY SHARE` in it, so a title removed in between is refused cleanly and a removal waits for a request in flight. The partial unique indexes hold "one open request" besides: with the title's lock switched off, the test saw the index refuse the second request.
- **A daily ceiling beside the quota.** A person who is not an administrator makes at most 100 requests in any 24 hours (`AXIS_REQUESTS_MAX_A_DAY`, not a setting), counted from `made_by`, the account that made the row, which a hand-over does not change. A withdrawal gives the quota back and not a place under the ceiling, so asking and withdrawing again and again cannot grow the table without bound. The refusal has its own error (answered 429) and My requests says so in one sentence.
- **A limit of 0 stops following too.** The limit is read with the account, before any follow, so a person who cannot ask for a kind follows nothing of it and is not shown decline reasons.
- **Who asked, while it is hidden.** A title's state names one request (of a series, the first open one). Besides administrators, only the maker of that very request is told the name; a person with a request for another season of the series is not.
- **When two records of a title merge,** the trigger on `meta_external_id` takes both titles' locks (the lower id first, with the key the requests layer takes) and keeps one open request for the movie and for each season: the approved one over one that waits, whichever record it is on, and else the kept record's. The other loses that movie or season; its maker and followers follow the one that stays; a series request keeps its other seasons and stays open with them, and one left with nothing is `replaced` with no decline reason (an administrator reads "Joined to another request for the same title").
- **The state is read in a bounded number of statements, not one.** The movies' facts and the movies on disk come from the movies module's own statements, so that "in the library" and "downloading" are judged by the engine's own rules; a third statement reads each movie's last grab, a fourth the seasons of the managed series with their episodes counted by the database. Measured by the test on 5,000 movies and 1,500 series: 8 statements for 20 titles, 5 for 50 requests. The cards' part reads, on the page's worker thread, the state of the titles and the accounts whose limit is 0 (only those).
- **A series Axis does not manage has no state from files.** A movie on disk that Axis does not manage reads as available (the movies module says so). A series on disk that Axis does not manage does not: its seasons' states come from requests alone until it is managed.
- **"Aired"** is an air date up to today (UTC). The TV module's own delay after airing (`tv.air_delay`) is not applied, so a request can read "partly available" for the hours in which the engine does not yet look for an episode.
- **A declined request is "replaced" when it is asked for again**: a movie's always; a series' when the new request lists every season the declined one listed. A declined request for seasons 1 and 2, asked again for season 2 only, stays declined for season 1.
- **Withdrawing with followers** tries the earliest follower only, as the specification says. If that person has no quota left (or is disabled), the request is withdrawn for all, though a later follower might have had room.
- **A title Axis already looks for**: a movie that is managed and has no file gives a request that is approved at once ("existing"). For a series this holds only when every season asked for is monitored already; otherwise the request waits, because approving it would widen what is fetched.
- **The people's switches and limits** are on `/requests/people`, a page of the Requests module that the Users page links to. The Users page belongs to the accounts' code, which knows nothing of requests but the module's id.
- **An account that is deleted** leaves its requests with the name it had and no account. Its waiting requests are withdrawn (or handed over) by a call the Users page makes after the deletion is committed; every later such call also deals with any waiting request that names no one.
- **An administrator can make a request** through the model (not limited, not offered in the interface, where an administrator has Add).
- **A decline's reason** must be given: 1 to 300 characters, one line.
- **`via`** is recorded on each request now (always `web` in P1), so that P2's daily ceiling can count the rows made through the portal API without a second change to the table.
- **Requests is a module** (`requests`) that can be switched off: its pages then answer 404 and no card draws its piece.
- **The search's adult filter is not stored.** It drops adult titles from the provider's answers when they are parsed, so a search never stores one; a title stored another way (a lookup by identifier, an import, an identification) has no mark, can be opened on its page as every stored title can by an ordinary user, and can be asked for by its number. P2's `title` operation must refuse such a title, so P2 stores the mark first.

Decided for P1 from the open questions: a request shows who asked while `requests.show_who_asked` is on (question 4); a new season of a series that was requested whole is not requested automatically (question 5).

### As built in P2

Slice P2 is built (`src/api/axis-portal-*.c`, `src/web/axis-web-auth-portal.c`, `src/web/axis-web-portal.c`, migrations `0084_title_adult.sql` and `0085_portal_api.sql`). Where it differs from the text above, and what it had to settle:

- **Before P2: the adult mark is stored.** `meta_title.adult` (0084) is set whenever a provider's answer about a title carries the mark, is never cleared by an answer that does not, and follows a merge of two records. The engine's ask path refuses a marked title for anyone but an administrator, with the answer of a title that does not exist, before anything is followed or counted; the engine's own Request route calls that path. The portal API reads the mark from the record itself for every answer: a marked title is in no answer of `search` or `browse`, and `title`, `art` and `request` answer it as a number never given. This is stricter than "the same adult filter as the engine's own search": the engine's search shows marked titles while `metadata.tmdb_adult` is on, and the portal never does.
- **A second listener is a second server.** What "Believed and not confirmed" asked is answered below it: the portal listener is its own `HtmxServer`, with its own middleware (the gate), its own router (the eleven operations) and its own connection limits, on the one main loop. Nothing of the main listener's shell is installed on it, so no page, static file, login form, worker route, probe or metrics route exists there; the local port of a request is never asked.
- **The gate's order differs in two places.** A path that is not under `/api/v1/portal/` is answered 404 before anything else (so `/`, `/login` or `/healthz` on the portal listener are 404 with or without a token, as the specification's scenario says). And a query string is refused (400) before the token is looked at, so that a token in an address is refused "with or without the header", as the scenario says; the design had the token first. The rest is as designed: the daemon serves (503; also while the Requests module is off), the address is not throttled (429), a portal token (401), nothing a browser sends (403), POST (405), JSON (415) of at most 16 KiB (413, refused by the connection limits when the head arrives), an operation that exists (404). Then the proof (401), then the members (422).
- **The proof is judged before the body's members.** The one reader of bodies (`axis_portal_request_parse()`) hands back the proof by itself, and whether the other members are the operation's is told only to a caller whose proof holds. A body that is not one JSON object is 422 whoever sends it.
- **The portal listener reads no forwarding header.** The client's address there is the connection's own.
- **Portal logins have counts of their own.** The username's, the reported address's, the device's and the account's devices' throttles of the portal API are separate from the engine's login's, with the same numbers: failures at the portal delay no login at the engine's interface, and the other way round. The device value is signed with the engine's device key and recorded with the engine's devices.
- **A remembered device is not held to the hourly ceiling.** The ceiling (30 failed logins an hour, then one a minute) is for logins without a valid device value. Otherwise a stranger who keeps the ceiling reached, and takes the one admission each minute, would lock every requester out, which the device value exists to prevent. A login with a valid device is held to that device's count and the account's devices' count, which only its owner's failures fill. A device value counts only for a requester's account.
- **The reported address has a throttle of its own** (20 free failures, as the engine's address throttle), which a success never clears and which only adds a refusal. The daemon cannot check the address, so this bounds nothing against a portal in the wrong hands; it makes an honest portal's flood from one address cheaper.
- **One hashing place, by a queue.** Portal logins run on the engine's two hashing threads, one at a time: one is checked, at most eight wait, and a login beyond that is answered 503 at once. A login writes nothing to an account, so a hash made under older limits is not replaced by a login through the portal (it is at the engine's own login).
- **Uniform failures include a name that cannot be one, and a failure of the database's read of the account** (one hash each; the second is answered 503).
- **The search budget.** A person's ten a minute count every search. The thirty a minute in all count the searches that ask the provider: one answered from the ten-minute memory asks nobody and is not counted there. A search without a `kind` asks for both kinds and counts two. An answer the provider failed is not kept in memory.
- **The portal's daily ceiling is counted in the ask's transaction** (`requests.portal_per_day`), under a lock of its own that is taken after the title's and the person's, from the rows made through the portal API on the calendar day, whatever became of them: a withdrawal does not give a place back, so asking and withdrawing cannot be used to keep the portal's day open.
- **`browse` offers the provider's own lists.** "Recommended for you" is worked out from what the library holds, so it is not a list for people who only ask; `hello` does not name it and `browse` refuses it.
- **`title` and `art` read the store and nothing else.** A title a requester opens does not count as viewed, so opening titles through the portal does not make the refresh job fetch them.
- **A request's states through the API.** The eight states of the specification, with `removed` (P1's sixth decision) read as `failed` with the cause `removed`: the fourth of the fixed causes. The causes are `not_added`, `download_failed`, `needs_admin` and `removed`.
- **`asked_by` only while the setting is on.** In the engine's pages the maker of a request is told their own name while the setting is off; the API carries no `asked_by` at all then.
- **The members of the answers** that the specification leaves open: `hello` is `protocol`, `kinds`, `lists`, `show_names`; `login` is `proof`, `name`, `expires_in` (seconds), `device`; `whoami` is `name`, `movies` and `seasons`, each `limit`, `left`, `window_ends` (seconds since 1970); `request` is `outcome` (`made`, `followed`, `available`, `unchanged`), `request` (when one was made), `left_out` (each `season` and `why`: `available` or `requested`), `followed` (how many requests the person began to follow), `movies` and `seasons` (the quota afterwards); `requests` is `requests`, `page`, `more`; `logout` and `withdraw` are an empty object. A refusal is `error` (a fixed word), `message` and, for a limit, `wait` in seconds (also in `Retry-After`).
- **Withdrawing is for who made the request.** A follower cannot withdraw it (404, the answer of an identifier never issued), and the eleven operations have none to stop following; a later slice must decide whether `withdraw` should do that for a follower.
- **A change of an account's role ends every session of the account**, whichever roles it is between, an administrator's own included; setting the role an account has changes nothing.
- **The role's three barriers.** The engine's login answers a requester's right password as a wrong one. The session store finds no session of the engine's interface for a requester, and no portal session for anyone else (a row whose kind does not fit its account's role is ended when it is presented and purged from the database). The middleware refuses a requester again: where a request's identity is read it is not taken from a requester's session, and a request that was identified as a requester's all the same goes on as one without a session, on every path, public ones too; the route check refuses it once more. Each barrier was seen to hold with the others switched off (`tasks.md`, 2.72).
- **The page "Request portal"** is `/admin/portal`, a page of the Requests module (a module that is off has no pages), listed under System. A token has no name: the page lists when it was made, by whom, when and from where it was last used, and whether it is revoked.
- **Settings.** `server.portal_listen_port` (`AXIS_PORTAL_LISTEN_PORT`, 0 by default; it must differ from the main port) and `requests.portal_per_day` (30).
- **Not built in P2, though the design's text places it there or leaves it open:** the day's counts against each ceiling on the page (task 4.4); the refusal of an engine login from an address that presented a portal token (task 4.3).

#### After the review

A first independent review of P2 found no blocker, four findings that should be fixed (S1 to S4) and eleven minor ones (M1 to M11). What changed, where it differs from the bullets above:

- **A requester takes another role only with a new password (S1, M9).** The form of a change away from `requester` has a password field, held to the rule of the new role; without one, or with the password the account has now (checked by one hash), the change is refused, and the store refuses it again with the row locked. The password a held portal saw therefore fails at the engine's login afterwards. A change to `requester` keeps the password, as before.
- **The adult mark has three values (S2, M5).** `meta_title.adult` is NULL, "not known", for every title stored before 0084 and for one stored from something that is not a provider's answer. Every provider answer that is read sets it: TMDb's from its member (false without one), TheTVDB's false, because its records have no such member. A mark once true is never cleared. For anyone who is not an administrator, NULL is treated as marked, in the portal API's `title`, `art`, `search`, `browse` and `request` and in the engine's ask path: the title is a number never given until an answer about it is stored (a search that finds it, a lookup, the refresh job).
- **The ask path refuses a marked or unknown title first (M2).** The mark and the asker's role are read with the title, before its kind and whether it is closed, so `request` with the wrong kind, or for a closed marked title, is the answer of a number never given.
- **Every operation has a budget, and the API has threads of its own (S3).** 600 operations a minute for a token (every request that presents a valid one, counted in the gate after the token is found) and 120 for a person (every operation with a valid proof, counted before its body is judged), answered 429 `token-limit` or `person-limit` with the time to wait. Each budget is one time a key (the generic cell rate algorithm), at most 4096 keys; a key whose budget is whole again is forgotten when room is needed, and a new key waits while there is none. The operations run on a thread pool of the API's own, four threads, so they never hold a thread of GLib's shared pool that the engine's pages use; at most 32 are running or waiting.
- **One person's share of the portal's day (S4).** What one person's portal requests count today, withdrawn ones included, is at most a third of `requests.portal_per_day` (at least one), counted in the same statement and under the same lock as the day's total, and refused with 429 `portal-share` until midnight. The day's total is checked first.
- **The gate looks the token up first (M1).** A valid token is never held to its address's count of wrong ones; the address throttle applies to a request without a valid token only. The gate's order is now: 404, 503, 400 (query), then without a valid token 429 (address) or 401, with one 429 (the token's budget), then 403, 405, 415, 413, 404 as before.
- **`asked_by` is a requester's name only (M3).** With `requests.show_who_asked` on, a title carries `asked_by` only when the account that made the request is a requester: the name of an account that can log in to the engine's interface is never handed to the portal.
- **`title` and `art` check the kind (M6)**, as `search` and `browse` do: a title of a kind that cannot be asked for now is a number never given.
- **A search page above two is refused by the reader (M7)**, with its own message, before the search budgets count it.
- **A token's use is written once a minute whatever the address (M8).**
- **The portal listener has an address of its own (M10):** `server.portal_listen_host` (`AXIS_PORTAL_LISTEN_HOST`), the main listener's host when not set.
- **What could not be judged by reading.** A login answered busy before its handler returns (the engine's own password queue full) is answered at once: htmx-glib applies a deferred response that is completed before it is returned (`htmx_response_attach_message()`), and `/portal/api/login/engine-queue-full` shows it. An older `axisd` refuses to start on a database migrated past its own migrations (`verify_history()` in `src/db/axis-db-migrate.c`), so a rollback of the image past 0085 does not run; the database must be restored from a backup taken before. The `meta_search` rows that requesters' searches make are pruned by the metadata refresh run (every 15 minutes by default) once they are 30 days old, as the engine's own are; nothing prunes them sooner (a known limit).
- **M4 and M11** are recorded as known limits and in the tests' own comments; the three tests M11 named as missing are `/portal/api/fields/answers` (the wrong kind and a closed title for a marked one), `/portal/api/fields/mark-not-known` with `/requests/model/adult-mark-not-known`, and `/portal/api/limits/one-persons-share` with `/requests/quota/portal-ceiling`.

### As built in P3

Slice P3 is built (`clients/portal/`, the split files `src/web/axis-secret.[ch]` and `src/web/axis-web-headers.[ch]`, `tests/test-portal.c`, `tests/test-portal-process.c`, two fuzz targets). Where it differs from the text of this document, and what it had to settle:

- **One function answers every request.** The portal registers no route with htmx-glib's router. Its middleware answers everything, in a fixed order that the header of `clients/portal/axis-portal-app.h` lists: the path, the method, the stylesheet and the probe, HTTPS, the query and the form, the login, the session cookie, the CSRF token, the route's own values and limits, and only then the daemon. A path is one of eleven written exactly (`axis_portal_route_parse()`); there is no pattern for a longer path or a prefix to match.
- **The pages are written by the portal's own writer, not by htmx-glib's node builder.** The builder escapes two characters in an attribute's value. The portal's writer (`clients/portal/axis-portal-view.c`) has one escape for element text and attribute values alike (`&`, `<`, `>`, `"`, `'` and the backtick; control characters dropped), and every attribute value is between double quotes. htmx-glib is the server, the request and the response.
- **No script is served.** The pages are plain forms and links; htmx's own script is not among the portal's static files, which are one stylesheet. "An htmx request is answered 401" is kept for a request that says it is one.
- **What is shared with the daemon, and what is not.** Linked by name: the trusted-proxy list and its resolver, the login throttle, the checks on what a client sends, the secrets, the security headers, the request limits; from the core library, the log writer and the bounded JSON reader. Not linked: the daemon's reader of bodies and writer of answers (`src/api/axis-portal-proto.c`). The portal has a writer of bodies and readers of answers of its own, and its own list of the eleven names; a test compares the names with the daemon's and gives every body the portal writes to the daemon's reader. libsodium is linked (random values, the CSRF tokens), and json-glib because htmx-glib needs it; the portal reads JSON with the bounded reader only.
- **The readers are strict.** An answer with a member its operation does not define, one twice, a wrong type, a number out of range or a word that is not one of the fixed ones is refused whole, and the page is the plain 503. `hello` must say protocol 1. An account's name must have the form of a username, so nothing else is ever shown or logged as a name. A text of a provider or an administrator is cut to its length, and a control character, or one that changes the direction of the text, becomes a space. A title that is answered must be the title that was asked for. The daemon's own sentence of a refusal is never read: a page says the portal's words for the refusal's fixed word.
- **Settings.** `url`, `token_file` (or `token`), `ca_file`, `allow_insecure_http`, `request_timeout`, `web_listen_host` (127.0.0.1), `web_listen_port` (7802), `trusted_proxies`, `insecure_browser_http`, `max_connections`, `max_connections_per_address`, `log_level`, `log_format`; each also `AXIS_PORTAL_<NAME>`. The port is not `listen_port`: the daemon's `AXIS_PORTAL_LISTEN_PORT` is the port of its portal API, and a portal that finds that variable in its environment refuses to start and says which is which. Any other variable that begins with `AXIS_PORTAL_` and is not a setting is refused too.
- **HTTPS is required of every page, not only of the login.** A request that no listed proxy reports as HTTPS is answered with a page that says so, whatever it asks for (the stylesheet and the probe excepted), and the daemon is not asked. `insecure_browser_http` is the override "for a test"; with it the cookies have no `Secure` and no `__Host-` prefix, which a browser would refuse over plain HTTP. It is refused unless the listener is a loopback address.
- **The cookies.** `__Host-axis_portal_session` (the proof and its seal; `Lax`; as long as the daemon says the proof lasts, 90 days at most), `__Host-axis_portal_form` (the pre-login form's; `Strict`; 30 minutes) and `__Host-axis_portal_device` (the device value; `Strict`; 90 days). Each is read only when it is there once and in its form.
- **The two tokens.** The pre-login form's token is a keyed hash of the form's cookie under a key that the process makes at start and never shows, so a cookie that someone else planted gives them no token. The session's CSRF token is derived from the proof alone, as the engine derives its own from the session cookie: it survives a restart of the portal, which the form's token does not.
- **A throttle the design did not have.** Each request with a session cookie the portal sealed (see "After the review") costs a call to the daemon until the daemon has confirmed it. Cookies the daemon does not know are counted for the client's address, with the login throttle's numbers (20 free, then a doubling delay): a stranger at one address cannot make the portal ask without end.
- **The login throttle of the portal** counts, for the client's address, a login the daemon refused as wrong and one whose fields cannot be a username and a password at all (which is answered as a wrong one, and never sent). A login that works clears the address.
- **A login ends the session the browser carried before** at the daemon too, by a `logout` nobody waits for. **A logout that the daemon did not hear** is answered 503 and keeps the cookie, so that the person can try again and is not told that a session has ended which has not.
- **`whoami` and posters.** `whoami` is asked when what it said is older than 30 seconds, and its answer is dropped at a 401. A poster the portal holds is served from memory to a person the daemon knew within those 30 seconds, and otherwise after `whoami` was asked; one it does not hold is asked for with the proof. Posters are kept up to 32 MiB in all and ten minutes each, and what is served as a picture must begin as a picture of its type does.
- **The answer to a post is a page.** After a request, the title's page with what the daemon said (made, followed, in the library already, asked already; the seasons left out and why). After a withdrawal, "My requests". Not a redirect: nothing of the outcome goes into an address.
- **A title that someone asked for already** has a button to follow that request. It posts the same request, which the daemon answers as followed.
- **The log.** Each refusal by a limit and each failure of the daemon is a line, as the specification asks, up to ten of a kind in a minute (see "After the review"). A withdrawal's line has the username and no title: `withdraw` takes a request's identifier, and the portal does not know the title.
- **The probe** is answered to a loopback peer, with a loopback `Host`, that sends no forwarding header of any kind. `axis-portal --healthcheck` asks it of the portal on this machine, at the loopback address it listens on or the loopback address of its family when it listens on all of them.
- **Escapes in a path.** libsoup undoes them before the portal sees the path, and its "raw-paths" can be set only when the server is made, which htmx-glib does. An escaped path is therefore another spelling of a route, with every check of that route (a known limit; the proxy must not decide by the path).
- **The image builds and tests the program, and does not carry it.** The image's build runs `make test`, which now builds `axis-portal`, checks its link and runs its tests that need no database. The Containerfile is unchanged, and `make install` does not install the binary: that is P4's.
- **Not built in P3, though this document places it near:** `PR_SET_NO_NEW_PRIVS` and "not dumpable" at start (task 4.2); an example configuration file (4.1).

#### After the review

A first review of P3 found no blocker, four should-fix findings (S1 to S4) and seventeen minor ones (M1 to M17). What changed:

- **A cookie the portal did not seal costs the daemon nothing (S1).** The session cookie's value is `<proof>.<seal>`: the seal is a keyed BLAKE2b of the proof under a key derived from the portal's token with the fixed label `axis portal cookie v1` (`AXIS_PORTAL_SEAL_KEY_PURPOSE`), so the token itself is never a key. It is checked in constant time before anything is asked; a cookie that fails is none, is sent to `/login`, is dropped, and counts against nobody. A new token is a new key: every cookie sealed with the old one is none, and everyone logs in again. A restart with the same token logs nobody out.
- **Unconfirmed sessions and logins share a part of the daemon's calls (S1).** Calls for sessions the daemon has not confirmed in the last 15 minutes, and logins, from every address together, are 120 a minute (one GCRA budget), well under the 600 the daemon gives the token: whatever strangers at many addresses send, the sessions the daemon has confirmed keep the rest. A refusal is 429 and asks nothing.
- **A full throttle table forgets nobody's failures (S1).** The portal's two address throttles are told `axis_login_throttle_share_when_full()`: when the table is full, no address that has failures is dropped to make room; every address it has no room for is counted on one shared count, which is forgotten after 15 minutes without a failure. A sender who rotates addresses cannot make the table forget the ones it used and start each again. An IPv6 address counts by its /64, as before. The engine's own throttles are unchanged.
- **A confirmed session is not locked out by its address (S2).** Sessions the daemon confirmed (a `whoami`, a request or a login that it answered) are kept for 15 minutes, at most 4096 of them, and a cookie of one is not held to its address's throttle of unknown cookies. One device that misbehaves behind a home's address delays only the cookies nobody has vouched for.
- **Ten lines of a kind in a minute (S3).** Each kind of refusal, failed logins, and each kind of failure of the daemon (`clients/portal/axis-portal-log.h`) writes its first ten lines of a minute; the rest are counted, and one line says how many when the minute ends, or when the portal stops. The specification's "each refusal by a limit" is now "each kind of refusal, with a bound".
- **Kubernetes' service links (S4).** The variables Kubernetes adds for a Service called `axis-portal` (`AXIS_PORTAL_SERVICE_HOST`, `_SERVICE_PORT`, `_SERVICE_PORT_<NAME>`, `AXIS_PORTAL_PORT`, `AXIS_PORTAL_PORT_<N>_<PROTO>` and its `_PROTO`, `_PORT`, `_ADDR`), each with a value of its shape, are left aside and named in one line at start. Any other unknown variable still stops the portal, by its name when the name is of `A-Z`, `0-9` and `_` (a name with other characters might be a pasted token, and is described, not printed); another Service's links are told how to leave them out.
- **The minors.** M1: searches and request posts are counted after the daemon has confirmed the session, so cookies nobody vouched for take no room in the counts. M2: a login that works clears its address's failures; the daemon's 429 is not the address's failure. M3: a listed proxy that gives no client in `X-Forwarded-For` is answered 503 with a line of the log. M4: a post refused as from another site is logged with the `Origin` and the host compared. M5: `Strict-Transport-Security: max-age=31536000` on every answer to an HTTPS visit. M6: the portal's own content security policy, `default-src 'none'; style-src 'self'; img-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'`, also on the refusals of the request limits (a new option of `axis-web-limits.h`). M7: the probe needs a loopback `Host` and no `X-Real-IP`, `Via`, `X-Forwarded-Proto` or `X-Forwarded-Host` either. M8: `--healthcheck` asks the listener's own loopback address, and refuses a listener on one address that is not loopback. M9: a token file that every user may read is warned of at start; `AXIS_PORTAL_TOKEN` is overwritten in place and taken out of the environment once read. M10: `insecure_browser_http` is refused unless the listener is a loopback address. M11: as many pages as calls (64), and a call's slot is given back before its answer is handled. M12: the page for plain HTTP names no setting; the log says it once. M13: a search with a query that `Sec-Fetch-Site` reports as from another site is 403. M14: a test compares the worker API's path as text. M15: the tests the review named say what they assert or assert more; `fuzz-portal-form` reaches the other readers of what a browser sends. M16: the readers refuse an answer for another page than the one asked for, and no page links past its route's last. M17: a refusal inside a page has the person's frame, and one without it links to `/`, not to the login.
- **Left as known limits** (`openspec/project.md`): the inline `token` of a key file stays in GLib's key file reader, which is freed without being wiped (M9: `token_file` is the documented way); the request handler itself is not fuzzed, only the readers it calls (M15); the 64 calls in flight are not reached by a test (M11); a peer that is not loopback is not tried against the probe (M15).

### As built in P4

Slice P4 is built (`deploy/Containerfile`, `deploy/examples/traefik/compose.portal.yaml`, `deploy/examples/kubernetes/portal.yaml` and `portal-axisd.yaml`, `deploy/examples/quadlet/axis-portal.container` and `axis-portal.network`, the portal's parts of `tests/check-container.sh` and `tests/check-examples.sh`, `tests/check-portal-client.py`, `tests/tools/check-portal-review.py`, and in the code the address rule, the ceilings' record and notice, and the hardening at start). It has not been reviewed by someone who did not write it (task 4.90). Where it differs from the text of this document, and what it had to settle:

- **Open question 1 is decided: no slim image.** The portal ships in the one image, run with `--entrypoint /usr/bin/axis-portal`, and no second final stage is built. One image is built, published for two architectures, and kept current by one pipeline; a second image name would double each of those for a program that is a few hundred kilobytes of the same build. The cost stands as section 5 states it: the portal's container also holds ffmpeg, Python, Node.js and yt-dlp. What bounds a person who takes the container over is how it is run (read-only, no capability, no new privileges, not root, a limit on processes and memory, no volume, a network that reaches one port), not what its file system leaves out. The image's own `ENV` (the daemon's listen address and port, its data directory, `HOME`) is in the portal's environment too; none of it is a secret, and the portal reads none of it.
- **The main listener is closed to the portal where the platform allows it, by the address it listens on.** Section 5 says that a Compose or Podman network cannot limit by port, so the portal's container can reach port 7800. In the Traefik example it cannot: the daemon is given a fixed address on each of two networks, the portal API listens on its address on the portal's network (`AXIS_PORTAL_LISTEN_HOST`), and the main listener on its address on Traefik's network (`AXIS_LISTEN_HOST`), so nothing answers port 7800 on the network the portal is on. `axisd --healthcheck` asks the configured address, so the health check still works. The Kubernetes example closes it by NetworkPolicy, as designed. The Quadlet example does not close it: its main port is published to the host, and the daemon's address on its first network is not fixed there, so the main listener stays on every address and the portal's container can open the login page; the address rule and the role are what answer that, as section 5 says.
- **Quadlet: the proxy is on the host, and the portal trusts its own address.** The Quadlet example has no reverse proxy; the portal publishes its port on 127.0.0.1 for one that the person runs on the machine. With rootless Podman a request through a published port arrives in the container from the container's own address on its network (seen by hand), so the portal has a fixed address there and lists it as its trusted proxy. A plain request through the published port is then one from the listed proxy that names no client, and is answered 503; from any other address it is 403. Every program on that machine can reach the published port and claim to be the proxy, which the README says.
- **Leaving the portal off a network does not keep it from that network: its own networks are internal.** "Believed and not confirmed" said that a Compose network of two services isolates them from the others. It does not, under Podman: on the first run by hand, from the portal's container the name `postgres` did not resolve, and a connection to PostgreSQL's address on the network `private` opened all the same, as did one to the daemon's main listener at its address on Traefik's network. Podman routes between its networks unless told otherwise. So the specification's scenario "the name does not resolve or the connection is not made" would have passed while the database's port was open to a container that someone holds. The portal's networks are now `internal: true` (Compose) and `Internal=true` (Quadlet): the portal's container has no default route, so every address outside its own networks, the internet included, is unreachable. The checks try PostgreSQL and the main listener by address, not only by name, and assert that the container has no default route. Docker is believed to isolate bridge networks from each other by itself, and `internal` is its key too; Docker was not run.
- **Three networks in the Traefik example, not two.** The portal must share a network with Traefik, and it must not be on the network where Traefik reaches Axis, or it would reach the main listener there. So: `axis-proxy` (Traefik and Axis, as before), `axis-portal-proxy` (Traefik and the portal), `axis-portal` (Axis and the portal). The portal's router carries `traefik.docker.network` so that Traefik reaches it on the right one.
- **The portal is switched on by a second Compose file, a Kustomize patch and two more unit files**, not by a profile or by commented lines in the examples' main files (the Quadlet example excepted: three commented lines in `axis.container`). `compose.yaml` alone is the example as before; `-f compose.yaml -f compose.portal.yaml` adds the portal and the daemon's settings in one step, so that the daemon cannot be given a portal listener without the networks that go with it. For the merge, the `networks` of the `axis` service in `compose.yaml` became a map (podman-compose 1.6 cannot merge a list with a map).
- **Only the three examples the specification names carry the portal.** The minimal and full-stack Compose examples and the run commands have no reverse proxy with TLS, and the portal serves no login without one; the remote-worker example has no daemon. The deployment guide sends their readers to the Traefik example.
- **"No tmpfs" is the runtime's to grant.** The portal needs no writable directory, and `tests/check-container.sh` and the Quadlet unit run it with none (`--read-only-tmpfs=false`, `ReadOnlyTmpfs=false`). Compose has no key for that: under Podman, a read-only Compose container still gets Podman's own tmpfs on `/tmp`, `/run` and `/var/tmp`. Under Docker it gets none. The example gives the portal no `tmpfs` line of its own.
- **The address rule compares the client's address, not the connection's.** The gate remembers the address of the connection a valid token arrived on (the portal listener reads no forwarding header), keyed as the throttles key an address (an IPv6 address by its /64), for 24 hours after the last time, at most 256 addresses (the one unused longest is forgotten for a new one). The engine's login refuses when the client's address, as the main listener resolves it through its trusted proxies, is one of them. So a login that arrives through a listed reverse proxy is judged by the person's own address, and administrators behind the proxy are not refused because the proxy also carried a portal's calls. The refusal is the page of a wrong password, with no hash, and it touches no throttle: it can delay nobody else. It is written to the audit log with its reason. A token presented counts whatever became of the request (a refusal by the budget, or as a browser's, too); a wrong token counts for nothing.
- **The rule can lock an administrator out, and the documents say how to get back in.** Where the portal and an administrator's browser reach the daemon from one address (a gateway that is not a listed proxy; both on the daemon's own machine without containers), the administrator's login is refused for as long as the portal keeps calling. The addresses are in memory: stopping the portal and restarting the daemon clears them. Nothing exempts the loopback address: a portal on the daemon's own machine is as able to carry a stolen password as any other.
- **A ceiling is "reached" at a moment, recorded in memory.** The hour's ceiling of failed logins is reached by the failure that fills the hour (the ones admitted beyond it reach nothing anew), and at most once an hour. The day's ceiling of requests is reached by the first request it refuses on a calendar day: a day that ends at exactly the ceiling, with nobody refused, is not recorded. Up to 32 moments are kept for each. The notice and the page count the moments of the last 24 hours, so both are empty after a restart of the daemon, as its throttles are. One person's share of the day (`portal-share`) is not a ceiling of the portal and raises no notice.
- **One notice for both ceilings**, as the specification says ("one notice"), of the kind `requests.portal_limit`, under "Worth a look", read from memory with no statement. The page "Request portal" has a card "Limits": what the portal's requests count today against `requests.portal_per_day` (one statement, as the ask counts them), one person's share, the failed logins of the last hour against 30, and how often each ceiling was reached in the last 24 hours.
- **At start the portal also sets its core file limit to 0**, besides no-new-privileges and not dumpable, and refuses to start when either of those two cannot be set. A build with the address sanitizer (the tests', never a release) makes itself dumpable again as it exits, because the leak checker stops the process's threads through ptrace and a process that is not dumpable refuses that; while it serves it is as a release is. `/proc/<pid>/environ` of a portal is now refused to its own user's other processes, so the test of the token's removal from the environment (P3, M9) reads it only when it runs as root.
- **A line for forwarded headers from an address that is not listed.** The specification's scenario "The proxy is not listed" asks that the log say once that forwarded headers came from an address that is not trusted. P3's line said that a visit over plain HTTP was refused. P4 adds the other, with the address, which is what the administrator must list; both are written once.
- **"The engine is not behind the portal's proxy" is checked on other paths than the login page.** The scenario names the engine's login page, `/admin/users` and `/api/v1/portal/hello`. `/login` is also the path of the portal's own login page, which answers 200 there; the check asks `/setup`, `/admin/users`, `/api/v1/portal/hello`, `/api/v1/worker/claim`, `/readyz` and `/storage`, and that the page at `/login` is the portal's form.
- **`check-portal-review.py` reads one fact from the program.** Whether the review is recorded is whether `axis-portal` still logs its "not reviewed" line at start. While it does, ten places that tell how to run the portal must say so and must say "only from a network you trust"; once it does not, none of them may, and the deployment guide must name the review. P5 extends the second half.
- **`tests/check-examples.sh` removes a name server file that an earlier run left.** Found while the Quadlet part was run by hand: Podman's name server keeps a file for each network by its name, a container that went without being taken out of it stays in the file, and the file outlives its network. The next network of that name then answers `postgres` with two addresses, one of them dead, and a connection that tries the dead one first waits three seconds, which is the deadline of the daemon's lock connection: the daemon restarted on the portal's network never became ready. The script now removes such files for networks of its own prefix that no longer exist. This was state of the machine and not of the examples.
- **Kubernetes: rendered, not applied.** The manifests with the portal are rendered by `kubectl kustomize` and were never applied to a cluster; task 4.16 stays open and the known limits say that the NetworkPolicy was not exercised.

## 4. Search for requesting

A search goes from the portal to `search`, and from the daemon to the configured details provider, through the engine's existing title search with its adult filter. The portal never talks to a provider and has no key.

Limits: the query has at most 100 characters and no control characters; a person has 10 searches a minute and the portal API 30 in all; the same query for the same kind is answered from memory for 10 minutes. Browsing asks the provider nothing, because Discover's lists are a stored snapshot.

What the requester sees for a title: the poster, the name, the year, the overview, the kind and the state; for a series, each season with its state; and who asked, while the setting allows it.

Posters: the content security policy forbids loading anything from a third party, so the portal serves posters itself, from `art`, which answers from the daemon's own artwork by title. The portal keeps a bounded cache in memory. A poster that the daemon does not hold is not fetched on a requester's account; the title is shown without a picture until the engine has it.

## 5. Deployment

**The image.** `axis-portal` ships in the same image as `axisd` and `axis-worker` and runs with another entry point. The cost is that the internet-facing container's filesystem also holds ffmpeg, Python, Node.js and yt-dlp, which a person who takes over the portal could use. A second, small final stage in the same Containerfile, with the portal and its libraries only, would remove them. That is an open question; the recommendation is to add it in P4.

**Network layout.**

- The reverse proxy for the internet reaches the portal and no other Axis service.
- The portal and the daemon share a network that holds those two.
- The portal is not on PostgreSQL's network or the download clients' network, and has no media mount.
- The daemon's main port is published as it is today, for the local network, not through the internet-facing proxy.

**The main listener and the portal's network.** A Kubernetes NetworkPolicy can allow the portal the daemon's port 7801 and nothing else. A Compose or Podman network cannot limit by port, so there the portal's container can also reach port 7800: the login page, the static files and the probes, as any host on the local network can. Two things answer that. A requester's password opens nothing there, because the role cannot log in. And the daemon refuses a login at its interface from any address that presented a valid portal token in the last 24 hours, so that an administrator's password, should one be typed into a portal that is held, cannot be used from the portal's own container. It is defence in depth, not a wall: the examples say that administrators should not type their password into the portal, and `login` there does not accept it.

**TLS and the proxy.** The portal must sit behind a reverse proxy that terminates TLS. The portal serves a login only when it knows the request came over HTTPS, from its trusted proxies (`AXIS_PORTAL_TRUSTED_PROXIES`, with the rules of `server.trusted_proxies`: addresses, ranges and names). The cookies carry the `__Host-` prefix and `Secure` always. The proxy must do what libsoup does not let the portal do: time a slow reader, cap connections before they are accepted, and send HSTS.

Between the portal and the daemon the examples use plain HTTP on the two-container network, with the portal's `allow_insecure_http` set and a comment that says why. The token and the requesters' passwords cross that network. Across hosts, the portal must reach the daemon through TLS, which it verifies by default.

**Container hardening.** A read-only root filesystem, every capability dropped, no new privileges, a non-root and arbitrary user id, no volume and no tmpfs, a limit on processes and memory. The program sets `PR_SET_NO_NEW_PRIVS` and makes itself not dumpable at start, so that a core file cannot hold passwords in transit. Landlock and seccomp are on the roadmap for the subprocesses and would suit the portal well; they are not in these slices.

**What the portal logs.** To standard error, optionally as JSON: its start, each successful login with the name and the address, each failed login with the address only (a password is sometimes typed as a username), each request made or withdrawn with the name and the title's identifier, each refusal by a limit, and each failure to reach the daemon. Never a password, a token, a proof, a device value or a cookie.

## 6. What stays out

- **Self-registration and invitations.** An administrator creates each requester. An invitation would be a link, opened through the internet-facing portal, that creates an account: a write to accounts that the scope does not have, for the convenience of not sending a password. For a household it is not worth a slice. If it is ever wanted, it should be a one-time code that the administrator makes in the engine and that only sets a password on an account that already exists.
- **Other access.** A requester has no part in anything else in Axis. The portal passes no page on.
- **Notifications outside Axis.** Later, with notices in general.
- **Other media types.** With their modules.
- The rest of the list is in the proposal.

## A later slice: reports and "what changed"

Not built in this change, and not specified here. Recorded so that the model leaves room:

- **A report of a problem with a title.** A person picks a title they can see as available or partly available, a category from a fixed list (wrong title, bad picture or sound, wrong or missing subtitles, another problem) and a short note. A report is open or resolved, and an administrator resolves it on the Requests page with an answer. The note is the first free text from a requester, so that slice must settle how it is bounded, escaped and limited, and it needs a review of its own.
- **What changed.** A list in the portal of the changes to a person's requests since they last looked (approved, declined with its reason, in the library, failed), worked out from the request's decision times and the engine's records, which stands in for notifications.

## Slices

Each slice stands alone, is reviewed alone, and leaves nothing that faces the internet until P4.

| Slice | What it builds | Reachable from where | Migration |
|---|---|---|---|
| P1 | The request model, quotas, followers, states, automatic approval, the Requests pages, the notices, Request for an ordinary user in the engine's interface. | The engine's interface only. | `0083_requests.sql` |
| P2 | The requester role, the portal token, the portal listener, the eleven operations and their enforcement tests, portal sessions, the daemon's limits, the page "Request portal". | The portal listener, off by default. No program speaks to it but the tests. | `0084_title_adult.sql`, `0085_portal_api.sql` |
| P3 | `axis-portal`: its pages against that API, its CSRF and cookies, its own limits, its log, its link check, its fuzz targets. | A developer's machine. Not in the examples. | None |
| P4 | The image, the container hardening, the examples and their checks, the proxy guide, the address rule, the notices for the ceilings, the known limits. | Where a person puts it, with the "not reviewed" statement. | None |
| P5 | An independent security review of P1 to P4 as a whole, its findings fixed with tests, and the documents changed to say what was reviewed. | The internet, from here on. | None |

When only some slices are built, only their requirements go into the specifications.

## Where it is hard

- **Impersonation by a held portal.** Stated above. The design bounds it and cannot prevent it.
- **One listener today.** `axisd` makes one `HtmxServer` and listens once. The portal listener is a second server with a router of its own, and the limits, the authentication state and the stop sequence that are tied to the one server must be made to serve two. This is the largest change to existing code.
- **Shared password hashing.** Two hashes at once is the engine's whole budget. The reserved place for the engine's own logins halves what the portal can use, so a busy portal login can wait a second or two. That is the right way round.
- **Throttles are in memory.** A restart of the daemon clears the login throttle and the portal's hourly and per-minute budgets. Quotas and the daily ceiling are counted from rows and survive.
- **The reported client address.** The daemon sees the portal's address for every login. It cannot check the address the portal reports, so the username and the total are the limits that count. A stranger can therefore delay one requester's login from a new browser, as at the engine's login today.
- **Sessions are written asynchronously.** A logout is immediate in memory and is written to the database later, a known limit of the engine's sessions that portal sessions inherit.
- **Adding a series and monitoring seasons in one step.** `axis_tv_add()` adds a series with one monitoring choice and may ask the provider for seasons. Approval needs "add with nothing monitored, then monitor these seasons" as one outcome, and an approval that fails between the two must leave a state that "Try again" completes. The provider can also be unreachable at the moment of an automatic approval.
- **Derived states cost a read.** A search page of 20 titles needs the state of each, and "My requests" needs the state of each request. The Movies and TV lists already read every title's facts in places (a known limit). The state query must be one query for a page, with a test at a library of realistic size.
- **A season's state for a series Axis does not have** is worked out from requests alone; for one it has, from episodes and files. The two must agree at the moment of approval.
- **A calendar window in local time** has a short and a long day each year, and a week boundary on Monday that not every household expects. The window is a setting, and the page says when it ends.
- **libsoup in front of the internet.** The engine's known limits apply to the portal: a connection is seen only after it is accepted, a slow reader is not timed, and a path with an encoded slash is answered by libsoup with no security headers. The proxy must cover the first two. The portal is tested and fuzzed like the daemon, and the sandbox is the answer to what the tests miss.
- **htmx-glib's gaps** apply too: attribute values are escaped for two characters only, there is no request-size limit of its own, and the cookie parser accepts duplicate names. The portal's pages put provider text in element text only, never in an attribute that is not quoted by the builder, and the portal refuses a request with two session cookies.
- **The same image.** See "The image" above.

## Believed and not confirmed

- That libsoup's server reports the local port of a request reliably, or that two server instances can share one main loop and one stop sequence without disturbing the limits. P2 starts by confirming it. **Confirmed in P2, for the second half; the first is not needed.** Two `HtmxServer` instances, each with its own libsoup server, middleware chain, router and connection limits (`axis_web_limits_install()` keeps its state on the server object, and has no process-wide state), run on one main loop and are stopped one after the other: `tests/test-portal-api.c` runs both in every case, and the daemon's main listener and its limits behave as before beside it. Nothing asks for the local port of a request: a request that arrives at the portal's server is the portal listener's. The daemon's own start of the second listener from the configuration is run by `tests/test-daemon.c` (`/portal/api/listener/off-by-default`).
- That a Compose network of two services isolates them from the others as the examples assume, under Docker as well as Podman. Docker has not been run for any example. **Refuted for Podman in P4:** a container reaches the addresses of the host's other Podman networks unless its own networks are internal ("As built in P4"). The portal's networks are internal now. For Docker it is still believed and not confirmed.
- That the browser support for the `__Host-` prefix and `SameSite` is as assumed in the browsers that were not tried (Firefox, Safari, a phone).

## Independent review

Each slice from P2 on touches accounts, sessions or a token-authenticated API, so each is reviewed by someone who did not write it before it is merged, as `openspec/project.md` requires, and this section is updated with the reviewer and the findings. P1 is reviewed for its checks of who may read and change a request. P5 is a review of the whole from the outside: the reviewer is given the portal's address and a requester account, then the portal's container, and tries each line of "If the portal is taken over" in both directions.

A first independent review of P1 read the slice's diff and found no blocker, five findings that should be fixed and eleven minor ones. Each is fixed with a test where it could be, or recorded as a known limit, and "As built in P1" says what changed: a placing that removed a series, the name of who asked shown through a request for another season, an approved request that never closed when its title was removed, rows without a bound from asking and withdrawing, a merge that dropped people and seasons, and the minor ones besides. Task 1.90 stays open until the fixes are reviewed.

A first independent review of P2 read the slice's diff and found no blocker, four findings that should be fixed and eleven minor ones. Each is fixed with a test or recorded as a known limit, and "As built in P2", under "After the review", says what changed: a requester's password that opened the engine after a change of role, titles with no adult mark, operations without a budget on the engine's shared threads, one requester who could use up the portal's day, and the minor ones besides. Task 2.90 stays open until the fixes are reviewed.

P3 is built and has not been reviewed by someone who did not write it (task 3.90). Where such a review should look first: `serve()` in `clients/portal/axis-portal-app.c` (the order of the checks, and that nothing reaches the daemon before them), `axis-portal-wire.c` (what a body can carry, and what an answer may), `axis-portal-view.c` (the one escape), and the cookies and the two tokens.

P4 is built and has not been reviewed by someone who did not write it (task 4.90). Where such a review should look first: the networks of `deploy/examples/traefik/compose.portal.yaml` as a container runtime builds them, under Docker too, which was not run (what the portal's container reaches by address, not only by name); `axis_web_auth_portal_note_address()` and its use in `handle_login()` (`src/web/axis-web-auth-pages.c`): which address is remembered, which is compared, and who can be locked out; the portal's container as `tests/check-container.sh` runs it against what each example asks of its runtime; and the Kubernetes NetworkPolicies, which were never applied. P5 is a proposal.

## Open questions

1. **A slim image for the portal.** Recommended: yes, a second final stage in the same Containerfile, in P4, and the examples use it. The main image still holds the binary.
2. **May an ordinary user log in at the portal?** Recommended: no in these slices. An ordinary user's password also opens the engine's interface, which shows far more, so it should not be typed into the internet-facing piece.
3. **A password change through the portal.** Recommended: a later slice with its own review, with the current password required and the throttle of the engine's account page. Until then an administrator sets passwords.
4. **Show who asked, by default.** Decided as on, for household use. Recommended to keep, with the note that a held portal can then read which requester asked for which title.
5. **New seasons of a series that was requested whole.** Recommended: not automatic. A later slice can offer "ask me when a new season is announced" as part of "what changed".

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

As built in P1, the rows also hold: the name of the account that asked (kept so that a request still says who asked when the account is deleted; the account's own column is then empty); when the request was counted against that person's quota (when it was made, or when a follower took it over); why a request waits although the person's switch is on; the engine's own reason when an approved title could not be added, and when it was added; and whether it came through the engine's interface or the portal API, which P2's daily ceiling counts. `request_season` repeats the title and carries an "open" flag that a trigger keeps equal to its request's decision, so that the unique index can be on the season itself. A trigger moves a title's requests to the record that is kept when two records of a title merge, as the movies' and the series' own triggers do.

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
| P2 | The requester role, the portal token, the portal listener, the eleven operations and their enforcement tests, portal sessions, the daemon's limits, the page "Request portal". | The portal listener, off by default. No program speaks to it but the tests. | `portal_api`, with the next free number then |
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

- That libsoup's server reports the local port of a request reliably, or that two server instances can share one main loop and one stop sequence without disturbing the limits. P2 starts by confirming it.
- That a Compose network of two services isolates them from the others as the examples assume, under Docker as well as Podman. Docker has not been run for any example.
- That the browser support for the `__Host-` prefix and `SameSite` is as assumed in the browsers that were not tried (Firefox, Safari, a phone).

## Independent review

Each slice from P2 on touches accounts, sessions or a token-authenticated API, so each is reviewed by someone who did not write it before it is merged, as `openspec/project.md` requires, and this section is updated with the reviewer and the findings. P1 is reviewed for its checks of who may read and change a request. P5 is a review of the whole from the outside: the reviewer is given the portal's address and a requester account, then the portal's container, and tries each line of "If the portal is taken over" in both directions.

No review has been made yet. This document is a proposal.

## Open questions

1. **A slim image for the portal.** Recommended: yes, a second final stage in the same Containerfile, in P4, and the examples use it. The main image still holds the binary.
2. **May an ordinary user log in at the portal?** Recommended: no in these slices. An ordinary user's password also opens the engine's interface, which shows far more, so it should not be typed into the internet-facing piece.
3. **A password change through the portal.** Recommended: a later slice with its own review, with the current password required and the throttle of the engine's account page. Until then an administrator sets passwords.
4. **Show who asked, by default.** Decided as on, for household use. Recommended to keep, with the note that a held portal can then read which requester asked for which title.
5. **New seasons of a series that was requested whole.** Recommended: not automatic. A later slice can offer "ask me when a new season is announced" as part of "what changed".

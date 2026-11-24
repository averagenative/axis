# Add a request portal

## Why

People who are not administrators want to ask for a movie or a series, and to see what became of the request. A typical case is a few members of one household who reach the service from the public internet. Today that takes a separate request application in front of other media managers. Axis has no equivalent: an ordinary user of Axis can look at almost every page and can change nothing but their own account.

A request portal is the one piece of Axis that is meant to face the internet. That makes its security design the point of this change, more than its pages. The roadmap in `openspec/project.md` already says so: requests come "after the account system has had time to harden".

## What Changes

This change is built in five slices. Nothing is reachable from the internet until the fourth slice is built, and no document says the portal may be exposed until the fifth, an independent security review, has passed.

- **Requests inside the engine (P1).** A request is a person, a title, a kind (movie or series) and, for a series, a list of seasons. The person chooses the whole series, the first season, the latest season or picked seasons, and the choice becomes a season list when the request is made. There are no requests for single episodes. A request waits for an administrator unless the person is approved automatically for that kind. An administrator approves or declines on a new Requests page, and waiting requests are a notice under "Needs attention". Approval adds the title with the library folder and quality profile that the administrator set as defaults, and starts a search. The requester never chooses either. Every title shows its state to every person who can ask (available, partly available, downloading, approved, requested, declined, or can be requested). A second person who asks for the same thing follows the existing request. Each person has a quota for each kind over a day, a week or a month. An ordinary user gets a Request button where an administrator has Add, and a list of their own requests.
- **A scoped token and a narrow API (P2).** A new credential class, the portal token, opens only the portal API: eleven operations, each with a fixed set of fields. The API is answered on a listener of its own that is off by default, so an installation without a portal gains no new surface. A third, weakest role, the requester, can log in through the portal API only and never to the engine's own interface.
- **The `axis-portal` program (P3).** A second client binary beside `axis-worker`, built from the same tree. It serves its own small pages (log in, browse, search, a title, my requests), holds no database connection, no media and no secret but its token, and writes nothing to disk. It is not a reverse proxy: no page of the engine can be reached through it.
- **Limits, lockout, hardening and deployment (P4).** Quotas and rate limits on both sides, with the daemon as the authority. Container hardening. An optional portal service in the Compose, Kubernetes and Quadlet examples, on a network that reaches the daemon's portal listener and nothing else.
- **An independent security review (P5).** A gate. Until it passes, the examples and the guide say that the portal is not yet reviewed for exposure to the internet.

A later slice, described in `design.md` and not specified here, adds reports of a problem with a title (a category, a note, open or resolved) and a "what changed" list in the portal that stands in for notifications.

What stays out:

- No requester access to anything else in Axis, and no proxying of the engine's pages.
- No self-registration, no account import, no single sign-on, no login by a proxy's header and no login without a password. An administrator creates each requester. Invitations are not planned (see `design.md`).
- No free text from a requester. A request carries a title, a kind and seasons, nothing typed.
- No requests for single episodes.
- No choice of quality, of a second copy in another quality, or of a library folder by the requester, and no request made in the portal on another person's behalf. An administrator does those in the engine's interface.
- No change to a request after it is made, except to withdraw it while it waits.
- No votes and no newsletters.
- No notification outside Axis (e-mail or push) in these slices. The requester learns what happened from the portal's "My requests" page.
- No password change or password reset through the portal in these slices.
- Movies and series only. Music, books and games requests come with their modules.

## Capabilities

### New Capabilities

- `requests`: the request model, approval, automatic approval, quotas, duplicates, the derived state of a request, and the Requests pages of the engine's interface.
- `portal`: the portal API with its scope and limits, and the `axis-portal` program with its pages.

### Modified Capabilities

- `web-auth`: the requester role, the portal API as a third credential class, portal sessions kept apart from the engine's, and a reserved place in password hashing for the engine's own logins.
- `attention`: notices for requests that wait, for an approved request that could not be added, and for a portal that keeps reaching its limits.
- `deployment`: the portal binary in the image, the optional portal service in the examples, and the rule about when the guide may say that the portal can be exposed.

## Impact

- **Database.** Two migrations: `0083_requests.sql` (P1: `request`, `request_season`, `request_follower`, `request_closed_title`, the per-person request settings on `app_user`) and a `portal_api` migration with the next free number when P2 is built (P2: the `requester` role, `portal_token`, a kind on `web_session`). Both only add tables, columns and a wider check constraint, and are safe on a live database.
- **Daemon.** New code in `src/requests/` (the model and its rules), `src/api/` (the portal API beside the worker API), `src/web/` (the Requests pages, the Request portal page, the role and credential-class checks in the one authentication middleware), and a second listener in `src/axisd/main.c`. `axis_movies_add()` and `axis_tv_add()` are called by an approval, unchanged.
- **A new binary.** `clients/portal/`, linked like `axis-worker` without libpq, with a link check of its own.
- **Image and examples.** `axis-portal` is added to the image. The Compose, Kubernetes and Quadlet examples gain an optional portal service and a network for it.
- **Settings.** `requests.*` (quotas, defaults for placement, automatic approval limits) and `server.portal_listen_port`.
- **Review.** This change touches accounts, sessions and a token-authenticated API, so every slice from P2 on is reviewed by someone who did not write it, and P5 is a review of the whole.
- **Known limits.** Each slice adds its limits to `openspec/project.md`. The ones known before any code is written are in `design.md` under "Where it is hard".

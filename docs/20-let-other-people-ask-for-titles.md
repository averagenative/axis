# Let other people ask for titles

This page shows you how people who are not administrators ask Axis for a
movie or for seasons of a series, and how you, as an administrator, approve
or decline what they ask for. Allow about 15 minutes.

A request is a person, a title, and for a series a list of seasons. A person
never types anything into a request, and never chooses a library folder or a
quality profile. A request waits for an administrator, and while it waits
Axis adds nothing and searches for nothing.

Requests work in two places. Inside Axis's own pages, a person with a
**User** account asks with a **Request** button, and still sees the other
pages of Axis. In the request portal, a separate small site, a person with a
**Requester** account can ask and can see nothing else. The request portal
is optional and has not been reviewed for exposure to the internet. See
[Set up the request portal](#set-up-the-request-portal-administrators).

## Before you begin

- You need an administrator account to approve requests and to set limits.
- Each person who asks needs an account with the role **User**. See
  [Add and manage accounts](12-themes-and-accounts.md#add-and-manage-accounts-administrators).
- The **Requests** module must be on. It is on by default. To check, open
  **System**, then **Modules**.
- To approve a movie request, Axis needs a library folder for movies and a
  quality profile. To approve a series request, it needs a library folder
  for TV. See [Add disks and library folders](02-add-disks-and-library-folders.md).

## Set where approved titles go (administrators)

1. Open **Media**, then **Requests**.
2. In the **Defaults** card, choose a **Library folder for requested movies**
   and a **Quality profile for requested movies**. Do the same for series.
3. Select **Save the defaults**. Axis shows "The defaults were saved."

Each approval starts with these choices, and you can change them for one
request. If you set no defaults, every approval asks you for the folder and
the profile, and no request is approved automatically.

## Ask for a movie (users)

1. Find the movie. Use the search box at the top of any page, or open the
   movie's page.
2. Read its state. Every title shows one:

   | State | What it means |
   | --- | --- |
   | **Can be requested** | Nobody asked for it, and it is not in the library. |
   | **Requested** | Someone asked, and the request waits for an administrator. |
   | **Approved** | An administrator approved it, and Axis is looking for it. |
   | **Downloading** | Axis found it and is downloading it. |
   | **Partly available** | A series: some of its aired episodes are in the library. |
   | **Available** | It is in the library. |
   | **Declined** | An administrator declined the last request for it. You can ask again. |
   | **Closed to requests** | An administrator decided that nobody can ask for it. |

3. Select **Request**. Axis shows "Requested. It waits for an administrator."

If someone asked for the same movie before you, Axis shows "It was requested
already: you now follow that request." Following a request counts nothing
against your limit, and the request is listed among your own.

## Ask for seasons of a series (users)

1. Open the series' page. The **Requests** table lists each season with its
   state.
2. Choose what to ask for: **The whole series**, **The first season**, **The
   latest season**, or **The seasons ticked below**. For the last choice, tick
   the seasons.
3. Select **Request**.

Axis turns your choice into a list of seasons at that moment. The whole
series is every season the series has then, without the specials. To ask for
the specials, tick **Specials**. A season that is announced later is not part
of an earlier request: ask for it when it exists.

Seasons that are in the library, or that someone asked for already, are left
out of your request. You follow the requests that hold them. You cannot ask
for a single episode.

## See what became of your requests (users)

Open **Media**, then **Requests**. The page is called **My requests**.

- **What you can still ask for** shows what is left of your limits, for
  example "Movies: 2 of 5 left this week."
- **What you asked for** lists each request with its **Title**, **Seasons**,
  **State** and when you asked.

A request that failed says why in one of three sentences:

| What you see | What it means |
| --- | --- |
| "The title could not be added." | An administrator has to look at it. |
| "A download failed. Axis is looking again." | Axis tries the next release by itself. |
| "A download finished and needs the administrator." | The download arrived, and an administrator must decide how to import it. |

A request that reads **Removed from Axis** is closed: an administrator
removed the title after it was added. You can ask for the title again. The
closed request still counts against your limit for the window it was made in.

To take back a request that still waits, select **Withdraw**. Withdrawing
gives its count back. If other people follow the request, the first of them
takes it over if they have room in their limit. To stop following another
person's request, select **Stop following**.

You cannot change a request after you make it, and you cannot withdraw one
that was approved.

## Approve or decline a request (administrators)

1. Open **Media**, then **Requests**. The **Waiting for you** card lists the
   requests that wait, the oldest first. Each row shows who asked, the
   seasons, how many other people asked for the same thing, how many aired
   episodes an approval looks for, and how often the title was declined
   before.
2. To approve, check the **Library folder** and the **Quality profile**, and
   select **Approve**. Axis adds the title and starts its search. For a
   series, Axis monitors the seasons of the request and changes no other
   season. For a series that Axis already has, the series keeps its own
   folder and profile.
3. To decline, enter a **Reason for declining** of at most 300 characters and
   select **Decline**. The person who asked sees the reason, and so does
   everyone who follows the request. Nobody else does.

To stop requests for a title for good, tick **Do not ask again: close the
title to requests** when you decline, or select **Close the title to
requests** on the row. The **Closed to requests** card lists the closed
titles. Select **Open to requests again** to undo it.

When two administrators decide the same request at the same moment, one
decision counts. The other administrator sees "The request was already
decided."

## Try again when a title could not be added (administrators)

An approved request stays approved when Axis cannot add its title, for
example because the library folder was removed or its disk is offline. The
row in the **Decided** card shows the reason and a **Try again** button.

1. Choose another **Library folder** or **Quality profile** if the reason
   says that one is missing.
2. Select **Try again**. Axis shows "The title was added and its search
   started."

Axis never removes what an approval added, even when the approval fails part
of the way. A series request can fail after Axis added the series and
monitored some of its seasons, for example when a season that the request
lists is no longer in the series' details. The series then stays in **TV**
with those seasons monitored, and **Try again** monitors the rest.

**Try again** is offered only while the title was never added. Once the
title is in Axis, what you change on its own page (for example a season you
stop monitoring) stays as you left it.

## Set limits and automatic approval for a person (administrators)

1. Open **Media**, then **Requests**. In the **Defaults** card, select
   **People: automatic approval and limits**. The **Users** page has the same
   link.
2. On the person's row, set any of the following and select **Save**:

   - **Approve their movie requests automatically** and **Approve their
     series requests automatically**. Both are off for a new account. With a
     switch on, that person's requests of that kind are approved when they
     are made, with the default folder and profile.
   - **Movie requests in each window** and **Season requests in each
     window**. Leave a field empty to use the limit for everyone. Enter 0 so
     that the person cannot ask for that kind.

An automatic approval is bounded. The person's limit still applies. A series
request that lists more than three seasons waits for you. A request also
waits when no default folder and profile are set, or when one of them was
removed. The request's row says why it waits.

To change the limits for everyone, open **System**, then **Settings**, and
find the **Requests** card:

| Setting | Default | What it does |
| --- | --- | --- |
| **A person's request limits count over** | A week, from Monday | The calendar window, in the server's local time. |
| **Movie requests for each person in that time** | 5 | A movie counts as one. |
| **Season requests for each person in that time** | 5 | Each season of a request counts as one. |
| **Most seasons approved automatically in one request** | 3 | A longer request waits for you. |
| **Show people who asked for a title** | On | When off, only administrators and the person who asked see the name. |

A request that is declined keeps its count. A request that is withdrawn
gives it back. Administrators are not limited.

Besides these limits, a person who is not an administrator can make at most
100 requests in any 24 hours, whatever became of them. Withdrawing a request
does not give back its place under this ceiling. You cannot change it. It
stops a person, or someone who knows their password, from filling the
database by asking and withdrawing again and again.

A person whose limit for a kind is 0 cannot follow another person's request
of that kind either.

## What happens when an account goes

When you disable or delete an account on the **Users** page, Axis withdraws
the requests of that account that still wait. If another person follows one
of them, that person takes it over as for a withdrawal. Approved requests
and the titles that they added stay as they are.

## Set up the request portal (administrators)

The request portal is a separate program, `axis-portal`, with pages of its
own: a login, a search, a page for each title, and **My requests**. A person
who uses it needs a **Requester** account, and sees nothing else of Axis. You
do not need the portal to use requests inside Axis's own pages.

**The request portal has not been reviewed for exposure to the internet.**
The portal, its interface to Axis and the **Requester** role have not had
their independent security review. Until the deployment guide says that the
review has passed, let the portal be reached only from a network you trust,
such as your home network or a VPN. Do not open it to the internet.

The portal runs in a container of its own, behind a reverse proxy with
HTTPS. It holds one secret, its token, and has no access to the database or
to your media. Allow about 30 minutes for the setup.

### Before you begin

- Axis runs in containers from one of the deployment examples: Traefik with
  Compose, Kubernetes, or Podman Quadlet. The minimal and full-stack Compose
  examples have no reverse proxy with HTTPS, so they have no portal.
- Your image contains `axis-portal`: a release after 0.1.0-rc2, or an image
  that you build with `make container`.
- You have a host name for the portal, such as `requests.example.com`,
  besides the one for Axis.
- You set the defaults for requests. See [Set where approved titles
  go](#set-where-approved-titles-go-administrators).

### The steps

Each deployment example has the commands for its own files. The steps are
the same in all of them:

1. Switch on the daemon's portal listener, and start Axis again. The example
   says which lines to add. On the **Request portal** page (**System**, then
   **Request portal**), **The portal's listener** then says "On: the portal
   API is answered on port 7801".
2. On that page, select **Make a token**. Axis shows the token once. Copy
   it.
3. Give the token to the portal as a secret file. The example says where the
   file goes. Never put the token in a command line or in a compose file.
4. Create a requester account. See [The Requester role](#the-requester-role).
   Give the username and the password to the person.
5. Start the portal.
6. Open `https://requests.example.com/login`, with your host name. The page
   shows a **Username** field and a **Password** field. Log in as the
   requester.

The commands for each example:

- [Traefik with Compose](../deploy/examples/traefik/README.md#the-request-portal)
- [Kubernetes](../deploy/examples/kubernetes/README.md#the-request-portal)
- [Podman Quadlet](../deploy/examples/quadlet/README.md#the-request-portal)

When you finish, you have a requester account, a portal token, the portal
running, and a login page at your own address.

[The request portal](../deploy/README.md#the-request-portal) in the
deployment guide explains the networks, what the reverse proxy must do, and
what someone who takes over the portal can and cannot do.

### What a requester does in the portal

1. Open the portal's address and log in.
2. Search for a title with the search box, or pick one from the lists on the
   first page.
3. Open the title. The page shows its state, as in [Ask for a
   movie](#ask-for-a-movie-users).
4. Select **Request**. For a series, choose the whole series, the first
   season, the latest season, or the seasons that you tick.
5. Open **My requests** to see what became of each request. To take back a
   request that still waits, select **Withdraw**.

A requester cannot change a password in the portal. You set it on the
**Users** page.

### Two rules for administrators

- **Never type an administrator's password into the portal.** The portal's
  login accepts requesters only. Whoever controls the portal sees every
  password that is typed into it.
- **Axis refuses a login from the portal's address.** For 24 hours after the
  portal last called Axis, Axis refuses a login at its own login page from
  the address that the portal called from. Axis answers as for a wrong
  password. A session that you already have keeps working. If you are
  refused although your password is right, the Axis log has a line that
  begins with `login refused: client=` and says that the address presented a
  portal token. Log in from another address. If the portal and your browser
  reach Axis from the same address, stop the portal, restart Axis, and log
  in.

### The Requester role

A **Requester** is an account for the request portal only. A requester cannot
log in to Axis's own pages at all, so a requester sees no file names, no
release names and no downloads.

1. Open **System**, then **Users**.
2. In the **New account** card, enter a **Username** and a **Password**, and
   set **Role** to **Requester (the request portal only)**.
3. Select **Create account**.

A requester's password needs at least 12 characters and must not contain the
username. Axis refuses a password that breaks the rule and states the rule.

To change an account's role, use **Change role to user**, **Change role to
administrator** or **Change role to requester** in the account's row. A
change of role ends every session of that account at once, in Axis and in
the portal. If you change your own role, you are logged out. Axis refuses to
make the only enabled administrator a requester.

To give a requester another role, type a new password in the field beside
**Change role to user** or **Change role to administrator**. The person typed
the old password into the portal, so it must not open Axis's own pages: Axis
refuses the change without a new password, or with the one the account has
now. The new password follows the rule of the new role (at least 10
characters). An account that you make a requester keeps its password.

When a requester is logged in through a portal, the account's row shows
**End 1 portal session** (or the number of sessions). Select it to log that
person out of the portal at once. Disabling the account or setting a new
password does the same.

A requester's limits and automatic approval are set like any other person's.
See [Set limits and automatic approval for a
person](#set-limits-and-automatic-approval-for-a-person-administrators).

### The Request portal page

Open **System**, then **Request portal**. The page shows three things.

- **The portal's listener** says whether Axis answers the portal's interface.
  It is off by default. To switch it on, set `portal_listen_port` in the
  `[server]` section of the configuration file, or `AXIS_PORTAL_LISTEN_PORT`
  in the environment, to a port that Axis does not use, and restart Axis.
  That port answers the portal's interface and nothing else: no page, no
  login form and no other part of Axis. Axis's main port never answers the
  portal's interface. The port listens on the same address as the main port
  unless you set `portal_listen_host` (or `AXIS_PORTAL_LISTEN_HOST`) to an
  address of its own. Use the address that Axis has on the network it shares
  with the portal alone, so that nothing else on your network reaches it.
- **Limits** shows what the requests made through the portal count today
  against the daily limit, how many logins through the portal failed in the
  last hour, and how often each of the two limits was reached in the last 24
  hours. Axis keeps these counts of reached limits in memory, so a restart of
  Axis clears them.
- **Tokens** lists the tokens that a portal identifies itself with. A token
  opens the portal's interface and nothing else, and by itself it acts as no
  one: every action also needs the session of a requester who logged in. The
  **From** column shows the address that last used each token.

To make a token, select **Make a token**. Axis shows the token once. Copy it
then: Axis keeps only a hash of it and cannot show it again. At most four
tokens can be active.

To replace a token without refusing the portal's calls, make a second token,
give it to the portal, then select **Revoke this token** on the first. A
revoked token is refused from the portal's next request.

When the portal starts with a new token, everyone who is logged in to the
portal must log in again. The portal signs its session cookies with a key
that it derives from its token, so a new token makes the old cookies
invalid. A restart with the same token logs nobody out.

The setting **Requests through the request portal in one day** (30 by
default, under **System**, then **Settings**, in the **Requests** card)
limits what all requests made through a portal count in one calendar day: a
movie counts one and each season counts one. It bounds what a portal in the
wrong hands can ask for. One person's requests can count at most a third of
it, withdrawn ones included, so that one person cannot use up the day for
everyone.

Axis also limits how often a portal can call it: 600 calls a minute for a
token and 120 for each person. Axis tells a portal that calls more often to
wait.

Axis counts the logins that fail through the portal. After 30 failures in an
hour, over all usernames, Axis admits one login a minute from a browser that
it does not remember, until the hour has passed. A browser that logged in
before is not held to this limit.

When the portal reaches the daily limit for requests or the hourly limit for
failed logins, Axis shows a notice under **Needs attention**, in the group
**Worth a look**, for 24 hours. The notice says which limit was reached and
how often, and links to the **Request portal** page. It names no account. A
limit that is reached again and again can mean that someone guesses
passwords, or that someone controls the portal.

Axis hides some titles from requesters: a title that Axis stored before this
version, and a title that came from an import or from a list that you
brought over. Axis cannot tell whether the provider marks such a title as
adult. The title stays hidden until Axis gets the title's record from its
provider again, from a search that finds the title or from the refresh of
the titles that Axis keeps up to date. Until then, an ordinary user cannot
ask for the title either.

## What can go wrong

| What you see | What to do |
| --- | --- |
| "No movie request is left until Monday." | You reached your limit. Wait for the next window, withdraw a request that still waits, or ask an administrator for a higher limit. |
| "The request lists 4 seasons and 3 seasons are left until Monday." | Ask for fewer seasons. Axis refuses the whole request and makes none. |
| "You cannot ask for series." | An administrator set your limit for that kind to 0. |
| "You made 100 requests in the last 24 hours, withdrawn ones included, and that is the most anyone may; ask again later." | You reached the daily ceiling. Wait until your oldest request of the last 24 hours is a day old. |
| "That library folder is not one for movies." | You chose a folder for TV to approve a movie, or the other way round. Choose a folder of the request's kind. |
| "Its title was added and is still in Axis." | **Try again** applies only to a title that was never added. |
| "The title is closed to requests." | An administrator closed the title. Only an administrator can open it again. |
| "Series cannot be requested here." | The TV module is off. An administrator can switch it on under **System**, then **Modules**. |
| "Choose a library folder and a quality profile for it." | No default is set, and the approval named none. Choose both and approve again. |
| "The request is no longer waiting." | An administrator decided it before you withdrew it. |
| A request stays **Requested** although automatic approval is on | The row on the **Requests** page says why: no defaults are set, a default is missing, or the request lists too many seasons. |
| A page of yours shows no **Request** button | You are an administrator and use **Add**; or your limit for that kind is 0; or the title is closed, available, or requested already. |

## Verified

- The requests model and its pages are covered by the test suite, under the
  sanitizers: asking, seasons, limits, followers, decisions, automatic
  approval, the states, who can read and change a request, and the two
  notices. The tests force two requests and two decisions to meet at the
  same moment.
- No request has been made in a real browser, and none has run against a
  real library, a real indexer, or a real download client. The pages have
  not been tried on a phone.
- The **Requester** role, the tokens and the portal's interface are covered
  by the test suite, under the sanitizers: what a token opens, what a
  requester's session opens, and what each answer holds.
- The portal program is covered by the test suite, under the sanitizers,
  against a stand-in for Axis and against a real Axis: its pages, its
  cookies, its limits, and what it sends on.
- The setup steps were run with rootless Podman, by a script that does what
  the steps say: the image was built, the Traefik example was started with
  the portal, a requester logged in at the portal's host name through
  Traefik over HTTPS and asked for a movie, and the request waited on the
  **Requests** page. From the portal's container, PostgreSQL and the
  daemon's main port could not be reached, by name or by address.
- The Quadlet example was started with the portal the same way. It has no
  reverse proxy, so nobody logged in: the script sent one request as a proxy
  would, and the portal served its login form.
- Not run: Docker, the Kubernetes manifests of the portal on a cluster, a
  reverse proxy in front of the Quadlet example, a certificate from a real
  certificate authority, a real browser, and a phone.
- The request portal has not been reviewed for exposure to the internet.
  Nobody who did not write it has tried to break it. Let it be reached only
  from a network you trust. Do not expose Axis itself, or the daemon's portal
  port, to the internet either.

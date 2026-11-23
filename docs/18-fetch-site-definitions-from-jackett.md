# Fetch site definitions from Jackett

This page shows you how to fetch site definitions from Jackett's repository,
update them later, and put an earlier version back. The example is a
definition file called `madeupsite.yml`. Allow about 5 minutes.

A site definition describes how to search a torrent site that has no API of
its own. Axis comes with four of its own
([Add a built-in torrent site](03-connect-indexers-and-download-clients.md#add-a-built-in-torrent-site)).
The Jackett project keeps several hundred more. Axis comes with none of
those files. You fetch the ones that you choose to your own server, and Axis
reads them as data.

## Before you begin

You need:

- An administrator account. Only administrators can list, fetch, update or
  put back a definition.
- A server that can reach `api.github.com` and `raw.githubusercontent.com`
  over HTTPS. If you bind Axis's outbound traffic to a VPN interface, these
  requests go through that interface, as an indexer's requests do, and Axis
  refuses them while the interface is down.

Know the following before you fetch anything:

- **The licence.** Jackett is published under the
  [GNU General Public License, version 2 (GPL-2.0)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
  Its definitions are in the folder
  [`src/Jackett.Common/Definitions`](https://github.com/Jackett/Jackett/tree/master/src/Jackett.Common/Definitions)
  of its repository. Install only definitions that you have the right to use.
- **Nothing happens by itself.** Axis asks GitHub only when you select a
  button on these pages. It fetches nothing at start, nothing on a schedule,
  and it updates nothing by itself.
- **Not every definition can be used.** Axis runs definitions of public sites
  that need no login. It installs any other valid definition too, and marks it
  **Not usable yet** with what it needs.
- **GitHub limits requests.** Without an account, GitHub answers about 60
  requests an hour from one address. Listing the definitions takes 2 requests.

## See what Jackett has

1. Open **System**, then **Indexers**.
2. In the card **Other torrent sites, through site definitions**, find
   **Fetch definitions from Jackett**.
3. Select **See what Jackett has**.

Axis asks GitHub for the newest commit of Jackett's repository and for the
names of the definition files in it. No file is fetched. The page that opens
shows the files, the commit that Axis read them at, and when.

For each file, the column **On this Axis** says one of the following:

| What it says | What it means |
|---|---|
| **Not installed** | You do not have this definition. |
| **Installed: the same file** | You have exactly this file. |
| **Installed, and differs: Jackett's file has changed since yours was fetched** | An update is available. |
| **Installed, and differs: yours was installed or changed by hand** | You installed or changed this definition yourself. A fetch leaves it as it is unless you say otherwise. |

Axis lists only files whose names are lower-case letters, digits and hyphens
followed by `.yml`. The page says how many other entries it left out. A file
whose name has a capital letter is one of them. That is normal: if the page
says it left out one entry, a capital letter in one name is the usual reason,
and you have lost nothing you can fetch.

To find a definition, type part of its name in **Find a definition by name**
and select **Find**. Ticks are not kept from one search to the next, so fetch
what you ticked before you search again.

## Fetch the definitions you want

1. Tick each definition that you want. You can fetch at most 50 at once.
2. Select **Fetch the ticked definitions**.

Axis downloads each file at the commit that it listed, so what you saw is
what you get, whatever has changed at Jackett since. Axis then checks each
file exactly as it checks a file that you install by hand. The card **What
the fetch did** says what happened to each file:

| What it says | What it means |
|---|---|
| **Installed.** | The definition is new on this Axis. |
| **Updated.** | The definition replaced the one that you had. Axis keeps the earlier version. |
| **Already installed: the same file.** | Nothing changed. |
| **The same as the one you installed by hand; now tracked as fetched from Jackett.** | The file that you installed by hand is exactly Jackett's file. Nothing is replaced. From now on Axis can check it for newer versions like any fetched definition. |
| **Not installed.** | Axis did not take the file. The sentence that follows says why. |

When an update replaces a definition that indexers already use, the row also
says how many: for example, **2 indexers use it.** It then says either that
the indexers run the new version from their next search, or that Axis cannot
run the new version, so those indexers do not search until you put the earlier
version back.

If Axis cannot run the new version and indexers use the definition, Axis
refuses the update and leaves the earlier version in place. The row says how
many indexers use the definition, why Axis cannot run Jackett's file, and that
the earlier version is left as it is. To replace it anyway, tick **Also
replace definitions installed or changed by hand**. The same tick also
replaces a definition that you installed or changed by hand.

A fetched definition searches nothing until you add an indexer from it:

1. In **What the fetch did**, select the file's name. The definition's page
   opens.
2. In **Add an indexer that runs this definition**, select **Add the indexer**.

## Update the definitions that you fetched

1. Open **System**, then **Indexers**, and find **Fetch definitions from
   Jackett**.
2. Select **Check Jackett for newer versions**. Axis reads the list again, and
   marks each definition whose file changed there **Newer at Jackett**.
   Nothing is updated yet.
3. Do one of the following:
   - To update one definition, select **Update** on its row.
   - To update several, tick them and select **Update the ticked
     definitions**.

A definition whose file Jackett no longer has is marked **No longer at
Jackett**. Axis keeps your copy, and there is nothing to update it from.

A definition that you changed by hand since you fetched it is marked
**Changed by hand**. An update leaves it as it is, and says so. To replace it,
tick **Also replace definitions installed or changed by hand** before you
update.

The indexers that run an updated definition use the new version from their
next search.

## Put the earlier version back

Axis keeps the version that an update replaced, one version back.

1. Open the definition's page: **System**, **Indexers**, then the
   definition's name.
2. Select **Put the earlier version back**.

The earlier version is in use again. The version that it replaced is not
kept, so you can put a version back only once after each update. To get the
newer version again, update the definition again.

## If something goes wrong

- **"GitHub refused to give the list of Jackett's definitions for now"**:
  your address has used up GitHub's requests for the hour. Other programs on
  your network count too. Nothing was changed. The message ends with how long
  to wait, for example "Try again in about 20 minutes". When GitHub does not
  say, it reads "Try again in an hour".
- **"What GitHub sent is not the file that was listed"**: the file that
  arrived is not the one in the list. Axis installed nothing. Select **See
  what Jackett has now**, then fetch the file again.
- **"That file was not installed"** with a reason: the file is not a
  definition that Axis can read. The reason names the line or the part. Axis
  takes such a file from nowhere, whether you fetch it or install it by hand.
- **"the definition in it has the id ..., not its file's name"**: Axis
  expects a file called `madeupsite.yml` to hold the definition with the id
  `madeupsite`. It refuses a file that says otherwise.
- **"GitHub does not have ... where Axis looks for it"**: Jackett may have
  moved its definitions. Install the file by hand with **Install the
  definition**, and report the problem.
- **"the outbound interface ... is down"**: your outbound binding is set and
  its interface is not up. See the **Outbound network and VPN** page, and
  "Sending Axis's traffic through a VPN" in the
  [deployment notes](../deploy/README.md#sending-axiss-traffic-through-a-vpn).

## Verified

- Written from the code and its tests, which run against a stand-in for
  GitHub with made-up definitions.
- The assumptions behind the fetch were confirmed against GitHub and
  Jackett's real repository: the addresses that Axis asks, the shape of
  GitHub's answers, and the names of Jackett's files.
- No fetched definition has searched its real site. Expect corrections
  there.

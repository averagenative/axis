# Use Axis from a phone

This page shows you how to add a movie or a series from your phone, and how
to check on it later. The pages are the same pages you use on a computer;
on a screen narrower than about 640 pixels they lay themselves out for one
thumb. Allow about two minutes for the first add.

## Before you begin

You need:

- Axis reachable from your phone. On your home network, open the address
  you use on your computer. From outside, put Axis behind a reverse proxy
  with HTTPS ([Run behind a reverse proxy](13-run-behind-a-reverse-proxy.md)).
- An account. Adding needs an administrator; any account can look.
- Everything [Find and download a movie](04-find-and-download-a-movie.md)
  needs: a TMDb token, a working indexer and download client, and a
  library folder for the kind of title you add.

## Find your way around

- The five sections (**Dashboard**, **Media**, **Activity**, **Media
  management** and **System**) are in the bar at the bottom of the screen.
  Tap one to open it.
- Under the top line, the pages of the open section are one strip. Swipe it
  sideways to see them all. In **Media** it starts with **Movies**, **TV**
  and **Discover**, then the pages of the one that is open.
- The search box stays at the top of the screen when you scroll down, so
  you can start a search from anywhere.
- A table on a phone shows each row as a block: the name and its state
  first, the progress next, then each value after its column's heading.

## Add a movie or a series

1. Tap the search box at the top and type part of the title, for example
   `lantern`. The results appear as you type. To look for one kind only,
   choose **Movies** or **TV** in the box beside it.
2. Find the title in the list. Each result shows its poster, kind, year and
   the start of its overview, and its state when you already have it, for
   example **Wanted** or **In library**. Tap the result.
3. The title's page opens with the poster beside the name, year and running
   time, then the overview. Under it, **Options** names the quality
   profile and the library folder that the add uses.
4. To use those, tap **Add to Movies** (for a series, **Add to TV**).
   To change them first, tap **Options**, choose, and then tap the
   button.
5. The page opens again and says "The movie was added." Its state shows
   what happens next, usually **Searching** with "A search is queued."

You can also add from **Discover**: tap **Media**, swipe the strip to
**Discover**, and pick a list such as **Popular**. Tap **Add to Movies** (or
**Add to TV**) on a title to add it with the defaults, or **Add with options**
to open its page.

## Check on it later

- **Media**, then **Movies** or **TV**: swipe the strip of states (**All**,
  **Wanted**, **Downloading**, **Needs attention** and the rest) and tap one
  to see only those titles.
- **Activity**, then **Downloads**: each download shows its state and a
  progress bar, then its size, speed and time left.
- **Activity**, then **Imports**: an import that is **Blocked** says why under
  **Why**, and its page says what resolves it.
- On a movie's or a series' page, the state comes first, with the reason
  when it needs you. The buttons that act on it are under the reason.

## What can go wrong

| What you see | What to do |
| --- | --- |
| The page looks like the computer's, very small | Your browser asked for the desktop site. Turn off **Request Desktop Website** (Safari) or **Desktop site** (Chrome) for the address. |
| A strip ends at the screen's edge and you cannot find a page | Swipe the strip sideways. |
| On a computer, the add form shows **Options** instead of its fields | Your browser is older than 2025 (it has no `::details-content`). Click **Options** to show the fields, or update the browser. |
| "Not among your movies. An administrator can add it." | You are not an administrator. Ask one, or log in as one. |
| Nothing happens after **Add to Movies**, and the state stays **Searching** | The search is queued. **Activity**, then **Jobs**, shows it. If it fails, the movie's page says why. |

## Verified

- The phone layout came with the change `add-phone-layout`. It was checked
  in headless Chrome at 390 by 844 pixels, with a phone's touch and screen
  emulated, on every page of a demo instance filled with made-up titles
  (`tests/tools/axis-demo-shots.sh`). No page is wider than the screen.
- It has not been tried on a real phone, and not in Safari. The disclosure
  of the add form was also checked in Firefox.
- Adding, the states and the notices are the computer's own and are covered
  by the test suite.

# Choose a theme and manage accounts

This page shows you how to pick a color theme, change your password, end
sessions, and (as an administrator) add people and set the default theme.
Allow about 10 minutes.

Axis has two roles. An **Administrator** can change settings and start
searches, downloads, imports and conversions. A **User** can look, and can
ask for a movie or a series
([Let other people ask for titles](20-let-other-people-ask-for-titles.md)),
but changes nothing else.

## Before you begin

You need to be logged in. To manage other people, you need an
administrator account.

## Choose your own theme

1. Select your name in the header of any page. This opens the **Your
   account** page.
2. In the **Theme** card, pick one under **Colour theme**. Each choice has a
   preview and a note: **Dark**, **Light** or **Follows the device**.

   | Theme | Kind |
   | --- | --- |
   | **Oxblood** | Dark |
   | **Dusk** | Dark |
   | **Forest** | Dark |
   | **Ember** | Dark |
   | **Plum** | Dark |
   | **Midnight** | Dark (true black for OLED screens; the default) |
   | **Graphite** | Dark |
   | **High contrast** | Dark |
   | **Paper** | Light |
   | **Linen** | Light |
   | **Match my device** | Follows the device: Oxblood on a dark device, Linen on a light one |

   <!-- screenshot: the Theme card on the Your account page with Midnight selected -->

3. Select **Save theme**. Axis shows "Your theme has been saved." The theme
   applies to your account only, on every browser you log in from.

To go back to what everyone else sees, choose **Use the default**. Its note
says which theme that is now.

## Change your password and end sessions

1. On the **Your account** page, in **Change password**, enter your **Current
   password**, a **New password (at least 10 characters)**, and the new
   password again. Select **Change password**. Axis ends your other
   sessions.
2. In **Browsers logged in to your account**, each row shows **Started**,
   **Last used**, **Address** and **Browser**. Your own is marked **This
   session**. Select **End this session** on any other row to log that browser out at once.

After too many wrong passwords, Axis answers "Too many wrong passwords. Try
again in N seconds."

## Log in when someone else keeps failing

After a few wrong passwords for one username, Axis makes everyone wait
before the next try for that username, with the right password too: "Too
many failed attempts. Try again in N seconds." The wait doubles with each
failure, up to 15 minutes.

The browser you log in with is remembered for 90 days after each
successful login. A remembered browser has a count of failures of its own,
so a stranger who keeps failing with your username does not lock you out
of your own browser. Your remembered browsers together also share a second
count: after ten wrong passwords on them, they wait too. On a browser that
has never logged in to your account, or after Axis's data directory was
lost, you wait like anyone else. Log in once, and that browser is
remembered from then on.

Axis remembers your 20 most recent browsers. Changing your password, or an
administrator disabling the account or setting its password, ends every
remembered browser of the account, so log in again on each one.

When you are logged in, the current password on the **Your account** page is
held to your session's own count, so a stranger who keeps failing with your
username does not stop you from changing your password.

## Add and manage accounts (administrators)

1. Open **System**, then **Users**. The **User accounts** page and its **All
   accounts** card list each person
   with **Username**, **Role** and **Status**.
2. In **New account**, enter a **Username**, a **Password (at least 10
   characters)** and a **Role**, and select **Create account**.
3. For an existing account, use the actions on its row:

   - **Change role to user** or **Change role to administrator**
   - **Disable this account** or **Enable this account**. Disabling ends the
     account's sessions.
   - **Set password**, which ends the account's sessions.
   - **Delete this account**, which asks "Delete the account NAME?".

## Set the default theme and session length (administrators)

1. Open **System**, then **Settings**, and find the **Web interface** card.
2. **Default theme** sets the theme of the login and setup pages, and of
   every user who has not chosen one. Users who chose a theme keep it. Pick
   a theme and save.
3. **Session idle limit** sets how long a browser may go unused before it
   must log in again. The default is 30 days. A session also ends 90 days
   after it was opened, however busy.

## What can go wrong

| What you see | What to do |
| --- | --- |
| "The current password is not correct." | Retype it. After repeated failures, wait. |
| "Too many failed attempts" on the login page, though you typed the right password | Someone failed with your username. Wait the seconds it says, or log in from a browser you logged in with since your last password change. |
| "Live updates are off in this tab" | Your account already has the most live pages open that Axis allows (16, in its tabs and browsers). Close another Axis tab, or reload the page; the tab tries again within a minute. |
| "The two new passwords do not match." | Retype both. |
| "An account called NAME already exists." | Choose another username. |
| "Choose one of the themes listed." | Pick a theme in the list. |
| You are logged out after a password reset or a disable | Both end that account's sessions. Log in again. |
| You lost the only administrator's password | Ask another administrator to set it on the **User accounts** page. The `AXIS_ADMIN_*` settings create an administrator only while no account exists ([the first administrator](../deploy/README.md#the-first-administrator)). |

## Verified

- Accounts, sessions, the login throttle and themes are covered by the test
  suite. The queue records no separate run on real hardware for them, but
  every real run in the other pages needed a login.
- Remembered browsers came with the change `fix-web-hunt-findings`; they
  are covered by the test suite and have not been tried in a real browser.
- Midnight became the default theme in the change `default-theme-midnight`
  (1,974 tests passing). No account was migrated: an account that chose a
  theme keeps it.

/*
 * axis-download-qbittorrent.h - the qBittorrent download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * qBittorrent's Web API v2 (qBittorrent 4.1 and later, 5.x included) is plain
 * HTTP under "<base>/api/v2/".  The login is a POST of a form with the user
 * name and the password; the session is a cookie (SID, or QBT_SID_<port> in
 * 5.x), kept by the core's in-memory cookie jar and sent to this client's
 * address only.  Its CSRF check wants the Referer and Origin of a request to
 * name the client's own address, so the kind sends both, equal to it.  Torrents
 * have a category of their own, which is Axis's.  The kind:
 *
 *   session  login once per connection.  A call answered 403 (no session)
 *           logs in again and is retried once, but only a call that changes
 *           nothing (the list, the test, the questions before an add, a
 *           removal).  An add that lost its session is reported as not sent
 *           and is never retried by the kind.
 *   login    "Ok." (4.x) or a 204 with no body (5.x) is success and a cookie
 *           must come with it; "Fails." (4.x) or 401 (5.x) is a wrong password;
 *           403 is a ban of Axis's address (too many failed logins).  A refused
 *           login is not tried again by itself for a while (ten minutes after a
 *           wrong password, fifteen after a ban), whatever calls come, so that
 *           tracking cannot walk into a ban or extend one; the wait is kept in
 *           memory against the client's user name and password, so saving
 *           others ends it, and so does a test (one deliberate login).
 *   test    login; /app/version and /app/webapiVersion for the version; the
 *           category read from /torrents/categories and made with
 *           /torrents/createCategory when it is missing
 *   add     the category is made sure of; a hash that is in qBittorrent
 *           already is refused (it is not Axis's to take over); a multipart
 *           POST of /torrents/add with the magnet ("urls") or the file
 *           ("torrents"), the category, and "paused" and "stopped" both false;
 *           the answer ("Ok." or "Fails.") has no hash, so the torrent is asked
 *           for by the hash Axis computed (/torrents/info?hashes=), a few
 *           times, and must be in Axis's category; "Ok." and the torrent never
 *           showing is reported as unreadable, which the layer matches against
 *           the list later and never sends again
 *   list    /torrents/info?category=, then only the torrents of exactly
 *           Axis's category
 *   remove  /torrents/info?hashes= first: a torrent it does not list is
 *           %AXIS_DOWNLOAD_ERROR_GONE (the delete answers 200 either way, so
 *           only the list can tell); then /torrents/delete with the hash and
 *           deleteFiles, and the torrent must be gone from /torrents/info
 *   folder  (category_folder) the category's own save path, when it is a
 *           full path and neither the default save path nor another
 *           category's folder is it or lies inside it; asked only for a
 *           torrent qBittorrent no longer has after its import
 *
 * qBittorrent's queue priority is not a priority per torrent: the core's is
 * ignored by this kind, and a Usenet release is refused before any request.
 * No save path is sent: there is no setting for one, so qBittorrent's own is
 * used.
 *
 * What qBittorrent answers is read by download/axis-download-qbittorrent-parse.h.
 *
 * Like the Deluge kind, it keeps small things in memory beside what the layer
 * records: the last state of each torrent it listed, for a state word it does
 * not know, and the waits above.  They are lost on a restart; nothing
 * depends on them for being right, only for being quiet.
 */

#ifndef AXIS_DOWNLOAD_QBITTORRENT_H
#define AXIS_DOWNLOAD_QBITTORRENT_H

#include "download/axis-download-kind.h"

G_BEGIN_DECLS

#define AXIS_DOWNLOAD_QBITTORRENT_NAME "qbittorrent"

/* How long a refused login is not tried again by itself, in seconds. */
#define AXIS_DOWNLOAD_QBITTORRENT_WRONG_HOLD_S  600
#define AXIS_DOWNLOAD_QBITTORRENT_BAN_HOLD_S    900

/* The kind, to register with axis_downloads_add_kind(). */
const AxisDownloadKindInfo *
axis_download_qbittorrent_kind(void);

/* For tests: forgets what the kind remembers of torrents, of the state words
 * it has logged and of refused logins. */
void
axis_download_qbittorrent_forget(void);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_QBITTORRENT_H */

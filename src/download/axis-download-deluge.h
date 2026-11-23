/*
 * axis-download-deluge.h - the Deluge download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Deluge's web interface answers JSON-RPC at "<base>/json": a POST of
 * {"method", "params", "id"}, answered {"result", "error", "id"}.  The web
 * interface is a front end to a daemon it must be connected to, and torrents
 * have no category of their own: Axis's is a label of the Label plugin.
 * Every call is a POST of JSON to that one path, so nothing but the path is
 * in any address; the web password is in the body of the login only, and the
 * session cookie ("_session_id") is kept by the core's in-memory cookie jar
 * and sent to this client's address only.  The kind:
 *
 *   session  auth.login once per connection (the jar keeps the cookie; a
 *           connection that has one is asked auth.check_session before an
 *           add or a removal, which changes nothing).  A call answered "not
 *           authenticated" logs in again and is retried once, but only a
 *           call that changes nothing (the list, the test, the questions
 *           before an add).  An add that lost its session is reported as
 *           not sent and is never retried by the kind.
 *   test    login; web.connected (and, when not connected, web.connect to
 *           the first host web.get_hosts lists, said in the result);
 *           core.get_enabled_plugins must hold Label; label.get_labels, the
 *           label created with label.add when it is not there; the version
 *           (daemon.get_version, else daemon.info)
 *   add     the label is made sure of first, then core.add_torrent_file or
 *           core.add_torrent_magnet; the info-hash it answers must be the
 *           one Axis computed (the magnet's or the file's), else the add
 *           failed and the torrent is not touched; then label.set_torrent,
 *           and when that fails core.remove_torrent (without data) and the
 *           add fails: an unlabelled torrent would be invisible to Axis and
 *           left behind
 *   list    core.get_torrents_status for the label, with the keys needed
 *   remove  core.remove_torrent, with or without data; an error that says
 *           InvalidTorrentError "torrent_id <this hash> not in session."
 *           (seen from Deluge 2.2.0) is %AXIS_DOWNLOAD_ERROR_GONE: Deluge
 *           has no such torrent, something else removed it
 *   folder  (category_folder) the Label plugin's "Move completed to" folder
 *           of the label (label.get_options), when it is on and set, and only
 *           when neither Deluge's own folders for every download
 *           (core.get_config_values of download_location and
 *           move_completed_path alone) nor another label's folder is it or
 *           lies inside it; asked only for a torrent Deluge no longer has
 *           after its import
 *
 * The core's priority is ignored by this kind (Deluge has no simple
 * per-torrent priority at add time), and a Usenet release is refused before
 * any request.  Labels in Deluge are lower case: a label that is not is
 * refused by the test, the add and the list, with that said.
 *
 * What Deluge answers is read by download/axis-download-deluge-parse.h.
 *
 * Like the SABnzbd kind, it keeps one small thing in memory beside what the
 * layer records: the last state of each torrent it listed, for a state word
 * it does not know.  It is lost on a restart; nothing depends on it for being
 * right, only for being quiet.
 */

#ifndef AXIS_DOWNLOAD_DELUGE_H
#define AXIS_DOWNLOAD_DELUGE_H

#include "download/axis-download-kind.h"

G_BEGIN_DECLS

#define AXIS_DOWNLOAD_DELUGE_NAME "deluge"

/* The kind, to register with axis_downloads_add_kind(). */
const AxisDownloadKindInfo *
axis_download_deluge_kind(void);

/* For tests: forgets what the kind remembers of torrents and of the state
 * words it has logged. */
void
axis_download_deluge_forget(void);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_DELUGE_H */

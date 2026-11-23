/*
 * axis-download-sabnzbd.h - the SABnzbd download client kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * SABnzbd is a Usenet downloader with one endpoint, "<base>/api", which
 * takes "mode", "output=json" and "apikey".  This kind makes every call a
 * POST with those in the request body, so the API key is in no address, in
 * no log of requested addresses (ours, a reverse proxy's) and in nothing
 * the core's scrubbing does not already cover.  Its calls:
 *
 *   test    mode=version, then mode=get_cats (the version needs no key at
 *           SABnzbd: the category call is what tells a wrong key, answered
 *           in its JSON, from a client that cannot be reached)
 *   add     mode=addfile, multipart/form-data: the .nzb in the field "name"
 *           under a file name made here, with "cat", "priority" and
 *           "nzbname" (the release's name, which is how a lost answer is
 *           matched later); success is "status" true with exactly one
 *           identifier in "nzo_ids"
 *   list    mode=queue and mode=history, asked for the category, with a
 *           limit
 *   remove  mode=queue&name=delete or mode=history&name=delete, by where
 *           the job was last seen (queue first when that is not known)
 *
 * What SABnzbd answers is read by download/axis-download-sabnzbd-parse.h.
 * A release for another protocol (a magnet link, a torrent) is refused
 * before any request.
 *
 * The kind keeps one small thing in memory beside what the layer records:
 * for each job it listed, its last known state and whether it was in the
 * queue or the history.  A status word it does not know keeps that state
 * (and is logged once per word), and a removal goes to the right list.  It
 * is lost on a restart; nothing depends on it for being right, only for
 * being quiet and quick.
 */

#ifndef AXIS_DOWNLOAD_SABNZBD_H
#define AXIS_DOWNLOAD_SABNZBD_H

#include "download/axis-download-kind.h"

G_BEGIN_DECLS

#define AXIS_DOWNLOAD_SABNZBD_NAME "sabnzbd"

/* The most jobs asked for in one list call (of each of the queue and the
 * history). */
#define AXIS_DOWNLOAD_SABNZBD_LIST_LIMIT 500

/* The kind, to register with axis_downloads_add_kind(). */
const AxisDownloadKindInfo *
axis_download_sabnzbd_kind(void);

/* For tests: forgets what the kind remembers of jobs and of the status
 * words it has logged, and any boundaries set below. */
void
axis_download_sabnzbd_forget(void);

/* For tests: the boundaries (copied, ended by %NULL) the next uploads try
 * first, in order, before any random one; so that a boundary that occurs in
 * a release can be made to be the one tried.  %NULL clears them. */
void
axis_download_sabnzbd_set_boundaries(const gchar *const *boundaries);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_SABNZBD_H */

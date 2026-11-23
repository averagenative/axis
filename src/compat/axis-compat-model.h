/*
 * axis-compat-model.h - what the media server tells us, as plain data
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The media server is outside our control: everything it sends is
 * untrusted.  The parsers (axis-compat-plex.h) turn its answers into these
 * structures, and only these structures cross into the rest of the
 * daemon.  Every field is optional (an unknown number is -1, an unknown
 * text is %NULL), every number is range-checked, every text is valid UTF-8
 * without control characters and cut to a limit, and every list has a
 * maximum length.  A decision word the daemon does not know is
 * %AXIS_COMPAT_DECISION_UNKNOWN, never a guess.
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_COMPAT_MODEL_H
#define AXIS_COMPAT_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* Limits on what is kept from one answer. */
#define AXIS_COMPAT_MAX_LIBRARIES     256
#define AXIS_COMPAT_MAX_FOLDERS       32
#define AXIS_COMPAT_MAX_DEVICES       5000
#define AXIS_COMPAT_MAX_ACCOUNTS      5000
#define AXIS_COMPAT_MAX_SESSIONS      128
#define AXIS_COMPAT_MAX_STREAMS       64
#define AXIS_COMPAT_MAX_HISTORY       1000
#define AXIS_COMPAT_MAX_REASONS       32

/* Text limits, in characters. */
#define AXIS_COMPAT_MAX_ID_CHARS      128
#define AXIS_COMPAT_MAX_NAME_CHARS    128
#define AXIS_COMPAT_MAX_PATH_CHARS    1024
#define AXIS_COMPAT_MAX_TOKEN_CHARS   40
#define AXIS_COMPAT_MAX_REASON_CHARS  64

/**
 * AxisCompatDecision:
 * @AXIS_COMPAT_DECISION_NONE: the server stated nothing
 * @AXIS_COMPAT_DECISION_DIRECT: "directplay": played as it is
 * @AXIS_COMPAT_DECISION_COPY: "copy": passed through into another container
 * @AXIS_COMPAT_DECISION_TRANSCODE: "transcode": converted (a burned-in
 *   subtitle is a conversion too)
 * @AXIS_COMPAT_DECISION_UNKNOWN: a word the daemon does not know
 */
typedef enum
{
	AXIS_COMPAT_DECISION_NONE,
	AXIS_COMPAT_DECISION_DIRECT,
	AXIS_COMPAT_DECISION_COPY,
	AXIS_COMPAT_DECISION_TRANSCODE,
	AXIS_COMPAT_DECISION_UNKNOWN
} AxisCompatDecision;

/* Three-valued: a fact the server did not state is not false. */
typedef enum
{
	AXIS_COMPAT_UNSTATED = -1,
	AXIS_COMPAT_NO = 0,
	AXIS_COMPAT_YES = 1
} AxisCompatTri;

typedef struct
{
	gchar *machine_id;	/* may be NULL */
	gchar *version;		/* may be NULL */
	gchar *name;		/* may be NULL (from the server's root document) */
} AxisCompatIdentity;

typedef struct
{
	gchar     *key;		/* the server's id of the library */
	gchar     *kind;	/* "movie", "show", "artist" or "other" */
	gchar     *title;	/* may be NULL */
	GPtrArray *folders;	/* (element-type utf8): the library's folders */
} AxisCompatLibrary;

typedef struct
{
	gint64 plex_id;		/* the id the history uses, or -1 */
	gchar *client_id;	/* the id sessions use (machineIdentifier) */
	gchar *name;		/* may be NULL */
	gchar *platform;	/* may be NULL */
	gchar *product;		/* the app, may be NULL */
} AxisCompatDevice;

typedef struct
{
	gchar *id;
	gchar *name;		/* may be NULL */
} AxisCompatAccount;

/* A past play: who, what, where and when, and no playback decision. */
typedef struct
{
	gchar  *account_id;	/* may be NULL */
	gint64  device_id;	/* the device's plex_id, or -1 */
	gchar  *library_key;	/* may be NULL */
	gchar  *rating_key;	/* may be NULL */
	gint64  viewed_at;	/* unix seconds, or -1 */
} AxisCompatPlay;

typedef struct
{
	GPtrArray *plays;	/* (element-type AxisCompatPlay) */
	gint64     total;	/* the server's total, or -1 */
} AxisCompatHistory;

/* stream types as the server numbers them */
#define AXIS_COMPAT_STREAM_VIDEO     1
#define AXIS_COMPAT_STREAM_AUDIO     2
#define AXIS_COMPAT_STREAM_SUBTITLE  3

typedef struct
{
	gint               type;		/* 1, 2, 3, or 0 for any other */
	AxisCompatDecision decision;
	gchar             *codec;		/* lower case, or NULL */
	gchar             *profile;		/* lower case, or NULL */
	gint               bit_depth;		/* or -1 */
	gint               width;		/* or -1 */
	gint               height;		/* or -1 */
	gint               channels;		/* or -1 */
	gint64             bitrate_kbps;	/* or -1 */
	AxisCompatTri      selected;
	AxisCompatTri      hdr;			/* from the transfer characteristic */
} AxisCompatStream;

/* What the server states about a conversion in progress. */
typedef struct
{
	gboolean           present;
	AxisCompatDecision video_decision;
	AxisCompatDecision audio_decision;
	AxisCompatDecision subtitle_decision;
	gchar             *source_video_codec;
	gchar             *source_audio_codec;
	gchar             *video_codec;
	gchar             *audio_codec;
	gchar             *container;
	gchar             *protocol;
	gint               width;
	gint               height;
	/* The reasons the server itself states for the conversion, each as it
	 * sent it (cleaned, cut to AXIS_COMPAT_MAX_REASON_CHARS), at most
	 * AXIS_COMPAT_MAX_REASONS of them; %NULL when none is stated (Plex
	 * states none), which is not the same as a conversion for no reason. */
	GPtrArray         *reasons;		/* (element-type utf8) */
} AxisCompatTranscode;

/**
 * AxisCompatSession:
 *
 * One item being played, as one poll saw it.  The media, the part and the
 * streams are those of the one media and one part the item lists; an item
 * that lists more is kept without streams (@n_media_ambiguous), because
 * which one plays is not known.
 */
typedef struct
{
	gchar              *session_key;	/* the server's session id; NULL: cannot be recorded */
	gchar              *rating_key;
	gchar              *library_key;
	gchar              *item_type;
	gchar              *device_client_id;	/* Player.machineIdentifier */
	gchar              *device_name;
	gchar              *platform;
	gchar              *product;
	AxisCompatTri       player_local;
	gchar              *location;		/* "lan", "wan", "cellular" or NULL */
	gint64              bandwidth_kbps;	/* or -1 */
	gchar              *account_id;
	gchar              *account_name;

	AxisCompatTranscode transcode;

	gboolean            ambiguous;		/* more than one media or part */
	AxisCompatDecision  part_decision;
	gchar              *container;		/* the source's container */
	gchar              *file;		/* the path the server states, or NULL */
	gint64              media_bitrate_kbps;	/* or -1 */
	GPtrArray          *streams;		/* (element-type AxisCompatStream) */
} AxisCompatSession;

/* The item detail request: only the file path matters. */
typedef struct
{
	gchar *file;		/* the first part's path, or NULL */
	gint   n_parts;
} AxisCompatItem;

void axis_compat_identity_clear(AxisCompatIdentity *identity);
void axis_compat_library_free(AxisCompatLibrary *library);
void axis_compat_device_free(AxisCompatDevice *device);
void axis_compat_account_free(AxisCompatAccount *account);
void axis_compat_play_free(AxisCompatPlay *play);
void axis_compat_history_clear(AxisCompatHistory *history);
void axis_compat_stream_free(AxisCompatStream *stream);
void axis_compat_session_free(AxisCompatSession *session);
void axis_compat_item_clear(AxisCompatItem *item);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatLibrary, axis_compat_library_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatDevice, axis_compat_device_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatSession, axis_compat_session_free)

AxisCompatDecision
axis_compat_decision_from_string(const gchar *word);

const gchar *
axis_compat_decision_to_string(AxisCompatDecision decision);

G_END_DECLS

#endif /* AXIS_COMPAT_MODEL_H */

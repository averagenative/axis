/*
 * axis-release-private.h - what the parser and its tables share
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_RELEASE_PRIVATE_H
#define AXIS_RELEASE_PRIVATE_H

#include <glib.h>

#include "release/axis-release.h"

G_BEGIN_DECLS

/* What a rule's words say. */
typedef enum
{
	AXIS_RULE_RESOLUTION = 1,	/* value: AxisReleaseResolution; rank: evidence */
	AXIS_RULE_SOURCE,		/* value: AxisReleaseSource; rank: precedence */
	AXIS_RULE_SERVICE,		/* code: the service id */
	AXIS_RULE_CODEC,		/* value: AxisReleaseCodec */
	AXIS_RULE_BITS,			/* value: the bit depth */
	AXIS_RULE_HDR,			/* value: AxisReleaseHdr flags */
	AXIS_RULE_AUDIO,		/* value: AxisReleaseAudio flags; rank 1: also Atmos */
	AXIS_RULE_ATMOS,
	AXIS_RULE_EDITION,		/* value: an edition by itself (Extended, Unrated) */
	AXIS_RULE_EDITION_WORD,		/* value: an edition only before Cut, Edition... */
	AXIS_RULE_EDITION_END,		/* Cut, Edition, Version, Collection */
	AXIS_RULE_LANGUAGE,		/* code: ISO 639-1; value: AxisReleaseLangMark flags too */
	AXIS_RULE_LANG_MARK,		/* value: AxisReleaseLangMark flags */
	AXIS_RULE_REVISION,		/* value: AxisReleaseRevision flags */
	AXIS_RULE_MARK,			/* value: AxisReleaseMark flags */
	AXIS_RULE_STOP,			/* value: AxisReleaseStop flags */
	AXIS_RULE_COMPLETE
} AxisRuleField;

/* Resolution rule value: the resolution, with this bit for interlaced. */
#define AXIS_RULE_INTERLACED 0x100

/*
 * One rule: one to four words, separated by single spaces, compared with
 * the tokens without regard to ASCII case.  A hard rule is only ever a
 * quality word.
 */
typedef struct
{
	const gchar *words;
	guint8       field;
	guint8       hard;
	guint8       rank;
	guint32      value;
	const gchar *code;
} AxisReleaseRule;

/**
 * axis_release_rule_at:
 * @tokens: the tokens
 * @n: how many
 * @i: where to look
 * @span: (out): how many tokens the rule covers
 *
 * Returns: (nullable): the rule with the most words that matches the
 *   tokens from @i, or %NULL
 */
const AxisReleaseRule *
axis_release_rule_at(
	const gchar *const *tokens,
	guint               n,
	guint               i,
	guint              *span
);

/* The one-word rule that is exactly @word, or %NULL. */
const AxisReleaseRule *
axis_release_rule_word(const gchar *word);

/* The longest one-word audio rule that @word starts with, or %NULL;
 * *@length is its length. */
const AxisReleaseRule *
axis_release_rule_audio_prefix(
	const gchar *word,
	gsize       *length
);

/* What a file extension (without its dot) is: %TRUE if it is one to take
 * off, with the stop mark it sets in *@stop (0 for none). */
gboolean
axis_release_extension(
	const gchar *extension,
	gsize        length,
	guint       *stop
);

/* Whether @label (@length bytes) is a top-level domain that sites use. */
gboolean
axis_release_tld(
	const gchar *label,
	gsize        length
);

/* Whether @word is what a reposter appends after the group ("xpost"). */
gboolean
axis_release_repost_suffix(const gchar *word);

/* Whether @word (@length bytes) is a tracker's or an index's tag that it
 * writes into the names it lists ("TGx", "eztv", "rarbg"): never a group. */
gboolean
axis_release_site_tag(
	const gchar *word,
	gsize        length
);

/* Whether @word is an English article. */
gboolean
axis_release_article(const gchar *word);

G_END_DECLS

#endif /* AXIS_RELEASE_PRIVATE_H */

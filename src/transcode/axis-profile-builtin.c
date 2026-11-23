/*
 * axis-profile-builtin.c - the built-in profiles, and the one function that
 * makes a target from a definition
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The built-in definitions are written here in canonical form; the daemon
 * writes them to the database at start-up (axis-profile-store.c), adding a
 * revision when one changes, and migration 0014 holds the same text as
 * each one's first revision.  A change here is a new revision of that
 * built-in on every installation, and a new revision is locked: change one
 * only on purpose.
 */

#include "transcode/axis-profile.h"

#include <string.h>

/* ── The built-ins ──────────────────────────────────────────────── */

#define FOUR_K_RULES \
	"[{\"crf\":24,\"max_bytes\":20000000000,\"resolution\":\"2160p\"}," \
	"{\"crf\":28,\"min_bytes\":50000000000,\"resolution\":\"2160p\"}]"

static const AxisProfileBuiltin builtins[AXIS_PROFILE_N_BUILTINS] = {
	{
		"space_saver", "Space saver", "Video",
		"HEVC 10-bit with the original audio and subtitles kept: the largest saving, for reasonably "
		"modern players.",
		"{\"audio\":\"copy\",\"bit_depth\":10,\"codec\":\"hevc\",\"container\":\"mkv\",\"efficient_bpp_milli\":150,"
		"\"encoders\":[\"software\",\"nvenc\",\"vaapi\"],\"format\":1,\"hdr\":\"keep\",\"keep_languages\":[],"
		"\"kind\":\"video\",\"libraries\":[\"movies\",\"tv\",\"other\"],\"min_saving_bytes\":0,"
		"\"min_saving_percent\":15,\"quality\":{\"default\":26,\"rules\":" FOUR_K_RULES "},\"subtitles\":\"copy\"}"
	},
	{
		"balanced", "Balanced", "Video",
		"HEVC 8-bit for SDR sources (10-bit for HDR ones), the original audio kept and a stereo AAC "
		"track added, so that phones and simple TVs always have something they can play.",
		"{\"audio\":\"copy_add_stereo\",\"bit_depth\":8,\"codec\":\"hevc\",\"container\":\"mkv\","
		"\"efficient_bpp_milli\":150,\"encoders\":[\"software\",\"nvenc\",\"vaapi\"],\"format\":1,\"hdr\":\"keep\","
		"\"keep_languages\":[],\"kind\":\"video\",\"libraries\":[\"movies\",\"tv\",\"other\"],\"min_saving_bytes\":0,"
		"\"min_saving_percent\":15,\"quality\":{\"default\":26,\"rules\":" FOUR_K_RULES "},\"stereo_bitrate\":160000,"
		"\"subtitles\":\"copy\"}"
	},
	{
		"max_compat", "Maximum compatibility", "Video",
		"H.264 8-bit SDR in MP4, a stereo AAC track first with the original kept after it, plain text "
		"subtitles: plays on essentially anything, saves much less. HEVC and HDR sources are skipped.",
		"{\"audio\":\"stereo_first\",\"bit_depth\":8,\"codec\":\"h264\",\"container\":\"mp4\","
		"\"efficient_bpp_milli\":120,\"encoders\":[\"software\",\"nvenc\",\"vaapi\"],\"format\":1,\"hdr\":\"skip\","
		"\"keep_languages\":[],\"kind\":\"video\",\"libraries\":[\"movies\",\"tv\",\"other\"],\"min_saving_bytes\":0,"
		"\"min_saving_percent\":15,\"quality\":{\"default\":20,\"rules\":[]},\"stereo_bitrate\":160000,"
		"\"subtitles\":\"text_only\"}"
	},
	{
		"keep_lossless", "Keep lossless", "Music",
		"Lossless stays lossless: FLAC at its highest compression level, from FLAC, ALAC, WAV and the "
		"like. A modest saving, nothing lost.",
		"{\"codec\":\"flac\",\"format\":1,\"kind\":\"audio\",\"libraries\":[\"music\"],\"min_saving_bytes\":0,"
		"\"min_saving_percent\":10,\"reduce_to_cd\":false}"
	},
	{
		"music_space_saver", "Space saver", "Music",
		"Lossless albums to Opus at 128 kbit/s: roughly a quarter of the size and transparent to most "
		"listeners, but not every player handles Opus, and it cannot be undone.",
		"{\"allow_lossy_source\":false,\"bitrate\":128000,\"codec\":\"opus\",\"efficient_percent\":125,\"format\":1,"
		"\"kind\":\"audio\",\"libraries\":[\"music\"],\"min_saving_bytes\":0,\"min_saving_percent\":10}"
	},
	{
		"music_compat", "Maximum compatibility", "Music",
		"Lossless albums to MP3 (LAME V0): plays on phones, car stereos and anything else, and cannot "
		"be undone.",
		"{\"allow_lossy_source\":false,\"codec\":\"mp3\",\"efficient_percent\":125,\"format\":1,\"kind\":\"audio\","
		"\"libraries\":[\"music\"],\"min_saving_bytes\":0,\"min_saving_percent\":10,\"vbr_quality\":0}"
	},
	{
		"audiobooks", "Audiobooks", "Audiobooks",
		"Speech-tuned Opus at 48 kbit/s (32 for a mono source). Lossy sources are allowed when they are "
		"well above that; chapters are kept.",
		"{\"allow_lossy_source\":true,\"bitrate\":48000,\"codec\":\"opus\",\"efficient_percent\":125,\"format\":1,"
		"\"kind\":\"audio\",\"libraries\":[\"audiobooks\"],\"min_saving_bytes\":0,\"min_saving_percent\":10,"
		"\"mono_bitrate\":32000}"
	}
};

const AxisProfileBuiltin *
axis_profile_builtins(void)
{
	return builtins;
}

const AxisProfileBuiltin *
axis_profile_builtin(const gchar *slug)
{
	guint i;

	for (i = 0; slug != NULL && i < AXIS_PROFILE_N_BUILTINS; i++) {
		if (strcmp(builtins[i].slug, slug) == 0)
			return &builtins[i];
	}
	return NULL;
}

gboolean
axis_profile_slug_reserved(const gchar *slug)
{
	return axis_profile_builtin(slug) != NULL;
}

/* ── Targets ────────────────────────────────────────────────────── */

const gchar *
axis_profile_family(const AxisProfileDefinition *def)
{
	if (def->kind == AXIS_PROFILE_KIND_VIDEO)
		return "Video";
	if (def->libraries == AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS))
		return "Audiobooks";
	return "Music";
}

/* A language list as a vector the target can keep: one per distinct list,
 * kept for the life of the process (the lists are short and few). */
static const gchar *const *
language_vector(const AxisProfileDefinition *def)
{
	static GMutex lock;
	static GHashTable *vectors;
	GString *key;
	const gchar **vector;
	guint i;

	if (def->n_languages == 0)
		return NULL;
	key = g_string_new(NULL);
	for (i = 0; i < def->n_languages && i < AXIS_PROFILE_MAX_LANGUAGES; i++)
		g_string_append_printf(key, "%s%s", i > 0 ? "," : "", def->languages[i]);
	g_mutex_lock(&lock);
	if (vectors == NULL)
		vectors = g_hash_table_new(g_str_hash, g_str_equal);
	vector = g_hash_table_lookup(vectors, key->str);
	if (vector == NULL) {
		vector = g_new0(const gchar *, def->n_languages + 1);
		for (i = 0; i < def->n_languages && i < AXIS_PROFILE_MAX_LANGUAGES; i++)
			vector[i] = g_intern_string(def->languages[i]);
		g_hash_table_insert(vectors, (gpointer)g_intern_string(key->str), vector);
	}
	g_mutex_unlock(&lock);
	g_string_free(key, TRUE);
	return vector;
}

gboolean
axis_profile_to_target(
	const AxisProfileDefinition *def,
	const AxisProfileIdentity   *identity,
	AxisEstimateTarget          *target
){
	const gchar *const *names = axis_profile_resolutions();
	guint i;

	g_return_val_if_fail(def != NULL && target != NULL, FALSE);

	memset(target, 0, sizeof(*target));
	/* Fail closed: nothing is made of a definition that does not check. */
	if (!axis_profile_definition_check(def, NULL))
		return FALSE;

	target->id = g_intern_string(identity != NULL && identity->slug != NULL ? identity->slug : "profile");
	target->name = g_intern_string(identity != NULL && identity->name != NULL ? identity->name : "Profile");
	target->family_name = g_intern_string(identity != NULL && identity->family != NULL ? identity->family :
		axis_profile_family(def));
	target->summary = g_intern_string(identity != NULL && identity->summary != NULL ? identity->summary : "");
	target->revision_id = identity != NULL ? identity->revision_id : 0;
	target->revision = identity != NULL ? identity->revision : 0;
	target->root_types = def->libraries;
	target->codec = def->codec;
	target->min_saving = (gdouble)def->min_saving_percent / 100.0;
	target->min_saving_bytes = def->min_saving_bytes;
	target->audio_quality = -1;

	if (def->kind == AXIS_PROFILE_KIND_VIDEO) {
		target->media = AXIS_TARGET_VIDEO;
		target->profile = def->codec == AXIS_TCODEC_H264 ? "High" : def->bit_depth == 10 ? "Main 10" : "Main";
		target->bit_depth = def->bit_depth;
		target->hdr_bit_depth = def->keep_hdr ? 10 : 0;
		for (i = 0; i < def->n_encoders; i++)
			target->encoders[i] = def->encoders[i];
		target->n_encoders = def->n_encoders;
		target->encoder = def->encoders[0];
		target->quality = axis_estimate_quality_level(def->codec, def->crf);
		for (i = 0; i < def->n_rules; i++) {
			AxisEstimateQualityRule *rule = &target->quality_rules[i];

			rule->resolution = def->rules[i].resolution > 0 ? names[def->rules[i].resolution - 1] : NULL;
			rule->min_bytes = def->rules[i].min_bytes;
			rule->max_bytes = def->rules[i].max_bytes;
			rule->level = axis_estimate_quality_level(def->codec, def->rules[i].crf);
		}
		target->n_quality_rules = def->n_rules;
		target->audio = def->audio;
		target->audio_bitrate = def->stereo_bitrate;
		target->keep_audio_languages = language_vector(def);
		target->subtitles = def->subtitles;
		target->container = def->container;
		target->efficient_milli = def->efficient_milli;
		return TRUE;
	}

	target->media = AXIS_TARGET_AUDIO;
	target->encoder = AXIS_ENCODER_SOFTWARE;
	/* FLAC is written at compression level 8 (quality level 4); the lossy
	 * codecs take a bitrate, not a level. */
	target->quality = 4;
	target->audio = AXIS_AUDIO_CONVERT;
	target->subtitles = AXIS_SUBTITLES_NONE;
	target->allow_lossy_source = def->allow_lossy;
	target->reduce_to_cd = def->reduce_to_cd;
	switch (def->codec) {
	case AXIS_TCODEC_FLAC:
		target->container = AXIS_CONTAINER_FLAC;
		target->efficient_milli = 1000;
		break;
	case AXIS_TCODEC_MP3:
		target->container = AXIS_CONTAINER_MP3;
		target->audio_quality = def->vbr_quality;
		target->audio_bitrate = axis_profile_mp3_bitrate(def->vbr_quality);
		target->efficient_milli = (gint64)def->efficient_percent * 10;
		break;
	case AXIS_TCODEC_AAC:
		target->container = AXIS_CONTAINER_MP4;
		target->audio_bitrate = def->bitrate;
		target->audio_bitrate_mono = def->mono_bitrate;
		target->efficient_milli = (gint64)def->efficient_percent * 10;
		break;
	default:
		target->container = AXIS_CONTAINER_OGG;
		target->audio_bitrate = def->bitrate;
		target->audio_bitrate_mono = def->mono_bitrate;
		target->efficient_milli = (gint64)def->efficient_percent * 10;
		break;
	}
	return TRUE;
}

/* ── In words ───────────────────────────────────────────────────── */

static void
fact(
	GPtrArray   *facts,
	const gchar *label,
	gchar       *text
){
	AxisProfileFact *f = g_new0(AxisProfileFact, 1);

	f->label = label;
	f->text = text;
	g_ptr_array_add(facts, f);
}

static void
fact_free(gpointer data)
{
	AxisProfileFact *f = data;

	g_free(f->text);
	g_free(f);
}

static gchar *
size_words(gint64 bytes)
{
	if (bytes % 1000000000 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " GB", bytes / 1000000000);
	if (bytes % 1000000 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " MB", bytes / 1000000);
	return g_strdup_printf("%" G_GINT64_FORMAT " bytes", bytes);
}

static gchar *
libraries_words(const AxisProfileDefinition *def)
{
	static const struct
	{
		AxisMediaType  type;
		const gchar   *words;
	} names[] = {
		{ AXIS_MEDIA_MOVIES, "movies" }, { AXIS_MEDIA_TV, "TV" }, { AXIS_MEDIA_OTHER, "other video" },
		{ AXIS_MEDIA_MUSIC, "music" }, { AXIS_MEDIA_AUDIOBOOKS, "audiobooks" }
	};
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		if ((def->libraries & AXIS_TARGET_ROOT(names[i].type)) != 0)
			g_string_append_printf(out, "%s%s", out->len > 0 ? ", " : "", names[i].words);
	}
	return g_string_free(out, FALSE);
}

static gchar *
quality_words(const AxisProfileDefinition *def)
{
	const gchar *const *names = axis_profile_resolutions();
	GString *out = g_string_new(NULL);
	guint i;

	g_string_append_printf(out, "CRF %d", def->crf);
	for (i = 0; i < def->n_rules; i++) {
		const AxisProfileRule *r = &def->rules[i];

		g_string_append_printf(out, "%s CRF %d for ", i == 0 ? "; but, the first that applies:" : ";", r->crf);
		g_string_append(out, r->resolution > 0 ? names[r->resolution - 1] : "any");
		g_string_append(out, " sources");
		if (r->min_bytes > 0) {
			g_autofree gchar *size = size_words(r->min_bytes);

			g_string_append_printf(out, " of %s or more", size);
		}
		if (r->max_bytes > 0) {
			g_autofree gchar *size = size_words(r->max_bytes);

			g_string_append_printf(out, "%s under %s", r->min_bytes > 0 ? " and" : "", size);
		}
	}
	return g_string_free(out, FALSE);
}

GPtrArray *
axis_profile_describe(const AxisProfileDefinition *def)
{
	GPtrArray *facts = g_ptr_array_new_with_free_func(fact_free);
	guint i;

	fact(facts, "Libraries it is for", libraries_words(def));
	if (def->kind == AXIS_PROFILE_KIND_VIDEO) {
		GString *encoders = g_string_new(NULL);

		fact(facts, "Video", g_strdup_printf("%s %s, %d-bit", axis_target_codec_label(def->codec),
			def->codec == AXIS_TCODEC_H264 ? "High" : def->bit_depth == 10 ? "Main 10" : "Main", def->bit_depth));
		fact(facts, "HDR", g_strdup(def->keep_hdr ? "HDR10 and HLG sources become 10-bit HEVC with their metadata; "
			"Dolby Vision and HDR10+ are never converted" : "SDR only: HDR sources are not converted (never "
			"tone-mapped)"));
		/* The shared label names the audio encoders too; a video profile's
		 * software encoder is the codec's own. */
		for (i = 0; i < def->n_encoders; i++)
			g_string_append_printf(encoders, "%s%s", i > 0 ? ", then " : "",
				def->encoders[i] != AXIS_ENCODER_SOFTWARE ? axis_encoder_family_label(def->encoders[i]) :
				def->codec == AXIS_TCODEC_H264 ? "software (x264)" : "software (x265)");
		fact(facts, "Encoders", g_string_free(encoders, FALSE));
		fact(facts, "Quality", quality_words(def));
		switch (def->audio) {
		case AXIS_AUDIO_COPY_ADD_STEREO:
			fact(facts, "Audio", g_strdup_printf("every track copied, and a stereo AAC track at %d kbit/s added "
				"unless there is one", def->stereo_bitrate / 1000));
			break;
		case AXIS_AUDIO_STEREO_FIRST:
			fact(facts, "Audio", g_strdup_printf("a stereo AAC track at %d kbit/s first, the original kept after it",
				def->stereo_bitrate / 1000));
			break;
		default:
			fact(facts, "Audio", g_strdup("every track copied, never re-encoded"));
			break;
		}
		if (def->n_languages > 0) {
			GString *list = g_string_new("only ");

			for (i = 0; i < def->n_languages; i++)
				g_string_append_printf(list, "%s%s", i > 0 ? ", " : "", def->languages[i]);
			g_string_append(list, " (and tracks of unknown language); a file that would be left without sound "
				"is not converted");
			fact(facts, "Audio languages", g_string_free(list, FALSE));
		} else {
			fact(facts, "Audio languages", g_strdup("every track kept"));
		}
		fact(facts, "Subtitles", g_strdup(def->subtitles == AXIS_SUBTITLES_PLAIN_TEXT ? "plain text only: a file "
			"with image or styled subtitles is not converted" : "every track copied"));
		fact(facts, "Container", g_strdup(def->container == AXIS_CONTAINER_MP4 ? "MP4" : "Matroska"));
		fact(facts, "Already efficient", g_strdup_printf("%s at or below %d.%03d bits per pixel",
			axis_target_codec_label(def->codec), def->efficient_milli / 1000, def->efficient_milli % 1000));
	} else {
		switch (def->codec) {
		case AXIS_TCODEC_FLAC:
			fact(facts, "Audio", g_strdup_printf("FLAC at compression level 8%s", def->reduce_to_cd ?
				", more than 16 bits or 48 kHz reduced to 16 bits at 44.1 kHz" : ", same bits and rate"));
			break;
		case AXIS_TCODEC_MP3:
			fact(facts, "Audio", g_strdup_printf("MP3, LAME V%d (about %d kbit/s)", def->vbr_quality,
				axis_profile_mp3_bitrate(def->vbr_quality) / 1000));
			break;
		default:
			if (def->mono_bitrate > 0)
				fact(facts, "Audio", g_strdup_printf("%s at %d kbit/s (%d for a mono source)",
					axis_target_codec_label(def->codec), def->bitrate / 1000, def->mono_bitrate / 1000));
			else
				fact(facts, "Audio", g_strdup_printf("%s at %d kbit/s", axis_target_codec_label(def->codec),
					def->bitrate / 1000));
			break;
		}
		fact(facts, "Sources", g_strdup(def->codec == AXIS_TCODEC_FLAC ? "lossless only" : def->allow_lossy ?
			"lossless, and lossy ones well above the bitrate" : "lossless only: never lossy to lossy"));
		if (def->codec != AXIS_TCODEC_FLAC)
			fact(facts, "Already efficient", g_strdup_printf("%s at or below %d%% of the bitrate",
				axis_target_codec_label(def->codec), def->efficient_percent));
		fact(facts, "Albums", g_strdup("converted whole, never half; tags and cover art carried over"));
	}
	if (def->min_saving_bytes > 0) {
		g_autofree gchar *bytes = size_words(def->min_saving_bytes);

		fact(facts, "Minimum saving", g_strdup_printf("%d%% of a file's size (an album's, for audio), and at "
			"least %s", def->min_saving_percent, bytes));
	} else {
		fact(facts, "Minimum saving", g_strdup_printf("%d%% of a file's size (an album's, for audio)",
			def->min_saving_percent));
	}
	return facts;
}

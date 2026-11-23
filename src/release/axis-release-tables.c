/*
 * axis-release-tables.c - the fixed tables a release name is read with
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A word is hard when it is only ever a quality word, and soft when it is
 * also an ordinary word or a mark that names put before the year (see
 * axis-release.h for what that changes).  Every rule here is a fact about
 * how release names are written; none of them guesses.
 */

#include "release/axis-release-private.h"

#include <string.h>

#define HARD 1
#define SOFT 0

#define RULE(w, f, h, rk, v, c) { (w), (f), (h), (rk), (v), (c) }

#define RES(w, h, rk, v)  RULE(w, AXIS_RULE_RESOLUTION, h, rk, v, NULL)
#define SRC(w, h, rk, v)  RULE(w, AXIS_RULE_SOURCE, h, rk, AXIS_RELEASE_SOURCE_##v, NULL)
#define SVC(w, h, c)      RULE(w, AXIS_RULE_SERVICE, h, 0, 0, c)
#define VID(w, v)         RULE(w, AXIS_RULE_CODEC, HARD, 0, AXIS_RELEASE_CODEC_##v, NULL)
#define BITS(w, v)        RULE(w, AXIS_RULE_BITS, HARD, 0, v, NULL)
#define HDR(w, h, v)      RULE(w, AXIS_RULE_HDR, h, 0, AXIS_RELEASE_HDR_##v, NULL)
#define AUD(w, h, v)      RULE(w, AXIS_RULE_AUDIO, h, 0, AXIS_RELEASE_AUDIO_##v, NULL)
#define AUDA(w, v)        RULE(w, AXIS_RULE_AUDIO, HARD, 1, AXIS_RELEASE_AUDIO_##v, NULL)
#define ED(w, v)          RULE(w, AXIS_RULE_EDITION, SOFT, 0, AXIS_RELEASE_EDITION_##v, NULL)
#define EDW(w, v)         RULE(w, AXIS_RULE_EDITION_WORD, SOFT, 0, AXIS_RELEASE_EDITION_##v, NULL)
#define EDE(w)            RULE(w, AXIS_RULE_EDITION_END, SOFT, 0, 0, NULL)
#define LANG(w, c)        RULE(w, AXIS_RULE_LANGUAGE, SOFT, 0, 0, c)
#define LANGM(w, c, v)    RULE(w, AXIS_RULE_LANGUAGE, SOFT, 0, AXIS_RELEASE_LANG_##v, c)
#define LMK(w, v)         RULE(w, AXIS_RULE_LANG_MARK, SOFT, 0, AXIS_RELEASE_LANG_##v, NULL)
#define REV(w, v)         RULE(w, AXIS_RULE_REVISION, SOFT, 0, AXIS_RELEASE_REVISION_##v, NULL)
#define MARK(w, v)        RULE(w, AXIS_RULE_MARK, SOFT, 0, AXIS_RELEASE_MARK_##v, NULL)
#define STOP(w, v)        RULE(w, AXIS_RULE_STOP, SOFT, 0, AXIS_RELEASE_STOP_##v, NULL)

static const AxisReleaseRule rules[] = {
	/* Resolution.  An explicit 1080p (rank 3) wins over 4K (1) and UHD
	 * (0); "1920x1080" is read in the parser (rank 2). */
	RES("480p",  HARD, 3, AXIS_RELEASE_RESOLUTION_480P),
	RES("576p",  HARD, 3, AXIS_RELEASE_RESOLUTION_576P),
	RES("720p",  HARD, 3, AXIS_RELEASE_RESOLUTION_720P),
	RES("1080p", HARD, 3, AXIS_RELEASE_RESOLUTION_1080P),
	RES("2160p", HARD, 3, AXIS_RELEASE_RESOLUTION_2160P),
	RES("480i",  HARD, 3, AXIS_RELEASE_RESOLUTION_480P | AXIS_RULE_INTERLACED),
	RES("576i",  HARD, 3, AXIS_RELEASE_RESOLUTION_576P | AXIS_RULE_INTERLACED),
	RES("1080i", HARD, 3, AXIS_RELEASE_RESOLUTION_1080P | AXIS_RULE_INTERLACED),
	RES("4k",    HARD, 1, AXIS_RELEASE_RESOLUTION_2160P),
	RES("uhd",   HARD, 0, AXIS_RELEASE_RESOLUTION_2160P),

	/* Source.  Rank is precedence when a name states two: a recording
	 * mark (9) always shows; remux and full disc (5) over the disc or
	 * stream they came from; a named WEB-DL or WEBRip (2) over bare WEB. */
	SRC("cam",          SOFT, 9, CAM),
	SRC("camrip",       HARD, 9, CAM),
	SRC("cam-rip",      HARD, 9, CAM),
	SRC("hdcam",        HARD, 9, CAM),
	SRC("hd-cam",       HARD, 9, CAM),
	SRC("hqcam",        HARD, 9, CAM),
	SRC("ts",           SOFT, 9, TELESYNC),
	SRC("telesync",     HARD, 9, TELESYNC),
	SRC("hdts",         HARD, 9, TELESYNC),
	SRC("hd-ts",        HARD, 9, TELESYNC),
	SRC("tsrip",        HARD, 9, TELESYNC),
	SRC("ts-rip",       HARD, 9, TELESYNC),
	SRC("tc",           SOFT, 9, TELECINE),
	SRC("telecine",     HARD, 9, TELECINE),
	SRC("hdtc",         HARD, 9, TELECINE),
	SRC("hd-tc",        HARD, 9, TELECINE),
	SRC("scr",          SOFT, 9, SCREENER),
	SRC("screener",     SOFT, 9, SCREENER),
	SRC("dvdscr",       HARD, 9, SCREENER),
	SRC("dvd-scr",      HARD, 9, SCREENER),
	SRC("dvdscreener",  HARD, 9, SCREENER),
	SRC("bdscr",        HARD, 9, SCREENER),
	SRC("webscr",       HARD, 9, SCREENER),
	SRC("workprint",    SOFT, 9, WORKPRINT),
	SRC("wp",           SOFT, 9, WORKPRINT),
	SRC("dvd",          HARD, 1, DVD),
	SRC("dvdrip",       HARD, 1, DVD),
	SRC("dvd-rip",      HARD, 1, DVD),
	SRC("hdtv",         HARD, 1, HDTV),
	SRC("hdtvrip",      HARD, 1, HDTV),
	SRC("hdtv-rip",     HARD, 1, HDTV),
	SRC("ahdtv",        HARD, 1, HDTV),
	SRC("pdtv",         HARD, 1, HDTV),
	SRC("sdtv",         HARD, 1, HDTV),
	SRC("tvrip",        HARD, 1, HDTV),
	SRC("tv-rip",       HARD, 1, HDTV),
	SRC("dsr",          HARD, 1, HDTV),
	SRC("dsrip",        HARD, 1, HDTV),
	SRC("satrip",       HARD, 1, HDTV),
	SRC("dthrip",       HARD, 1, HDTV),
	SRC("dvbrip",       HARD, 1, HDTV),
	SRC("webrip",       HARD, 2, WEBRIP),
	SRC("web-rip",      HARD, 2, WEBRIP),
	SRC("web rip",      HARD, 2, WEBRIP),
	SRC("webmux",       HARD, 2, WEBRIP),
	SRC("web-dl",       HARD, 2, WEBDL),
	SRC("webdl",        HARD, 2, WEBDL),
	SRC("web dl",       HARD, 2, WEBDL),
	SRC("web",          SOFT, 1, WEBDL),
	SRC("bluray",       HARD, 1, BLURAY),
	SRC("blu-ray",      HARD, 1, BLURAY),
	SRC("blu ray",      HARD, 1, BLURAY),
	SRC("blurayrip",    HARD, 1, BLURAY),
	SRC("bluray-rip",   HARD, 1, BLURAY),
	SRC("bdrip",        HARD, 1, BLURAY),
	SRC("bd-rip",       HARD, 1, BLURAY),
	SRC("brrip",        HARD, 1, BLURAY),
	SRC("br-rip",       HARD, 1, BLURAY),
	SRC("bdmux",        HARD, 1, BLURAY),
	SRC("bd",           SOFT, 1, BLURAY),
	SRC("br",           SOFT, 1, BLURAY),
	SRC("remux",        HARD, 5, REMUX),
	SRC("bdremux",      HARD, 5, REMUX),
	SRC("bd-remux",     HARD, 5, REMUX),
	SRC("dvdremux",     HARD, 5, REMUX),
	SRC("complete bluray", HARD, 5, DISC),
	SRC("complete uhd bluray", HARD, 5, DISC),
	SRC("bd25",         HARD, 5, DISC),
	SRC("bd50",         HARD, 5, DISC),
	SRC("bd66",         HARD, 5, DISC),
	SRC("bd100",        HARD, 5, DISC),
	SRC("bdmv",         HARD, 5, DISC),
	SRC("br-disk",      HARD, 5, DISC),
	SRC("dvd5",         HARD, 5, DISC),
	SRC("dvd9",         HARD, 5, DISC),
	SRC("dvdr",         HARD, 5, DISC),
	SRC("dvd-r",        HARD, 5, DISC),
	SRC("iso",          SOFT, 5, DISC),

	/* Streaming services: the id is what is stored. */
	SVC("amzn",   HARD, "amazon"),
	SVC("amazon", SOFT, "amazon"),
	SVC("nf",     HARD, "netflix"),
	SVC("nflx",   HARD, "netflix"),
	SVC("netflix", SOFT, "netflix"),
	SVC("dsnp",   HARD, "disney"),
	SVC("dsny",   HARD, "disney"),
	SVC("hmax",   HARD, "hbomax"),
	SVC("max",    SOFT, "max"),
	SVC("atvp",   HARD, "apple"),
	SVC("aptv",   HARD, "apple"),
	SVC("hulu",   HARD, "hulu"),
	SVC("pcok",   HARD, "peacock"),
	SVC("pmtp",   HARD, "paramount"),
	SVC("pmnt",   HARD, "paramount"),
	SVC("stan",   SOFT, "stan"),
	SVC("crav",   HARD, "crave"),
	SVC("cr",     SOFT, "crunchyroll"),
	SVC("funi",   HARD, "funimation"),
	SVC("hidive", HARD, "hidive"),
	SVC("roku",   SOFT, "roku"),
	SVC("tubi",   SOFT, "tubi"),
	SVC("sho",    SOFT, "showtime"),
	SVC("stz",    HARD, "starz"),
	SVC("ip",     SOFT, "iplayer"),
	SVC("all4",   HARD, "all4"),
	SVC("now",    SOFT, "now"),
	SVC("dscp",   HARD, "discovery"),
	SVC("amc",    SOFT, "amc"),
	SVC("ma",     SOFT, "moviesanywhere"),
	SVC("vudu",   HARD, "vudu"),
	SVC("crkl",   HARD, "crackle"),
	SVC("bcore",  HARD, "braviacore"),
	SVC("dcu",    HARD, "dcuniverse"),
	SVC("mubi",   SOFT, "mubi"),
	SVC("shdr",   HARD, "shudder"),
	SVC("cc",     SOFT, "comedycentral"),
	SVC("nick",   SOFT, "nickelodeon"),
	SVC("pbs",    SOFT, "pbs"),
	SVC("itvx",   HARD, "itv"),
	SVC("skst",   HARD, "skyshowtime"),
	SVC("joyn",   SOFT, "joyn"),
	SVC("zee5",   HARD, "zee5"),
	SVC("hotstar", SOFT, "hotstar"),

	/* Video codecs. */
	VID("x264",   H264),
	VID("h264",   H264),
	VID("h.264",  H264),
	VID("h-264",  H264),
	VID("x-264",  H264),
	VID("h 264",  H264),
	VID("avc",    H264),
	VID("x265",   H265),
	VID("h265",   H265),
	VID("h.265",  H265),
	VID("h-265",  H265),
	VID("x-265",  H265),
	VID("h 265",  H265),
	VID("hevc",   H265),
	VID("av1",    AV1),
	VID("vc1",    VC1),
	VID("vc-1",   VC1),
	VID("mpeg2",  MPEG2),
	VID("mpeg-2", MPEG2),
	VID("xvid",   XVID),
	VID("divx",   XVID),
	VID("vp9",    VP9),

	/* Bit depth. */
	BITS("8bit",   8),
	BITS("8-bit",  8),
	BITS("8 bit",  8),
	BITS("10bit",  10),
	BITS("10-bit", 10),
	BITS("10bits", 10),
	BITS("10 bit", 10),
	BITS("hi10p",  10),
	BITS("hi10",   10),
	BITS("12bit",  12),
	BITS("12-bit", 12),
	BITS("12 bit", 12),

	/* HDR.  "HDR" alone is not HDR10: the kind is not stated. */
	HDR("hdr",          HARD, HDR),
	HDR("hdr10",        HARD, HDR10),
	HDR("hdr10+",       HARD, HDR10PLUS),
	HDR("hdr10plus",    HARD, HDR10PLUS),
	HDR("dv",           SOFT, DOLBY_VISION),
	HDR("dovi",         HARD, DOLBY_VISION),
	HDR("dolbyvision",  HARD, DOLBY_VISION),
	HDR("dolby vision", HARD, DOLBY_VISION),
	HDR("dolby-vision", HARD, DOLBY_VISION),
	HDR("hlg",          HARD, HLG),
	HDR("sdr",          HARD, SDR),

	/* Audio.  DDPA is DD+ with Atmos. */
	AUD("truehd",     HARD, TRUEHD),
	AUD("true-hd",    HARD, TRUEHD),
	AUD("dts-x",      HARD, DTS_X),
	AUD("dts:x",      HARD, DTS_X),
	AUD("dtsx",       HARD, DTS_X),
	AUD("dts-hd ma",  HARD, DTS_HD_MA),
	AUD("dts hd ma",  HARD, DTS_HD_MA),
	AUD("dts-hd-ma",  HARD, DTS_HD_MA),
	AUD("dts-hdma",   HARD, DTS_HD_MA),
	AUD("dtshd-ma",   HARD, DTS_HD_MA),
	AUD("dtshdma",    HARD, DTS_HD_MA),
	AUD("dtsma",      HARD, DTS_HD_MA),
	AUD("dts-ma",     HARD, DTS_HD_MA),
	AUD("dts ma",     HARD, DTS_HD_MA),
	AUD("dts-hd",     HARD, DTS_HD),
	AUD("dts hd",     HARD, DTS_HD),
	AUD("dtshd",      HARD, DTS_HD),
	AUD("dts-hd hra", HARD, DTS_HD),
	AUD("dts-hdhra",  HARD, DTS_HD),
	AUD("dts-hr",     HARD, DTS_HD),
	AUD("dts",        HARD, DTS),
	AUD("dts-es",     HARD, DTS),
	AUD("ddp",        HARD, EAC3),
	AUD("dd+",        HARD, EAC3),
	AUD("eac3",       HARD, EAC3),
	AUD("e-ac-3",     HARD, EAC3),
	AUD("e-ac3",      HARD, EAC3),
	AUDA("ddpa",      EAC3),
	AUD("dd",         SOFT, AC3),
	AUD("ac3",        HARD, AC3),
	AUD("ac3d",       HARD, AC3),
	AUD("ac-3",       HARD, AC3),
	AUD("dd-ex",      HARD, AC3),
	AUD("aac",        HARD, AAC),
	AUD("he-aac",     HARD, AAC),
	AUD("aac-lc",     HARD, AAC),
	AUD("flac",       HARD, FLAC),
	AUD("opus",       SOFT, OPUS),
	AUD("mp3",        HARD, MP3),
	AUD("pcm",        HARD, PCM),
	AUD("lpcm",       HARD, PCM),
	RULE("atmos", AXIS_RULE_ATMOS, HARD, 0, 0, NULL),

	/* Editions.  A word of the first kind is an edition by itself; one
	 * of the second only before Cut, Edition, Version or Collection. */
	ED("extended",    EXTENDED),
	ED("unrated",     UNRATED),
	ED("uncut",       UNCUT),
	ED("remastered",  REMASTERED),
	ED("remaster",    REMASTERED),
	ED("restored",    RESTORED),
	ED("imax",        IMAX),
	ED("criterion",   CRITERION),
	ED("theatrical",  THEATRICAL),
	ED("open matte",  OPEN_MATTE),
	ED("open-matte",  OPEN_MATTE),
	ED("dc",          DIRECTORS),
	EDW("directors",   DIRECTORS),
	EDW("director's",  DIRECTORS),
	EDW("director\xe2\x80\x99s", DIRECTORS),
	EDW("final",       FINAL_CUT),
	EDW("special",     SPECIAL),
	EDW("collectors",  COLLECTORS),
	EDW("collector's", COLLECTORS),
	EDW("anniversary", ANNIVERSARY),
	EDW("ultimate",    ULTIMATE),
	EDW("definitive",  DEFINITIVE),
	EDW("alternate",   ALTERNATE),
	EDW("alternative", ALTERNATE),
	EDE("cut"),
	EDE("edition"),
	EDE("version"),
	EDE("collection"),

	/* Languages, by name and by the codes names use. */
	LANG("english", "en"), LANG("eng", "en"),
	LANG("french", "fr"), LANG("fre", "fr"), LANG("fra", "fr"), LANG("truefrench", "fr"),
	LANG("vff", "fr"), LANG("vfq", "fr"), LANG("vfi", "fr"), LANG("vf", "fr"), LANG("vf2", "fr"),
	LANGM("vostfr", "fr", SUBBED),
	LANG("german", "de"), LANG("ger", "de"), LANG("deu", "de"),
	LANG("italian", "it"), LANG("ita", "it"),
	LANG("spanish", "es"), LANG("spa", "es"), LANG("esp", "es"), LANG("castellano", "es"),
	LANG("latino", "es"),
	LANG("portuguese", "pt"), LANG("por", "pt"), LANG("brazilian", "pt"), LANG("pt-br", "pt"),
	LANG("russian", "ru"), LANG("rus", "ru"),
	LANG("japanese", "ja"), LANG("jap", "ja"), LANG("jpn", "ja"),
	LANG("korean", "ko"), LANG("kor", "ko"),
	LANGM("korsub", "ko", HARDSUB),
	LANG("chinese", "zh"), LANG("chi", "zh"), LANG("chn", "zh"), LANG("mandarin", "zh"),
	LANG("cantonese", "zh"),
	LANG("hindi", "hi"), LANG("hin", "hi"),
	LANG("tamil", "ta"), LANG("tam", "ta"),
	LANG("telugu", "te"), LANG("tel", "te"),
	LANG("malayalam", "ml"), LANG("kannada", "kn"), LANG("bengali", "bn"),
	LANG("punjabi", "pa"), LANG("marathi", "mr"), LANG("urdu", "ur"),
	LANG("arabic", "ar"), LANG("ara", "ar"),
	LANG("hebrew", "he"), LANG("heb", "he"),
	LANG("turkish", "tr"), LANG("tur", "tr"),
	LANG("greek", "el"), LANG("gre", "el"),
	LANG("polish", "pl"), LANG("pol", "pl"),
	LANG("czech", "cs"), LANG("cze", "cs"),
	LANG("slovak", "sk"),
	LANG("hungarian", "hu"), LANG("hun", "hu"),
	LANG("romanian", "ro"), LANG("rum", "ro"),
	LANG("bulgarian", "bg"),
	LANG("ukrainian", "uk"), LANG("ukr", "uk"),
	LANG("serbian", "sr"), LANG("croatian", "hr"), LANG("slovenian", "sl"),
	LANG("dutch", "nl"), LANG("dut", "nl"), LANG("flemish", "nl"),
	LANG("swedish", "sv"), LANG("swe", "sv"),
	LANG("danish", "da"), LANG("dan", "da"),
	LANG("norwegian", "no"), LANG("nor", "no"),
	LANG("finnish", "fi"), LANG("fin", "fi"),
	LANG("icelandic", "is"),
	LANG("thai", "th"), LANG("tha", "th"),
	LANG("vietnamese", "vi"), LANG("vie", "vi"),
	LANG("indonesian", "id"), LANG("ind", "id"),
	LANG("malay", "ms"),
	LANG("persian", "fa"), LANG("farsi", "fa"),
	LANGM("esub", "en", SUBBED), LANGM("esubs", "en", SUBBED),

	/* What a name says about its languages. */
	LMK("multi",            MULTI),
	LMK("multi-audio",      MULTI),
	LMK("multi audio",      MULTI),
	LMK("dual",             DUAL),
	LMK("dual-audio",       DUAL),
	LMK("dual audio",       DUAL),
	LMK("dualaudio",        DUAL),
	LMK("dl",               DUAL),
	LMK("dubbed",           DUBBED),
	LMK("dub",              DUBBED),
	LMK("subbed",           SUBBED),
	LMK("subs",             SUBBED),
	LMK("sub",              SUBBED),
	LMK("multisubs",        SUBBED),
	LMK("multisub",         SUBBED),
	LMK("multi-subs",       SUBBED),
	LMK("multiple subtitle", SUBBED),
	LMK("softsub",          SUBBED),
	LMK("softsubs",         SUBBED),
	LMK("hc",               HARDSUB),
	LMK("hardsub",          HARDSUB),
	LMK("hardsubs",         HARDSUB),
	LMK("hardcoded",        HARDSUB),

	/* Revision. */
	REV("proper",  PROPER),
	REV("proper2", PROPER),
	REV("repack",  REPACK),
	REV("repack2", REPACK),
	REV("repack3", REPACK),
	REV("rerip",   RERIP),
	REV("real",    REAL),

	/* Other marks. */
	MARK("limited",   LIMITED),
	MARK("internal",  INTERNAL),
	MARK("nfofix",    FIX),
	MARK("dirfix",    FIX),
	MARK("subfix",    FIX),
	MARK("syncfix",   FIX),
	MARK("prooffix",  FIX),
	MARK("samplefix", FIX),
	MARK("audiofix",  FIX),
	MARK("readnfo",   READNFO),
	MARK("read nfo",  READNFO),
	MARK("hybrid",    HYBRID),

	/* Marks that stop a grab. */
	STOP("sample",     SAMPLE),
	STOP("password",   PASSWORD),
	STOP("passworded", PASSWORD),
	STOP("3d",         3D),
	STOP("sbs",        3D),
	STOP("hsbs",       3D),
	STOP("h-sbs",      3D),
	STOP("half-sbs",   3D),
	STOP("half sbs",   3D),
	STOP("hou",        3D),
	STOP("h-ou",       3D),
	STOP("half-ou",    3D),
	STOP("half ou",    3D),

	RULE("complete", AXIS_RULE_COMPLETE, SOFT, 0, 0, NULL),
};

/* How many words a rule has, and the length of each (at most four). */
static guint
rule_words(
	const gchar *words,
	const gchar *start[4],
	gsize        length[4]
){
	guint n = 0;
	const gchar *p = words;

	while (*p != '\0' && n < 4) {
		const gchar *space = strchr(p, ' ');
		gsize len = space != NULL ? (gsize)(space - p) : strlen(p);

		start[n] = p;
		length[n] = len;
		n++;
		if (space == NULL)
			break;
		p = space + 1;
	}
	return n;
}

static gboolean
word_equal(
	const gchar *token,
	const gchar *word,
	gsize        length
){
	return strlen(token) == length && g_ascii_strncasecmp(token, word, length) == 0;
}

const AxisReleaseRule *
axis_release_rule_at(
	const gchar *const *tokens,
	guint               n,
	guint               i,
	guint              *span
){
	const AxisReleaseRule *best = NULL;
	guint best_span = 0;
	guint r;

	*span = 0;
	if (i >= n)
		return NULL;
	for (r = 0; r < G_N_ELEMENTS(rules); r++) {
		const gchar *start[4];
		gsize length[4];
		guint words = rule_words(rules[r].words, start, length);
		guint k;

		if (words <= best_span || i + words > n)
			continue;
		for (k = 0; k < words; k++) {
			if (!word_equal(tokens[i + k], start[k], length[k]))
				break;
		}
		if (k == words) {
			best = &rules[r];
			best_span = words;
		}
	}
	*span = best_span;
	return best;
}

const AxisReleaseRule *
axis_release_rule_word(const gchar *word)
{
	guint r;

	if (word == NULL)
		return NULL;
	for (r = 0; r < G_N_ELEMENTS(rules); r++) {
		if (strchr(rules[r].words, ' ') == NULL && g_ascii_strcasecmp(rules[r].words, word) == 0)
			return &rules[r];
	}
	return NULL;
}

const AxisReleaseRule *
axis_release_rule_audio_prefix(
	const gchar *word,
	gsize       *length
){
	const AxisReleaseRule *best = NULL;
	gsize best_length = 0;
	guint r;

	*length = 0;
	for (r = 0; r < G_N_ELEMENTS(rules); r++) {
		gsize len;

		if (rules[r].field != AXIS_RULE_AUDIO || strchr(rules[r].words, ' ') != NULL)
			continue;
		len = strlen(rules[r].words);
		if (len > best_length && g_ascii_strncasecmp(word, rules[r].words, len) == 0) {
			best = &rules[r];
			best_length = len;
		}
	}
	*length = best_length;
	return best;
}

/* ── Extensions, sites, reposters ───────────────────────────────── */

static const gchar *const plain_extensions[] = {
	"mkv", "mp4", "m4v", "avi", "mov", "wmv", "mpg", "mpeg", "m2ts", "mts", "webm", "flv", "vob",
	"ogm", "ogv", "3gp", "rmvb", "asf", "srt", "sub", "idx", "ass", "ssa", "nfo", "sfv", "nzb",
	"torrent", "par2", "txt", "jpg", "png",
	NULL
};

static const gchar *const archive_extensions[] = {
	"rar", "zip", "7z", "tar", "gz", "tgz", "bz2", "xz",
	NULL
};

static const gchar *const program_extensions[] = {
	"exe", "scr", "bat", "cmd", "msi", "lnk", "vbs", "ps1", "jar", "apk", "pif",
	NULL
};

static gboolean
in_list(
	const gchar *const *list,
	const gchar        *word,
	gsize               length
){
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		if (strlen(list[i]) == length && g_ascii_strncasecmp(list[i], word, length) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean
axis_release_extension(
	const gchar *extension,
	gsize        length,
	guint       *stop
){
	*stop = 0;
	if (in_list(plain_extensions, extension, length))
		return TRUE;
	if (in_list(archive_extensions, extension, length)) {
		*stop = AXIS_RELEASE_STOP_ARCHIVE;
		return TRUE;
	}
	/* The parts of a split RAR set: r00 to r99. */
	if (length == 3 && (extension[0] == 'r' || extension[0] == 'R') &&
	    g_ascii_isdigit(extension[1]) && g_ascii_isdigit(extension[2])) {
		*stop = AXIS_RELEASE_STOP_ARCHIVE;
		return TRUE;
	}
	if (in_list(program_extensions, extension, length)) {
		*stop = AXIS_RELEASE_STOP_EXECUTABLE;
		return TRUE;
	}
	return FALSE;
}

static const gchar *const tlds[] = {
	"com", "net", "org", "info", "biz", "to", "tv", "me", "cc", "io", "co", "xyz", "ws", "se", "nu",
	"ch", "li", "eu", "ru", "la", "ag", "am", "mx", "lol", "club", "site", "online", "top", "cx",
	"is", "sx", "pw", "in", "uk", "us", "ca", "de", "fr", "nl", "pl", "it", "es", "pro", "link",
	"live", "one", "vip", "win", "ac", "gs", "vg", "tk", "ml", "re", "si", "st", "su", "tw", "cz",
	"lt", "lv", "ee", "fi", "dk", "no", "at", "be", "br", "ar", "cl", "vn", "id", "my", "ph", "ir",
	"bz", "gg",
	NULL
};

gboolean
axis_release_tld(
	const gchar *label,
	gsize        length
){
	return in_list(tlds, label, length);
}

static const gchar *const repost_suffixes[] = {
	"xpost", "postbot", "obfuscated", "scrambled", "asrequested", "buymore", "whiterev", "repost",
	"rp", "rakuv", "chamele0n", "altezachen", "alternativetorequested", "repackpost", "nzbgeek",
	NULL
};

gboolean
axis_release_repost_suffix(const gchar *word)
{
	gsize length = strlen(word);

	/* "Rakuvfinhel", "AsRequestedByHDL": the reposter's word and more. */
	return in_list(repost_suffixes, word, length) ||
		(g_ascii_strncasecmp(word, "rakuv", 5) == 0 && length <= 16) ||
		(g_ascii_strncasecmp(word, "asrequested", 11) == 0 && length <= 24);
}

/* Trackers and indexes that write their name into the names they list
 * ("-GROUP[TGx]", "-GROUP [eztv]", "-GROUP TGx"), and the collectives
 * whose tag follows their encoder's name ("(... Tigole) [QxR]", "Joy) UTR"). */
static const gchar *const site_tags[] = {
	"tgx", "eztv", "eztvx", "ettv", "rarbg", "rartv", "orarbg", "ethd", "qxr", "utr", "taoe",
	NULL
};

gboolean
axis_release_site_tag(
	const gchar *word,
	gsize        length
){
	return in_list(site_tags, word, length);
}

gboolean
axis_release_article(const gchar *word)
{
	return g_ascii_strcasecmp(word, "the") == 0 || g_ascii_strcasecmp(word, "a") == 0 ||
		g_ascii_strcasecmp(word, "an") == 0;
}

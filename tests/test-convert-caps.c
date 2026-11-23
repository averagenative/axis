/*
 * test-convert-caps.c - the capability check against tools that lie: an
 * encoder that is listed and fails, 8 bits where 10 were asked, HDR10
 * values altered or dropped; the arguments it gives the hardware encoders;
 * the store, and measuring again when ffmpeg changes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The fake (tests/fixtures/convert/fake-ffmpeg.sh) runs the real ffmpeg
 * underneath and lies as each test asks; it always fails the hardware
 * encoders, so nothing here depends on this machine's GPU.  The real
 * ffmpeg's own check is in test-convert-real.c.  Without ffmpeg the tests
 * that need it skip; the store's tests need the database.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "axis-test-convert.h"
#include "axis-test-db.h"
#include "axis-test-storage.h"
#include "db/axis-db-migrate.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-convert-caps.h"

#define FAKE "tests/fixtures/convert/fake-ffmpeg.sh"

typedef struct
{
	gchar   *dir;
	gchar   *scratch;
	gchar   *ffmpeg;
	gchar   *ffprobe;
	gchar   *fake;
	GString *record;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	fx->dir = axis_test_storage_dir_new();
	fx->scratch = g_build_filename(fx->dir, "check", NULL);
	g_assert_cmpint(g_mkdir(fx->scratch, 0700), ==, 0);
	fx->ffmpeg = g_find_program_in_path("ffmpeg");
	fx->ffprobe = g_find_program_in_path("ffprobe");
	/* The child runs in the scratch directory: the script by its full
	 * name. */
	fx->fake = g_canonicalize_filename(FAKE, NULL);
	fx->record = g_string_new(NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->scratch);
	g_free(fx->ffmpeg);
	g_free(fx->ffprobe);
	g_free(fx->fake);
	g_string_free(fx->record, TRUE);
}

#define SKIP_WITHOUT_MEDIA(fx) do { if ((fx)->ffmpeg == NULL || (fx)->ffprobe == NULL) { \
	g_test_skip("ffmpeg is not installed"); return; } } while (0)

/* The check, with the fake in @mode standing in for ffmpeg. */
static AxisEncoderCapabilities *
measure_with(
	Fixture     *fx,
	const gchar *mode,
	GError     **error
){
	const gchar *ffmpeg[5];
	const gchar *ffprobe[2];
	AxisCapabilityTool tool;

	ffmpeg[0] = "/bin/sh";
	ffmpeg[1] = fx->fake;
	ffmpeg[2] = mode;
	ffmpeg[3] = fx->ffmpeg;
	ffmpeg[4] = NULL;
	ffprobe[0] = fx->ffprobe;
	ffprobe[1] = NULL;
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = ffmpeg;
	tool.ffprobe = ffprobe;
	tool.scratch = fx->scratch;
	tool.width = 320;
	tool.height = 240;
	tool.record = fx->record;
	return axis_convert_capabilities_measure(&tool, error);
}

static const AxisEncoderCapability *
cap(
	const AxisEncoderCapabilities *caps,
	const gchar                   *name
){
	return axis_encoder_capabilities_get(caps, axis_convert_encoder_named(name));
}

/* Nothing is left in the scratch directory. */
static void
assert_scratch_empty(Fixture *fx)
{
	g_autoptr(GDir) d = g_dir_open(fx->scratch, 0, NULL);

	g_assert_nonnull(d);
	g_assert_null(g_dir_read_name(d));
}

/* Spec: Encoder missing: listed, but its encode fails: not usable, with
 * the reason; the profile's next encoder is used. */
static void
test_encoder_listed_but_failing(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisMediaInfo) info = NULL;
	const AxisEncoderCapability *nvenc;
	AxisProfileDefinition def;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisConvertRefusal tried[AXIS_ENCODER_COUNT];

	SKIP_WITHOUT_MEDIA(fx);
	caps = measure_with(fx, "no-device", &error);
	g_assert_no_error(error);
	nvenc = cap(caps, "hevc_nvenc");
	g_assert_true(nvenc->measured);
	g_assert_false(nvenc->usable);
	g_assert_nonnull(strstr(nvenc->reason, "No NVENC capable devices found"));
	g_assert_false(cap(caps, "hevc_vaapi")->usable);
	g_assert_nonnull(strstr(cap(caps, "hevc_vaapi")->reason, "No VA display"));
	g_assert_true(cap(caps, "libx265")->usable);
	g_assert_true(cap(caps, "libx265")->hdr10);
	assert_scratch_empty(fx);

	/* NVENC first in the profile: passed over for the next. */
	g_assert_true(axis_profile_definition_parse(axis_profile_builtin("space_saver")->definition, -1, &def, NULL));
	def.encoders[0] = AXIS_ENCODER_NVENC;
	def.encoders[1] = AXIS_ENCODER_SOFTWARE;
	def.encoders[2] = AXIS_ENCODER_VAAPI;
	info = axis_test_convert_info("matroska,webm");
	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_finish(info);
	g_assert_true(axis_convert_plan_choose(&def, info, 8000000000, caps, NULL, &plan, &refusal, tried));
	g_assert_cmpstr(plan->encoder->name, ==, "libx265");
	g_assert_cmpint(tried[0], ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
}

/* 8 bits came out where 10 were asked: usable, not 10-bit, so not HDR10
 * either; never planned for a 10-bit target. */
static void
test_eight_bit_instead_of_ten(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	const AxisEncoderCapability *x265;

	SKIP_WITHOUT_MEDIA(fx);
	caps = measure_with(fx, "eight-bit", &error);
	g_assert_no_error(error);
	x265 = cap(caps, "libx265");
	g_assert_true(x265->usable);
	g_assert_false(x265->ten_bit);
	g_assert_false(x265->hdr10);
	g_assert_nonnull(strstr(x265->reason, "bits"));
	assert_scratch_empty(fx);
}

/* Spec: HDR not carried: values altered, or dropped from the bitstream:
 * not HDR10, and never planned for an HDR10 file. */
static void
test_hdr10_not_carried(Fixture *fx, gconstpointer data)
{
	static const gchar *const modes[] = { "hdr-altered", "hdr-dropped" };
	guint m;

	SKIP_WITHOUT_MEDIA(fx);
	for (m = 0; m < G_N_ELEMENTS(modes); m++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisEncoderCapabilities) caps = measure_with(fx, modes[m], &error);
		g_autoptr(AxisMediaInfo) info = NULL;
		const AxisEncoderCapability *x265;
		AxisProfileDefinition def;
		AxisConvertPlan *plan = NULL;
		AxisConvertRefusal refusal = AXIS_CONVERT_OK;

		g_assert_no_error(error);
		x265 = cap(caps, "libx265");
		g_assert_true(x265->usable);
		g_assert_true(x265->ten_bit);
		if (x265->hdr10)
			g_error("%s: HDR10 was found carried", modes[m]);
		g_test_message("%s: %s", modes[m], x265->reason);
		g_assert_nonnull(strstr(x265->reason, "HDR10"));
		assert_scratch_empty(fx);

		info = axis_test_convert_info("matroska,webm");
		axis_test_convert_video(info, 0, "hevc", 3840, 2160, "yuv420p10le", 10, "bt2020", "smpte2084", "bt2020nc",
			"tv", AXIS_HDR_HDR10);
		axis_test_convert_finish(info);
		g_assert_true(axis_profile_definition_parse(axis_profile_builtin("space_saver")->definition, -1, &def,
			NULL));
		g_assert_false(axis_convert_plan_build(&def, info, 40000000000, AXIS_ENCODER_SOFTWARE, caps, NULL, &plan,
			&refusal));
		g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
	}
}

/* What the hardware encoders are given (recorded: the fake then fails
 * them, as a machine without the device does): the documented options,
 * the source as descriptor 3, the output descriptor 4. */
static void
test_hardware_arguments_recorded(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	g_auto(GStrv) lines = NULL;
	guint nvenc = 0;
	guint vaapi = 0;
	guint i;

	SKIP_WITHOUT_MEDIA(fx);
	caps = measure_with(fx, "no-device", &error);
	g_assert_no_error(error);
	lines = g_strsplit(fx->record->str, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *l = lines[i];

		if (strstr(l, "-c:0 hevc_nvenc ") != NULL || strstr(l, "-c:0 h264_nvenc ") != NULL) {
			nvenc++;
			g_assert_nonnull(strstr(l, " -protocol_whitelist fd -format_whitelist "));
			g_assert_nonnull(strstr(l, " -fd 3 -i fd: "));
			/* Level 4: CQ 24 for HEVC, 20 for H.264 (two below the CRF). */
			g_assert_nonnull(strstr(l, strstr(l, "hevc_nvenc") != NULL ?
				" -pix_fmt:0 yuv420p -preset:0 p1 -rc:0 vbr -cq:0 24 -b:0 0 " :
				" -pix_fmt:0 yuv420p -preset:0 p1 -rc:0 vbr -cq:0 20 -b:0 0 "));
			g_assert_true(g_str_has_suffix(l, " -protocol_whitelist fd -fd 4 fd:"));
		}
		if (strstr(l, "-c:0 hevc_vaapi ") != NULL || strstr(l, "-c:0 h264_vaapi ") != NULL) {
			vaapi++;
			g_assert_nonnull(strstr(l, " -vaapi_device /dev/dri/renderD128 "));
			g_assert_nonnull(strstr(l, "range=tv,format=nv12,hwupload -rc_mode:0 CQP -qp:0 "));
			g_assert_true(g_str_has_suffix(l, " -protocol_whitelist fd -fd 4 fd:"));
		}
		/* No run of the check names a file: the test sources and outputs
		 * are descriptors too. */
		g_assert_null(strstr(l, fx->scratch));
	}
	/* One 8-bit test each (they fail there, so no 10-bit one follows). */
	g_assert_cmpuint(nvenc, ==, 2);
	g_assert_cmpuint(vaapi, ==, 2);
}

/* A tool that is not ffmpeg, one that is not there, a scratch directory
 * that is not private: refused, with nothing measured. */
static void
test_tool_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	const gchar *missing[2] = { "/nonexistent/ffmpeg", NULL };
	const gchar *ffprobe[2] = { "/nonexistent/ffprobe", NULL };
	AxisCapabilityTool tool;

	SKIP_WITHOUT_MEDIA(fx);
	caps = measure_with(fx, "bad-version", &error);
	g_assert_null(caps);
	g_assert_error(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_TOOL);
	g_clear_error(&error);

	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = missing;
	tool.ffprobe = ffprobe;
	tool.scratch = fx->scratch;
	g_assert_null(axis_convert_capabilities_measure(&tool, &error));
	g_assert_error(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_TOOL);
	g_clear_error(&error);

	g_assert_cmpint(g_chmod(fx->scratch, 0755), ==, 0);
	g_assert_null(measure_with(fx, "no-device", &error));
	g_assert_error(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_SCRATCH);
}

/* ── The store ──────────────────────────────────────────────────── */

typedef struct
{
	Fixture     base;
	AxisTestDb *db;
	AxisDbPool *pool;
} DbFixture;

static void
db_setup(DbFixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	fixture_setup(&fx->base, data);
	axis_test_log_init();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 2, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);
}

static void
db_teardown(DbFixture *fx, gconstpointer data)
{
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	if (fx->db != NULL)
		axis_test_db_free(fx->db);
	fixture_teardown(&fx->base, data);
	axis_test_log_reset();
}

/* Stored and read back as they were. */
static void
test_store_roundtrip(DbFixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisEncoderCapabilities) back = NULL;
	g_autoptr(AxisEncoderCapabilities) none = NULL;
	AxisDbConn *conn;
	guint i;

	if (fx->db == NULL)
		return;
	g_free(caps->tool_digest);
	caps->tool_digest = g_strdup("ab12000000000000000000000000000000000000000000000000000000000000");
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("hevc_nvenc"), FALSE, FALSE, FALSE,
		"[hevc_nvenc @ 0x0] No NVENC capable devices found; it's 'quoted'");
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("hevc_vaapi"), TRUE, TRUE, FALSE,
		"its HDR10 test output is not what the plan says");
	caps->mov_text = FALSE;
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_convert_capabilities_load(conn, AXIS_CONVERT_LOCAL_MACHINE, &none, &error));
	g_assert_null(none);
	g_assert_true(axis_convert_capabilities_store(conn, AXIS_CONVERT_LOCAL_MACHINE, caps, &error));
	g_assert_no_error(error);
	/* Twice: replaced, not added to. */
	g_assert_true(axis_convert_capabilities_store(conn, AXIS_CONVERT_LOCAL_MACHINE, caps, &error));
	g_assert_true(axis_convert_capabilities_load(conn, AXIS_CONVERT_LOCAL_MACHINE, &back, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_nonnull(back);
	g_assert_cmpstr(back->tool_version, ==, caps->tool_version);
	g_assert_cmpstr(back->tool_digest, ==, caps->tool_digest);
	g_assert_false(back->mov_text);
	g_assert_true(back->aac);
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		g_assert_true(back->encoders[i].measured);
		g_assert_cmpint(back->encoders[i].usable, ==, caps->encoders[i].usable);
		g_assert_cmpint(back->encoders[i].ten_bit, ==, caps->encoders[i].ten_bit);
		g_assert_cmpint(back->encoders[i].hdr10, ==, caps->encoders[i].hdr10);
		g_assert_cmpstr(back->encoders[i].reason, ==, caps->encoders[i].reason);
	}
	{
		g_autofree gchar *n = axis_test_db_query(fx->db, "SELECT count(*) FROM encoder_capability");

		g_assert_cmpstr(n, ==, "6");
	}
}

/* The table refuses what the check never writes: HDR10 without 10 bits, a
 * digest that is not one, an encoder not in the table. */
static void
test_store_constraints(DbFixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	static const gchar *const refused[] = {
		"INSERT INTO encoder_tool (machine, tool_version, tool_digest, aac, mov_text, subrip) VALUES"
		" ('local', '8.1', 'nothex', true, true, true)",
		"INSERT INTO encoder_tool (machine, tool_version, tool_digest, aac, mov_text, subrip) VALUES"
		" ('Local!', '8.1', '" "0000000000000000000000000000000000000000000000000000000000000000', true, true, true)",
		"INSERT INTO encoder_capability (machine, encoder, usable, ten_bit, hdr10) VALUES"
		" ('local', 'libx265', true, false, true)",
		"INSERT INTO encoder_capability (machine, encoder, usable, ten_bit, hdr10) VALUES"
		" ('local', 'libaom-av1', true, false, false)",
		"INSERT INTO encoder_capability (machine, encoder, usable, ten_bit, hdr10, reason) VALUES"
		" ('local', 'libx264', false, false, false, E'line\\nbreak')"
	};
	guint i;

	if (fx->db == NULL)
		return;
	axis_test_db_exec(fx->db, "INSERT INTO encoder_tool (machine, tool_version, tool_digest, aac, mov_text, subrip)"
		" VALUES ('local', '8.1.3', '0000000000000000000000000000000000000000000000000000000000000000', true, true,"
		" true)");
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		g_assert_false(axis_db_conn_exec(conn, refused[i], 0, NULL, NULL, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	axis_db_pool_release(fx->pool, conn);
}

/* Spec: New ffmpeg version: measured again before any arguments are built;
 * the same version is not. */
static void
test_new_version_measured_again(DbFixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	const gchar *ffmpeg[5];
	const gchar *ffprobe[2];
	AxisCapabilityTool tool;
	gboolean measured = FALSE;
	g_autofree gchar *version = NULL;
	guint i;

	if (fx->db == NULL)
		return;
	if (fx->base.ffmpeg == NULL || fx->base.ffprobe == NULL) {
		g_test_skip("ffmpeg is not installed");
		return;
	}
	ffmpeg[0] = "/bin/sh";
	ffmpeg[1] = fx->base.fake;
	ffmpeg[3] = fx->base.ffmpeg;
	ffmpeg[4] = NULL;
	ffprobe[0] = fx->base.ffprobe;
	ffprobe[1] = NULL;
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = ffmpeg;
	tool.ffprobe = ffprobe;
	tool.scratch = fx->base.scratch;

	for (i = 0; i < 3; i++) {
		g_autoptr(AxisEncoderCapabilities) caps = NULL;

		/* Version A, A again, then B. */
		ffmpeg[2] = i < 2 ? "version-a" : "version-b";
		g_assert_true(axis_convert_capabilities_ensure(fx->pool, AXIS_CONVERT_LOCAL_MACHINE, &tool, &caps, &measured,
			&error));
		g_assert_no_error(error);
		g_assert_nonnull(caps);
		g_assert_cmpint(measured, ==, i != 1);
		g_assert_cmpstr(caps->tool_version, ==, i < 2 ? "99.1-fake-a" : "99.2-fake-b");
		/* Every encoder was measured, and failed (the fake encodes
		 * nothing): stored as such. */
		g_assert_true(cap(caps, "libx265")->measured);
		g_assert_false(cap(caps, "libx265")->usable);
	}
	version = axis_test_db_query(fx->db, "SELECT tool_version FROM encoder_tool WHERE machine = 'local'");
	g_assert_cmpstr(version, ==, "99.2-fake-b");
	{
		g_autofree gchar *n = axis_test_db_query(fx->db, "SELECT count(*) FROM encoder_capability");

		g_assert_cmpstr(n, ==, "6");
	}
}

#define ADD(name, func) g_test_add("/convert/caps/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)
#define ADD_DB(name, func) g_test_add("/convert/caps/db/" name, DbFixture, NULL, db_setup, func, db_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("listed-but-failing", test_encoder_listed_but_failing);
	ADD("eight-bit-instead-of-ten", test_eight_bit_instead_of_ten);
	ADD("hdr10-not-carried", test_hdr10_not_carried);
	ADD("hardware-arguments-recorded", test_hardware_arguments_recorded);
	ADD("tool-refused", test_tool_refused);
	ADD_DB("store-roundtrip", test_store_roundtrip);
	ADD_DB("store-constraints", test_store_constraints);
	ADD_DB("new-version-measured-again", test_new_version_measured_again);

	return g_test_run();
}

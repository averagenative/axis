/*
 * axis-convert-caps.c - the capability check: test encodes with the
 * builder's own arguments, read back with the probe's own calls
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See axis-convert-caps.h.  Limits of each run: the caller's timeout
 * (120 s by default), CPU time sixteen times that plus a minute (encoders
 * run threads), the probe runner's descriptor and core limits, at most
 * 256 MiB written to the one output file it is given, and 8 GiB of address
 * space for the software encoders.  The hardware encoders run without an
 * address space limit: their drivers map device memory into the process
 * and reserve large ranges (unverified on Axis's side: a limit that is too
 * low shows as "not capable", never as a wrong result).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-convert-caps.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "probe/axis-probe-describe.h"
#include "probe/axis-probe-run.h"

G_DEFINE_QUARK(axis-convert-error-quark, axis_convert_error)

#define VERSION_CAP      (64 * 1024)
#define LISTING_CAP      (1024 * 1024)
#define PROGRESS_CAP     (1024 * 1024)
#define ERROR_CAP        (16 * 1024)

/* The HDR10 test values: not the usual BT.2020 display, so that an
 * encoder (or ffmpeg) that writes values of its own does not pass by
 * chance. */
#define TEST_MASTER_DISPLAY "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,50)"
#define TEST_MAX_CLL        "1000,400"

/* ── Running ────────────────────────────────────────────────────── */

static gchar **
command(
	const gchar *const *prefix,
	const gchar *const *rest
){
	GPtrArray *argv = g_ptr_array_new();
	guint i;

	for (i = 0; prefix != NULL && prefix[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(prefix[i]));
	for (i = 0; rest != NULL && rest[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(rest[i]));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

static guint
timeout_of(const AxisCapabilityTool *tool)
{
	return tool->timeout_ms > 0 ? tool->timeout_ms : AXIS_CONVERT_CHECK_TIMEOUT_MS;
}

static void
run(
	const AxisCapabilityTool *tool,
	gchar                   **argv,
	gint                      in_fd,
	gint                      out_fd,
	gboolean                  hardware,
	gsize                     output_cap,
	AxisProbeRunResult       *result
){
	AxisProbeRunSpec spec;

	if (tool->record != NULL) {
		g_autofree gchar *line = g_strjoinv(" ", argv);

		g_string_append_printf(tool->record, "%s\n", line);
	}
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = in_fd;
	spec.output_fd = out_fd;
	spec.file_size_limit = out_fd > 2 ? AXIS_CONVERT_CHECK_FILE_LIMIT : 0;
	spec.timeout_ms = timeout_of(tool);
	spec.output_cap = output_cap;
	spec.error_cap = ERROR_CAP;
	spec.cancellable = tool->cancellable;
	spec.memory_limit = hardware ? 0 : AXIS_CONVERT_CHECK_MEMORY;
	spec.working_directory = tool->scratch;
	spec.cpu_seconds = timeout_of(tool) / 1000 * 16 + 60;
	axis_probe_run(&spec, result);
}

/* What a failed run said, as a sentence. */
static gchar *
describe_failure(
	const gchar              *what,
	const AxisProbeRunResult *result
){
	g_autofree gchar *raw = result->errors != NULL && result->errors->len > 0 ?
		g_strndup((const gchar *)result->errors->data, result->errors->len) : NULL;
	g_autofree gchar *said = raw != NULL ? axis_library_text_clean(raw, 240) : NULL;

	if (said != NULL)
		g_strstrip(said);
	return g_strdup_printf("%s %s%s%s", what, axis_probe_run_status_describe(result->status),
		said != NULL && said[0] != '\0' ? ": " : "", said != NULL ? said : "");
}

/* A new file in the scratch directory, open for reading and writing. */
static gint
scratch_file(
	const AxisCapabilityTool  *tool,
	const gchar               *name,
	gchar                    **path,
	GError                   **error
){
	g_autofree gchar *full = g_build_filename(tool->scratch, name, NULL);
	gint fd;

	(void)unlink(full);
	fd = open(full, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		g_set_error(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_SCRATCH, "cannot make %s in the scratch "
			"directory: %s", name, g_strerror(errno));
		return -1;
	}
	if (path != NULL)
		*path = g_steal_pointer(&full);
	return fd;
}

/* ── Identity ───────────────────────────────────────────────────── */

gboolean
axis_convert_tool_identity(
	const AxisCapabilityTool  *tool,
	gchar                    **version,
	gchar                    **digest,
	GError                   **error
){
	static const gchar *const rest[] = { "-version", NULL };
	g_auto(GStrv) argv = NULL;
	AxisProbeRunResult result;
	gchar *found;

	*version = NULL;
	*digest = NULL;
	g_return_val_if_fail(tool != NULL && tool->ffmpeg != NULL && tool->ffmpeg[0] != NULL, FALSE);

	argv = command(tool->ffmpeg, rest);
	run(tool, argv, -1, 0, FALSE, VERSION_CAP, &result);
	if (result.status != AXIS_PROBE_RUN_OK) {
		g_autofree gchar *why = describe_failure("ffmpeg -version", &result);

		axis_probe_run_result_clear(&result);
		g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_TOOL, why);
		return FALSE;
	}
	found = axis_convert_parse_version((const gchar *)result.output->data, result.output->len);
	if (found == NULL) {
		axis_probe_run_result_clear(&result);
		g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_TOOL,
			"the program does not say it is ffmpeg");
		return FALSE;
	}
	*version = found;
	*digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, result.output->data, result.output->len);
	axis_probe_run_result_clear(&result);
	return TRUE;
}

/* ── The test sources ───────────────────────────────────────────── */

typedef struct
{
	gint           fd;
	gchar         *path;
	AxisMediaInfo *info;
	gint64         size;
} Source;

static void
source_clear(Source *s)
{
	if (s->fd >= 0)
		close(s->fd);
	if (s->path != NULL)
		(void)unlink(s->path);
	g_free(s->path);
	axis_media_info_free(s->info);
	memset(s, 0, sizeof(*s));
	s->fd = -1;
}

/* One second of ffmpeg's test picture, made with @middle (filters and
 * encoder), into the scratch directory; read back with the probe.  FALSE
 * (with @why) when it cannot be made. */
static gboolean
make_source(
	const AxisCapabilityTool  *tool,
	const gchar               *name,
	const gchar *const        *middle,
	Source                    *source,
	gchar                    **why,
	GError                   **error
){
	g_autofree gchar *picture = g_strdup_printf("testsrc2=size=%ux%u:rate=24:duration=1",
		tool->width > 0 ? tool->width : AXIS_CONVERT_CHECK_WIDTH,
		tool->height > 0 ? tool->height : AXIS_CONVERT_CHECK_HEIGHT);
	g_autoptr(GPtrArray) rest = g_ptr_array_new();
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *probe_error = NULL;
	AxisProbeRunResult result;
	struct stat st;
	guint i;

	*why = NULL;
	source->fd = scratch_file(tool, name, &source->path, error);
	if (source->fd < 0)
		return FALSE;
	g_ptr_array_add(rest, (gpointer)"-nostdin");
	g_ptr_array_add(rest, (gpointer)"-hide_banner");
	g_ptr_array_add(rest, (gpointer)"-loglevel");
	g_ptr_array_add(rest, (gpointer)"error");
	g_ptr_array_add(rest, (gpointer)"-f");
	g_ptr_array_add(rest, (gpointer)"lavfi");
	g_ptr_array_add(rest, (gpointer)"-i");
	g_ptr_array_add(rest, picture);
	for (i = 0; middle[i] != NULL; i++)
		g_ptr_array_add(rest, (gpointer)middle[i]);
	g_ptr_array_add(rest, (gpointer)"-f");
	g_ptr_array_add(rest, (gpointer)"matroska");
	g_ptr_array_add(rest, (gpointer)"-protocol_whitelist");
	g_ptr_array_add(rest, (gpointer)"fd");
	g_ptr_array_add(rest, (gpointer)"-fd");
	g_ptr_array_add(rest, (gpointer)"4");
	g_ptr_array_add(rest, (gpointer)"fd:");
	g_ptr_array_add(rest, NULL);
	argv = command(tool->ffmpeg, (const gchar *const *)rest->pdata);
	run(tool, argv, -1, source->fd, FALSE, PROGRESS_CAP, &result);
	if (result.status == AXIS_PROBE_RUN_CANCELLED) {
		axis_probe_run_result_clear(&result);
		g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_CANCELLED, "the check was stopped");
		return FALSE;
	}
	if (result.status != AXIS_PROBE_RUN_OK) {
		*why = describe_failure("making the test source", &result);
		axis_probe_run_result_clear(&result);
		return TRUE;
	}
	axis_probe_run_result_clear(&result);
	if (!axis_probe_describe_fd(tool->ffprobe, source->fd, timeout_of(tool), tool->cancellable, &source->info,
	        &probe_error)) {
		*why = g_strdup_printf("the test source could not be read back: %s", probe_error);
		return TRUE;
	}
	source->size = fstat(source->fd, &st) == 0 ? (gint64)st.st_size : 0;
	return TRUE;
}

/* ── The bitstream's own HDR10 values ───────────────────────────── */

gchar *
axis_convert_check_bitstream(
	const AxisCapabilityTool *tool,
	const AxisConvertPlan    *plan,
	gint                      output_fd
){
	static const gchar *const rest[] = {
		"-nostdin", "-hide_banner", "-loglevel", "error", "-protocol_whitelist", "fd", "-fd", "3", "-i", "fd:",
		"-map", "0:0", "-c", "copy", "-f", "hevc", "-protocol_whitelist", "fd", "-fd", "4", "fd:", NULL
	};
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisMediaInfo) raw = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *probe_error = NULL;
	g_auto(GStrv) argv = NULL;
	const AxisConvertStream *expected;
	const AxisMediaStream *s;
	AxisProbeRunResult result;
	gchar *why = NULL;
	gint raw_fd;

	g_return_val_if_fail(tool != NULL && plan != NULL, g_strdup("no plan"));

	if (plan->streams->len == 0 || plan->encoder->codec != AXIS_TCODEC_HEVC)
		return NULL;
	expected = g_ptr_array_index(plan->streams, 0);
	if (!expected->hdr10.mastering && !expected->hdr10.light)
		return NULL;
	raw_fd = scratch_file(tool, "bitstream.hevc", &path, &local);
	if (raw_fd < 0)
		return g_strdup(local->message);
	argv = command(tool->ffmpeg, rest);
	if (lseek(output_fd, 0, SEEK_SET) != 0) {
		why = g_strdup("the output could not be read");
		goto done;
	}
	run(tool, argv, output_fd, raw_fd, FALSE, PROGRESS_CAP, &result);
	if (result.status != AXIS_PROBE_RUN_OK) {
		why = describe_failure("copying the picture's bitstream out", &result);
		axis_probe_run_result_clear(&result);
		goto done;
	}
	axis_probe_run_result_clear(&result);
	if (!axis_probe_describe_fd(tool->ffprobe, raw_fd, timeout_of(tool), tool->cancellable, &raw, &probe_error)) {
		why = g_strdup_printf("the picture's bitstream could not be read: %s", probe_error);
		goto done;
	}
	s = raw->main_video >= 0 ? g_ptr_array_index(raw->streams, raw->main_video) : NULL;
	if (s == NULL || !axis_hdr10_metadata_equal(&s->hdr10, &expected->hdr10))
		why = g_strdup("the HDR10 values are not in the picture's bitstream itself, or not the same there");

done:
	close(raw_fd);
	(void)unlink(path);
	return why;
}

/* ── One test encode ────────────────────────────────────────────── */

/* A definition of @codec, @bit_depth, HDR kept or not, the one family. */
static void
test_definition(
	AxisProfileDefinition *def,
	AxisTargetCodec        codec,
	AxisEncoderFamily      family,
	gint                   bit_depth
){
	memset(def, 0, sizeof(*def));
	def->kind = AXIS_PROFILE_KIND_VIDEO;
	def->libraries = AXIS_TARGET_ROOT(AXIS_MEDIA_MOVIES);
	def->codec = codec;
	def->bit_depth = bit_depth;
	def->keep_hdr = codec == AXIS_TCODEC_HEVC;
	def->encoders[0] = family;
	def->n_encoders = 1;
	def->crf = codec == AXIS_TCODEC_HEVC ? 26 : 22;
	def->audio = AXIS_AUDIO_COPY;
	def->subtitles = AXIS_SUBTITLES_COPY;
	def->container = AXIS_CONTAINER_MATROSKA;
	def->efficient_milli = 150;
	def->vbr_quality = -1;
	def->min_saving_percent = 15;
}

/* Runs the builder's plan for @source with @encoder; NULL when the output
 * is exactly what the plan says, else why not. */
static gchar *
test_encode(
	const AxisCapabilityTool *tool,
	const AxisConvertEncoder *encoder,
	const gchar              *digest,
	gint                      bit_depth,
	const Source             *source,
	const gchar              *label,
	gboolean                 *cancelled
){
	g_autoptr(AxisEncoderCapabilities) trial = axis_encoder_capabilities_new();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisMediaInfo) output = NULL;
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) local = NULL;
	g_autofree gchar *name = g_strdup_printf("%s-%s.mkv", encoder->name, label);
	g_autofree gchar *path = NULL;
	g_autofree gchar *probe_error = NULL;
	g_auto(GStrv) argv = NULL;
	AxisProfileDefinition def;
	AxisConvertOptions options;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisProbeRunResult result;
	gchar *why = NULL;
	gint out_fd;

	*cancelled = FALSE;
	test_definition(&def, encoder->codec, encoder->family, bit_depth);
	/* The plan as if the encoder could: that is what is being found out. */
	trial->tool_digest = g_strdup(digest);
	trial->aac = trial->mov_text = trial->subrip = TRUE;
	axis_encoder_capabilities_set(trial, encoder, TRUE, TRUE, TRUE, NULL);
	memset(&options, 0, sizeof(options));
	/* What an encoder can do does not depend on how hard it tries. */
	options.software_preset = "ultrafast";
	options.nvenc_preset = "p1";
	options.threads = tool->threads > 0 ? tool->threads : 2;
	if (!axis_convert_plan_build(&def, source->info, source->size, encoder->family, trial, &options, &plan,
	        &refusal))
		return g_strdup_printf("its %s test plan was refused: %s", label, axis_convert_refusal_describe(refusal));

	out_fd = scratch_file(tool, name, &path, &local);
	if (out_fd < 0)
		return g_strdup(local->message);
	argv = command(tool->ffmpeg, (const gchar *const *)plan->args);
	if (lseek(source->fd, 0, SEEK_SET) != 0) {
		why = g_strdup("the test source could not be read");
		goto done;
	}
	run(tool, argv, source->fd, out_fd, encoder->family != AXIS_ENCODER_SOFTWARE, PROGRESS_CAP, &result);
	if (result.status == AXIS_PROBE_RUN_CANCELLED) {
		*cancelled = TRUE;
		axis_probe_run_result_clear(&result);
		why = g_strdup("the check was stopped");
		goto done;
	}
	if (result.status != AXIS_PROBE_RUN_OK) {
		g_autofree gchar *what = g_strdup_printf("its %s test encode", label);

		why = describe_failure(what, &result);
		axis_probe_run_result_clear(&result);
		goto done;
	}
	axis_probe_run_result_clear(&result);
	if (!axis_probe_describe_fd(tool->ffprobe, out_fd, timeout_of(tool), tool->cancellable, &output, &probe_error)) {
		why = g_strdup_printf("its %s test output could not be read: %s", label, probe_error);
		goto done;
	}
	if (!axis_convert_plan_check_output(plan, output, mismatches)) {
		why = g_strdup_printf("its %s test output is not what the plan says: %s", label,
			mismatches->len > 0 ? (const gchar *)g_ptr_array_index(mismatches, 0) : "?");
		goto done;
	}
	/* HDR10's values in the bitstream, not only where a decoder could have
	 * found them in the container. */
	{
		g_autofree gchar *bitstream = axis_convert_check_bitstream(tool, plan, out_fd);

		if (bitstream != NULL)
			why = g_strdup_printf("its %s test output: %s", label, bitstream);
	}

done:
	close(out_fd);
	(void)unlink(path);
	return why;
}

/* ── The whole check ────────────────────────────────────────────── */

static void
set_reason(
	AxisEncoderCapabilities  *caps,
	const AxisConvertEncoder *encoder,
	gboolean                  usable,
	gboolean                  ten_bit,
	gboolean                  hdr10,
	const gchar              *reason
){
	/* Stored and shown: a bounded sentence of plain characters. */
	g_autofree gchar *clean = reason != NULL ? axis_library_text_clean(reason, 480) : NULL;

	axis_encoder_capabilities_set(caps, encoder, usable, ten_bit, hdr10, clean);
}

AxisEncoderCapabilities *
axis_convert_capabilities_measure(
	const AxisCapabilityTool  *tool,
	GError                   **error
){
	static const gchar *const listing_rest[] = { "-hide_banner", "-encoders", NULL };
	static const gchar *const sdr_middle[] = {
		"-vf", "setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv",
		"-c:v", "ffv1", "-pix_fmt", "yuv420p", NULL
	};
	static const gchar *const hlg_middle[] = {
		"-vf", "setparams=field_mode=prog:color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc:"
		"range=tv,format=yuv420p10le", "-c:v", "ffv1", NULL
	};
	static const gchar *const hdr10_middle[] = {
		"-vf", "setparams=field_mode=prog:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:"
		"range=tv,format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-x265-params",
		"colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:"
		"master-display=" TEST_MASTER_DISPLAY ":max-cll=" TEST_MAX_CLL ":log-level=error", NULL
	};
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *listing = NULL;
	g_autofree gchar *sdr_why = NULL;
	g_autofree gchar *hlg_why = NULL;
	g_autofree gchar *hdr10_why = NULL;
	const AxisConvertEncoder *table = axis_convert_encoders();
	AxisProbeRunResult result;
	Source sdr;
	Source hlg;
	Source hdr10;
	gsize listing_length = 0;
	gboolean any = FALSE;
	gboolean ok = FALSE;
	guint i;

	g_return_val_if_fail(tool != NULL && tool->ffmpeg != NULL && tool->ffprobe != NULL && tool->scratch != NULL,
		NULL);

	memset(&sdr, 0, sizeof(sdr));
	memset(&hlg, 0, sizeof(hlg));
	memset(&hdr10, 0, sizeof(hdr10));
	sdr.fd = hlg.fd = hdr10.fd = -1;

	{
		struct stat st;

		if (lstat(tool->scratch, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
		    (st.st_mode & 077) != 0) {
			g_set_error(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_SCRATCH, "%s is not a private directory of "
				"this user", tool->scratch);
			return NULL;
		}
	}

	caps = axis_encoder_capabilities_new();
	if (!axis_convert_tool_identity(tool, &caps->tool_version, &caps->tool_digest, error))
		return NULL;
	caps->measured_at_us = g_get_real_time();

	argv = command(tool->ffmpeg, listing_rest);
	run(tool, argv, -1, 0, FALSE, LISTING_CAP, &result);
	if (result.status != AXIS_PROBE_RUN_OK) {
		g_autofree gchar *why = describe_failure("ffmpeg -encoders", &result);

		axis_probe_run_result_clear(&result);
		g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_TOOL, why);
		return NULL;
	}
	listing_length = result.output->len;
	listing = g_strndup((const gchar *)result.output->data, listing_length);
	axis_probe_run_result_clear(&result);
	caps->aac = axis_convert_encoders_lists(listing, listing_length, "aac");
	caps->mov_text = axis_convert_encoders_lists(listing, listing_length, "mov_text");
	caps->subrip = axis_convert_encoders_lists(listing, listing_length, "srt");

	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		if (!axis_convert_encoders_lists(listing, listing_length, table[i].name)) {
			set_reason(caps, &table[i], FALSE, FALSE, FALSE, "this ffmpeg does not have the encoder");
		} else if ((tool->skip_families & AXIS_CONVERT_FAMILY_BIT(table[i].family)) != 0) {
			/* Not measured at all: nothing is known about it. */
			g_free(caps->encoders[i].reason);
			caps->encoders[i].reason = g_strdup(AXIS_CONVERT_SKIPPED_REASON);
		} else {
			any = TRUE;
		}
	}
	if (!any)
		return g_steal_pointer(&caps);

	/* The sources.  The HDR10 one needs libx265 to write its metadata. */
	if (!make_source(tool, "source-sdr.mkv", sdr_middle, &sdr, &sdr_why, error) ||
	    !make_source(tool, "source-hlg.mkv", hlg_middle, &hlg, &hlg_why, error))
		goto out;
	if (axis_convert_encoders_lists(listing, listing_length, "libx265")) {
		if (!make_source(tool, "source-hdr10.mkv", hdr10_middle, &hdr10, &hdr10_why, error))
			goto out;
	} else {
		hdr10_why = g_strdup("no HDR10 test source can be made: this ffmpeg has no libx265 to write its metadata");
	}

	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisConvertEncoder *encoder = &table[i];
		g_autofree gchar *why = NULL;
		gboolean cancelled = FALSE;

		if (axis_encoder_capabilities_get(caps, encoder)->measured ||
		    (tool->skip_families & AXIS_CONVERT_FAMILY_BIT(encoder->family)) != 0)
			continue;
		if (g_cancellable_is_cancelled(tool->cancellable)) {
			g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_CANCELLED, "the check was stopped");
			goto out;
		}
		/* 8 bits. */
		if (sdr_why != NULL) {
			set_reason(caps, encoder, FALSE, FALSE, FALSE, sdr_why);
			continue;
		}
		why = test_encode(tool, encoder, caps->tool_digest, 8, &sdr, "8-bit", &cancelled);
		if (cancelled) {
			g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_CANCELLED, "the check was stopped");
			goto out;
		}
		if (why != NULL) {
			set_reason(caps, encoder, FALSE, FALSE, FALSE, why);
			continue;
		}
		if (!encoder->ten_bit) {
			set_reason(caps, encoder, TRUE, FALSE, FALSE, NULL);
			continue;
		}
		/* 10 bits, HLG. */
		if (hlg_why != NULL) {
			set_reason(caps, encoder, TRUE, FALSE, FALSE, hlg_why);
			continue;
		}
		why = test_encode(tool, encoder, caps->tool_digest, 10, &hlg, "10-bit", &cancelled);
		if (cancelled) {
			g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_CANCELLED, "the check was stopped");
			goto out;
		}
		if (why != NULL) {
			set_reason(caps, encoder, TRUE, FALSE, FALSE, why);
			continue;
		}
		/* HDR10. */
		if (hdr10_why != NULL || hdr10.info == NULL) {
			set_reason(caps, encoder, TRUE, TRUE, FALSE, hdr10_why != NULL ? hdr10_why :
				"the HDR10 test source could not be made");
			continue;
		}
		why = test_encode(tool, encoder, caps->tool_digest, 10, &hdr10, "HDR10", &cancelled);
		if (cancelled) {
			g_set_error_literal(error, AXIS_CONVERT_ERROR, AXIS_CONVERT_ERROR_CANCELLED, "the check was stopped");
			goto out;
		}
		set_reason(caps, encoder, TRUE, TRUE, why == NULL, why);
	}
	ok = TRUE;

out:
	source_clear(&sdr);
	source_clear(&hlg);
	source_clear(&hdr10);
	return ok ? g_steal_pointer(&caps) : NULL;
}

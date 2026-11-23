/*
 * fuzz-profile.c - libFuzzer target for conversion profile definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every input is tried two ways:
 *
 *  - as a definition's text (stored rows and forms are outside input):
 *    the parser refuses it with at least one problem, each with a message,
 *    or accepts it, and then its canonical text parses again to the same
 *    definition and the same canonical text (a fixed point), its hash is
 *    64 hex digits, a target can be made of it, and a definition always
 *    carries its own proof;
 *  - as the bytes of a generated valid definition and an arbitrary file:
 *    whatever the definition, a file a fixed rule covers is never eligible
 *    and has that rule's reason, and whatever converts is estimated
 *    strictly smaller by at least the floor (fuzz-profile-gen.h).
 *
 * A broken property is printed before the abort.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz-profile-gen.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
broken(
	const gchar *what,
	const gchar *text
){
	g_printerr("fuzz-profile: %s: %s\n", what, text != NULL ? text : "(none)");
	abort();
}

static void
as_text(
	const uint8_t *data,
	size_t         size
){
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	AxisProfileDefinition def;
	AxisProfileDefinition again;
	AxisEstimateTarget target;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *hash = NULL;
	guint i;

	if (!axis_profile_definition_parse((const gchar *)data, (gssize)size, &def, problems)) {
		if (problems->len == 0)
			broken("refused without a problem", NULL);
		for (i = 0; i < problems->len; i++) {
			const AxisProfileProblem *p = g_ptr_array_index(problems, i);

			if (p->message == NULL || p->message[0] == '\0' || p->field == NULL)
				broken("a problem without a message", NULL);
		}
		return;
	}
	if (problems->len != 0)
		broken("accepted with a problem", NULL);
	first = axis_profile_definition_canonical(&def);
	if (!axis_profile_definition_parse(first, -1, &again, NULL))
		broken("the canonical text of a valid definition is refused", first);
	second = axis_profile_definition_canonical(&again);
	if (strcmp(first, second) != 0)
		broken("the canonical text is not a fixed point", second);
	if (memcmp(&def, &again, sizeof(def)) != 0)
		broken("parsing the canonical text gives another definition", first);
	if (strlen(first) > AXIS_PROFILE_MAX_TEXT)
		broken("the canonical text is longer than the format allows", first);
	hash = axis_profile_hash(first);
	if (strlen(hash) != AXIS_PROFILE_HASH_LENGTH)
		broken("the hash is not 64 hex digits", hash);
	if (!axis_profile_to_target(&def, NULL, &target))
		broken("no target from a valid definition", first);
	if (!axis_profile_carries_proof(&def, &again, NULL))
		broken("a definition does not carry its own proof", first);
}

static void
as_generated(
	const uint8_t *data,
	size_t         size
){
	static AxisEstimateModel *model;
	AxisGenInput in = { data, size, 0, NULL };
	AxisProfileDefinition def;
	AxisEstimateTarget built;
	AxisEstimateTarget target;
	AxisEstimateConfig config;
	AxisEstimateFile files[3];
	AxisMediaInfo *infos[3];
	const AxisEstimateFile *unit[3];
	AxisEstimateResult results[3];
	g_autofree gchar *canonical = NULL;
	guint n;
	guint i;

	if (model == NULL)
		model = axis_estimate_model_new();
	axis_gen_definition(&in, &def);
	canonical = axis_profile_definition_canonical(&def);
	if (!axis_profile_definition_parse(canonical, -1, NULL, NULL))
		broken("a generated definition is refused", canonical);
	if (!axis_profile_to_target(&def, NULL, &built))
		broken("no target from a generated definition", canonical);
	axis_estimate_config_init(&config);
	config.encoder = (AxisEncoderFamily)axis_gen_pick(&in, AXIS_ENCODER_COUNT);
	axis_estimate_target_configure(&built, &config, &target);

	n = 1 + axis_gen_pick(&in, 3);
	for (i = 0; i < n; i++) {
		infos[i] = axis_gen_file(&in, &files[i]);
		files[i].index.file_id = i + 1;
		files[i].index.unit_files = n;
		unit[i] = &files[i];
	}
	axis_estimate_evaluate(unit, n, &target, model, results);
	for (i = 0; i < n; i++)
		axis_gen_check_decision(&files[i], &target, &results[i], n > 1 && target.media == AXIS_TARGET_AUDIO);
	if (target.media == AXIS_TARGET_AUDIO)
		axis_gen_check_album(&target, results, n);
	/* One file alone, as a conversion would ask. */
	{
		AxisEstimateResult one;

		files[0].index.unit_files = 1;
		axis_estimate_eligibility(&files[0], &target, model, &one);
		axis_gen_check_decision(&files[0], &target, &one, FALSE);
	}
	for (i = 0; i < n; i++)
		axis_media_info_free(infos[i]);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	as_text(data, size);
	as_generated(data, size);
	return 0;
}

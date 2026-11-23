/*
 * fuzz-def-yaml.c - libFuzzer target for the YAML subset reader of indexer
 * definitions (indexer/def/axis-def-yaml.h) and the judging of a
 * definition (indexer/def/axis-def.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a file.  Whatever it is, the code returns without crashing
 * or allocating without bound, and:
 *
 *  - the reader gives a tree or a reason, never both; a reason about the
 *    text names its line;
 *  - a tree holds no more than AXIS_YAML_MAX_NODES nodes nested no deeper
 *    than AXIS_YAML_MAX_DEPTH; every scalar and key is valid UTF-8 without
 *    NUL of at most AXIS_YAML_MAX_SCALAR bytes; a map's keys are distinct;
 *  - a definition, when there is one, has an id of 1 to 64 of the allowed
 *    characters, a name, links, and is usable exactly when it lists no
 *    need; its reason is a sentence for a page (valid, no control
 *    characters) exactly when it is not usable.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-yaml.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fail(int line)
{
	fprintf(stderr, "fuzz-def-yaml: the check at line %d failed\n", line);
	abort();
}

static void
check_text(
	const gchar *text,
	gsize        max
){
	if (text == NULL || !g_utf8_validate(text, -1, NULL) || strlen(text) > max)
		fail(__LINE__);
}

static guint
check_node(
	const AxisYamlNode *node,
	guint               depth
){
	guint count = 1;
	guint i;
	guint k;

	if (node == NULL || depth > AXIS_YAML_MAX_DEPTH + 1)
		fail(__LINE__);
	switch (node->kind) {
	case AXIS_YAML_SCALAR:
		check_text(node->text, AXIS_YAML_MAX_SCALAR);
		if (node->keys != NULL || node->values != NULL)
			fail(__LINE__);
		return 1;
	case AXIS_YAML_MAP:
		if (node->keys == NULL || node->values == NULL || node->keys->len != node->values->len)
			fail(__LINE__);
		for (i = 0; i < node->keys->len; i++) {
			check_text(g_ptr_array_index(node->keys, i), AXIS_YAML_MAX_SCALAR);
			for (k = 0; k < i; k++) {
				if (strcmp(g_ptr_array_index(node->keys, i), g_ptr_array_index(node->keys, k)) == 0)
					fail(__LINE__);
			}
		}
		break;
	case AXIS_YAML_SEQ:
		if (node->keys != NULL || node->values == NULL)
			fail(__LINE__);
		break;
	default:
		fail(__LINE__);
	}
	for (i = 0; i < node->values->len; i++)
		count += check_node(g_ptr_array_index(node->values, i), depth + 1);
	return count;
}

static void
check_sentence(const gchar *text)
{
	const gchar *p;

	check_text(text, 8192);
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20)
			fail(__LINE__);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text = (const gchar *)data;
	gchar *problem = NULL;
	AxisYamlNode *root;
	AxisDef *def;
	guint i;

	if (size > AXIS_DEF_MAX_BYTES + 16)
		return 0;
	root = axis_yaml_parse(text, size, &problem);
	if (root != NULL) {
		if (problem != NULL)
			fail(__LINE__);
		if (check_node(root, 1) > AXIS_YAML_MAX_NODES)
			fail(__LINE__);
		axis_yaml_node_free(root);
	} else {
		if (problem == NULL)
			fail(__LINE__);
		check_sentence(problem);
		g_free(problem);
		problem = NULL;
	}

	def = axis_def_load(text, size, &problem);
	if (def == NULL) {
		if (problem == NULL)
			fail(__LINE__);
		check_sentence(problem);
		g_free(problem);
		return 0;
	}
	if (problem != NULL || def->id == NULL || def->id[0] == '\0' || strlen(def->id) > 64 ||
	    strspn(def->id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != strlen(def->id))
		fail(__LINE__);
	check_sentence(def->name);
	if (def->links->len == 0)
		fail(__LINE__);
	for (i = 0; i < def->needs->len; i++)
		check_sentence(((const AxisDefNeed *)g_ptr_array_index(def->needs, i))->what);
	problem = axis_def_problem(def);
	if (axis_def_usable(def) != (def->needs->len == 0) || axis_def_usable(def) != (problem == NULL))
		fail(__LINE__);
	if (problem != NULL)
		check_sentence(problem);
	g_free(problem);
	axis_def_free(def);
	return 0;
}

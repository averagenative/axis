/*
 * fuzz-library-name.c - libFuzzer target for the file name codec, the
 * text shown for names, the kind and ignore rules, and the pattern matcher
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  a name (any bytes but NUL and '/'): its stored form is valid UTF-8
 *     and decodes to exactly the name; it is the name itself exactly when
 *     the name is valid UTF-8 without a backslash; what is shown of it has
 *     no control character and is valid UTF-8.
 *  1  any text as a stored form: if it decodes, encoding the result gives
 *     it back (one canonical form per name).
 *  2  a pattern and a name: the matcher agrees with a slow reference.
 *  3  a path: its stored form is its segments' forms joined by '/'.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_shown(const gchar *text)
{
	const guchar *p;

	if (!g_utf8_validate(text, -1, NULL))
		abort();
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			abort();
		/* No C1 control and no direction override, raw. */
		if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f)
			abort();
		if (p[0] == 0xe2 && p[1] == 0x80 && p[2] >= 0xaa && p[2] <= 0xae)
			abort();
	}
}

static void
check_name(
	const gchar *name,
	gsize        length
){
	GString *stored = g_string_new(NULL);
	GString *back = NULL;
	gboolean valid = axis_library_name_encode(stored, name, length);
	gchar *shown;
	gsize i;

	if (!g_utf8_validate(stored->str, (gssize)stored->len, NULL) || strlen(stored->str) != stored->len)
		abort();
	if (valid != axis_library_utf8_valid(name, length))
		abort();
	if ((valid && memchr(name, '\\', length) == NULL) !=
	    (stored->len == length && memcmp(stored->str, name, length) == 0))
		abort();
	for (i = 0; i < stored->len; i++) {
		if (stored->str[i] == '/')
			abort();
	}
	if (!axis_library_name_decode(stored->str, &back) || back->len != length ||
	    memcmp(back->str, name, length) != 0)
		abort();
	if (axis_library_name_is_invalid(stored->str) != !valid)
		abort();

	shown = axis_library_name_display(stored->str);
	check_shown(shown);
	g_free(shown);
	shown = axis_library_name_exact(stored->str);
	check_shown(shown);
	g_free(shown);
	shown = axis_library_text_clean(stored->str, 50);
	check_shown(shown);
	if (g_utf8_strlen(shown, -1) > 50)
		abort();
	g_free(shown);

	(void)axis_file_kind_from_name(name, length);
	(void)axis_library_ignore_builtin(name, length, TRUE);
	(void)axis_library_ignore_builtin(name, length, FALSE);

	g_string_free(back, TRUE);
	g_string_free(stored, TRUE);
}

/* ── The slow reference matcher ─────────────────────────────────── */

#define REF_PATTERN_MAX 16
#define REF_NAME_MAX    32

typedef struct
{
	const guchar *pattern;
	gsize         plen;
	const guchar *name;
	gsize         nlen;
	gint8         memo[REF_PATTERN_MAX + 1][REF_NAME_MAX + 1];
} Ref;

static gsize
char_len(
	const guchar *p,
	gsize         left
){
	gsize n = axis_library_utf8_sequence(p, left);

	return n > 0 ? n : 1;
}

static gboolean
ref_match(
	Ref   *ref,
	gsize  pi,
	gsize  ni
){
	gboolean result;

	if (ref->memo[pi][ni] != 0)
		return ref->memo[pi][ni] > 0;

	if (pi == ref->plen) {
		result = ni == ref->nlen;
	} else if (ref->pattern[pi] == '*') {
		result = ref_match(ref, pi + 1, ni) ||
			(ni < ref->nlen && ref_match(ref, pi, ni + char_len(ref->name + ni, ref->nlen - ni)));
	} else if (ni == ref->nlen) {
		result = FALSE;
	} else if (ref->pattern[pi] == '?') {
		result = ref_match(ref, pi + 1, ni + char_len(ref->name + ni, ref->nlen - ni));
	} else {
		gsize at = ref->pattern[pi] == '\\' && pi + 1 < ref->plen ? pi + 1 : pi;
		gsize pc = char_len(ref->pattern + at, ref->plen - at);
		gsize nc = char_len(ref->name + ni, ref->nlen - ni);

		result = pc == nc && memcmp(ref->pattern + at, ref->name + ni, pc) == 0 &&
			ref_match(ref, at + pc, ni + nc);
	}

	ref->memo[pi][ni] = result ? 1 : -1;
	return result;
}

static void
check_glob(
	const guchar *data,
	gsize         size
){
	gsize plen;
	gchar pattern[REF_PATTERN_MAX + 1];
	Ref ref;
	gsize i;

	if (size == 0)
		return;
	plen = data[0] % (REF_PATTERN_MAX + 1);
	data++;
	size--;
	plen = MIN(plen, size);
	for (i = 0; i < plen; i++) {
		/* The pattern is a C string. */
		if (data[i] == '\0')
			return;
		pattern[i] = (gchar)data[i];
	}
	pattern[plen] = '\0';

	memset(&ref, 0, sizeof(ref));
	ref.pattern = (const guchar *)pattern;
	ref.plen = plen;
	ref.name = data + plen;
	ref.nlen = MIN(size - plen, (gsize)REF_NAME_MAX);

	if (axis_library_glob_match(pattern, (const gchar *)ref.name, ref.nlen) != ref_match(&ref, 0, 0))
		abort();
	(void)axis_library_pattern_check(pattern, NULL);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *rest;
	gsize length;

	if (size == 0)
		return 0;
	rest = (const gchar *)data + 1;
	length = size - 1;

	switch (data[0] % 4) {
	case 0: {
		/* A name: no NUL, no '/'. */
		gchar *name = g_malloc(length + 1);
		gsize n = 0;
		gsize i;

		for (i = 0; i < length; i++) {
			if (rest[i] != '\0' && rest[i] != '/')
				name[n++] = rest[i];
		}
		name[n] = '\0';
		check_name(name, n);
		g_free(name);
		break;
	}
	case 1: {
		gchar *text = g_strndup(rest, length);
		GString *bytes = NULL;

		if (axis_library_name_decode(text, &bytes)) {
			GString *again = g_string_new(NULL);

			if (memchr(bytes->str, '\0', bytes->len) != NULL)
				abort();
			axis_library_name_encode(again, bytes->str, bytes->len);
			if (strcmp(again->str, text) != 0)
				abort();
			g_string_free(again, TRUE);
			g_string_free(bytes, TRUE);
		}
		g_free(axis_library_name_display(text));
		g_free(axis_library_name_exact(text));
		g_free(text);
		break;
	}
	case 2:
		check_glob((const guchar *)rest, length);
		break;
	default: {
		/* A path: the whole is the parts. */
		gchar *path = g_strndup(rest, length);
		gchar **segments = g_strsplit(path, "/", -1);
		GString *whole = g_string_new(NULL);
		GString *parts = g_string_new(NULL);
		guint i;

		axis_library_name_encode(whole, path, strlen(path));
		for (i = 0; segments[i] != NULL; i++) {
			if (i > 0)
				g_string_append_c(parts, '/');
			axis_library_name_encode(parts, segments[i], strlen(segments[i]));
		}
		if (strcmp(whole->str, parts->str) != 0)
			abort();
		g_string_free(whole, TRUE);
		g_string_free(parts, TRUE);
		g_strfreev(segments);
		g_free(path);
		break;
	}
	}

	return 0;
}

/*
 * axis-download-bencode.c - a minimal, strict bencode reader for the
 * info-hash of a .torrent
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-bencode.h"

#include <string.h>

#include "download/axis-download-model.h"

/* The longest integer read, in digits (a 64-bit number has 20). */
#define MAX_INTEGER_DIGITS 32

/* The longest string length, in digits (more than any length that fits). */
#define MAX_LENGTH_DIGITS  10

typedef struct
{
	const guint8 *data;
	gsize         length;
	gsize         pos;
	guint64       items;
	const gchar  *problem;
} Reader;

static gboolean
refuse(
	Reader      *r,
	const gchar *problem
){
	if (r->problem == NULL)
		r->problem = problem;
	return FALSE;
}

static gboolean
at_end(const Reader *r)
{
	return r->pos >= r->length;
}

static guint8
peek(const Reader *r)
{
	return r->data[r->pos];
}

/* i<digits>e: no leading zero, no "-0". */
static gboolean
read_integer(Reader *r)
{
	gsize digits = 0;
	gboolean negative = FALSE;
	gboolean leading_zero = FALSE;

	r->pos++;	/* 'i' */
	if (!at_end(r) && peek(r) == '-') {
		negative = TRUE;
		r->pos++;
	}
	while (!at_end(r) && g_ascii_isdigit(peek(r))) {
		if (digits == 0 && peek(r) == '0')
			leading_zero = TRUE;
		digits++;
		r->pos++;
		if (digits > MAX_INTEGER_DIGITS)
			return refuse(r, "it holds a number that is too long");
	}
	if (digits == 0 || at_end(r) || peek(r) != 'e')
		return refuse(r, "it holds a number that is not written as bencode");
	if (leading_zero && (digits > 1 || negative))
		return refuse(r, "it holds a number with a leading zero");
	r->pos++;	/* 'e' */
	return TRUE;
}

/* <length>:<bytes> */
static gboolean
read_string(
	Reader        *r,
	const guint8 **start,
	gsize         *n
){
	guint64 length = 0;
	gsize digits = 0;
	gboolean leading_zero = FALSE;

	while (!at_end(r) && g_ascii_isdigit(peek(r))) {
		if (digits == 0 && peek(r) == '0')
			leading_zero = TRUE;
		length = length * 10 + (guint64)(peek(r) - '0');
		digits++;
		r->pos++;
		if (digits > MAX_LENGTH_DIGITS)
			return refuse(r, "it holds a string whose length is too long to be true");
	}
	if (digits == 0 || at_end(r) || peek(r) != ':')
		return refuse(r, "it holds a string that is not written as bencode");
	if (leading_zero && digits > 1)
		return refuse(r, "it holds a string length with a leading zero");
	r->pos++;	/* ':' */
	if (length > r->length - r->pos)
		return refuse(r, "it is cut short");
	if (start != NULL)
		*start = r->data + r->pos;
	if (n != NULL)
		*n = (gsize)length;
	r->pos += (gsize)length;
	return TRUE;
}

static gboolean
skip_value(
	Reader *r,
	guint   depth
);

static gboolean
count_item(Reader *r)
{
	if (++r->items > AXIS_BENCODE_MAX_ITEMS)
		return refuse(r, "it holds too many items");
	return TRUE;
}

static gboolean
skip_list(
	Reader *r,
	guint   depth
){
	r->pos++;	/* 'l' */
	for (;;) {
		if (at_end(r))
			return refuse(r, "it is cut short");
		if (peek(r) == 'e') {
			r->pos++;
			return TRUE;
		}
		if (!count_item(r) || !skip_value(r, depth + 1))
			return FALSE;
	}
}

static gboolean
skip_dict(
	Reader *r,
	guint   depth
){
	r->pos++;	/* 'd' */
	for (;;) {
		if (at_end(r))
			return refuse(r, "it is cut short");
		if (peek(r) == 'e') {
			r->pos++;
			return TRUE;
		}
		if (!g_ascii_isdigit(peek(r)))
			return refuse(r, "it holds a dictionary whose key is not a string");
		if (!count_item(r) || !read_string(r, NULL, NULL))
			return FALSE;
		if (at_end(r))
			return refuse(r, "it is cut short");
		if (!skip_value(r, depth + 1))
			return FALSE;
	}
}

static gboolean
skip_value(
	Reader *r,
	guint   depth
){
	if (depth > AXIS_BENCODE_MAX_DEPTH)
		return refuse(r, "it is nested too deeply");
	if (at_end(r))
		return refuse(r, "it is cut short");
	switch (peek(r)) {
	case 'i':
		return read_integer(r);
	case 'l':
		return skip_list(r, depth);
	case 'd':
		return skip_dict(r, depth);
	default:
		if (g_ascii_isdigit(peek(r)))
			return read_string(r, NULL, NULL);
		return refuse(r, "it is not bencode");
	}
}

gboolean
axis_bencode_valid(
	const guint8 *data,
	gsize         length
){
	Reader r;

	if (data == NULL || length == 0)
		return FALSE;
	memset(&r, 0, sizeof(r));
	r.data = data;
	r.length = length;
	return skip_value(&r, 0) && r.pos == r.length;
}

static gboolean
key_is(
	const guint8 *key,
	gsize         n,
	const gchar  *name
){
	return n == strlen(name) && memcmp(key, name, n) == 0;
}

/* The info dictionary's "name", when it is a string; the dictionary itself
 * has been checked already. */
static gchar *
info_name(
	const guint8 *data,
	gsize         length
){
	Reader r;

	memset(&r, 0, sizeof(r));
	r.data = data;
	r.length = length;
	r.pos = 1;	/* 'd' */
	while (!at_end(&r) && peek(&r) != 'e') {
		const guint8 *key = NULL;
		gsize n = 0;

		if (!read_string(&r, &key, &n) || at_end(&r))
			return NULL;
		if (key_is(key, n, "name") && g_ascii_isdigit(peek(&r))) {
			const guint8 *value = NULL;
			gsize size = 0;
			g_autofree gchar *text = NULL;

			if (!read_string(&r, &value, &size) || size > 4096)
				return NULL;
			text = g_strndup((const gchar *)value, size);
			if (strlen(text) != size || !g_utf8_validate(text, -1, NULL))
				return NULL;
			return axis_download_text_clean(text, AXIS_DOWNLOAD_NAME_MAX_CHARS);
		}
		if (!skip_value(&r, 1))
			return NULL;
	}
	return NULL;
}

gboolean
axis_download_torrent_read(
	const guint8  *data,
	gsize          length,
	gchar        **info_hash,
	gchar        **name,
	const gchar  **problem
){
	Reader r;
	gsize info_start = 0;
	gsize info_end = 0;
	gboolean have_info = FALSE;

	if (info_hash != NULL)
		*info_hash = NULL;
	if (name != NULL)
		*name = NULL;
	memset(&r, 0, sizeof(r));
	r.data = data;
	r.length = data != NULL ? length : 0;
	if (r.length == 0 || peek(&r) != 'd') {
		if (problem != NULL)
			*problem = "it is not a torrent file (it does not start with a dictionary)";
		return FALSE;
	}
	r.pos = 1;
	for (;;) {
		const guint8 *key = NULL;
		gsize n = 0;
		gsize start;

		if (at_end(&r)) {
			refuse(&r, "it is cut short");
			break;
		}
		if (peek(&r) == 'e') {
			r.pos++;
			break;
		}
		if (!g_ascii_isdigit(peek(&r))) {
			refuse(&r, "it holds a dictionary whose key is not a string");
			break;
		}
		if (!count_item(&r) || !read_string(&r, &key, &n))
			break;
		if (at_end(&r)) {
			refuse(&r, "it is cut short");
			break;
		}
		start = r.pos;
		if (!skip_value(&r, 1))
			break;
		if (key_is(key, n, "info")) {
			if (have_info) {
				refuse(&r, "it has two info dictionaries");
				break;
			}
			if (data[start] != 'd') {
				refuse(&r, "its info is not a dictionary");
				break;
			}
			have_info = TRUE;
			info_start = start;
			info_end = r.pos;
		}
	}
	if (r.problem == NULL && r.pos != r.length)
		refuse(&r, "it has bytes after its end");
	if (r.problem == NULL && !have_info)
		refuse(&r, "it has no info dictionary");
	if (r.problem != NULL) {
		if (problem != NULL)
			*problem = r.problem;
		return FALSE;
	}
	if (info_hash != NULL)
		*info_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA1, data + info_start, info_end - info_start);
	if (name != NULL)
		*name = info_name(data + info_start, info_end - info_start);
	return TRUE;
}

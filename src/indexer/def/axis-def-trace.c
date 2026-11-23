/*
 * axis-def-trace.c - the account of how a definition read a site's answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/def/axis-def-trace.h"

#include <string.h>

#include "indexer/axis-indexer-model.h"

/* The most characters of a path or a verdict kept. */
#define LINE_CHARS 600

static void
field_free(AxisDefTraceField *field)
{
	if (field == NULL)
		return;
	g_free(field->name);
	g_free(field->how);
	g_free(field->read);
	g_free(field->value);
	g_free(field->note);
	g_free(field);
}

static void
row_free(AxisDefTraceRow *row)
{
	if (row == NULL)
		return;
	g_clear_pointer(&row->fields, g_ptr_array_unref);
	g_free(row->fate);
	g_free(row->excerpt);
	g_free(row);
}

static void
answer_free(AxisDefTraceAnswer *answer)
{
	if (answer == NULL)
		return;
	g_free(answer->path);
	g_free(answer->kind);
	g_free(answer->expect);
	g_free(answer->rows_selector);
	g_free(answer->verdict);
	g_free(answer->excerpt);
	g_clear_pointer(&answer->rows, g_ptr_array_unref);
	g_free(answer);
}

AxisDefTrace *
axis_def_trace_new(gboolean excerpts)
{
	AxisDefTrace *trace = g_new0(AxisDefTrace, 1);

	trace->excerpts = excerpts;
	trace->requests = g_ptr_array_new_with_free_func(g_free);
	trace->answers = g_ptr_array_new_with_free_func((GDestroyNotify)answer_free);
	return trace;
}

void
axis_def_trace_free(AxisDefTrace *trace)
{
	if (trace == NULL)
		return;
	g_free(trace->definition);
	g_free(trace->words);
	g_clear_pointer(&trace->requests, g_ptr_array_unref);
	g_clear_pointer(&trace->answers, g_ptr_array_unref);
	g_free(trace->outcome);
	g_free(trace->download);
	g_free(trace);
}

gchar *
axis_def_trace_text(
	const gchar *text,
	guint        max_chars
){
	g_autofree gchar *clean = axis_indexer_text_clean(text, max_chars + 1);
	const gchar *end;

	if (clean == NULL)
		return g_strdup("");
	if ((guint)g_utf8_strlen(clean, -1) <= max_chars)
		return g_steal_pointer(&clean);
	end = g_utf8_offset_to_pointer(clean, max_chars);
	return g_strdup_printf("%.*s...", (int)(end - clean), clean);
}

AxisDefTraceAnswer *
axis_def_trace_add_answer(
	AxisDefTrace *trace,
	const gchar  *path,
	gsize         length
){
	AxisDefTraceAnswer *answer;

	if (trace == NULL || trace->answers->len >= AXIS_DEF_TRACE_MAX_ANSWERS)
		return NULL;
	answer = g_new0(AxisDefTraceAnswer, 1);
	answer->path = axis_def_trace_text(path, LINE_CHARS);
	answer->length = length;
	answer->kind = g_strdup("");
	answer->rows = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	g_ptr_array_add(trace->answers, answer);
	return answer;
}

AxisDefTraceRow *
axis_def_trace_add_row(
	AxisDefTraceAnswer *answer,
	guint               number
){
	AxisDefTraceRow *row;

	if (answer == NULL || answer->rows->len >= AXIS_DEF_TRACE_ROWS)
		return NULL;
	row = g_new0(AxisDefTraceRow, 1);
	row->number = number;
	row->fields = g_ptr_array_new_with_free_func((GDestroyNotify)field_free);
	g_ptr_array_add(answer->rows, row);
	return row;
}

void
axis_def_trace_add_field(
	AxisDefTraceRow *row,
	const gchar     *name,
	const gchar     *how,
	const gchar     *read,
	const gchar     *value,
	const gchar     *note
){
	AxisDefTraceField *field;

	if (row == NULL || row->fields->len >= 128)
		return;
	field = g_new0(AxisDefTraceField, 1);
	field->name = axis_def_trace_text(name, 64);
	field->how = axis_def_trace_text(how, AXIS_DEF_TRACE_VALUE_CHARS);
	field->read = read != NULL ? axis_def_trace_text(read, AXIS_DEF_TRACE_VALUE_CHARS) : NULL;
	field->value = value != NULL ? axis_def_trace_text(value, AXIS_DEF_TRACE_VALUE_CHARS) : NULL;
	field->note = note != NULL ? axis_def_trace_text(note, AXIS_DEF_TRACE_VALUE_CHARS) : NULL;
	g_ptr_array_add(row->fields, field);
}

AxisDefTraceAnswer *
axis_def_trace_last(AxisDefTrace *trace)
{
	if (trace == NULL || trace->answers->len == 0)
		return NULL;
	return g_ptr_array_index(trace->answers, trace->answers->len - 1);
}

void
axis_def_trace_add_failure(
	AxisDefTrace *trace,
	const gchar  *path,
	guint         status,
	const gchar  *why
){
	AxisDefTraceAnswer *answer = axis_def_trace_add_answer(trace, path, 0);

	if (answer == NULL)
		return;
	answer->status = status;
	answer->verdict = axis_def_trace_text(why != NULL ? why : "no answer", LINE_CHARS);
}

void
axis_def_trace_set_outcome(
	AxisDefTrace *trace,
	const gchar  *outcome
){
	if (trace == NULL)
		return;
	g_free(trace->outcome);
	trace->outcome = axis_def_trace_text(outcome, LINE_CHARS);
}

/* "  label: value" when there is a value. */
static void
line(
	GString     *out,
	guint        depth,
	const gchar *label,
	const gchar *value
){
	guint i;

	if (value == NULL || value[0] == '\0')
		return;
	for (i = 0; i < depth; i++)
		g_string_append(out, "  ");
	g_string_append_printf(out, "%s: %s\n", label, value);
}

/* An excerpt, each of its lines indented. */
static void
block(
	GString     *out,
	guint        depth,
	const gchar *label,
	const gchar *text
){
	g_auto(GStrv) lines = NULL;
	guint i;
	guint k;

	if (text == NULL || text[0] == '\0')
		return;
	for (k = 0; k < depth; k++)
		g_string_append(out, "  ");
	g_string_append_printf(out, "%s:\n", label);
	lines = g_strsplit(text, "\n", 200);
	for (i = 0; lines[i] != NULL; i++) {
		for (k = 0; k < depth + 1; k++)
			g_string_append(out, "  ");
		g_string_append_printf(out, "| %s\n", lines[i]);
	}
}

gchar *
axis_def_trace_to_text(const AxisDefTrace *trace)
{
	GString *out = g_string_new(NULL);
	guint i;
	guint k;
	guint f;

	if (trace == NULL)
		return g_string_free(out, FALSE);
	line(out, 0, "Definition", trace->definition);
	line(out, 0, "Words", trace->words);
	for (i = 0; i < trace->requests->len; i++)
		line(out, 0, "Request", g_ptr_array_index(trace->requests, i));
	for (i = 0; i < trace->answers->len; i++) {
		const AxisDefTraceAnswer *answer = g_ptr_array_index(trace->answers, i);
		g_autofree gchar *size = g_strdup_printf("%" G_GSIZE_FORMAT " bytes%s%s", answer->length,
			answer->kind[0] != '\0' ? ", read as " : "", answer->kind);
		g_autofree gchar *status = answer->status != 0 ? g_strdup_printf("%u", answer->status) : NULL;
		g_autofree gchar *counts = NULL;

		g_string_append_printf(out, "Answer %u\n", i + 1);
		line(out, 1, "Path", answer->path);
		line(out, 1, "Status", status);
		line(out, 1, "Size", size);
		if (answer->challenge)
			line(out, 1, "Challenge", "the answer is a browser challenge's page, not the site's");
		if (answer->expect != NULL) {
			g_autofree gchar *expect = g_strdup_printf("%s: %s", answer->expect, answer->expect_found ? "found" :
				"NOT FOUND");

			line(out, 1, "Every answer has", expect);
		}
		if (answer->rows_selector != NULL) {
			g_autofree gchar *rows = g_strdup_printf("%s: matched %u", answer->rows_selector, answer->rows_matched);

			line(out, 1, "Rows", rows);
			counts = g_strdup_printf("%u kept, %u lost for a field every result must have, %u left out on purpose "
				"(%u not video)", answer->kept, answer->lost, answer->left_out, answer->not_video);
			line(out, 1, "Of them", counts);
		}
		line(out, 1, "Verdict", answer->verdict);
		block(out, 1, "Excerpt of the page", answer->excerpt);
		for (k = 0; k < answer->rows->len; k++) {
			const AxisDefTraceRow *row = g_ptr_array_index(answer->rows, k);

			g_string_append_printf(out, "  Row %u: %s\n", row->number, row->fate != NULL ? row->fate : "");
			for (f = 0; f < row->fields->len; f++) {
				const AxisDefTraceField *field = g_ptr_array_index(row->fields, f);
				GString *text = g_string_new(field->how);

				if (field->read != NULL && g_strcmp0(field->read, field->value) != 0)
					g_string_append_printf(text, " read \"%s\"", field->read);
				if (field->value != NULL)
					g_string_append_printf(text, " is \"%s\"", field->value);
				if (field->note != NULL)
					g_string_append_printf(text, " (%s)", field->note);
				line(out, 2, field->name, text->str);
				g_string_free(text, TRUE);
			}
			block(out, 2, "Excerpt of the row", row->excerpt);
		}
	}
	line(out, 0, "Download", trace->download);
	line(out, 0, "Outcome", trace->outcome);
	return g_string_free(out, FALSE);
}

/*
 * axis-compat-model.c - freeing the parsed structures, and the decision words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-model.h"

#include <string.h>

void
axis_compat_identity_clear(AxisCompatIdentity *identity)
{
	if (identity == NULL)
		return;
	g_clear_pointer(&identity->machine_id, g_free);
	g_clear_pointer(&identity->version, g_free);
	g_clear_pointer(&identity->name, g_free);
}

void
axis_compat_library_free(AxisCompatLibrary *library)
{
	if (library == NULL)
		return;
	g_free(library->key);
	g_free(library->kind);
	g_free(library->title);
	if (library->folders != NULL)
		g_ptr_array_unref(library->folders);
	g_free(library);
}

void
axis_compat_device_free(AxisCompatDevice *device)
{
	if (device == NULL)
		return;
	g_free(device->client_id);
	g_free(device->name);
	g_free(device->platform);
	g_free(device->product);
	g_free(device);
}

void
axis_compat_account_free(AxisCompatAccount *account)
{
	if (account == NULL)
		return;
	g_free(account->id);
	g_free(account->name);
	g_free(account);
}

void
axis_compat_play_free(AxisCompatPlay *play)
{
	if (play == NULL)
		return;
	g_free(play->account_id);
	g_free(play->library_key);
	g_free(play->rating_key);
	g_free(play);
}

void
axis_compat_history_clear(AxisCompatHistory *history)
{
	if (history == NULL)
		return;
	if (history->plays != NULL)
		g_ptr_array_unref(history->plays);
	history->plays = NULL;
}

void
axis_compat_stream_free(AxisCompatStream *stream)
{
	if (stream == NULL)
		return;
	g_free(stream->codec);
	g_free(stream->profile);
	g_free(stream);
}

void
axis_compat_session_free(AxisCompatSession *session)
{
	if (session == NULL)
		return;
	g_free(session->session_key);
	g_free(session->rating_key);
	g_free(session->library_key);
	g_free(session->item_type);
	g_free(session->device_client_id);
	g_free(session->device_name);
	g_free(session->platform);
	g_free(session->product);
	g_free(session->location);
	g_free(session->account_id);
	g_free(session->account_name);
	g_free(session->transcode.source_video_codec);
	g_free(session->transcode.source_audio_codec);
	g_free(session->transcode.video_codec);
	g_free(session->transcode.audio_codec);
	g_free(session->transcode.container);
	g_free(session->transcode.protocol);
	if (session->transcode.reasons != NULL)
		g_ptr_array_unref(session->transcode.reasons);
	g_free(session->container);
	g_free(session->file);
	if (session->streams != NULL)
		g_ptr_array_unref(session->streams);
	g_free(session);
}

void
axis_compat_item_clear(AxisCompatItem *item)
{
	if (item == NULL)
		return;
	g_clear_pointer(&item->file, g_free);
}

AxisCompatDecision
axis_compat_decision_from_string(const gchar *word)
{
	if (word == NULL || word[0] == '\0')
		return AXIS_COMPAT_DECISION_NONE;
	if (g_ascii_strcasecmp(word, "directplay") == 0)
		return AXIS_COMPAT_DECISION_DIRECT;
	if (g_ascii_strcasecmp(word, "copy") == 0)
		return AXIS_COMPAT_DECISION_COPY;
	/* A subtitle burned into the picture is a conversion. */
	if (g_ascii_strcasecmp(word, "transcode") == 0 || g_ascii_strcasecmp(word, "burn") == 0)
		return AXIS_COMPAT_DECISION_TRANSCODE;
	return AXIS_COMPAT_DECISION_UNKNOWN;
}

const gchar *
axis_compat_decision_to_string(AxisCompatDecision decision)
{
	switch (decision) {
	case AXIS_COMPAT_DECISION_DIRECT:
		return "direct";
	case AXIS_COMPAT_DECISION_COPY:
		return "copy";
	case AXIS_COMPAT_DECISION_TRANSCODE:
		return "transcode";
	case AXIS_COMPAT_DECISION_UNKNOWN:
		return "unknown";
	default:
		return "none";
	}
}

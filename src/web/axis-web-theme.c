/*
 * axis-web-theme.c - the built-in colour themes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-theme.h"

#include <string.h>

/*
 * theme_color is each theme's --surface in data/web/themes.css: the
 * header's colour, which is what the browser's own bar should match.
 */
static const AxisWebTheme themes[] = {
	{ "oxblood", "Oxblood",
	  "Near-black with a faint red cast; deep reds for the brand, teal for actions",
	  AXIS_WEB_THEME_DARK, "dark", "#170b0b", NULL },
	{ "dusk", "Dusk", "Deep navy, with a sky-blue accent",
	  AXIS_WEB_THEME_DARK, "dark", "#101e37", NULL },
	{ "forest", "Forest", "Deep green, with a pale sky accent",
	  AXIS_WEB_THEME_DARK, "dark", "#0b2918", NULL },
	{ "ember", "Ember", "Warm brown, with an orange accent",
	  AXIS_WEB_THEME_DARK, "dark", "#2a170b", NULL },
	{ "plum", "Plum", "Deep violet, with a pink-violet accent",
	  AXIS_WEB_THEME_DARK, "dark", "#1f122d", NULL },
	{ "midnight", "Midnight", "True black for OLED screens, with a cool accent",
	  AXIS_WEB_THEME_DARK, "dark", "#080b0e", NULL },
	{ "graphite", "Graphite", "Neutral greys with no colour cast and an amber accent",
	  AXIS_WEB_THEME_DARK, "dark", "#1e1e1e", NULL },
	{ "contrast", "High contrast", "Maximum contrast, for accessibility",
	  AXIS_WEB_THEME_DARK, "dark", "#101318", NULL },
	{ "paper", "Paper", "Light, on plain white, with a teal accent",
	  AXIS_WEB_THEME_LIGHT, "light", "#fdfcf9", NULL },
	{ "linen", "Linen", "Light, on warm off-white: the oxblood brand with deep teal actions",
	  AXIS_WEB_THEME_LIGHT, "light", "#fcf8f1", NULL },
	{ "auto", "Match my device", "Oxblood on a dark device, linen on a light one",
	  AXIS_WEB_THEME_AUTO, "light dark", "#170b0b", "#fcf8f1" }
};

const gchar *const axis_web_theme_ids[] = {
	"oxblood", "dusk", "forest", "ember", "plum", "midnight", "graphite", "contrast", "paper", "linen", "auto",
	NULL
};

/* The names above as the account page shows them, in the same order, for
 * the Default theme setting. */
const gchar *const axis_web_theme_labels[] = {
	"Oxblood", "Dusk", "Forest", "Ember", "Plum", "Midnight", "Graphite", "High contrast", "Paper", "Linen",
	"Match my device", NULL
};

static const AxisWebTheme *default_theme;

guint
axis_web_theme_count(void)
{
	return G_N_ELEMENTS(themes);
}

const AxisWebTheme *
axis_web_theme_at(guint index)
{
	g_return_val_if_fail(index < G_N_ELEMENTS(themes), NULL);

	return &themes[index];
}

const AxisWebTheme *
axis_web_theme_find(const gchar *id)
{
	guint i;

	if (id == NULL)
		return NULL;

	for (i = 0; i < G_N_ELEMENTS(themes); i++) {
		if (strcmp(themes[i].id, id) == 0)
			return &themes[i];
	}

	return NULL;
}

const AxisWebTheme *
axis_web_theme_get_default(void)
{
	return default_theme != NULL ? default_theme : axis_web_theme_find(AXIS_WEB_THEME_FALLBACK);
}

void
axis_web_theme_set_default(const gchar *id)
{
	const AxisWebTheme *theme = axis_web_theme_find(id);

	default_theme = theme != NULL ? theme : axis_web_theme_find(AXIS_WEB_THEME_FALLBACK);
}

const AxisWebTheme *
axis_web_theme_resolve(const gchar *stored)
{
	const AxisWebTheme *theme = axis_web_theme_find(stored);

	return theme != NULL ? theme : axis_web_theme_get_default();
}

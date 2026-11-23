/*
 * axis-web-theme.h - the built-in colour themes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A theme is a block of custom properties in data/web/themes.css selected
 * by the data-theme attribute the server writes on the page's root
 * element.  This table lists which themes exist, what to write in the
 * page's colour-scheme and theme-colour metas, and which one is the
 * default.  The colours themselves are only in the stylesheet; a test
 * checks that this table and the stylesheet agree.
 *
 * The default is the setting web.default_theme (followed by
 * axis_web_settings_sync()); it starts as "midnight".  Main thread only.
 */

#ifndef AXIS_WEB_THEME_H
#define AXIS_WEB_THEME_H

#include <glib.h>

G_BEGIN_DECLS

/* The theme used when nothing else says otherwise. */
#define AXIS_WEB_THEME_FALLBACK "midnight"

/**
 * AxisWebThemeKind:
 * @AXIS_WEB_THEME_DARK: a dark theme
 * @AXIS_WEB_THEME_LIGHT: a light theme
 * @AXIS_WEB_THEME_AUTO: dark or light as the device is set
 */
typedef enum
{
	AXIS_WEB_THEME_DARK,
	AXIS_WEB_THEME_LIGHT,
	AXIS_WEB_THEME_AUTO
} AxisWebThemeKind;

/**
 * AxisWebTheme:
 * @id: the id stored with an account and written in data-theme
 * @name: the name shown in the picker
 * @character: one line saying what the theme is like
 * @kind: dark, light or either
 * @color_scheme: the value of the color-scheme meta
 * @theme_color: the page's header colour, the browser's own bar colour
 *   (the theme's --surface); for an auto theme, the dark one
 * @theme_color_light: for an auto theme, the colour on a light device;
 *   %NULL for the others
 */
typedef struct
{
	const gchar      *id;
	const gchar      *name;
	const gchar      *character;
	AxisWebThemeKind  kind;
	const gchar      *color_scheme;
	const gchar      *theme_color;
	const gchar      *theme_color_light;
} AxisWebTheme;

guint
axis_web_theme_count(void);

/* Returns: (transfer none): the theme at @index, in the picker's order */
const AxisWebTheme *
axis_web_theme_at(guint index);

/* Returns: (transfer none) (nullable): the theme called @id, or %NULL
 * if this version has none */
const AxisWebTheme *
axis_web_theme_find(const gchar *id);

/* The ids, ended by %NULL: the choices of web.default_theme. */
extern const gchar *const axis_web_theme_ids[];
extern const gchar *const axis_web_theme_labels[];

/* Returns: (transfer none): the default theme (never %NULL) */
const AxisWebTheme *
axis_web_theme_get_default(void);

/* Sets the default theme; an id this version does not have sets the
 * fallback. */
void
axis_web_theme_set_default(const gchar *id);

/* Returns: (transfer none): the theme for an account that stored
 * @stored (%NULL for none): that one if it exists, else the default */
const AxisWebTheme *
axis_web_theme_resolve(const gchar *stored);

G_END_DECLS

#endif /* AXIS_WEB_THEME_H */

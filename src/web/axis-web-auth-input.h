/*
 * axis-web-auth-input.h - checks on what a client sends to the auth code
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure functions over text; depends on GLib only, so that the fuzz
 * targets can build it alone.  Everything here treats its input as
 * hostile and errs on the side of refusing.
 */

#ifndef AXIS_WEB_AUTH_INPUT_H
#define AXIS_WEB_AUTH_INPUT_H

#include <glib.h>

G_BEGIN_DECLS

/* Usernames: 1 to 64 of [a-z0-9._-], starting with a letter or digit. */
#define AXIS_USERNAME_MAX_LEN 64

/* The username rule, as the pages and start-up state it. */
#define AXIS_USERNAME_RULES "A username is 1 to 64 letters, digits, '.', '_' or '-', starting with a letter or digit."

/* Passwords: at least this many characters, at most this many bytes. */
#define AXIS_PASSWORD_MIN_CHARS 10
#define AXIS_PASSWORD_MAX_BYTES 1024

/* The longest "next" path that is honoured. */
#define AXIS_NEXT_MAX_LEN 2048

/* Longest Cookie header examined; a longer one finds nothing. */
#define AXIS_COOKIE_HEADER_MAX 16384

/* A user agent is kept to this many bytes for display. */
#define AXIS_USER_AGENT_MAX_LEN 200

/**
 * axis_cookie_find:
 * @header: (nullable): the request's Cookie header ("a=1; b=2"); several
 *   headers must be joined with "; " first
 * @name: the cookie name, compared exactly
 *
 * The header is split at ';', and each part, with spaces and tabs
 * trimmed, at its first '='.  A cookie that is present more than once is
 * ambiguous (another site on the same domain may have set one of them),
 * so it is not returned at all.
 *
 * Returns: (transfer full) (nullable): the value of the only cookie
 *   called @name, with surrounding spaces and tabs removed; %NULL if
 *   there is none, more than one, or the header is too long
 */
gchar *
axis_cookie_find(
	const gchar *header,
	const gchar *name
);

/**
 * axis_auth_next_is_safe:
 * @next: (nullable): a path a client asked to be sent to after logging in
 *
 * Accepts only a path on this site: it starts with exactly one '/' (not
 * "//" and not "/\\"), has 1 to %AXIS_NEXT_MAX_LEN bytes, all printable
 * ASCII other than space, '\\', '<', '>', '"', '`', '{', '}', '|' and
 * '^', and every '%' is followed by two hexadecimal digits.  So no scheme,
 * no host, and nothing a browser strips or rewrites (control characters,
 * spaces, backslashes) can turn it into one.
 *
 * Returns: %TRUE if @next may be used as a redirect target
 */
gboolean
axis_auth_next_is_safe(const gchar *next);

/**
 * axis_auth_normalise_username:
 * @input: (nullable): a username as typed
 *
 * Trims surrounding spaces and lower-cases ASCII letters; the result must
 * then be 1 to %AXIS_USERNAME_MAX_LEN characters of a-z, 0-9, '.', '_'
 * and '-', starting with a letter or digit.
 *
 * Returns: (transfer full) (nullable): the stored form of the name, or
 *   %NULL if @input cannot be a username
 */
gchar *
axis_auth_normalise_username(const gchar *input);

/**
 * axis_auth_token_is_wellformed:
 * @token: (nullable): a token from a cookie or a form
 * @length: the exact length a token of its kind has
 *
 * Returns: %TRUE if @token is exactly @length characters of the URL-safe
 *   base64 alphabet (A-Z, a-z, 0-9, '-', '_')
 */
gboolean
axis_auth_token_is_wellformed(
	const gchar *token,
	gsize        length
);

/**
 * axis_auth_check_password:
 * @password: (nullable): a new password
 *
 * Returns: (nullable): %NULL if @password is acceptable (valid UTF-8, at
 *   least %AXIS_PASSWORD_MIN_CHARS characters, at most
 *   %AXIS_PASSWORD_MAX_BYTES bytes), else a sentence for the user saying
 *   what is wrong (static; it never quotes the password)
 */
const gchar *
axis_auth_check_password(const gchar *password);

/**
 * axis_auth_origin_matches:
 * @origin: (nullable): the request's Origin header
 * @host: (nullable): the request's Host header
 * @https: whether the request is known to have arrived over HTTPS
 *
 * Whether @origin names this site: "http://" or "https://" followed by
 * exactly @host (compared without case, a default port on either side
 * ignored), nothing after it.  An "http" origin does not match a request
 * known to be HTTPS.  "null", a missing host and anything else do not
 * match.
 *
 * Returns: %TRUE if @origin is this site
 */
gboolean
axis_auth_origin_matches(
	const gchar *origin,
	const gchar *host,
	gboolean     https
);

/**
 * axis_auth_clean_user_agent:
 * @user_agent: (nullable): a User-Agent header
 *
 * Returns: (transfer full): @user_agent with every byte outside printable
 *   ASCII replaced by '?', cut to %AXIS_USER_AGENT_MAX_LEN bytes; "" for
 *   %NULL
 */
gchar *
axis_auth_clean_user_agent(const gchar *user_agent);

G_END_DECLS

#endif /* AXIS_WEB_AUTH_INPUT_H */

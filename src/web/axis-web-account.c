/*
 * axis-web-account.c - the account page (password, sessions) and the
 * administrators' users page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every change is made in the database first, on a worker thread; the
 * in-memory accounts and sessions follow on the main loop once it has
 * succeeded.  After a change the browser is sent back to the page
 * (post/redirect/get) with a fixed key for the message to show, so no
 * text from a request is ever reflected.
 */

#include "web/axis-web-auth-private.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-theme.h"

#include <string.h>

#define USERNAME_RULES AXIS_USERNAME_RULES

/* ── Shared ─────────────────────────────────────────────────────── */

static gchar *
format_time(gint64 us)
{
	return axis_web_format_time(us, "-");
}

static HtmxNode *
card(const gchar *heading)
{
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	axis_web_auth_add(section, title);

	return HTMX_NODE(section);
}

/* A card with the id @id, holding the page's notice when that is its place. */
static HtmxNode *
card_at(
	const gchar   *heading,
	const gchar   *id,
	AxisWebNotice *notice
){
	HtmxNode *node = card(heading);

	htmx_element_set_id(HTMX_ELEMENT(node), id);
	if (notice != NULL)
		axis_web_notice_put(notice, node, id);

	return node;
}

/* A small form that posts @hidden_name=@hidden_value (if any) to @action
 * with one button. */
static HtmxNode *
action_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *hidden_name,
	const gchar *hidden_value,
	const gchar *label,
	const gchar *css_class
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (hidden_name != NULL)
		axis_web_auth_add(form, axis_web_auth_hidden(hidden_name, hidden_value));
	axis_web_auth_add(form, axis_web_auth_button(label, css_class));

	return form;
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no page at this address. The link may be old, or what it showed was removed.");
}

/* The :id route parameter as an account id, or 0. */
static gint64
param_id(GHashTable *params)
{
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	guint64 value = 0;
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > 18 || text[0] == '0')
		return 0;
	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return 0;
		value = value * 10 + (guint64)(*p - '0');
	}

	return (gint64)value;
}

static gint64
retry_seconds(gint64 wait_us)
{
	return MAX((wait_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1);
}

/* ── Account page ───────────────────────────────────────────────── */

static const gchar *
account_message(const gchar *key)
{
	if (g_strcmp0(key, "password") == 0)
		return "Your password has been changed. Your other sessions have been ended.";
	if (g_strcmp0(key, "revoked") == 0)
		return "The session has been ended.";
	if (g_strcmp0(key, "theme") == 0)
		return "Your theme has been saved.";

	return NULL;
}

static HtmxNode *
sessions_table(
	AxisWebAuth         *auth,
	HtmxRequest         *request,
	const AxisAuthState *state
){
	g_autoptr(GPtrArray) sessions = axis_session_store_list_for_user(auth->sessions,
		state->user_id, axis_web_auth_now(auth));
	static const AxisWebColumn columns[] = {
		{ "Started", AXIS_WEB_COL_DATE },
		{ "Last used", AXIS_WEB_COL_DATE },
		{ "Address", AXIS_WEB_COL_TEXT },
		{ "Browser", AXIS_WEB_COL_WIDE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("sessions", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < sessions->len; i++) {
		const AxisSession *session = g_ptr_array_index(sessions, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *action = htmx_td_new();
		g_autofree gchar *started = format_time(session->created_us);
		g_autofree gchar *seen = format_time(session->last_seen_us);

		axis_web_auth_add(row, cell_text(started));
		axis_web_auth_add(row, cell_text(seen));
		axis_web_auth_add(row, cell_text(session->client_address));
		axis_web_auth_add(row, cell_text(session->user_agent));

		if (g_strcmp0(session->public_id, state->public_id) == 0) {
			htmx_node_set_text_content(HTMX_NODE(action), "This session");
		} else {
			g_autofree gchar *path = g_strdup_printf("/account/sessions/%s/revoke",
				session->public_id);

			axis_web_auth_add(action, action_form(request, path, NULL, NULL, "End this session", NULL));
		}
		axis_web_auth_add(row, action);
		axis_web_auth_add(body, row);
	}
	axis_web_auth_add(table, body);

	return HTMX_NODE(table);
}

/* ── Theme ──────────────────────────────────────────────────────── */

#define THEME_DEFAULT_CHOICE "default"

static HtmxNode *
span_with(
	const gchar *css_class,
	const gchar *text
){
	HtmxSpan *span = htmx_span_new();

	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), css_class);
	if (text != NULL)
		htmx_node_set_text_content(HTMX_NODE(span), text);

	return HTMX_NODE(span);
}

/*
 * A small page in @theme: it carries that theme's data-theme attribute, so
 * the theme's own variables colour it whatever theme the page has.  It
 * shows the page colour, the header with the mark in the theme's own
 * colours, a card with its text, and the accent.  @key makes the mark's
 * mask id unique on the page.
 */
static HtmxNode *
swatch(
	const AxisWebTheme *theme,
	const gchar        *key
){
	HtmxNode *sample = span_with("swatch", NULL);
	HtmxNode *bar = span_with("swatch-bar", NULL);
	HtmxNode *card_node = span_with("swatch-card", NULL);
	g_autofree gchar *mask_id = g_strdup_printf("axis-hub-swatch-%s", key);

	htmx_element_set_attribute(HTMX_ELEMENT(sample), "data-theme", theme->id);
	axis_web_auth_add(bar, axis_web_logo_new(mask_id, NULL));
	axis_web_auth_add(bar, span_with(NULL, "Axis"));
	axis_web_auth_add(sample, bar);
	axis_web_auth_add(card_node, span_with(NULL, "Text on a card"));
	axis_web_auth_add(card_node, span_with("swatch-muted", "Quieter text"));
	axis_web_auth_add(card_node, span_with("swatch-link", "A link"));
	axis_web_auth_add(card_node, span_with("swatch-chip", "Action"));
	axis_web_auth_add(sample, card_node);

	return sample;
}

/* One radio button with its card: a preview (if @theme) and the name. */
static HtmxNode *
theme_option(
	const gchar        *value,
	const AxisWebTheme *preview,
	const gchar        *name,
	const gchar        *note,
	gboolean            checked
){
	HtmxLabel *label = htmx_label_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_RADIO);
	HtmxNode *card_node = span_with("theme-card", NULL);
	HtmxNode *title = span_with("theme-name", name);

	htmx_element_add_class(HTMX_ELEMENT(label), "theme-option");
	htmx_input_set_name(input, "theme");
	htmx_input_set_value(input, value);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	axis_web_auth_add(label, input);

	if (preview != NULL)
		axis_web_auth_add(card_node, swatch(preview, value));
	axis_web_auth_add(title, span_with("theme-note", note));
	axis_web_auth_add(card_node, title);
	axis_web_auth_add(label, card_node);

	return HTMX_NODE(label);
}

static const gchar *
kind_note(const AxisWebTheme *theme)
{
	switch (theme->kind) {
	case AXIS_WEB_THEME_LIGHT:
		return "Light";
	case AXIS_WEB_THEME_AUTO:
		return "Follows the device";
	case AXIS_WEB_THEME_DARK:
	default:
		return "Dark";
	}
}

static HtmxNode *
theme_card(
	HtmxRequest         *request,
	const AxisAuthState *state,
	AxisWebNotice       *notice
){
	HtmxNode *themes = card_at("Theme", "theme", notice);
	HtmxNode *form = axis_web_auth_post_form(request, "/account/theme");
	HtmxFieldset *group = htmx_fieldset_new();
	HtmxLegend *legend = htmx_legend_new_with_text("Colour theme");
	HtmxP *intro = htmx_p_new();
	const AxisWebTheme *default_theme = axis_web_theme_get_default();
	gboolean chosen = axis_web_theme_find(state->theme) != NULL;
	g_autofree gchar *default_note = NULL;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(intro),
		"Choose how Axis looks for you. It applies to your account only, on every browser you log in from.");
	axis_web_auth_add(themes, intro);

	htmx_element_add_class(HTMX_ELEMENT(form), "form-wide");
	htmx_element_add_class(HTMX_ELEMENT(group), "theme-grid");
	htmx_element_add_class(HTMX_ELEMENT(legend), "visually-hidden");
	axis_web_auth_add(group, legend);

	default_note = g_strdup_printf("Currently %s", default_theme->name);
	axis_web_auth_add(group, theme_option(THEME_DEFAULT_CHOICE, default_theme, "Use the default",
		default_note, !chosen));
	for (i = 0; i < axis_web_theme_count(); i++) {
		const AxisWebTheme *theme = axis_web_theme_at(i);

		axis_web_auth_add(group, theme_option(theme->id, theme, theme->name, kind_note(theme),
			chosen && strcmp(state->theme, theme->id) == 0));
	}
	axis_web_auth_add(form, group);
	axis_web_auth_add(form, axis_web_auth_button("Save theme", "button-primary"));
	axis_web_auth_add(themes, form);

	return themes;
}

static HtmxResponse *
account_page(
	HtmxRequest *request,
	guint        status,
	gboolean     error,
	const gchar *message
){
	AxisWebAuth *auth = axis_web_auth_from_request(request);
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	HtmxNode *you = card("Username and role");
	HtmxDl *facts = htmx_dl_new();
	AxisWebNotice place;
	HtmxNode *password;
	HtmxNode *form = axis_web_auth_post_form(request, "/account/password");
	HtmxNode *sessions;
	HtmxP *intro = htmx_p_new();
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(title), "Your account");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_notice_begin(&place, request, error, g_strdup(message), HTMX_NODE(content));
	password = card_at("Change password", "change-password", &place);
	sessions = card_at("Browsers logged in to your account", "sessions", &place);

	htmx_element_add_class(HTMX_ELEMENT(facts), "facts");
	axis_web_auth_add(facts, htmx_dt_new_with_text("Username"));
	axis_web_auth_add(facts, htmx_dd_new_with_text(state->username));
	axis_web_auth_add(facts, htmx_dt_new_with_text("Role"));
	axis_web_auth_add(facts, htmx_dd_new_with_text(
		state->role == AXIS_USER_ROLE_ADMIN ? "Administrator" : "User"));
	axis_web_auth_add(you, facts);
	axis_web_auth_add(content, you);
	axis_web_auth_add(content, theme_card(request, state, &place));

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_field("current_password", "Current password",
		"current_password", HTMX_INPUT_PASSWORD, "current-password", NULL));
	axis_web_auth_add(form, axis_web_auth_field("new_password", "New password (at least 10 characters)",
		"new_password", HTMX_INPUT_PASSWORD, "new-password", NULL));
	axis_web_auth_add(form, axis_web_auth_field("new_password_confirm", "New password again",
		"new_password_confirm", HTMX_INPUT_PASSWORD, "new-password", NULL));
	axis_web_auth_add(form, axis_web_auth_button("Change password", "button-primary"));
	axis_web_auth_add(password, form);
	axis_web_auth_add(content, password);

	htmx_node_set_text_content(HTMX_NODE(intro),
		"The browsers logged in to your account. Ending a session logs that browser out at once.");
	axis_web_auth_add(sessions, intro);
	axis_web_auth_add(sessions, sessions_table(auth, request, state));
	axis_web_auth_add(content, sessions);

	axis_web_notice_end(&place);
	response = axis_web_page_respond(request, "Your account", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_account(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return account_page(request, 200, FALSE,
		account_message(htmx_request_get_query_param(request, "done")));
}

typedef enum
{
	CHANGE_OK,
	CHANGE_WRONG_PASSWORD
} ChangeOutcome;

typedef struct
{
	AxisWebAuth   *auth;
	gint64         user_id;
	gchar         *counted;	/* the session's count it is in flight on, until freed */
	gchar         *keep;		/* the token hash of the session to keep */
	gchar         *current;	/* the current password, if it is checked */
	gchar         *password;	/* the new one */
	gint           epoch;	/* out */
	ChangeOutcome  outcome;
	GError        *error;
} PasswordWork;

/* On the main loop, in every case (axis_web_auth_dispatch()). */
static void
password_work_free(gpointer data)
{
	PasswordWork *work = data;

	if (work->counted != NULL)
		axis_login_throttle_end(work->auth->by_session, work->counted);
	g_free(work->counted);
	g_free(work->keep);
	axis_secret_free(work->current);
	axis_secret_free(work->password);
	g_clear_error(&work->error);
	g_free(work);
}

static void
password_work(
	AxisWebAuth *auth,
	gpointer     data
){
	PasswordWork *work = data;
	g_autoptr(AxisUser) user = NULL;
	gchar *stored = NULL;
	gchar *hash = NULL;

	if (work->current != NULL) {
		if (!axis_user_store_get(auth->pool, work->user_id, &user, &stored, &work->error))
			goto out;
		if (user == NULL) {
			g_set_error_literal(&work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
				"the account no longer exists");
			goto out;
		}
		if (!axis_password_verify(stored, work->current)) {
			work->outcome = CHANGE_WRONG_PASSWORD;
			goto out;
		}
	}

	hash = axis_password_hash(work->password, &work->error);
	if (hash == NULL)
		goto out;
	if (axis_user_store_set_password(auth->pool, work->user_id, hash, work->keep,
	                                 &work->epoch, &work->error))
		work->outcome = CHANGE_OK;

out:
	axis_secret_free(work->current);
	work->current = NULL;
	axis_secret_free(work->password);
	work->password = NULL;
	g_free(stored);
	g_free(hash);
}

/*
 * After a password change in the database: the kept session moves to the
 * new epoch, then the account does, which ends every other session.
 */
static void
apply_new_epoch(
	AxisWebAuth *auth,
	gint64       user_id,
	const gchar *keep,
	gint         epoch
){
	const AxisUser *cached = axis_session_store_get_user(auth->sessions, user_id);
	g_autoptr(AxisUser) updated = NULL;

	if (cached == NULL)
		return;

	if (keep != NULL)
		axis_session_store_set_epoch(auth->sessions, keep, epoch);

	updated = axis_user_copy(cached);
	updated->epoch = MAX(cached->epoch, epoch);
	axis_session_store_put_user(auth->sessions, updated);
}

static HtmxResponse *
password_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	PasswordWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND))
		return axis_web_auth_redirect(request, "/login");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	if (work->outcome == CHANGE_WRONG_PASSWORD) {
		axis_login_throttle_fail(auth->by_session, work->counted, axis_web_auth_now_monotonic(auth));
		axis_web_auth_audit("password change refused, wrong current password: user=%s client=%s",
			state->username, state->client_address);
		return account_page(request, 403, TRUE, "The current password is not correct.");
	}

	axis_login_throttle_clear(auth->by_session, work->counted);
	apply_new_epoch(auth, work->user_id, work->keep, work->epoch);
	axis_web_auth_audit("password changed: user=%s client=%s", state->username,
		state->client_address);

	return axis_web_place_redirect(request, "/account", "password", "change-password");
}

static HtmxResponse *
handle_change_password(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *current = htmx_request_get_form_value(request, "current_password");
	const gchar *password = htmx_request_get_form_value(request, "new_password");
	const gchar *confirm = htmx_request_get_form_value(request, "new_password_confirm");
	const gchar *problem = axis_auth_check_password(password);
	gint64 wait;
	PasswordWork *work;

	if (problem != NULL)
		return account_page(request, 400, TRUE, problem);
	if (g_strcmp0(password, confirm) != 0)
		return account_page(request, 400, TRUE, "The two new passwords do not match.");
	if (current == NULL || strlen(current) > AXIS_PASSWORD_MAX_BYTES)
		return account_page(request, 403, TRUE, "The current password is not correct.");

	/* The current password is guessed at here as at the login page: on
	 * the session's own count (a stranger who keeps the username delayed
	 * does not stop its owner, logged in, from changing the password), and
	 * the checks in flight count as the failures they may be. */
	wait = axis_login_throttle_check(auth->by_session, state->token_hash,
		axis_web_auth_now_monotonic(auth));
	if (wait > 0) {
		g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT, retry_seconds(wait));
		g_autofree gchar *message = g_strdup_printf(
			"Too many wrong passwords. Try again in %s seconds.", seconds);
		HtmxResponse *response = account_page(request, 429, TRUE, message);

		htmx_response_add_header(response, "Retry-After", seconds);
		return response;
	}

	work = g_new0(PasswordWork, 1);
	work->auth = auth;
	work->user_id = state->user_id;
	work->counted = g_strdup(state->token_hash);
	axis_login_throttle_begin(auth->by_session, work->counted, axis_web_auth_now_monotonic(auth));
	work->keep = g_strdup(state->token_hash);
	work->current = g_strdup(current);
	work->password = g_strdup(password);

	return axis_web_auth_dispatch(auth, request, password_work, password_done, work,
		password_work_free);
}

static HtmxResponse *
handle_revoke_session(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	gboolean own = g_strcmp0(id, state->public_id) == 0;
	HtmxResponse *response;

	if (!axis_session_store_revoke_public(auth->sessions, state->user_id, id))
		return not_found(request);

	axis_web_auth_audit("session ended: user=%s session=%.8s client=%s", state->username, id,
		state->client_address);

	if (own) {
		response = axis_web_auth_redirect(request, "/login");
		axis_web_auth_clear_session_cookie(request, response);
		return response;
	}

	return axis_web_place_redirect(request, "/account", "revoked", "sessions");
}

typedef struct
{
	gint64  user_id;
	gchar  *theme;	/* NULL: use the default */
	GError *error;
} ThemeWork;

static void
theme_work_free(gpointer data)
{
	ThemeWork *work = data;

	g_free(work->theme);
	g_clear_error(&work->error);
	g_free(work);
}

static void
theme_work(
	AxisWebAuth *auth,
	gpointer     data
){
	ThemeWork *work = data;

	axis_user_store_set_theme(auth->pool, work->user_id, work->theme, &work->error);
}

static HtmxResponse *
theme_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	ThemeWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const AxisUser *cached;

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND))
		return axis_web_auth_redirect(request, "/login");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	/* The cached account follows the database, so that the very next page,
	 * from any browser of this user, is drawn in the new theme. */
	cached = axis_session_store_get_user(auth->sessions, work->user_id);
	if (cached != NULL) {
		g_autoptr(AxisUser) updated = axis_user_copy(cached);

		g_free(updated->theme);
		updated->theme = g_strdup(work->theme);
		axis_session_store_put_user(auth->sessions, updated);
	}

	axis_web_auth_audit("theme changed: user=%s theme=%s client=%s", state->username,
		work->theme != NULL ? work->theme : "(default)", state->client_address);

	return axis_web_place_redirect(request, "/account", "theme", "theme");
}

static HtmxResponse *
handle_set_theme(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *choice = htmx_request_get_form_value(request, "theme");
	ThemeWork *work;

	/* Only a theme this version has, or "use the default", is stored. */
	if (choice == NULL || (strcmp(choice, THEME_DEFAULT_CHOICE) != 0 &&
	                       axis_web_theme_find(choice) == NULL))
		return account_page(request, 400, TRUE, "Choose one of the themes listed.");

	work = g_new0(ThemeWork, 1);
	work->user_id = state->user_id;
	if (strcmp(choice, THEME_DEFAULT_CHOICE) != 0)
		work->theme = g_strdup(choice);

	return axis_web_auth_dispatch(auth, request, theme_work, theme_done, work, theme_work_free);
}

/* ── Users page ─────────────────────────────────────────────────── */

static const gchar *
users_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "created",  "The account has been created." },
		{ "disabled", "The account has been disabled and its sessions ended." },
		{ "enabled",  "The account has been enabled." },
		{ "deleted",  "The account has been deleted." },
		{ "role",     "The role has been changed." },
		{ "password", "The password has been set and the account's sessions ended." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].text;
	}

	return NULL;
}

static HtmxNode *
user_actions(
	HtmxRequest         *request,
	const AxisAuthState *state,
	const AxisUser      *user
){
	HtmxTd *cell = htmx_td_new();
	HtmxDiv *actions = htmx_div_new();
	g_autofree gchar *base = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT, user->id);
	g_autofree gchar *role = g_strconcat(base, "/role", NULL);
	g_autofree gchar *toggle = g_strconcat(base, user->disabled ? "/enable" : "/disable", NULL);
	g_autofree gchar *password = g_strconcat(base, "/password", NULL);
	g_autofree gchar *remove = g_strconcat(base, "/delete", NULL);
	g_autofree gchar *input_id = g_strdup_printf("password-%" G_GINT64_FORMAT, user->id);
	g_autofree gchar *confirm = g_strdup_printf("Delete the account %s?", user->username);
	HtmxNode *reset = axis_web_auth_post_form(request, password);
	HtmxNode *delete_form;
	HtmxInput *input = htmx_input_new(HTMX_INPUT_PASSWORD);
	HtmxLabel *label = htmx_label_new_for(input_id);

	htmx_element_add_class(HTMX_ELEMENT(actions), "actions");

	if (user->role == AXIS_USER_ROLE_ADMIN)
		axis_web_auth_add(actions, action_form(request, role, "role", "user", "Change role to user", NULL));
	else
		axis_web_auth_add(actions, action_form(request, role, "role", "admin", "Change role to administrator", NULL));
	axis_web_auth_add(actions, action_form(request, toggle, NULL, NULL,
		user->disabled ? "Enable this account" : "Disable this account", NULL));

	htmx_element_add_class(HTMX_ELEMENT(reset), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(label), "New password");
	htmx_element_set_id(HTMX_ELEMENT(input), input_id);
	htmx_input_set_name(input, "password");
	htmx_input_set_required(input, TRUE);
	htmx_input_set_placeholder(input, "New password");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "new-password");
	axis_web_auth_add(reset, label);
	axis_web_auth_add(reset, input);
	axis_web_auth_add(reset, axis_web_auth_button("Set password", NULL));
	axis_web_auth_add(actions, reset);

	/* htmx asks for confirmation; without it this is a plain form. */
	delete_form = action_form(request, remove, NULL, NULL, "Delete this account", "button-danger");
	htmx_element_set_attribute(HTMX_ELEMENT(delete_form), "hx-post", remove);
	htmx_element_set_attribute(HTMX_ELEMENT(delete_form), "hx-confirm", confirm);
	/* A refusal is a page: it replaces the page's content, not the form. */
	htmx_element_set_attribute(HTMX_ELEMENT(delete_form), "hx-target", "#content");
	axis_web_auth_add(actions, delete_form);

	axis_web_auth_add(cell, actions);
	(void)state;

	return HTMX_NODE(cell);
}

static HtmxResponse *
users_page(
	HtmxRequest *request,
	guint        status,
	gboolean     error,
	const gchar *message
){
	AxisWebAuth *auth = axis_web_auth_from_request(request);
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autoptr(GPtrArray) users = axis_session_store_list_users(auth->sessions);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	HtmxNode *accounts = card("All accounts");
	static const AxisWebColumn columns[] = {
		{ "Username", AXIS_WEB_COL_NAME },
		{ "Role", AXIS_WEB_COL_TEXT },
		{ "Status", AXIS_WEB_COL_STATE },
		{ "Actions", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("users", columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxNode *create = card("New account");
	HtmxNode *form = axis_web_auth_post_form(request, "/admin/users");
	HtmxDiv *role_field = htmx_div_new();
	HtmxLabel *role_label = htmx_label_new_for("role");
	HtmxSelect *role = htmx_select_new_with_name("role");
	HtmxOption *as_user = htmx_option_new_with_value("user", "User");
	HtmxOption *as_admin = htmx_option_new_with_value("admin", "Administrator");
	HtmxResponse *response;
	AxisWebNotice place;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "User accounts");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("The people who can log in to Axis. Administrators can change "
		"settings and start searches, downloads, imports and conversions; users can look but change nothing."));
	axis_web_notice_begin(&place, request, error, g_strdup(message), HTMX_NODE(content));
	htmx_element_set_id(HTMX_ELEMENT(accounts), "accounts");
	axis_web_notice_put(&place, accounts, "accounts");
	htmx_element_set_id(HTMX_ELEMENT(create), "new-account");
	axis_web_notice_put(&place, create, "new-account");

	for (i = 0; i < users->len; i++) {
		const AxisUser *user = g_ptr_array_index(users, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *first = htmx_td_new();
		HtmxSpan *who = htmx_span_new();
		g_autofree gchar *row_id = g_strdup_printf("user-%" G_GINT64_FORMAT, user->id);
		g_autofree gchar *name = user->id == state->user_id ?
			g_strdup_printf("%s (you)", user->username) : g_strdup(user->username);

		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		htmx_node_set_text_content(HTMX_NODE(who), name);
		axis_web_auth_add(first, who);
		axis_web_notice_put(&place, HTMX_NODE(first), row_id);
		axis_web_auth_add(row, first);
		axis_web_auth_add(row, cell_text(user->role == AXIS_USER_ROLE_ADMIN ? "Administrator" : "User"));
		axis_web_auth_add(row, cell_text(user->disabled ? "Disabled" : "Enabled"));
		axis_web_auth_add(row, user_actions(request, state, user));
		axis_web_auth_add(body, row);
	}
	axis_web_auth_add(table, body);
	axis_web_auth_add(accounts, table);
	axis_web_auth_add(content, accounts);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_field("new_username", "Username", "username",
		HTMX_INPUT_TEXT, "off", NULL));
	axis_web_auth_add(form, axis_web_auth_field("new_user_password", "Password (at least 10 characters)",
		"password", HTMX_INPUT_PASSWORD, "new-password", NULL));
	htmx_element_add_class(HTMX_ELEMENT(role_field), "field");
	htmx_node_set_text_content(HTMX_NODE(role_label), "Role");
	htmx_element_set_id(HTMX_ELEMENT(role), "role");
	axis_web_auth_add(role, as_user);
	axis_web_auth_add(role, as_admin);
	axis_web_auth_add(role_field, role_label);
	axis_web_auth_add(role_field, role);
	axis_web_auth_add(form, role_field);
	axis_web_auth_add(form, axis_web_auth_button("Create account", "button-primary"));
	axis_web_auth_add(create, form);
	axis_web_auth_add(content, create);

	axis_web_notice_end(&place);
	response = axis_web_page_respond(request, "User accounts", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_users(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return users_page(request, 200, FALSE,
		users_message(htmx_request_get_query_param(request, "done")));
}

/* Creating an account. */

typedef struct
{
	gchar        *username;
	gchar        *password;
	AxisUserRole  role;
	AxisUser     *created;
	GError       *error;
} CreateWork;

static void
create_work_free(gpointer data)
{
	CreateWork *work = data;

	g_free(work->username);
	axis_secret_free(work->password);
	axis_user_free(work->created);
	g_clear_error(&work->error);
	g_free(work);
}

static void
create_work(
	AxisWebAuth *auth,
	gpointer     data
){
	CreateWork *work = data;
	gchar *hash = axis_password_hash(work->password, &work->error);

	axis_secret_free(work->password);
	work->password = NULL;
	if (hash == NULL)
		return;

	work->created = axis_user_store_create(auth->pool, work->username, hash, work->role,
		&work->error);
	g_free(hash);
}

static HtmxResponse *
create_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	CreateWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_EXISTS)) {
		g_autofree gchar *message = g_strdup_printf("An account called %s already exists.",
			work->username);

		return users_page(request, 409, TRUE, message);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_session_store_put_user(auth->sessions, work->created);
	axis_web_auth_audit("account created: user=%s role=%s by=%s client=%s",
		work->created->username, axis_user_role_to_string(work->created->role),
		state->username, state->client_address);

	return axis_web_place_redirect(request, "/admin/users", "created", "new-account");
}

static gboolean
parse_role(
	const gchar  *text,
	AxisUserRole *role
){
	if (g_strcmp0(text, "admin") == 0)
		*role = AXIS_USER_ROLE_ADMIN;
	else if (g_strcmp0(text, "user") == 0)
		*role = AXIS_USER_ROLE_USER;
	else
		return FALSE;

	return TRUE;
}

static HtmxResponse *
handle_create_user(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	g_autofree gchar *username = axis_auth_normalise_username(
		htmx_request_get_form_value(request, "username"));
	const gchar *password = htmx_request_get_form_value(request, "password");
	const gchar *problem;
	AxisUserRole role;
	CreateWork *work;

	if (username == NULL)
		return users_page(request, 400, TRUE, USERNAME_RULES);
	problem = axis_auth_check_password(password);
	if (problem != NULL)
		return users_page(request, 400, TRUE, problem);
	if (!parse_role(htmx_request_get_form_value(request, "role"), &role))
		return users_page(request, 400, TRUE, "Choose a role.");

	work = g_new0(CreateWork, 1);
	work->username = g_steal_pointer(&username);
	work->password = g_strdup(password);
	work->role = role;

	return axis_web_auth_dispatch(auth, request, create_work, create_done, work, create_work_free);
}

/* Disabling, enabling, changing the role and deleting. */

typedef enum
{
	USER_DISABLE,
	USER_ENABLE,
	USER_ROLE,
	USER_DELETE
} UserOp;

typedef struct
{
	UserOp        op;
	gint64        id;
	AxisUserRole  role;
	gchar        *name;
	AxisUser     *result;
	GError       *error;
} UserWork;

static void
user_work_free(gpointer data)
{
	UserWork *work = data;

	g_free(work->name);
	axis_user_free(work->result);
	g_clear_error(&work->error);
	g_free(work);
}

static void
user_work(
	AxisWebAuth *auth,
	gpointer     data
){
	UserWork *work = data;

	switch (work->op) {
	case USER_DISABLE:
	case USER_ENABLE:
		work->result = axis_user_store_set_disabled(auth->pool, work->id,
			work->op == USER_DISABLE, &work->error);
		break;
	case USER_ROLE:
		work->result = axis_user_store_set_role(auth->pool, work->id, work->role, &work->error);
		break;
	case USER_DELETE:
		axis_user_store_delete(auth->pool, work->id, &work->error);
		break;
	}
}

static HtmxResponse *
user_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	UserWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean self_ended;
	const gchar *done;
	HtmxResponse *response;

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_LAST_ADMIN)) {
		g_autofree gchar *message = g_strdup_printf(
			"%s is the only enabled administrator, so it cannot be %s. Make another account an administrator "
			"first.", work->name,
			work->op == USER_DELETE ? "deleted" :
			work->op == USER_DISABLE ? "disabled" : "changed to the user role");

		axis_web_auth_audit("account change refused, last administrator: user=%s by=%s client=%s",
			work->name, state->username, state->client_address);
		return users_page(request, 409, TRUE, message);
	}
	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	if (work->op == USER_DELETE)
		axis_session_store_remove_user(auth->sessions, work->id);
	else
		axis_session_store_put_user(auth->sessions, work->result);

	switch (work->op) {
	case USER_DISABLE:
		done = "disabled";
		axis_web_auth_audit("account disabled, sessions ended: user=%s by=%s client=%s",
			work->name, state->username, state->client_address);
		break;
	case USER_ENABLE:
		done = "enabled";
		axis_web_auth_audit("account enabled: user=%s by=%s client=%s",
			work->name, state->username, state->client_address);
		break;
	case USER_ROLE:
		done = "role";
		axis_web_auth_audit("role changed: user=%s role=%s by=%s client=%s", work->name,
			axis_user_role_to_string(work->role), state->username, state->client_address);
		break;
	case USER_DELETE:
	default:
		done = "deleted";
		axis_web_auth_audit("account deleted: user=%s by=%s client=%s",
			work->name, state->username, state->client_address);
		break;
	}

	/* An administrator who disabled or deleted their own account has no
	 * session any more. */
	self_ended = work->id == state->user_id && (work->op == USER_DISABLE || work->op == USER_DELETE);
	if (self_ended) {
		response = axis_web_auth_redirect(request, "/login");
		axis_web_auth_clear_session_cookie(request, response);
		return response;
	}

	{
		/* The account's row; a deleted one has none: the card holds it. */
		g_autofree gchar *place = work->op == USER_DELETE ? g_strdup("accounts") :
			g_strdup_printf("user-%" G_GINT64_FORMAT, work->id);

		return axis_web_place_redirect(request, "/admin/users", done, place);
	}
}

static HtmxResponse *
user_change(
	HtmxRequest *request,
	GHashTable  *params,
	AxisWebAuth *auth,
	UserOp       op
){
	gint64 id = param_id(params);
	const AxisUser *target = id != 0 ? axis_session_store_get_user(auth->sessions, id) : NULL;
	UserWork *work;
	AxisUserRole role = AXIS_USER_ROLE_USER;

	if (target == NULL)
		return not_found(request);
	if (op == USER_ROLE && !parse_role(htmx_request_get_form_value(request, "role"), &role))
		return users_page(request, 400, TRUE, "Choose a role.");

	work = g_new0(UserWork, 1);
	work->op = op;
	work->id = id;
	work->role = role;
	work->name = g_strdup(target->username);

	return axis_web_auth_dispatch(auth, request, user_work, user_done, work, user_work_free);
}

static HtmxResponse *
handle_disable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return user_change(request, params, user_data, USER_DISABLE);
}

static HtmxResponse *
handle_enable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return user_change(request, params, user_data, USER_ENABLE);
}

static HtmxResponse *
handle_role(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return user_change(request, params, user_data, USER_ROLE);
}

static HtmxResponse *
handle_delete(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return user_change(request, params, user_data, USER_DELETE);
}

/* Setting another account's password. */

static HtmxResponse *
reset_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	PasswordWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const AxisUser *target;

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	target = axis_session_store_get_user(auth->sessions, work->user_id);
	axis_web_auth_audit("password set, sessions ended: user=%s by=%s client=%s",
		target != NULL ? target->username : "-", state->username, state->client_address);
	apply_new_epoch(auth, work->user_id, work->keep, work->epoch);

	{
		g_autofree gchar *place = g_strdup_printf("user-%" G_GINT64_FORMAT, work->user_id);

		return axis_web_place_redirect(request, "/admin/users", "password", place);
	}
}

static HtmxResponse *
handle_reset_password(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	gint64 id = param_id(params);
	const gchar *password = htmx_request_get_form_value(request, "password");
	const gchar *problem;
	PasswordWork *work;

	if (id == 0 || axis_session_store_get_user(auth->sessions, id) == NULL)
		return not_found(request);
	problem = axis_auth_check_password(password);
	if (problem != NULL)
		return users_page(request, 400, TRUE, problem);

	work = g_new0(PasswordWork, 1);
	work->user_id = id;
	/* Setting one's own password here keeps the session in use. */
	work->keep = id == state->user_id ? g_strdup(state->token_hash) : NULL;
	work->password = g_strdup(password);

	return axis_web_auth_dispatch(auth, request, password_work, reset_done, work,
		password_work_free);
}

void
axis_web_account_register(
	AxisWebAuth *self,
	AxisWeb     *web
){
	axis_web_get(web, "/account", handle_account, self);
	axis_web_post(web, "/account/password", handle_change_password, self);
	axis_web_post(web, "/account/theme", handle_set_theme, self);
	axis_web_post(web, "/account/sessions/:id/revoke", handle_revoke_session, self);

	axis_web_route_full(web, HTMX_METHOD_GET, "/admin/users", AXIS_WEB_ROUTE_ADMIN, handle_users, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users", AXIS_WEB_ROUTE_ADMIN,
		handle_create_user, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users/:id/disable", AXIS_WEB_ROUTE_ADMIN,
		handle_disable, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users/:id/enable", AXIS_WEB_ROUTE_ADMIN,
		handle_enable, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users/:id/role", AXIS_WEB_ROUTE_ADMIN,
		handle_role, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users/:id/delete", AXIS_WEB_ROUTE_ADMIN,
		handle_delete, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/users/:id/password", AXIS_WEB_ROUTE_ADMIN,
		handle_reset_password, self);
}

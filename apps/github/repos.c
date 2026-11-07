/*
 * The account's own repositories (not those it is a member or collaborator of),
 * most recently pushed to first, from GraphQL (only the fields shown, PAGE a
 * request, all of them up to MAX_REPOS): each row its name and when it was pushed
 * to, its description, then what it is (private, a fork, archived), its language
 * and its stars. The tab shows SHOWN, then a row to all of them on a screen of their
 * own, PER_PAGE a page (pager.c). Kept between openings and shown while loading
 * again, as the notifications are; forgotten when signing out. Tapping one opens it,
 * from what the list has: its name, description, address, stars and forks, a button
 * for its README (readme.c), and the header's to bookmark it; its open issues and
 * pull requests counted once GraphQL says. Another's (a pull request's or an issue's,
 * a bookmark) is loaded, with them in the same query. Bookmarks show above the account's
 * own, its organizations under them (how many repositories each has): one tapped
 * shows its repositories on that screen, loaded then. GitHub lists them only to a
 * token with the read:org scope: without, the section says so.
 */

#define _GNU_SOURCE /* timegm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#include <png.h>

#include "github.h"
#include "ui_symbols.h"

#define PAGE "100"     /* repositories loaded a request (GraphQL's most) */
#define MAX_REPOS 1000 /* loaded at most */
#define SHOWN 10       /* on the tab, the others on a screen of their own */
#define PER_PAGE 30    /* on that screen's pages */
#define ORGS 100       /* organizations listed */
#define AVATAR 56          /* an organization's photo, px */
#define AVATAR_SIZE "56"   /* the same, in the query */
#define AVATAR_COLOR 0x6e7681 /* behind its initial until then */
/* NON-NLS-BEGIN: GraphQL: the account's repositories, an organization's, only what the
 * list shows (the REST API's are many times larger), under the REST API's names
 * (repo_parse()), a page at a time after the cursor $after; the account's
 * organizations. On one line, without quotes: they go in a JSON string as they are */
#define REPO_NODE                                                                            \
	" name full_name: nameWithOwner description private: isPrivate fork: isFork"         \
	" archived: isArchived language: primaryLanguage { name }"                          \
	" stargazers_count: stargazerCount forks_count: forkCount html_url: url"              \
	" pushed_at: pushedAt"
#define REPO_FIELDS " pageInfo { hasNextPage endCursor } nodes {" REPO_NODE " }"
/* A repository's page: the same, and how many issues and pull requests are open */
#define REPO_PAGE_QUERY                                                                      \
	"query RepositoryPage($owner: String!, $name: String!) {"                           \
	" repository(owner: $owner, name: $name) {" REPO_NODE                                 \
	" issues(states: OPEN) { totalCount } pullRequests(states: OPEN) { totalCount } } }"
#define REPOS_QUERY                                                                          \
	"query($after: String) { viewer { repositories(first: " PAGE ", after: $after,"       \
	" ownerAffiliations: [OWNER], orderBy: {field: PUSHED_AT, direction: DESC}) {"       \
	REPO_FIELDS " } } }"
#define ORG_REPOS_QUERY                                                                      \
	"query($login: String!, $after: String) { organization(login: $login) {"            \
	" repositories(first: " PAGE ", after: $after,"                                      \
	" orderBy: {field: PUSHED_AT, direction: DESC}) {" REPO_FIELDS " } } }"
#define ORGS_QUERY                                                                           \
	"query { viewer { organizations(first: 100) {"                                       \
	" nodes { login name avatarUrl(size: " AVATAR_SIZE ") repositories { totalCount } } } } }"
/* NON-NLS-END */
#define BOOKMARKS_FILE DIR "/bookmarks.json"

struct repo {
	char *name, *description; /* description NULL: none */
	const char *full_name;    /* "o/r", interned */
	char *meta;               /* "Private · C · ★ 12", NULL: nothing to say */
	char *url;                /* on github.com, "" if GitHub gave none */
	long long stars, forks;
	time_t pushed;
};

/* Repositories loaded a page at a time */
struct repos {
	struct repo *items;
	unsigned count;
	bool loaded; /* at least once */
	bool more;   /* the request after the first, loading the rest */
	struct ui_fetch fetch;
};

/* The account's; the organization shown's, while its screen is open, and its login */
static struct repos r, org;
static const char *org_login; /* interned */

/* The account's organizations */
static struct {
	struct {
		const char *login, *name; /* interned; name: the login without one */
		const char *avatar;       /* its photo's address, interned; NULL: none */
		long long repos;
	} items[ORGS];
	unsigned count;
	bool no_scope; /* the token lacks read:org */
	struct ui_fetch fetch;
} o;

/* The list, while the screen is open */
static struct {
	lv_obj_t *list, *card, *empty;
	lv_obj_t *bookmarks_label, *bookmarks, *own_label; /* the sections, with bookmarks */
	lv_obj_t *orgs_label, *orgs; /* the organizations' */
} v;

static time_t parse_time(const char *iso)
{
	struct tm tm = {0};
	if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
		   &tm.tm_sec) != 6)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon--;
	return timegm(&tm);
}

static void repo_free(struct repo *it)
{
	free(it->name);
	free(it->description);
	free(it->meta);
	free(it->url);
	memset(it, 0, sizeof(*it));
}

static void clear(struct repos *l)
{
	for (unsigned i = 0; i < l->count; i++)
		repo_free(&l->items[i]);
	free(l->items);
	l->items = NULL;
	l->count = 0;
}

/* `part` after what `meta` has, with a dot between */
static void meta_add(char *meta, size_t size, const char *part)
{
	size_t n = strlen(meta);
	snprintf(meta + n, size - n, "%s%s", n ? " \xc2\xb7 " : "", part);
}

/* A repository of GitHub's (the list's, REPOS_QUERY's, or /repos/o/r) */
static void repo_parse(struct repo *it, const struct json *e)
{
	it->name = strdup(json_str(json_get(e, "name"), ""));
	it->full_name = ui_intern(json_str(json_get(e, "full_name"), ""));
	const char *description = json_str(json_get(e, "description"), "");
	it->description = description[0] ? strdup(description) : NULL;
	char meta[256] = "";
	if (json_bool(json_get(e, "private")))
		meta_add(meta, sizeof(meta), _("Private"));
	if (json_bool(json_get(e, "fork")))
		meta_add(meta, sizeof(meta), _("Fork"));
	if (json_bool(json_get(e, "archived")))
		meta_add(meta, sizeof(meta), _("Archived"));
	/* GraphQL's is an object with its name */
	const struct json *lang = json_get(e, "language");
	const char *language = json_str(lang, json_str(json_get(lang, "name"), ""));
	if (language[0])
		meta_add(meta, sizeof(meta), language);
	long long stars = json_int(json_get(e, "stargazers_count"), 0);
	if (stars > 0) {
		char s[32];
		snprintf(s, sizeof(s), "\xe2\x98\x85 %lld", stars); /* ★ */
		meta_add(meta, sizeof(meta), s);
	}
	it->meta = meta[0] ? strdup(meta) : NULL;
	it->url = strdup(json_str(json_get(e, "html_url"), ""));
	it->stars = stars;
	it->forks = json_int(json_get(e, "forks_count"), 0);
	it->pushed = parse_time(json_str(json_get(e, "pushed_at"), ""));
}

/* GraphQL's page of them: the first instead of those there were, the rest after */
static void parse(struct repos *l, const struct json *list, bool more)
{
	if (!more)
		clear(l);
	unsigned count = 0;
	for (const struct json *e = list->child; e; e = e->next)
		count++;
	struct repo *items = realloc(l->items, (l->count + count + 1) * sizeof(*l->items));
	if (!items)
		return;
	l->items = items;
	for (const struct json *e = list->child; e && l->count < MAX_REPOS; e = e->next) {
		memset(&l->items[l->count], 0, sizeof(*l->items));
		repo_parse(&l->items[l->count++], e);
	}
}

static lv_obj_t *wrapped_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	return label;
}

/* Bookmarks: repositories (the account's or others') at the top of the tab, newest
 * first, by their full name and description, kept in BOOKMARKS_FILE */

struct bookmark {
	char *full_name, *description;
};

static struct {
	struct bookmark *items;
	unsigned count;
	bool loaded;
} b;

static void bookmarks_load(void)
{
	if (b.loaded)
		return;
	b.loaded = true;
	FILE *f = fopen(BOOKMARKS_FILE, "r");
	if (!f)
		return;
	char *text = NULL;
	size_t size = 0;
	FILE *mem = open_memstream(&text, &size);
	char buf[4096];
	size_t n;
	while (mem && (n = fread(buf, 1, sizeof(buf), f)) > 0)
		fwrite(buf, 1, n, mem);
	fclose(f);
	if (!mem)
		return;
	fclose(mem);
	struct json *list = json_parse(text);
	free(text);
	unsigned count = 0;
	for (const struct json *e = list && list->kind == JSON_ARRAY ? list->child : NULL; e; e = e->next)
		count++;
	b.items = calloc(count ? count : 1, sizeof(*b.items));
	for (const struct json *e = count ? list->child : NULL; e && b.items; e = e->next) {
		const char *name = json_str(json_get(e, "full_name"), "");
		if (!name[0])
			continue;
		b.items[b.count].full_name = strdup(name);
		b.items[b.count++].description = strdup(json_str(json_get(e, "description"), ""));
	}
	json_free(list);
}

static void bookmarks_save(void)
{
	mkdir("/var/lib/app-data", 0755);
	mkdir(DIR, 0700);
	FILE *f = fopen(BOOKMARKS_FILE ".new", "w");
	if (!f) {
		printf("github: %s: %s\n", BOOKMARKS_FILE, strerror(errno));
		return;
	}
	fputc('[', f);
	for (unsigned i = 0; i < b.count; i++) {
		size_t n = strlen(b.items[i].description) * 6 + strlen(b.items[i].full_name) * 6 + 2;
		char *name = malloc(n), *description = malloc(n);
		if (name && description) {
			json_escape(name, n, b.items[i].full_name);
			json_escape(description, n, b.items[i].description);
			fprintf(f, "%s{\"full_name\":\"%s\",\"description\":\"%s\"}", i ? "," : "", name, description);
		}
		free(name);
		free(description);
	}
	fputs("]\n", f);
	if (fclose(f) != 0 || rename(BOOKMARKS_FILE ".new", BOOKMARKS_FILE) != 0)
		printf("github: %s: %s\n", BOOKMARKS_FILE, strerror(errno));
}

static int bookmark_find(const char *full_name)
{
	for (unsigned i = 0; i < b.count; i++)
		if (!strcmp(b.items[i].full_name, full_name))
			return (int)i;
	return -1;
}

static void bookmark_open_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i < b.count)
		repo_open_name(b.items[i].full_name);
}

/* The tab's section again, and the heading over the account's own when there is one */
static void bookmarks_update(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.bookmarks);
	for (unsigned i = 0; i < b.count; i++) {
		const struct bookmark *it = &b.items[i];
		lv_obj_t *row = ui_row_create(v.bookmarks, LV_SIZE_CONTENT, i + 1 < b.count, bookmark_open_cb,
					      (void *)(uintptr_t)i);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_t *text = ui_row_text(row);
		lv_obj_t *name = ui_one_line_label(text, "", UI_FONT_BODY);
		ui_set_text(name, it->full_name);
		lv_obj_set_width(name, LV_PCT(100));
		if (it->description[0])
			wrapped_label(text, it->description, UI_FONT_BODY, UI_COLOR_DIM);
	}
	lv_obj_set_hidden(v.bookmarks_label, !b.count);
	lv_obj_set_hidden(v.bookmarks, !b.count);
	lv_obj_set_hidden(v.own_label, !b.count);
}

/* A repository page's header button: the bookmark, an outline until the repository
 * is bookmarked, solid then; tapping it bookmarks or removes it */
struct bookmark_button {
	const char *full_name, *description; /* interned */
	lv_obj_t *icon;
};

static void bookmark_icon(const struct bookmark_button *r)
{
	lv_label_set_text_static(r->icon, bookmark_find(r->full_name) < 0 ? UI_SYMBOL_BOOKMARK_REGULAR
									  : UI_SYMBOL_BOOKMARK);
}

static void bookmark_toggle_cb(lv_event_t *e)
{
	struct bookmark_button *r = lv_event_get_user_data(e);
	int i = bookmark_find(r->full_name);
	if (i >= 0) {
		free(b.items[i].full_name);
		free(b.items[i].description);
		memmove(&b.items[i], &b.items[i + 1], (b.count - i - 1) * sizeof(*b.items));
		b.count--;
	} else {
		struct bookmark *items = realloc(b.items, (b.count + 1) * sizeof(*items));
		if (!items)
			return;
		b.items = items;
		/* Newest first */
		memmove(&items[1], &items[0], b.count * sizeof(*items));
		items[0].full_name = strdup(r->full_name);
		items[0].description = strdup(r->description);
		b.count++;
	}
	bookmarks_save();
	bookmark_icon(r);
	bookmarks_update();
}

static void bookmark_button_delete_cb(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

static void bookmark_button(lv_obj_t *header, const char *full_name, const char *description)
{
	struct bookmark_button *r = malloc(sizeof(*r));
	if (!r)
		return;
	r->full_name = ui_intern(full_name);
	r->description = ui_intern(description ? description : "");
	lv_obj_t *button = ui_header_button(header, UI_SYMBOL_BOOKMARK_REGULAR, bookmark_toggle_cb, r);
	lv_obj_add_event_cb(button, bookmark_button_delete_cb, LV_EVENT_DELETE, r);
	r->icon = lv_obj_get_child(button, 0);
	bookmark_icon(r);
}

/* A row of the repository's numbers: the name, the value on the right */
static void number_row(lv_obj_t *card, const char *name, long long value, bool divider)
{
	lv_obj_t *row = ui_row_create(card, 96, divider, NULL, NULL);
	lv_obj_t *label = ui_one_line_label(row, name, UI_FONT_BODY);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *number = ui_dim_label(row, "", UI_FONT_BODY);
	ui_set_text_fmt(number, "%lld", value);
}

static void readme_cb(lv_event_t *e)
{
	readme_open(lv_event_get_user_data(e));
}

static void issues_cb(lv_event_t *e)
{
	list_open(lv_event_get_user_data(e), false);
}

static void pulls_cb(lv_event_t *e)
{
	list_open(lv_event_get_user_data(e), true);
}

/* A row opening another page: its static text, an arrow on the right; with `count`,
 * a dim label for a number before the arrow (empty) */
static void link_row(lv_obj_t *card, const char *text, lv_event_cb_t cb, void *user_data, bool divider,
		     lv_obj_t **count)
{
	lv_obj_t *row = ui_row_create(card, 96, divider, cb, user_data);
	lv_obj_t *label = ui_one_line_label(row, text, UI_FONT_BODY);
	lv_obj_set_flex_grow(label, 1);
	if (count)
		*count = ui_dim_label(row, "", UI_FONT_BODY);
	ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
}

/* The repository page opened last, while it loads (REPO_PAGE_QUERY): one opened from
 * another's issue stops that one's request, done by then. Its status while there is
 * nothing else to show, its open issues' and pull requests' counts once filled */
static struct {
	lv_obj_t *scr, *header, *body, *status, *issues, *pulls;
	struct ui_fetch fetch;
} page;

/* Its cards and its header's bookmark, the texts copied (a reload of the list frees
 * them; the full name is interned) */
static void repo_fill(lv_obj_t *header, lv_obj_t *body, const struct repo *it)
{
	lv_obj_t *card = ui_card_create(body);
	lv_obj_set_style_pad_all(card, 24, 0);
	lv_obj_set_style_pad_row(card, 12, 0);
	wrapped_label(card, it->name, UI_FONT_TITLE, UI_COLOR_TEXT);
	if (it->description)
		wrapped_label(card, it->description, UI_FONT_BODY, UI_COLOR_DIM);
	if (it->url[0])
		wrapped_label(card, it->url, UI_FONT_CAPTION, UI_COLOR_DIM);

	card = ui_card_create(body);
	number_row(card, _("Stars"), it->stars, true);
	number_row(card, _("Forks"), it->forks, false);

	card = ui_card_create(body);
	link_row(card, _("Issues"), issues_cb, (void *)it->full_name, true, &page.issues);
	link_row(card, _("Pull requests"), pulls_cb, (void *)it->full_name, true, &page.pulls);
	link_row(card, "README", readme_cb, (void *)it->full_name, false, NULL); /* NON-NLS: the file's name */
	bookmark_button(header, it->full_name, it->description);
}

/* A count of REPO_PAGE_QUERY's (its connection's totalCount) in `label` */
static void count_set(lv_obj_t *label, const struct json *connection)
{
	const struct json *total = json_get(connection, "totalCount");
	if (label && total)
		ui_set_text_fmt(label, "%lld", json_int(total, 0));
}

/* Without the status: filled before; otherwise from the answer, or why it has none */
static void repo_loaded(const char *data, long code)
{
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	char error[160] = "";
	const struct json *d = j ? graphql_data(j, error, sizeof(error)) : NULL;
	const struct json *repo = json_get(d, "repository");
	bool found = repo && repo->kind == JSON_OBJECT;
	if (code == 401) {
		json_free(j);
		github_refused();
		return;
	}
	if (found && page.status) {
		struct repo it = {0};
		repo_parse(&it, repo);
		lv_obj_delete(page.status);
		page.status = NULL;
		repo_fill(page.header, page.body, &it);
		repo_free(&it);
	} else if (page.status) {
		if (!code)
			ui_set_text(page.status, _("Cannot reach GitHub"));
		else if (d)
			/* GraphQL's answer for a repository not there, or the token's not to see */
			ui_set_text(page.status, _("Repository not found"));
		else if (error[0])
			ui_set_text(page.status, error);
		else
			ui_set_text_fmt(page.status, _("GitHub answered with an error (HTTP %ld)"), code);
	}
	if (found) {
		count_set(page.issues, json_get(repo, "issues"));
		count_set(page.pulls, json_get(repo, "pullRequests"));
	}
	json_free(j);
}

static void page_delete_cb(lv_event_t *e)
{
	/* Another opened since has it */
	if (lv_event_get_target(e) != page.scr)
		return;
	ui_fetch_stop(&page.fetch);
	memset(&page, 0, sizeof(page));
}

/* The page of `full_name` ("o/r"), from `it` (NULL: loaded, "Loading..." until then),
 * with REPO_PAGE_QUERY's counts */
static void page_open(const char *full_name, const struct repo *it)
{
	ui_fetch_stop(&page.fetch);
	memset(&page, 0, sizeof(page));
	page.scr = ui_app_screen_create(_("Repository"), &page.header, &page.body);
	lv_obj_set_style_pad_all(page.body, 16, 0);
	lv_obj_set_style_pad_row(page.body, 16, 0);
	lv_obj_add_event_cb(page.scr, page_delete_cb, LV_EVENT_DELETE, NULL);
	if (it)
		repo_fill(page.header, page.body, it);
	else
		page.status = ui_empty_label(page.body, _("Loading..."));

	char owner[128], name[128], variables[600];
	const char *slash = strchr(full_name, '/');
	if (slash) {
		char part[128];
		snprintf(part, sizeof(part), "%.*s", (int)(slash - full_name), full_name);
		json_escape(owner, sizeof(owner), part);
		json_escape(name, sizeof(name), slash + 1);
		snprintf(variables, sizeof(variables), "{\"owner\":\"%s\",\"name\":\"%s\"}", owner, name);
		graphql(&page.fetch, REPO_PAGE_QUERY, variables, repo_loaded);
	}
	ui_push(page.scr);
}

/* One of the list's */
static void repo_open(const struct repo *it)
{
	page_open(it->full_name, it);
}

void repo_open_name(const char *full_name)
{
	page_open(full_name, NULL);
}

static void row_cb(lv_event_t *e)
{
	repo_open(lv_event_get_user_data(e));
}

/* A repository's row: its name, when it was pushed to, description, what it is. Its
 * list's rows go with their repositories (rebuilt when it changes) */
static void repo_row(lv_obj_t *card, struct repo *it, bool divider)
{
	lv_obj_t *row = ui_row_create(card, LV_SIZE_CONTENT, divider, row_cb, it);
	lv_obj_set_style_pad_ver(row, 20, 0);
	lv_obj_t *text = ui_row_text(row);

	lv_obj_t *top = ui_row_line(text);
	lv_obj_t *name = ui_one_line_label(top, "", UI_FONT_BODY);
	ui_set_text(name, it->name);
	lv_obj_set_flex_grow(name, 1);
	char when[32] = "";
	if (it->pushed)
		ui_when_format(when, sizeof(when), it->pushed);
	lv_obj_t *time = ui_dim_label(top, "", UI_FONT_CAPTION);
	ui_set_text(time, when);

	if (it->description)
		wrapped_label(text, it->description, UI_FONT_BODY, UI_COLOR_DIM);
	if (it->meta)
		wrapped_label(text, it->meta, UI_FONT_CAPTION, UI_COLOR_DIM);
}

/* All of a list, a page at a time, while the screen is open: the account's (titled
 * how many) or an organization's (its name) */
static struct {
	lv_obj_t *scr, *title, *list, *card, *status;
	struct repos *repos;
	char title_text[48]; /* "97 repositories" */
	struct pager pager;
} all;

/* Page `page` of its list, the list from its top */
static void all_go(int page)
{
	const struct repos *l = all.repos;
	all.pager.pages = (int)((l->count + PER_PAGE - 1) / PER_PAGE);
	if (page > all.pager.pages)
		page = all.pager.pages;
	all.pager.page = page < 1 ? 1 : page;
	if (l == &r) {
		snprintf(all.title_text, sizeof(all.title_text), P_("%u repository", "%u repositories", l->count),
			 l->count);
		lv_label_set_text_static(all.title, all.title_text);
	}
	lv_obj_clean(all.card);
	lv_obj_set_hidden(all.card, !l->count);
	unsigned start = (unsigned)(all.pager.page - 1) * PER_PAGE;
	unsigned end = start + PER_PAGE < l->count ? start + PER_PAGE : l->count;
	for (unsigned i = start; i < end; i++)
		repo_row(all.card, &l->items[i], i + 1 < end);
	lv_obj_scroll_to_y(all.list, 0, LV_ANIM_OFF);
	pager_update(&all.pager);
}

static void all_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (all.repos == &org) {
		ui_fetch_stop(&org.fetch);
		clear(&org);
		org.loaded = false;
	}
	memset(&all, 0, sizeof(all));
}

/* The screen for `repos`, titled `title` (static text) */
static void all_open(struct repos *repos, const char *title)
{
	if (all.scr)
		return;
	lv_obj_t *header, *body;
	all.scr = ui_app_screen_create(title, &header, &body);
	all.repos = repos;
	/* After the back button */
	all.title = lv_obj_get_child(header, 1);
	lv_obj_add_event_cb(all.scr, all_delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_set_scrollable(body, false);
	all.list = lv_obj_create(body);
	lv_obj_remove_style_all(all.list);
	lv_obj_set_width(all.list, LV_PCT(100));
	lv_obj_set_flex_grow(all.list, 1);
	lv_obj_set_flex_flow(all.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(all.list, 16, 0);
	lv_obj_set_scroll_dir(all.list, LV_DIR_VER);
	all.status = ui_empty_label(all.list, "");
	lv_obj_set_hidden(all.status, true);
	all.card = ui_card_create(all.list);
	pager_create(&all.pager, body, all_go);
	all_go(1);
	ui_push(all.scr);
}

static void all_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	all_open(&r, _("All repositories"));
}

static void status(const char *text)
{
	if (!v.list)
		return;
	if (text)
		ui_set_text(v.empty, text);
	lv_obj_set_hidden(v.empty, !text);
}

/* The organization's screen saying `text` instead of its list (NULL: the list) */
static void org_status(const char *text)
{
	if (all.repos != &org)
		return;
	if (text)
		ui_set_text(all.status, text);
	lv_obj_set_hidden(all.status, !text);
}

/* The organizations' rows: their names, how many repositories, an arrow */
static void orgs_update(void);

/* The rows again, from r.items: the SHOWN last pushed to, then the row to all of them */
static void rows_update(void)
{
	if (all.repos == &r)
		all_go(all.pager.page);
	if (!v.list)
		return;
	lv_obj_clean(v.card);
	unsigned shown = r.count < SHOWN ? r.count : SHOWN;
	for (unsigned i = 0; i < shown; i++)
		repo_row(v.card, &r.items[i], i + 1 < r.count);
	lv_obj_set_hidden(v.card, !r.count);
	if (r.count <= SHOWN)
		return;
	/* The last row: how many, an arrow */
	lv_obj_t *row = ui_row_create(v.card, 96, false, all_cb, NULL);
	lv_obj_t *label = ui_one_line_label(row, _("All repositories"), UI_FONT_BODY);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_t *count = ui_dim_label(row, "", UI_FONT_BODY);
	ui_set_text_fmt(count, "%u", r.count);
	ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
}

/* A page of `l` from GraphQL's `answer` (the repositories' connection at `path`, two
 * keys deep) in its place, then the next one asked (`login`: an organization's); its
 * text when there is none, "" for a page come */
static void page_loaded(struct repos *l, const char *data, long code, const char *const path[2], const char *query,
			const char *login, void (*done)(const char *, long), char *text, size_t size)
{
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	char error[128] = "";
	const struct json *d = j ? graphql_data(j, error, sizeof(error)) : NULL;
	const struct json *repos = json_get(json_get(d, path[0]), path[1]);
	const struct json *list = json_get(repos, "nodes");
	bool more = l->more;
	l->more = false;
	text[0] = '\0';
	if (list && list->kind == JSON_ARRAY) {
		l->loaded = true;
		parse(l, list, more);
		/* The rest after them */
		const struct json *info = json_get(repos, "pageInfo");
		const char *cursor = json_str(json_get(info, "endCursor"), NULL);
		if (json_bool(json_get(info, "hasNextPage")) && cursor && l->count < MAX_REPOS) {
			char escaped[256], name[128], variables[420];
			json_escape(escaped, sizeof(escaped), cursor);
			if (login) {
				json_escape(name, sizeof(name), login);
				snprintf(variables, sizeof(variables), "{\"login\":\"%s\",\"after\":\"%s\"}", name, escaped);
			} else {
				snprintf(variables, sizeof(variables), "{\"after\":\"%s\"}", escaped);
			}
			l->more = true;
			graphql(&l->fetch, query, variables, done);
		}
	} else if (!code) {
		snprintf(text, size, "%s", _("Cannot reach GitHub"));
	} else if (error[0]) {
		/* GraphQL's reason (English) */
		snprintf(text, size, "%s", error);
	} else {
		snprintf(text, size, _("GitHub answered with an error (HTTP %ld)"), code);
	}
	json_free(j);
}

static void loaded(const char *data, long code)
{
	if (v.list)
		ui_pull_refresh_done(v.list);
	if (code == 401) {
		github_sign_out(_("GitHub refused this token."));
		return;
	}
	static const char *const path[2] = {"viewer", "repositories"};
	char text[160];
	page_loaded(&r, data, code, path, REPOS_QUERY, NULL, loaded, text, sizeof(text));
	if (!text[0]) {
		rows_update();
		status(r.count ? NULL : _("No repositories"));
	} else if (!r.loaded || !r.count) {
		/* What was loaded before stays */
		status(text);
	}
}

static void org_loaded(const char *data, long code)
{
	if (code == 401) {
		github_refused();
		return;
	}
	static const char *const path[2] = {"organization", "repositories"};
	char text[160];
	page_loaded(&org, data, code, path, ORG_REPOS_QUERY, org_login, org_loaded, text, sizeof(text));
	if (all.repos != &org)
		return;
	if (!text[0]) {
		all_go(all.pager.page);
		org_status(org.count ? NULL : _("No repositories"));
	} else if (!org.count) {
		org_status(text);
	}
}

static void org_cb(lv_event_t *e)
{
	unsigned i = (uintptr_t)lv_event_get_user_data(e);
	if (i >= o.count || all.scr)
		return;
	clear(&org);
	org.loaded = org.more = false;
	org_login = o.items[i].login;
	all_open(&org, o.items[i].name);
	org_status(_("Loading..."));
	char login[128], variables[200];
	json_escape(login, sizeof(login), org_login);
	snprintf(variables, sizeof(variables), "{\"login\":\"%s\",\"after\":null}", login);
	graphql(&org.fetch, ORG_REPOS_QUERY, variables, org_loaded);
}

/* Organizations' photos, kept for the process (forgotten when signing out), by their
 * address: asked once, one at a time; tex 0 when it could not be had */
static struct {
	struct {
		const char *url; /* interned */
		unsigned tex;
	} items[ORGS];
	unsigned count;
	const char *fetching; /* its address */
	struct ui_fetch fetch;
} avatars;

static unsigned avatar_find(const char *url, bool *asked)
{
	for (unsigned i = 0; i < avatars.count; i++) {
		if (avatars.items[i].url == url) {
			*asked = true;
			return avatars.items[i].tex;
		}
	}
	*asked = avatars.fetching == url;
	return 0;
}

/* Transparent out of the circle: a soft edge (alpha kept inside) */
static void round_alpha(uint8_t *pixels, int w, int h)
{
	float cx = w / 2.0f, cy = h / 2.0f, r = (w < h ? w : h) / 2.0f;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			float a = r - sqrtf(dx * dx + dy * dy) + 0.5f;
			uint8_t *p = pixels + (y * w + x) * 4;
			if (a <= 0)
				p[3] = 0;
			else if (a < 1)
				p[3] = (uint8_t)(p[3] * a);
		}
	}
}

/* A photo GitHub sent, PNG or JPEG, round: a texture, 0 if it cannot be read */
static unsigned avatar_texture(const char *data, size_t size)
{
	int w = 0, h = 0;
	uint8_t *pixels = NULL;
	png_image img = {.version = PNG_IMAGE_VERSION};
	if (png_image_begin_read_from_memory(&img, data, size)) {
		/* LVGL's ARGB8888 is B, G, R, A in memory */
		img.format = PNG_FORMAT_BGRA;
		pixels = malloc(PNG_IMAGE_SIZE(img));
		if (pixels && png_image_finish_read(&img, NULL, pixels, 0, NULL)) {
			w = (int)img.width;
			h = (int)img.height;
		} else {
			png_image_free(&img);
			free(pixels);
			pixels = NULL;
		}
	} else {
		FILE *f = fmemopen((void *)data, size, "rb");
		if (f) {
			pixels = ui_jpeg_read(f, AVATAR, AVATAR, true, &w, &h);
			fclose(f);
		}
	}
	if (!pixels)
		return 0;
	round_alpha(pixels, w, h);
	unsigned tex = ui_texture_create(pixels, w, h);
	free(pixels);
	return tex;
}

static void orgs_update(void);
static void avatars_next(void);

static void avatar_loaded(const char *data, long code)
{
	unsigned tex = data && code == 200 ? avatar_texture(data, avatars.fetch.size) : 0;
	if (!tex)
		printf("github: %s: HTTP %ld, no photo\n", avatars.fetching, code);
	if (avatars.count < ORGS) {
		avatars.items[avatars.count].url = avatars.fetching;
		avatars.items[avatars.count++].tex = tex;
	}
	avatars.fetching = NULL;
	if (tex)
		orgs_update();
	avatars_next();
}

/* The next organization's photo not asked for yet */
static void avatars_next(void)
{
	if (avatars.fetching)
		return;
	for (unsigned i = 0; i < o.count; i++) {
		bool asked;
		if (o.items[i].avatar && !avatar_find(o.items[i].avatar, &asked) && !asked) {
			avatars.fetching = o.items[i].avatar;
			ui_fetch_start(&avatars.fetch, avatars.fetching, avatar_loaded);
			return;
		}
	}
}

/* Its first letter, upper case (ASCII; others a sign) */
static void initial(char out[2], const char *name)
{
	char c = name[0];
	out[0] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '#';
	out[1] = '\0';
}

static void orgs_update(void)
{
	if (!v.list)
		return;
	lv_obj_clean(v.orgs);
	if (o.no_scope) {
		lv_obj_t *row = ui_row_create(v.orgs, LV_SIZE_CONTENT, false, NULL, NULL);
		lv_obj_set_style_pad_ver(row, 20, 0);
		lv_obj_t *hint = wrapped_label(row, _("To see them, add the read:org scope to the token on github.com."),
					      UI_FONT_BODY, UI_COLOR_DIM);
		lv_obj_set_flex_grow(hint, 1);
	}
	for (unsigned i = 0; i < o.count; i++) {
		lv_obj_t *row = ui_row_create(v.orgs, 96, i + 1 < o.count, org_cb, (void *)(uintptr_t)i);
		/* Its photo, or its initial */
		lv_obj_t *avatar = ui_avatar_create(row, AVATAR);
		char letter[2];
		initial(letter, o.items[i].name);
		ui_avatar_set(avatar, AVATAR_COLOR, letter);
		bool asked;
		unsigned tex = o.items[i].avatar ? avatar_find(o.items[i].avatar, &asked) : 0;
		if (tex)
			ui_avatar_set_photo(avatar, tex);
		lv_obj_t *label = ui_one_line_label(row, o.items[i].name, UI_FONT_BODY);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_t *count = ui_dim_label(row, "", UI_FONT_BODY);
		ui_set_text_fmt(count, "%lld", o.items[i].repos);
		ui_dim_label(row, LV_SYMBOL_RIGHT, &ui_font_24);
	}
	lv_obj_set_hidden(v.orgs_label, !o.count && !o.no_scope);
	lv_obj_set_hidden(v.orgs, !o.count && !o.no_scope);
}

/* What was there stays if they cannot be had, but for the scope missing */
static void orgs_loaded(const char *data, long code)
{
	struct json *j = data && code == 200 ? json_parse(data) : NULL;
	char error[128];
	const struct json *d = j ? graphql_data(j, error, sizeof(error)) : NULL;
	const struct json *nodes = json_get(json_get(json_get(d, "viewer"), "organizations"), "nodes");
	const struct json *errors = json_get(j, "errors");
	bool no_scope = !strcmp(json_str(json_get(errors ? errors->child : NULL, "type"), ""), "INSUFFICIENT_SCOPES");
	if (no_scope || (nodes && nodes->kind == JSON_ARRAY)) {
		o.no_scope = no_scope;
		o.count = 0;
		for (const struct json *e = nodes ? nodes->child : NULL; e && o.count < ORGS; e = e->next) {
			const char *login = json_str(json_get(e, "login"), "");
			o.items[o.count].login = ui_intern(login);
			o.items[o.count].name = ui_intern(json_str(json_get(e, "name"), login));
			const char *avatar = json_str(json_get(e, "avatarUrl"), NULL);
			o.items[o.count].avatar = avatar ? ui_intern(avatar) : NULL;
			o.items[o.count++].repos = json_int(json_get(json_get(e, "repositories"), "totalCount"), 0);
		}
		orgs_update();
		avatars_next();
	} else if (code != 200 || error[0]) {
		printf("github: organizations: HTTP %ld %s\n", code, error);
	}
	json_free(j);
}

void repos_load(void)
{
	if (!r.loaded)
		status(_("Loading..."));
	r.more = false;
	graphql(&r.fetch, REPOS_QUERY, "{\"after\":null}", loaded);
	graphql(&o.fetch, ORGS_QUERY, "{}", orgs_loaded);
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	/* What it loads would have nowhere to go */
	ui_fetch_stop(&r.fetch);
	ui_fetch_stop(&o.fetch);
	memset(&v, 0, sizeof(v));
}

static void pull_cb(void *user_data)
{
	LV_UNUSED(user_data);
	repos_load();
}

void repos_create(lv_obj_t *body)
{
	v.list = lv_obj_create(body);
	lv_obj_remove_style_all(v.list);
	lv_obj_set_width(v.list, LV_PCT(100));
	lv_obj_set_flex_grow(v.list, 1);
	lv_obj_set_flex_flow(v.list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(v.list, 16, 0);
	lv_obj_set_scroll_dir(v.list, LV_DIR_VER);
	lv_obj_add_event_cb(v.list, delete_cb, LV_EVENT_DELETE, NULL);
	ui_pull_refresh(v.list, pull_cb, NULL);

	/* Bookmarks, then the account's own under a heading of their own, then its
	 * organizations */
	v.bookmarks_label = ui_section_label(v.list, _("Bookmarks"));
	v.bookmarks = ui_card_create(v.list);
	v.own_label = ui_section_label(v.list, _("Your repositories"));
	v.empty = ui_empty_label(v.list, "");
	lv_obj_set_hidden(v.empty, true);
	v.card = ui_card_create(v.list);
	lv_obj_set_hidden(v.card, true);
	v.orgs_label = ui_section_label(v.list, _("Organizations"));
	v.orgs = ui_card_create(v.list);
	bookmarks_load();
	bookmarks_update();
	rows_update();
	orgs_update();
}

void repos_show(bool shown)
{
	if (v.list)
		lv_obj_set_hidden(v.list, !shown);
}

void repos_forget_account(void)
{
	ui_fetch_stop(&r.fetch);
	ui_fetch_stop(&o.fetch);
	clear(&r);
	r.loaded = false;
	o.count = 0;
	o.no_scope = false;
	ui_fetch_stop(&avatars.fetch);
	for (unsigned i = 0; i < avatars.count; i++)
		ui_texture_delete(avatars.items[i].tex);
	memset(&avatars, 0, sizeof(avatars));
	rows_update();
	orgs_update();
	status(NULL);
}

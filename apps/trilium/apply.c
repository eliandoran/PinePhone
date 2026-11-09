/*
 * What a sync pulls, into the database, as Trilium's sync_update.ts puts it: a change
 * already applied (its changeId known) is skipped; one newer than the local change of
 * that entity, or as new, or the first of it, replaces the row (or erases it) and is
 * recorded as the server's (its instanceId); an older one leaves the local row, which
 * Trilium would push back (this client makes no changes of its own yet). A change
 * dated in the future counts as now. note_reordering sets the branches' positions;
 * an option that is not synced comes without its row and is skipped. Blobs' content
 * comes in base64 and is kept as bytes, empty content as "".
 *
 * Rows go in with the columns the server named, which must be this schema's (a newer
 * Trilium's would be lost otherwise): the tables and columns are checked against it,
 * so a name from the server never reaches the SQL unchecked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "trilium.h"

/* The tables sync sends, their primary keys; those that can be erased */
static const struct {
	const char *name, *key;
	bool erasable;
} tables[] = {
	{"notes", "noteId", true},
	{"branches", "branchId", true},
	{"attributes", "attributeId", true},
	{"revisions", "revisionId", true},
	{"attachments", "attachmentId", true},
	{"blobs", "blobId", true},
	{"options", "name", false},
	{"etapi_tokens", "etapiTokenId", false},
	{"recent_notes", "noteId", false},
};

static int table_find(const char *name)
{
	for (size_t i = 0; i < sizeof(tables) / sizeof(*tables); i++)
		if (!strcmp(tables[i].name, name))
			return (int)i;
	return -1;
}

static bool column_exists(sqlite3 *db, const char *table, const char *column)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?) WHERE name = ?", -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, column, -1, SQLITE_STATIC);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return found;
}

static int base64_value(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	return -1;
}

/* Bytes of base64 text (malloc()ed), *size their length; NULL if it is not base64 */
static unsigned char *base64_decode(const char *text, size_t *size)
{
	size_t len = strlen(text);
	unsigned char *out = malloc(len / 4 * 3 + 3);
	if (!out)
		return NULL;
	size_t n = 0;
	unsigned bits = 0, count = 0;
	for (const char *p = text; *p; p++) {
		if (*p == '=' || *p == '\n' || *p == '\r')
			continue;
		int v = base64_value(*p);
		if (v < 0) {
			free(out);
			return NULL;
		}
		bits = bits << 6 | (unsigned)v;
		if (++count == 4) {
			out[n++] = (unsigned char)(bits >> 16);
			out[n++] = (unsigned char)(bits >> 8);
			out[n++] = (unsigned char)bits;
			bits = count = 0;
		}
	}
	if (count == 3) {
		out[n++] = (unsigned char)(bits >> 10);
		out[n++] = (unsigned char)(bits >> 2);
	} else if (count == 2) {
		out[n++] = (unsigned char)(bits >> 4);
	} else if (count == 1) {
		free(out);
		return NULL;
	}
	*size = n;
	return out;
}

/* A JSON value bound as SQLite's: strings as text (a blob's content as its bytes),
 * whole numbers as integers, booleans as 0 or 1 */
static bool value_bind(sqlite3_stmt *st, int i, const struct json *v, bool content)
{
	switch (v->kind) {
	case JSON_NULL:
		return sqlite3_bind_null(st, i) == SQLITE_OK;
	case JSON_BOOL:
		return sqlite3_bind_int(st, i, v->num != 0) == SQLITE_OK;
	case JSON_NUMBER:
		if ((double)v->num == v->dbl)
			return sqlite3_bind_int64(st, i, v->num) == SQLITE_OK;
		return sqlite3_bind_double(st, i, v->dbl) == SQLITE_OK;
	case JSON_STRING:
		if (content && v->str[0]) {
			size_t size;
			unsigned char *bytes = base64_decode(v->str, &size);
			if (!bytes)
				return false;
			if (!size) {
				free(bytes);
				return sqlite3_bind_text(st, i, "", 0, SQLITE_STATIC) == SQLITE_OK;
			}
			return sqlite3_bind_blob64(st, i, bytes, size, free) == SQLITE_OK;
		}
		return sqlite3_bind_text(st, i, v->str, -1, SQLITE_TRANSIENT) == SQLITE_OK;
	default:
		return false;
	}
}

/* The entity's row in its table (sql.replace()) */
static bool row_replace(sqlite3 *db, const char *table, const struct json *row, const char **error)
{
	*error = N_("The server sent notes this app cannot store (a newer version of Trilium?).");
	if (!row || row->kind != JSON_OBJECT || !row->child)
		return false;
	char *sql;
	size_t len;
	FILE *f = open_memstream(&sql, &len);
	fprintf(f, "INSERT OR REPLACE INTO \"%s\" (", table); /* NON-NLS */
	int n = 0;
	for (const struct json *c = row->child; c; c = c->next) {
		/* Checked against the schema: a name of it, quoted safely */
		if (!column_exists(db, table, c->key) || strchr(c->key, '"')) {
			fclose(f);
			free(sql);
			fprintf(stderr, "trilium: %s has no column %s\n", table, c->key);
			return false;
		}
		fprintf(f, "%s\"%s\"", n++ ? ", " : "", c->key);
	}
	fputs(") VALUES (", f);
	for (int i = 0; i < n; i++)
		fputs(i ? ", ?" : "?", f);
	fputs(")", f);
	fclose(f);

	sqlite3_stmt *st;
	bool ok = sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
	free(sql);
	int i = 1;
	bool blob = !strcmp(table, "blobs");
	for (const struct json *c = row->child; ok && c; c = c->next)
		ok = value_bind(st, i++, c, blob && !strcmp(c->key, "content"));
	if (ok)
		ok = sqlite3_step(st) == SQLITE_DONE;
	if (!ok) {
		fprintf(stderr, "trilium: %s not stored: %s\n", table, sqlite3_errmsg(db));
		*error = N_("The notes could not be saved on the phone.");
	}
	sqlite3_finalize(st);
	return ok;
}

static bool row_erase(sqlite3 *db, int t, const char *id)
{
	if (!tables[t].erasable) {
		fprintf(stderr, "trilium: cannot erase %s %s\n", tables[t].name, id);
		return true; /* as Trilium: logged, the change recorded */
	}
	char sql[128];
	snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE \"%s\" = ?", tables[t].name, tables[t].key); /* NON-NLS */
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

static bool reorder(sqlite3 *db, const struct json *positions)
{
	if (!positions || positions->kind != JSON_OBJECT)
		return false;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "UPDATE branches SET notePosition = ? WHERE branchId = ?", -1, &st, NULL) != SQLITE_OK)
		return false;
	bool ok = true;
	for (const struct json *p = positions->child; ok && p; p = p->next) {
		sqlite3_reset(st);
		sqlite3_bind_int64(st, 1, json_int(p, 0));
		sqlite3_bind_text(st, 2, p->key, -1, SQLITE_STATIC);
		ok = sqlite3_step(st) == SQLITE_DONE;
	}
	sqlite3_finalize(st);
	return ok;
}

/* isErased, isSynced: 0 or 1 as Trilium's rows have them, or true and false */
static bool flag(const struct json *v)
{
	return v && (v->kind == JSON_BOOL || v->kind == JSON_NUMBER) && v->num != 0;
}

/* The change recorded as the server's (entity_changes.ts' putEntityChange()) */
static bool change_put(sqlite3 *db, const struct json *ec, const char *date, const char *instance)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db,
	                       "INSERT OR REPLACE INTO entity_changes (entityName, entityId, hash, isErased, changeId,"
	                       " componentId, instanceId, isSynced, utcDateChanged) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
	                       -1, &st, NULL) != SQLITE_OK)
		return false;
	char change_id[16];
	if (!json_str(json_get(ec, "changeId"), "")[0]) {
		/* As randomString(12) */
		static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		for (int i = 0; i < 12; i++)
			change_id[i] = chars[rand() % (int)(sizeof(chars) - 1)];
		change_id[12] = '\0';
	}
	const char *component = json_str(json_get(ec, "componentId"), "");
	sqlite3_bind_text(st, 1, json_str(json_get(ec, "entityName"), ""), -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, json_str(json_get(ec, "entityId"), ""), -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, json_str(json_get(ec, "hash"), ""), -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 4, flag(json_get(ec, "isErased")));
	sqlite3_bind_text(st, 5, json_str(json_get(ec, "changeId"), "")[0] ? json_str(json_get(ec, "changeId"), "") : change_id,
	                  -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 6, component[0] ? component : "NA", -1, SQLITE_STATIC); /* NON-NLS */
	sqlite3_bind_text(st, 7, instance, -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 8, flag(json_get(ec, "isSynced")));
	sqlite3_bind_text(st, 9, date, -1, SQLITE_STATIC);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	sqlite3_finalize(st);
	return ok;
}

/* utcDateChanged of the local change of that entity, false if there is none */
static bool local_date(sqlite3 *db, const char *name, const char *id, char *date, size_t size)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT utcDateChanged FROM entity_changes WHERE entityName = ? AND entityId = ?", -1,
	                       &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	if (found)
		snprintf(date, size, "%s", sqlite3_column_text(st, 0) ? (const char *)sqlite3_column_text(st, 0) : "");
	sqlite3_finalize(st);
	return found;
}

static bool applied_already(sqlite3 *db, const char *change_id)
{
	if (!change_id[0])
		return false;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "SELECT 1 FROM entity_changes WHERE changeId = ?", -1, &st, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(st, 1, change_id, -1, SQLITE_STATIC);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return found;
}

/* One record; false with *error if it could not be applied */
static bool apply_one(sqlite3 *db, const struct json *record, const char *instance, const char *now, bool *applied,
                      const char **error)
{
	*applied = false;
	*error = N_("The server's answer could not be read.");
	const struct json *ec = json_get(record, "entityChange");
	const struct json *entity = json_get(record, "entity");
	if (entity && entity->kind == JSON_NULL)
		entity = NULL;
	const char *name = json_str(json_get(ec, "entityName"), "");
	const char *id = json_str(json_get(ec, "entityId"), "");
	if (!name[0] || !id[0])
		return false;
	if (applied_already(db, json_str(json_get(ec, "changeId"), "")))
		return true;
	if (!entity && !strcmp(name, "options"))
		return true; /* an option that is not synced */

	/* A date in the future counts as now */
	const char *date = json_str(json_get(ec, "utcDateChanged"), "");
	if (strcmp(date, now) > 0)
		date = now;

	*error = N_("The notes could not be saved on the phone.");
	if (!strcmp(name, "note_reordering")) {
		if (!reorder(db, entity) || !change_put(db, ec, date, instance))
			return false;
		*applied = true;
		return true;
	}

	int t = table_find(name);
	if (t < 0) {
		*error = N_("The server sent notes this app cannot store (a newer version of Trilium?).");
		fprintf(stderr, "trilium: unknown entity %s\n", name);
		return false;
	}
	char local[40];
	bool known = local_date(db, name, id, local, sizeof(local));
	/* The local change wins only if it is newer, and not from the future */
	if (known && strcmp(local, now) <= 0 && strcmp(local, date) > 0) {
		fprintf(stderr, "trilium: %s %s: the local change is newer, kept\n", name, id);
		return true;
	}
	if (flag(json_get(ec, "isErased"))) {
		if (!row_erase(db, t, id))
			return false;
	} else if (!row_replace(db, name, entity, error)) {
		return false;
	}
	if (!change_put(db, ec, date, instance))
		return false;
	*applied = true;
	return true;
}

bool tr_apply(sqlite3 *db, const struct json *records, const char *instance, long long *applied,
              const char **error)
{
	char now[32];
	tr_utc_now(now, sizeof(now));
	if (!records || records->kind != JSON_ARRAY) {
		*error = N_("The server's answer could not be read.");
		return false;
	}
	for (const struct json *r = records->child; r; r = r->next) {
		bool done;
		if (!apply_one(db, r, instance, now, &done, error)) {
			const struct json *ec = json_get(r, "entityChange");
			fprintf(stderr, "trilium: %s %s not applied: %s\n", json_str(json_get(ec, "entityName"), "?"),
			        json_str(json_get(ec, "entityId"), "?"), sqlite3_errmsg(db));
			return false;
		}
		*applied += done;
	}
	return true;
}

#include "contacts.h"

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sipuri.h"
#include "store.h"

struct sp_contacts {
    sqlite3         *db;
    struct sp_store *store;

    /* Three result buffers, not one. The panel asks who is calling on
       every refresh while the contacts list may be on screen, so a
       lookup must not free the list the frontend is still reading. */
    sp_contact      *rows;
    size_t           n_rows;
    sp_contact       got;
    sp_contact       found;

    /* sp_contacts_picture's own result, separate from got/found so a
       row delegate calling it does not disturb whichever of those the
       call panel or the list is still reading. */
    void            *pic;
    size_t           pic_len;
    char            *pic_type;

    pthread_mutex_t  lock;
};

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

void sp_contacts_normalise(const char *in, char *out, size_t outlen) {
    if (!out || outlen == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    /* The user part, so that sip:6046@pbx and 6046 are one number.
       An input that is not a URI comes back as itself. */
    sp_party p;
    sp_sipuri_parse(in, &p);
    const char *src = p.user[0] ? p.user : in;

    char digits[64];
    size_t n = 0;
    bool plus = false;
    for (const char *s = src; *s && n + 1 < sizeof digits; s++) {
        if (*s == '+' && n == 0 && !plus) {
            plus = true;
            continue;
        }
        if (isdigit((unsigned char)*s))
            digits[n++] = *s;
    }
    digits[n] = '\0';

    /* NANP: eleven digits beginning with a 1 are a country code and
       ten significant ones, whether or not a plus was written. */
    const char *keep = (n == 11 && digits[0] == '1') ? digits + 1 : digits;

    snprintf(out, outlen, "%s", keep);
}

sp_contacts *sp_contacts_create(struct sp_store *store) {
    if (!store)
        return NULL;
    sp_contacts *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->store = store;
    c->db = sp_store_handle(store);
    if (pthread_mutex_init(&c->lock, NULL) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

static void free_one(sp_contact *c) {
    if (!c)
        return;
    free((void *)c->name);
    free((void *)c->company);
    free((void *)c->note);
    for (size_t i = 0; i < c->n_numbers; i++) {
        free((void *)c->numbers[i].label);
        free((void *)c->numbers[i].uri);
    }
    free((void *)c->numbers);
    free((void *)c->picture);
    free((void *)c->picture_type);
    memset(c, 0, sizeof *c);
}

/* Frees sp_contacts_picture's own result buffer, distinct from
   free_one because it is not attached to an sp_contact. */
static void free_pic(sp_contacts *c) {
    free(c->pic);
    c->pic = NULL;
    c->pic_len = 0;
    free(c->pic_type);
    c->pic_type = NULL;
}

static void free_rows(sp_contacts *c) {
    for (size_t i = 0; i < c->n_rows; i++)
        free_one(&c->rows[i]);
    free(c->rows);
    c->rows = NULL;
    c->n_rows = 0;
}

void sp_contacts_destroy(sp_contacts *c) {
    if (!c)
        return;
    free_rows(c);
    free_one(&c->got);
    free_one(&c->found);
    free_pic(c);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

static char *dup_col(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? strdup((const char *)t) : NULL;
}

/* Copies a BLOB column into a fresh buffer the caller owns; NULL when
   the column is NULL or empty, which sp_contact's picture/picture_len
   pair treats as "no picture" rather than "an empty one". */
static void *dup_blob_col(sqlite3_stmt *st, int col, size_t *len_out) {
    const void *blob = sqlite3_column_blob(st, col);
    const int n = sqlite3_column_bytes(st, col);
    *len_out = 0;
    if (!blob || n <= 0)
        return NULL;
    void *copy = malloc((size_t)n);
    if (!copy)
        return NULL;
    memcpy(copy, blob, (size_t)n);
    *len_out = (size_t)n;
    return copy;
}

/* Fills a contact's numbers, in the order they were saved. A contact
   with no numbers cannot be saved, so an empty list here means the
   rows were removed underneath us and the caller sees n_numbers 0
   rather than a dangling pointer. */
static void load_numbers(sp_contacts *c, sp_contact *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
            "SELECT id, label, uri FROM contact_number"
            " WHERE contact_id = ? ORDER BY position, id;", -1, &st, NULL)
        != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, out->id);

    sp_contact_number *list = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            const size_t grow = cap ? cap * 2 : 4;
            sp_contact_number *r = realloc(list, grow * sizeof *r);
            if (!r)
                break;
            list = r;
            cap = grow;
        }
        list[n].id = (int64_t)sqlite3_column_int64(st, 0);
        list[n].label = dup_col(st, 1);
        list[n].uri = dup_col(st, 2);
        n++;
    }
    sqlite3_finalize(st);
    out->numbers = list;
    out->n_numbers = n;
}

static void fill_contact(sp_contacts *c, sqlite3_stmt *st, sp_contact *out) {
    memset(out, 0, sizeof *out);
    out->id = (int64_t)sqlite3_column_int64(st, 0);
    out->name = dup_col(st, 1);
    out->company = dup_col(st, 2);
    out->note = dup_col(st, 3);
    out->favourite = sqlite3_column_int(st, 4) != 0;
    load_numbers(c, out);
}

#define CONTACT_COLS "id, name, company, note, favourite"

/* Appended to CONTACT_COLS for the single-contact reads only. The
   list query leaves these out on purpose: it fills a scrolling view,
   and at 512 KB apiece a book of two hundred would put a hundred
   megabytes through one call. */
#define CONTACT_COLS_PICTURE CONTACT_COLS ", picture, picture_type"
#define CONTACT_COL_PICTURE      5
#define CONTACT_COL_PICTURE_TYPE 6

static void fill_picture(sqlite3_stmt *st, sp_contact *out) {
    out->picture = dup_blob_col(st, CONTACT_COL_PICTURE, &out->picture_len);
    out->picture_type = dup_col(st, CONTACT_COL_PICTURE_TYPE);
}

/* A contains-anywhere LIKE pattern for what an operator typed.
 *
 * Binding the value keeps it out of the SQL, which is the injection
 * question, and answers nothing about LIKE: % and _ are wildcards
 * wherever they come from, so a search for "%" bound straight in
 * matches the whole book and a contact actually called "100%" can
 * never be found. They are escaped here, and the statements say
 * ESCAPE. */
static void like_pattern(const char *search, char *out, size_t outlen) {
    size_t n = 0;
    if (outlen < 4) {
        if (outlen)
            out[0] = '\0';
        return;
    }
    out[n++] = '%';
    for (const char *s = search; *s && n + 3 < outlen; s++) {
        if (*s == '%' || *s == '_' || *s == '\\')
            out[n++] = '\\';
        out[n++] = *s;
    }
    out[n++] = '%';
    out[n] = '\0';
}

sp_status sp_contacts_query(sp_contacts *c, const char *search, size_t limit,
                            const sp_contact **out, size_t *n_out) {
    if (!out || !n_out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    *n_out = 0;
    if (!c)
        return SP_OK;
    if (limit == 0 || limit > 100000)
        limit = 1000;

    const bool filtered = search && *search;

    pthread_mutex_lock(&c->lock);
    free_rows(c);

    /* Favourites first, then by name. A book an operator reaches for
       under load should put the numbers they reach for at the top. */
    const char *sql = filtered
        ? "SELECT " CONTACT_COLS " FROM contact WHERE"
          " name LIKE ?1 ESCAPE '\\' OR company LIKE ?1 ESCAPE '\\'"
          " OR id IN (SELECT contact_id FROM contact_number WHERE"
          "  uri LIKE ?1 ESCAPE '\\' OR match_key LIKE ?1 ESCAPE '\\')"
          " ORDER BY favourite DESC, name COLLATE NOCASE LIMIT ?2;"
        : "SELECT " CONTACT_COLS " FROM contact"
          " ORDER BY favourite DESC, name COLLATE NOCASE LIMIT ?2;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&c->lock);
        return SP_ERR_INTERNAL;
    }
    if (filtered) {
        char like[256];
        like_pattern(search, like, sizeof like);
        sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);

    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (c->n_rows == cap) {
            const size_t grow = cap ? cap * 2 : 32;
            sp_contact *r = realloc(c->rows, grow * sizeof *r);
            if (!r)
                break;
            c->rows = r;
            cap = grow;
        }
        fill_contact(c, st, &c->rows[c->n_rows]);
        c->n_rows++;
    }
    sqlite3_finalize(st);

    *out = c->rows;
    *n_out = c->n_rows;
    pthread_mutex_unlock(&c->lock);
    return SP_OK;
}

sp_status sp_contacts_get(sp_contacts *c, int64_t id, const sp_contact **out) {
    if (!out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    if (!c)
        return SP_ERR_NOT_FOUND;

    pthread_mutex_lock(&c->lock);
    free_one(&c->got);

    sqlite3_stmt *st = NULL;
    sp_status rc = SP_ERR_NOT_FOUND;
    if (sqlite3_prepare_v2(c->db,
            "SELECT " CONTACT_COLS_PICTURE " FROM contact WHERE id = ?;", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fill_contact(c, st, &c->got);
            fill_picture(st, &c->got);
            *out = &c->got;
            rc = SP_OK;
        }
    } else {
        rc = SP_ERR_INTERNAL;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

sp_status sp_contacts_lookup(sp_contacts *c, const char *uri,
                             const sp_contact **out) {
    if (!out)
        return SP_ERR_INVALID_ARG;
    *out = NULL;
    if (!c || !uri || !*uri)
        return SP_ERR_NOT_FOUND;

    char key[64];
    sp_contacts_normalise(uri, key, sizeof key);
    if (!key[0])
        return SP_ERR_NOT_FOUND;

    pthread_mutex_lock(&c->lock);
    free_one(&c->found);

    sqlite3_stmt *st = NULL;
    sp_status rc = SP_ERR_NOT_FOUND;
    if (sqlite3_prepare_v2(c->db,
            "SELECT " CONTACT_COLS_PICTURE " FROM contact WHERE id ="
            " (SELECT contact_id FROM contact_number WHERE match_key = ?"
            "  ORDER BY position, id LIMIT 1);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fill_contact(c, st, &c->found);
            fill_picture(st, &c->found);
            *out = &c->found;
            rc = SP_OK;
        }
    } else {
        rc = SP_ERR_INTERNAL;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

sp_status sp_contacts_picture(sp_contacts *c, int64_t id, const void **data,
                              size_t *len, const char **type) {
    if (!data || !len || !type)
        return SP_ERR_INVALID_ARG;
    *data = NULL;
    *len = 0;
    *type = NULL;
    if (!c)
        return SP_ERR_NOT_FOUND;

    pthread_mutex_lock(&c->lock);
    free_pic(c);

    sqlite3_stmt *st = NULL;
    sp_status rc = SP_ERR_NOT_FOUND;
    if (sqlite3_prepare_v2(c->db,
            "SELECT picture, picture_type FROM contact WHERE id = ?;", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            c->pic = dup_blob_col(st, 0, &c->pic_len);
            c->pic_type = dup_col(st, 1);
            rc = SP_OK;
        }
    } else {
        rc = SP_ERR_INTERNAL;
    }
    sqlite3_finalize(st);

    *data = c->pic;
    *len = c->pic_len;
    *type = c->pic_type;
    pthread_mutex_unlock(&c->lock);
    return rc;
}

static int bind_or_null(sqlite3_stmt *st, int idx, const char *s) {
    return (s && *s) ? sqlite3_bind_text(st, idx, s, -1, SQLITE_TRANSIENT)
                     : sqlite3_bind_null(st, idx);
}

static int bind_picture(sqlite3_stmt *st, int idx, const void *data,
                        size_t len) {
    return (data && len > 0)
               ? sqlite3_bind_blob(st, idx, data, (int)len, SQLITE_TRANSIENT)
               : sqlite3_bind_null(st, idx);
}

/* Bound from the same has-a-picture test as bind_picture() above, not
   from type alone: a row whose blob is NULL must not come out with a
   picture_type set, or a reader sees a type with nothing to decode. */
static int bind_picture_type(sqlite3_stmt *st, int idx, const void *data,
                             size_t len, const char *type) {
    return (data && len > 0) ? bind_or_null(st, idx, type)
                             : sqlite3_bind_null(st, idx);
}

/* Replaces the whole number list rather than reconciling it. An editor
   hands back what the contact now has; working out which rows moved
   would be a second model of the same thing, and the first one to
   disagree wins silently. */
static bool write_numbers(sp_contacts *c, int64_t id,
                          const sp_contact_number *nums, size_t n) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
            "DELETE FROM contact_number WHERE contact_id = ?;", -1, &st, NULL)
        != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    const bool cleared = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!cleared)
        return false;

    for (size_t i = 0; i < n; i++) {
        if (!nums[i].uri || !*nums[i].uri)
            continue;
        char key[64];
        sp_contacts_normalise(nums[i].uri, key, sizeof key);
        st = NULL;
        if (sqlite3_prepare_v2(c->db,
                "INSERT INTO contact_number"
                " (contact_id, label, uri, match_key, position)"
                " VALUES (?, ?, ?, ?, ?);", -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(st, 1, id);
        bind_or_null(st, 2, nums[i].label);
        sqlite3_bind_text(st, 3, nums[i].uri, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, (int)i);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
    }
    return true;
}

/* A contact with no dialable number is a note, and a book full of
   notes is how a directory stops being trusted. */
static size_t count_dialable(const sp_contact *in) {
    size_t n = 0;
    for (size_t i = 0; i < in->n_numbers; i++)
        if (in->numbers[i].uri && *in->numbers[i].uri)
            n++;
    return n;
}

sp_status sp_contacts_save(sp_contacts *c, const sp_contact *in,
                           int64_t *id_out) {
    if (!c || !in)
        return SP_ERR_INVALID_ARG;
    if (!in->name || !*in->name)
        return SP_ERR_INVALID_ARG;
    if (count_dialable(in) == 0)
        return SP_ERR_INVALID_ARG;
    /* An upper bound the format and the size together should never
       reach, kept as the thing that catches it if they do. 256x256 PNG
       of an avatar is usually well under 50 KB. */
    if (in->picture_len > 512 * 1024)
        return SP_ERR_INVALID_ARG;
    /* picture and picture_len must agree on whether there is a
       picture. A NULL pointer with a non-zero length, or a non-NULL
       pointer with a zero length, is a caller bug, and letting it
       through here means it degrades silently to "no picture"
       downstream instead of being refused where it happened. */
    if ((in->picture != NULL) != (in->picture_len > 0))
        return SP_ERR_INVALID_ARG;
    /* PNG is the only thing stored, so it is the only thing accepted:
       a type nothing can draw is worse stored than refused. */
    if (in->picture &&
        (!in->picture_type || strcmp(in->picture_type, "png") != 0))
        return SP_ERR_INVALID_ARG;

    pthread_mutex_lock(&c->lock);
    if (sp_store_exec(c->store, "BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&c->lock);
        return SP_ERR_INTERNAL;
    }

    const uint64_t now = now_us();
    int64_t id = in->id;
    sqlite3_stmt *st = NULL;
    bool ok = false;

    if (id == 0) {
        if (sqlite3_prepare_v2(c->db,
                "INSERT INTO contact"
                " (name, company, note, favourite, created_us, updated_us,"
                "  picture, picture_type)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?);", -1, &st, NULL)
            == SQLITE_OK) {
            sqlite3_bind_text(st, 1, in->name, -1, SQLITE_TRANSIENT);
            bind_or_null(st, 2, in->company);
            bind_or_null(st, 3, in->note);
            sqlite3_bind_int(st, 4, in->favourite ? 1 : 0);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
            bind_picture(st, 7, in->picture, in->picture_len);
            bind_picture_type(st, 8, in->picture, in->picture_len,
                              in->picture_type);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (ok)
                id = (int64_t)sqlite3_last_insert_rowid(c->db);
        }
    } else {
        if (sqlite3_prepare_v2(c->db,
                "UPDATE contact SET name = ?, company = ?, note = ?,"
                " favourite = ?, updated_us = ?, picture = ?,"
                " picture_type = ? WHERE id = ?;", -1, &st, NULL)
            == SQLITE_OK) {
            sqlite3_bind_text(st, 1, in->name, -1, SQLITE_TRANSIENT);
            bind_or_null(st, 2, in->company);
            bind_or_null(st, 3, in->note);
            sqlite3_bind_int(st, 4, in->favourite ? 1 : 0);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
            bind_picture(st, 6, in->picture, in->picture_len);
            bind_picture_type(st, 7, in->picture, in->picture_len,
                              in->picture_type);
            sqlite3_bind_int64(st, 8, id);
            ok = sqlite3_step(st) == SQLITE_DONE
                 && sqlite3_changes(c->db) > 0;
            sqlite3_finalize(st);
        }
    }

    if (ok)
        ok = write_numbers(c, id, in->numbers, in->n_numbers);

    sp_store_exec(c->store, ok ? "COMMIT;" : "ROLLBACK;");
    pthread_mutex_unlock(&c->lock);

    if (!ok)
        return in->id ? SP_ERR_NOT_FOUND : SP_ERR_INTERNAL;
    if (id_out)
        *id_out = id;
    return SP_OK;
}

sp_status sp_contacts_delete(sp_contacts *c, int64_t id) {
    if (!c)
        return SP_ERR_INVALID_ARG;

    pthread_mutex_lock(&c->lock);
    if (sp_store_exec(c->store, "BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&c->lock);
        return SP_ERR_INTERNAL;
    }

    /* The numbers go with the contact in the same transaction. The
       schema says ON DELETE CASCADE, but foreign keys are off by
       default in SQLite and correctness must not rest on a pragma
       somebody could forget. */
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(c->db,
            "DELETE FROM contact_number WHERE contact_id = ?;", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (ok) {
        ok = false;
        st = NULL;
        if (sqlite3_prepare_v2(c->db, "DELETE FROM contact WHERE id = ?;", -1,
                               &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            ok = sqlite3_step(st) == SQLITE_DONE
                 && sqlite3_changes(c->db) > 0;
            sqlite3_finalize(st);
        }
    }

    sp_store_exec(c->store, ok ? "COMMIT;" : "ROLLBACK;");
    pthread_mutex_unlock(&c->lock);
    return ok ? SP_OK : SP_ERR_NOT_FOUND;
}

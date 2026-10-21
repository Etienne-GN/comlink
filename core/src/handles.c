#include "handles.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    uint64_t id;
    int      native_id;
    void    *data;
} row;

struct sp_handle_table {
    row            *rows;
    size_t          n;
    size_t          cap;
    uint64_t        next_id;
    pthread_mutex_t lock;
};

sp_handle_table *sp_ht_create(void) {
    sp_handle_table *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->next_id = 1;
    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void sp_ht_destroy(sp_handle_table *t) {
    if (!t)
        return;
    pthread_mutex_destroy(&t->lock);
    free(t->rows);
    free(t);
}

static int grow(sp_handle_table *t) {
    size_t cap = t->cap ? t->cap * 2 : 16;
    row *r = realloc(t->rows, cap * sizeof *r);
    if (!r)
        return -1;
    t->rows = r;
    t->cap = cap;
    return 0;
}

uint64_t sp_ht_add(sp_handle_table *t, int native_id, void *data) {
    if (!t)
        return 0;
    pthread_mutex_lock(&t->lock);
    if (t->n == t->cap && grow(t) != 0) {
        pthread_mutex_unlock(&t->lock);
        return 0;
    }
    uint64_t id = t->next_id++;
    t->rows[t->n++] = (row){ .id = id, .native_id = native_id, .data = data };
    pthread_mutex_unlock(&t->lock);
    return id;
}

/* Caller holds the lock. */
static row *find(sp_handle_table *t, uint64_t id) {
    for (size_t i = 0; i < t->n; i++)
        if (t->rows[i].id == id)
            return &t->rows[i];
    return NULL;
}

int sp_ht_lookup(sp_handle_table *t, uint64_t id, int *native_id, void **data) {
    if (!t || id == 0)
        return -1;
    pthread_mutex_lock(&t->lock);
    row *r = find(t, id);
    if (r) {
        if (native_id)
            *native_id = r->native_id;
        if (data)
            *data = r->data;
    }
    pthread_mutex_unlock(&t->lock);
    return r ? 0 : -1;
}

uint64_t sp_ht_by_native(sp_handle_table *t, int native_id) {
    if (!t)
        return 0;
    uint64_t id = 0;
    pthread_mutex_lock(&t->lock);
    for (size_t i = 0; i < t->n; i++) {
        if (t->rows[i].native_id == native_id) {
            id = t->rows[i].id;
            break;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return id;
}

int sp_ht_set_data(sp_handle_table *t, uint64_t id, void *data) {
    if (!t || id == 0)
        return -1;
    pthread_mutex_lock(&t->lock);
    row *r = find(t, id);
    if (r)
        r->data = data;
    pthread_mutex_unlock(&t->lock);
    return r ? 0 : -1;
}

void sp_ht_snapshot(sp_handle_table *t, uint64_t *ids, size_t cap,
                    size_t *n_out) {
    if (!n_out)
        return;
    *n_out = 0;
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    for (size_t i = 0; i < t->n; i++) {
        if (ids && i < cap)
            ids[i] = t->rows[i].id;
    }
    *n_out = t->n;
    pthread_mutex_unlock(&t->lock);
}

int sp_ht_set_native(sp_handle_table *t, uint64_t id, int native_id) {
    if (!t || id == 0)
        return -1;
    pthread_mutex_lock(&t->lock);
    row *r = find(t, id);
    if (r)
        r->native_id = native_id;
    pthread_mutex_unlock(&t->lock);
    return r ? 0 : -1;
}

void sp_ht_retire(sp_handle_table *t, uint64_t id) {
    if (!t || id == 0)
        return;
    pthread_mutex_lock(&t->lock);
    for (size_t i = 0; i < t->n; i++) {
        if (t->rows[i].id == id) {
            t->rows[i] = t->rows[--t->n];
            break;
        }
    }
    pthread_mutex_unlock(&t->lock);
}

size_t sp_ht_live_count(sp_handle_table *t) {
    if (!t)
        return 0;
    pthread_mutex_lock(&t->lock);
    size_t n = t->n;
    pthread_mutex_unlock(&t->lock);
    return n;
}

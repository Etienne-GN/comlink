/*
 * Concurrency test for the two components that carried data races:
 * the rule evaluator's hit counters and the handle table.
 *
 * Deliberately free of pjsua, so the thread sanitizer reports on our
 * code rather than on pjproject internals.
 *
 * Run under tsan; passing without the sanitizer proves very little.
 */
#include "sp_test.h"
#include "handles.h"
#include "hdr_rules.h"
#include "yaml_tree.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WORKERS   4
#define ROUNDS 2000

// --- rule hit counters ---------------------------------------------------

static sp_hdr_rule *g_rules;
static size_t       g_n_rules;
static _Atomic int  g_stop_reader;

static void *rule_worker(void *arg) {
    (void)arg;
    const sp_hdr_action *acts[8];
    size_t na = 0;
    sp_rule_ctx ctx = {SP_DIR_TX, SP_MSG_REQUEST, "INVITE", 0, NULL};
    for (int i = 0; i < ROUNDS; i++)
        sp_rules_eval(g_rules, g_n_rules, &ctx, acts, 8, &na);
    return NULL;
}

/* Stands in for sp_rules_stats, which reads the counters from a
   frontend thread while pjsip workers increment them. */
static void *rule_reader(void *arg) {
    (void)arg;
    uint64_t last = 0;
    while (!atomic_load(&g_stop_reader)) {
        for (size_t i = 0; i < g_n_rules; i++) {
            uint64_t v = atomic_load_explicit(&g_rules[i].hits,
                                              memory_order_relaxed);
            /* Counters only ever go up. */
            if (i == 0) {
                SP_ASSERT(v >= last);
                last = v;
            }
        }
    }
    return NULL;
}

SP_TEST(rule_hit_counters_are_race_free_and_exact) {
    char err[256] = {0};
    sp_yaml_node *root = sp_yaml_parse_string(
        "header_rules:\n"
        "  - name: a\n    direction: tx\n    message: request\n"
        "    actions:\n      - remove: X\n"
        "  - name: b\n    direction: tx\n    message: any\n"
        "    actions:\n      - set: {name: Y, value: y}\n",
        err, sizeof err);
    SP_ASSERT(root != NULL);
    SP_ASSERT(sp_rules_parse(sp_yaml_get(root, "header_rules"), &g_rules,
                             &g_n_rules, err, sizeof err) == SP_OK);
    sp_yaml_free(root);
    SP_ASSERT(g_n_rules == 2);

    atomic_store(&g_stop_reader, 0);
    pthread_t reader;
    pthread_create(&reader, NULL, rule_reader, NULL);

    pthread_t w[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&w[i], NULL, rule_worker, NULL);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(w[i], NULL);

    atomic_store(&g_stop_reader, 1);
    pthread_join(reader, NULL);

    /* Every increment must be accounted for: a lost update here is a
       silently wrong hit count, which is how a rule that is not firing
       would look like one that is. */
    const uint64_t expect = (uint64_t)WORKERS * ROUNDS;
    SP_ASSERT(atomic_load(&g_rules[0].hits) == expect);
    SP_ASSERT(atomic_load(&g_rules[1].hits) == expect);

    sp_rules_free(g_rules, g_n_rules);
    g_rules = NULL;
    g_n_rules = 0;
}

// --- handle table --------------------------------------------------------

static sp_handle_table *g_ht;
static _Atomic int       g_stop_ht;

static void *ht_churn(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        uint64_t h = sp_ht_add(g_ht, i % 16, NULL);
        if (h)
            sp_ht_retire(g_ht, h);
    }
    return NULL;
}

/* Snapshot while rows churn. A snapshot must never contain the same
   handle twice; an index walk could, because retiring moves the last
   row into the hole. */
static void *ht_reader(void *arg) {
    (void)arg;
    uint64_t ids[256];
    while (!atomic_load(&g_stop_ht)) {
        size_t n = 0;
        sp_ht_snapshot(g_ht, ids, 256, &n);
        if (n > 256)
            n = 256;
        for (size_t i = 0; i < n; i++) {
            SP_ASSERT(ids[i] != 0);
            for (size_t j = i + 1; j < n; j++)
                SP_ASSERT(ids[i] != ids[j]);
        }
    }
    return NULL;
}

SP_TEST(handle_table_survives_concurrent_churn_and_enumeration) {
    g_ht = sp_ht_create();
    SP_ASSERT(g_ht != NULL);

    atomic_store(&g_stop_ht, 0);
    pthread_t readers[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&readers[i], NULL, ht_reader, NULL);

    pthread_t w[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&w[i], NULL, ht_churn, NULL);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(w[i], NULL);

    atomic_store(&g_stop_ht, 1);
    for (int i = 0; i < 2; i++)
        pthread_join(readers[i], NULL);

    /* Everything added was retired. */
    SP_ASSERT(sp_ht_live_count(g_ht) == 0);
    sp_ht_destroy(g_ht);
}

/* Handles must stay unique across threads: two threads adding at once
   must never be given the same id. */
static _Atomic uint64_t g_seen[WORKERS * 64];
static _Atomic int      g_seen_n;

static void *ht_unique(void *arg) {
    (void)arg;
    for (int i = 0; i < 64; i++) {
        uint64_t h = sp_ht_add(g_ht, 0, NULL);
        int slot = atomic_fetch_add(&g_seen_n, 1);
        if (slot < WORKERS * 64)
            atomic_store(&g_seen[slot], h);
    }
    return NULL;
}

SP_TEST(concurrent_adds_never_share_a_handle) {
    g_ht = sp_ht_create();
    atomic_store(&g_seen_n, 0);
    pthread_t w[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&w[i], NULL, ht_unique, NULL);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(w[i], NULL);

    const int n = atomic_load(&g_seen_n);
    SP_ASSERT(n == WORKERS * 64);
    for (int i = 0; i < n; i++) {
        const uint64_t a = atomic_load(&g_seen[i]);
        SP_ASSERT(a != 0);
        for (int j = i + 1; j < n; j++)
            SP_ASSERT(a != atomic_load(&g_seen[j]));
    }
    sp_ht_destroy(g_ht);
}

int main(void) {
    SP_RUN(rule_hit_counters_are_race_free_and_exact);
    SP_RUN(handle_table_survives_concurrent_churn_and_enumeration);
    SP_RUN(concurrent_adds_never_share_a_handle);
    return SP_TEST_RESULT();
}

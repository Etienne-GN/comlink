#include "sp_test.h"
#include "handles.h"

SP_TEST(ids_start_above_zero_and_increase) {
    sp_handle_table *t = sp_ht_create();
    uint64_t a = sp_ht_add(t, 0, NULL);
    uint64_t b = sp_ht_add(t, 1, NULL);
    SP_ASSERT(a != 0);
    SP_ASSERT(b > a);
    sp_ht_destroy(t);
}

SP_TEST(lookup_returns_native_id_and_data) {
    sp_handle_table *t = sp_ht_create();
    int marker = 42;
    uint64_t h = sp_ht_add(t, 7, &marker);
    int native = -1; void *data = NULL;
    SP_ASSERT(sp_ht_lookup(t, h, &native, &data) == 0);
    SP_ASSERT(native == 7);
    SP_ASSERT(data == &marker);
    sp_ht_destroy(t);
}

SP_TEST(retired_handle_is_not_found) {
    sp_handle_table *t = sp_ht_create();
    uint64_t h = sp_ht_add(t, 3, NULL);
    sp_ht_retire(t, h);
    int native; void *data;
    SP_ASSERT(sp_ht_lookup(t, h, &native, &data) == -1);
    sp_ht_destroy(t);
}

SP_TEST(unknown_handle_is_not_found) {
    sp_handle_table *t = sp_ht_create();
    int native; void *data;
    SP_ASSERT(sp_ht_lookup(t, 999999, &native, &data) == -1);
    SP_ASSERT(sp_ht_lookup(t, 0, &native, &data) == -1);
    sp_ht_destroy(t);
}

/* The bug this table exists to prevent: pjsua frees call slot 3 and
   hands it to a new call. A frontend still holding the old handle must
   not reach the new call. */
SP_TEST(reused_native_slot_does_not_resurrect_old_handle) {
    sp_handle_table *t = sp_ht_create();
    uint64_t old = sp_ht_add(t, 3, NULL);
    sp_ht_retire(t, old);
    uint64_t fresh = sp_ht_add(t, 3, NULL);

    SP_ASSERT(fresh != old);

    int native; void *data;
    SP_ASSERT(sp_ht_lookup(t, old, &native, &data) == -1);
    SP_ASSERT(sp_ht_lookup(t, fresh, &native, &data) == 0);
    SP_ASSERT(sp_ht_by_native(t, 3) == fresh);
    sp_ht_destroy(t);
}

SP_TEST(by_native_ignores_retired_entries) {
    sp_handle_table *t = sp_ht_create();
    uint64_t h = sp_ht_add(t, 5, NULL);
    SP_ASSERT(sp_ht_by_native(t, 5) == h);
    sp_ht_retire(t, h);
    SP_ASSERT(sp_ht_by_native(t, 5) == 0);
    sp_ht_destroy(t);
}

/* Enumeration must not skip or duplicate: retiring a row moves the
   last row into its place, which an index walk gets wrong. */
SP_TEST(snapshot_sees_every_live_handle_exactly_once) {
    sp_handle_table *t = sp_ht_create();
    uint64_t made[64];
    for (int i = 0; i < 64; i++)
        made[i] = sp_ht_add(t, i, NULL);
    /* Retire a scattering of them, including the last. */
    sp_ht_retire(t, made[0]);
    sp_ht_retire(t, made[31]);
    sp_ht_retire(t, made[63]);

    uint64_t seen[64];
    size_t n = 0;
    sp_ht_snapshot(t, seen, 64, &n);
    SP_ASSERT(n == 61);
    for (size_t i = 0; i < n; i++) {
        SP_ASSERT(seen[i] != made[0]);
        SP_ASSERT(seen[i] != made[31]);
        SP_ASSERT(seen[i] != made[63]);
        for (size_t j = i + 1; j < n; j++)
            SP_ASSERT(seen[i] != seen[j]);
    }
    sp_ht_destroy(t);
}

SP_TEST(snapshot_reports_the_true_count_when_truncated) {
    sp_handle_table *t = sp_ht_create();
    for (int i = 0; i < 20; i++)
        sp_ht_add(t, i, NULL);
    uint64_t small[5];
    size_t n = 0;
    sp_ht_snapshot(t, small, 5, &n);
    SP_ASSERT(n == 20);   /* true count, so truncation is detectable */
    sp_ht_destroy(t);
}

SP_TEST(live_count_tracks_add_and_retire) {
    sp_handle_table *t = sp_ht_create();
    SP_ASSERT(sp_ht_live_count(t) == 0);
    uint64_t a = sp_ht_add(t, 0, NULL);
    sp_ht_add(t, 1, NULL);
    SP_ASSERT(sp_ht_live_count(t) == 2);
    sp_ht_retire(t, a);
    SP_ASSERT(sp_ht_live_count(t) == 1);
    sp_ht_destroy(t);
}

SP_TEST(survives_more_adds_than_initial_capacity) {
    sp_handle_table *t = sp_ht_create();
    uint64_t first = 0;
    for (int i = 0; i < 500; i++) {
        uint64_t h = sp_ht_add(t, i % 16, NULL);
        if (i == 0) first = h;
        SP_ASSERT(h != 0);
    }
    SP_ASSERT(sp_ht_live_count(t) == 500);
    int native; void *data;
    SP_ASSERT(sp_ht_lookup(t, first, &native, &data) == 0);
    sp_ht_destroy(t);
}

int main(void) {
    SP_RUN(ids_start_above_zero_and_increase);
    SP_RUN(lookup_returns_native_id_and_data);
    SP_RUN(retired_handle_is_not_found);
    SP_RUN(unknown_handle_is_not_found);
    SP_RUN(reused_native_slot_does_not_resurrect_old_handle);
    SP_RUN(by_native_ignores_retired_entries);
    SP_RUN(snapshot_sees_every_live_handle_exactly_once);
    SP_RUN(snapshot_reports_the_true_count_when_truncated);
    SP_RUN(live_count_tracks_add_and_retire);
    SP_RUN(survives_more_adds_than_initial_capacity);
    return SP_TEST_RESULT();
}

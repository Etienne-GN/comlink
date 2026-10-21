#include "sp_test.h"
#include "paths.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

SP_TEST(beside_puts_the_leaf_next_to_the_config) {
    char *p = sp_path_beside("/home/op/.config/comlink/comlink.yaml",
                             "recordings");
    SP_ASSERT(p != NULL);
    if (!p) return;
    SP_ASSERT_STR(p, "/home/op/.config/comlink/recordings");
    free(p);
}

/* A bare filename means the working directory, which is the only
   honest reading of a config path with no directory in it. */
SP_TEST(beside_a_bare_filename_is_the_leaf_alone) {
    char *p = sp_path_beside("comlink.yaml", "traces");
    SP_ASSERT(p != NULL);
    if (!p) return;
    SP_ASSERT_STR(p, "traces");
    free(p);
}

SP_TEST(beside_tolerates_a_null_config_path) {
    char *p = sp_path_beside(NULL, "recordings");
    SP_ASSERT(p != NULL);
    if (!p) return;
    SP_ASSERT_STR(p, "recordings");
    free(p);
}

SP_TEST(mkdir_p_creates_every_missing_level) {
    char deep[512];
    snprintf(deep, sizeof deep, "%s/sp_paths/a/b/c", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_path_mkdir_p(deep) == 0);
    struct stat st;
    SP_ASSERT(stat(deep, &st) == 0);
    SP_ASSERT(S_ISDIR(st.st_mode));
}

/* Called twice on the same path it must succeed, because it runs
   every time a recording starts. */
SP_TEST(mkdir_p_is_idempotent) {
    char deep[512];
    snprintf(deep, sizeof deep, "%s/sp_paths/a/b/c", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_path_mkdir_p(deep) == 0);
    SP_ASSERT(sp_path_mkdir_p(deep) == 0);
}

/* A path whose parent is a file cannot be made, and the caller has to
   find out rather than discover an unwritten recording later. */
SP_TEST(mkdir_p_reports_a_path_it_cannot_make) {
    char file[512], under[640];
    snprintf(file, sizeof file, "%s/sp_paths_file", getenv("SP_TEST_TMP"));
    FILE *f = fopen(file, "w");
    SP_ASSERT(f != NULL);
    if (f) fclose(f);
    snprintf(under, sizeof under, "%s/nope", file);
    SP_ASSERT(sp_path_mkdir_p(under) == -1);
}

/* mkdir_p's internal buffer is 1024 bytes; a path that does not fit
   must be refused rather than silently truncated and created under
   the wrong, shorter name. */
SP_TEST(mkdir_p_refuses_a_path_too_long_for_its_buffer) {
    char prefix[600], segment[1200], long_path[2048];
    snprintf(prefix, sizeof prefix, "%s/sp_paths_long", getenv("SP_TEST_TMP"));
    memset(segment, 'x', sizeof segment - 1);
    segment[sizeof segment - 1] = '\0';
    snprintf(long_path, sizeof long_path, "%s/%s", prefix, segment);
    SP_ASSERT(strlen(long_path) >= 1024);
    SP_ASSERT(sp_path_mkdir_p(long_path) == -1);
    /* SP_TEST_TMP is the build directory, which is not cleaned
       between runs. A test that asserts prefix's absence without
       first removing it is testing the build directory's history,
       not this code -- a leftover from a prior (e.g. regressed) run
       would fail every run after it regardless of correctness. It
       has no children on any path through this test. */
    (void)rmdir(prefix);
    struct stat st;
    SP_ASSERT(stat(prefix, &st) != 0);
}

int main(void) {
    SP_RUN(beside_puts_the_leaf_next_to_the_config);
    SP_RUN(beside_a_bare_filename_is_the_leaf_alone);
    SP_RUN(beside_tolerates_a_null_config_path);
    SP_RUN(mkdir_p_creates_every_missing_level);
    SP_RUN(mkdir_p_is_idempotent);
    SP_RUN(mkdir_p_reports_a_path_it_cannot_make);
    SP_RUN(mkdir_p_refuses_a_path_too_long_for_its_buffer);
    return SP_TEST_RESULT();
}

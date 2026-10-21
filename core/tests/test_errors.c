#include "sp_test.h"
#include <string.h>
#include "comlink/core.h"

SP_TEST(every_status_has_a_distinct_message) {
    const sp_status all[] = {
        SP_OK, SP_ERR_INVALID_ARG, SP_ERR_STATE, SP_ERR_NOT_FOUND,
        SP_ERR_TRANSPORT, SP_ERR_AUTH, SP_ERR_MEDIA, SP_ERR_CONFIG,
        SP_ERR_INTERNAL, SP_ERR_DISABLED, SP_ERR_IO
    };
    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        SP_ASSERT(sp_strerror(all[i]) != NULL);
        SP_ASSERT(sp_strerror(all[i])[0] != '\0');
        for (size_t j = i + 1; j < n; j++)
            SP_ASSERT(strcmp(sp_strerror(all[i]), sp_strerror(all[j])) != 0);
    }
}

/* comlink/core.h's sp_status comment: "Appended, never inserted:
   these numbers cross the ABI to the Android build." That rule has no
   enforcement of its own -- every_status_has_a_distinct_message above
   would still pass if an insertion renumbered everything below it, and
   the Android binding on the other side of that ABI would silently
   start misreading every status past the insertion point. Pin the
   numbers so that inserting one here, rather than appending it, is a
   red test instead of a build that passes and a JNI layer that
   doesn't. */
SP_TEST(the_status_numbers_are_pinned_across_the_abi) {
    SP_ASSERT(SP_OK == 0);
    SP_ASSERT(SP_ERR_INVALID_ARG == 1);
    SP_ASSERT(SP_ERR_STATE == 2);
    SP_ASSERT(SP_ERR_NOT_FOUND == 3);
    SP_ASSERT(SP_ERR_TRANSPORT == 4);
    SP_ASSERT(SP_ERR_AUTH == 5);
    SP_ASSERT(SP_ERR_MEDIA == 6);
    SP_ASSERT(SP_ERR_CONFIG == 7);
    SP_ASSERT(SP_ERR_INTERNAL == 8);
    SP_ASSERT(SP_ERR_DISABLED == 9);
    SP_ASSERT(SP_ERR_IO == 10);
}

SP_TEST(unknown_status_does_not_return_null) {
    SP_ASSERT(sp_strerror((sp_status)9999) != NULL);
}

SP_TEST(ok_reads_as_success) {
    SP_ASSERT_STR(sp_strerror(SP_OK), "success");
}

SP_TEST(build_identity_is_populated) {
    const sp_build_info *b = sp_build();
    SP_ASSERT(b != NULL);
    if (!b) return;
    /* Every field has to say something. "unknown" is allowed for the
       commit, since a tarball build has no git, but an empty string
       would mean the build definitions never reached the compiler. */
    SP_ASSERT(b->version && b->version[0] != '\0');
    SP_ASSERT(b->git_commit && b->git_commit[0] != '\0');
    SP_ASSERT(b->configured_date && b->configured_date[0] != '\0');
    SP_ASSERT(b->pjproject_version && b->pjproject_version[0] != '\0');

    /* The version must not be the fallback the header uses when CMake
       failed to define it: that fallback existing silently is how a
       build ends up claiming to be 0.0.0. */
    SP_ASSERT(strcmp(b->version, "0.0.0") != 0);
    /* A dotted version, not a placeholder. */
    SP_ASSERT(strchr(b->version, '.') != NULL);
    /* pjproject is pinned at 2.17; a different major here means the
       submodule moved without anyone noticing. */
    SP_ASSERT(b->pjproject_version[0] == '2');

    /* Callable twice, and before any core exists. */
    SP_ASSERT(sp_build() == b);
}

int main(void) {
    SP_RUN(every_status_has_a_distinct_message);
    SP_RUN(the_status_numbers_are_pinned_across_the_abi);
    SP_RUN(unknown_status_does_not_return_null);
    SP_RUN(ok_reads_as_success);
    SP_RUN(build_identity_is_populated);
    return SP_TEST_RESULT();
}

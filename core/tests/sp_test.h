#ifndef SP_TEST_H
#define SP_TEST_H
#include <stdio.h>
#include <string.h>

static int sp_test_failures;

#define SP_ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        sp_test_failures++; \
    } \
} while (0)

#define SP_ASSERT_STR(a, b) do { \
    const char *sa_ = (a), *sb_ = (b); \
    if (!sa_ || !sb_ || strcmp(sa_, sb_) != 0) { \
        printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
               sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)"); \
        sp_test_failures++; \
    } \
} while (0)

#define SP_TEST(name) static void name(void)
#define SP_RUN(name) do { printf("- %s\n", #name); name(); } while (0)
#define SP_TEST_RESULT() (printf(sp_test_failures ? "FAILED (%d)\n" : "OK\n", \
                                 sp_test_failures), sp_test_failures ? 1 : 0)
#endif

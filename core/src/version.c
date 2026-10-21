#include "comlink/core.h"

#include <pjlib.h>

#ifndef SP_VERSION
#define SP_VERSION "0.0.0"
#endif
#ifndef SP_GIT_COMMIT
#define SP_GIT_COMMIT "unknown"
#endif
#ifndef SP_BUILD_DATE
#define SP_BUILD_DATE "unknown"
#endif

const sp_build_info *sp_build(void) {
    /* Static, so this works before a core exists: --version must not
       have to start a SIP stack to answer. */
    static sp_build_info info = {
        .version = SP_VERSION,
        .git_commit = SP_GIT_COMMIT,
        .configured_date = SP_BUILD_DATE,
        .pjproject_version = "unknown",
    };
    /* PJ_VERSION is a const char* variable rather than a macro, so it
       cannot be a static initialiser. Assigned on every call: two
       threads writing the same pointer is harmless, and a lock here
       would be worse than the problem. */
    info.pjproject_version = pj_get_version();
    return &info;
}

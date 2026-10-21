#include "thread.h"

#include <pjlib.h>

void sp_thread_ensure_registered(void) {
    /* The descriptor must outlive the thread's use of pjlib, so it is
       thread-local with static storage and never freed. */
    static __thread pj_thread_desc desc;
    static __thread pj_thread_t *thread;

    if (pj_thread_is_registered())
        return;

    pj_bzero(desc, sizeof(desc));
    pj_thread_register("sp_frontend", desc, &thread);
}

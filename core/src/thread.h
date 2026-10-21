/*
 * pjlib refuses to serve threads it does not know about: an
 * unregistered thread calling into pjsua trips an assertion. Qt UI
 * threads and Kotlin coroutine dispatchers are never pj-created, so
 * every public entry point self-registers its caller.
 *
 * Only call this once pjlib is initialised (i.e. after pjsua_create).
 */
#ifndef SP_THREAD_H
#define SP_THREAD_H

void sp_thread_ensure_registered(void);

#endif /* SP_THREAD_H */

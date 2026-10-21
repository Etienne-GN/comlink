/*
 * The transcript's capture port, driven directly: no core, no call,
 * no sound device.
 */
#include "sp_test.h"
#include "pcmsink.h"

#include <pjmedia.h>
#include <pjlib.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static pj_caching_pool g_cp;
static int  g_closed;
static bool g_ok;

static void on_closed(void *user, bool ok) {
    (void)user;
    g_closed++;
    g_ok = ok;
}

SP_TEST(audio_and_heartbeat_frames_both_advance_the_file) {
    pj_pool_t *pool = pj_pool_create(&g_cp.factory, "t", 4000, 4000, NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/sink.pcm", getenv("SP_TEST_TMP"));
    pjmedia_port *p = NULL;
    g_closed = 0;
    SP_ASSERT(sp_pcmsink_create(pool, path, 16000, 320, on_closed, NULL, &p)
              == SP_OK);
    if (!p) { pj_pool_release(pool); return; }

    pj_int16_t buf[320];
    for (int i = 0; i < 320; i++)
        buf[i] = 1000;
    pjmedia_frame f;
    memset(&f, 0, sizeof f);
    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.buf = buf;
    f.size = sizeof buf;
    SP_ASSERT(pjmedia_port_put_frame(p, &f) == PJ_SUCCESS);
    /* What the bridge hands a sink nobody is talking into: no buffer,
       and the time still has to pass in the file. */
    pjmedia_frame hb;
    memset(&hb, 0, sizeof hb);
    hb.type = PJMEDIA_FRAME_TYPE_NONE;
    SP_ASSERT(pjmedia_port_put_frame(p, &hb) == PJ_SUCCESS);

    SP_ASSERT(g_closed == 0);
    SP_ASSERT(pjmedia_port_destroy(p) == PJ_SUCCESS);
    SP_ASSERT(g_closed == 1 && g_ok);

    struct stat st;
    memset(&st, 0, sizeof st);
    SP_ASSERT(stat(path, &st) == 0 && st.st_size == 2 * 320 * 2);
    /* Somebody's voice: readable by this user only. */
    SP_ASSERT((st.st_mode & 077) == 0);
    FILE *fp = fopen(path, "rb");
    pj_int16_t got[640];
    memset(got, 0xff, sizeof got);
    SP_ASSERT(fp && fread(got, 2, 640, fp) == 640);
    if (fp)
        fclose(fp);
    SP_ASSERT(got[0] == 1000 && got[319] == 1000);
    SP_ASSERT(got[320] == 0 && got[639] == 0);
    pj_pool_release(pool);
}

SP_TEST(an_unwritable_path_is_refused) {
    pj_pool_t *pool = pj_pool_create(&g_cp.factory, "t", 4000, 4000, NULL);
    pjmedia_port *p = NULL;
    g_closed = 0;
    SP_ASSERT(sp_pcmsink_create(pool, "/nonexistent/dir/x.pcm", 16000, 320,
                                on_closed, NULL, &p) == SP_ERR_IO);
    SP_ASSERT(p == NULL);
    SP_ASSERT(g_closed == 0);
    pj_pool_release(pool);
}

SP_TEST(a_symlink_at_the_path_is_refused_not_followed) {
    pj_pool_t *pool = pj_pool_create(&g_cp.factory, "t", 4000, 4000, NULL);
    char target[512], path[512];
    snprintf(target, sizeof target, "%s/sink-target.txt", getenv("SP_TEST_TMP"));
    snprintf(path, sizeof path, "%s/sink-link.pcm", getenv("SP_TEST_TMP"));
    FILE *f = fopen(target, "w");
    if (f) { fputs("keep", f); fclose(f); }
    unlink(path);
    SP_ASSERT(symlink(target, path) == 0);
    pjmedia_port *p = NULL;
    SP_ASSERT(sp_pcmsink_create(pool, path, 16000, 320, on_closed, NULL, &p)
              == SP_ERR_IO);
    SP_ASSERT(p == NULL);
    struct stat st;
    SP_ASSERT(stat(target, &st) == 0 && st.st_size == 4);
    unlink(path);
    pj_pool_release(pool);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    pj_init();
    pj_caching_pool_init(&g_cp, NULL, 0);
    SP_RUN(audio_and_heartbeat_frames_both_advance_the_file);
    SP_RUN(an_unwritable_path_is_refused);
    SP_RUN(a_symlink_at_the_path_is_refused_not_followed);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    return SP_TEST_RESULT();
}

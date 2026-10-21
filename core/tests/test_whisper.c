/*
 * The real engine. The French case needs a model, which is far too
 * large to commit: it runs when COMLINK_WHISPER_MODEL names one, and
 * says SKIP otherwise.
 */
#define _GNU_SOURCE
#include "sp_test.h"
#include "stt.h"
#include "transcript.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SP_TEST(this_build_has_an_engine) {
    SP_ASSERT(sp_stt_whisper() != NULL);
}

SP_TEST(a_file_that_is_not_a_model_fails_cleanly) {
    const sp_stt_engine *e = sp_stt_whisper();
    if (!e) { SP_ASSERT(0); return; }
    static float pcm[16000];
    atomic_bool abort_ = false;
    sp_stt_segment *segs = NULL;
    size_t ns = 0;
    char err[256] = {0};
    SP_ASSERT(e->transcribe(e->self, SP_FIXTURE_DIR "/not-a-model.bin",
                            pcm, 16000, 1, &abort_, &segs, &ns,
                            err, sizeof err) == -1);
    SP_ASSERT(err[0] != '\0');
    /* What to do about it, not only that it happened. */
    SP_ASSERT(strstr(err, "download it again") != NULL);
    SP_ASSERT(segs == NULL && ns == 0);
}

SP_TEST(french_speech_comes_back_as_french_words) {
    const char *model = getenv("COMLINK_WHISPER_MODEL");
    const char *clip = SP_FIXTURE_DIR "/transcript-fr.pcm";
    if (!model || !*model || access(model, R_OK) != 0
        || access(clip, R_OK) != 0) {
        printf("  SKIP: needs COMLINK_WHISPER_MODEL and %s\n", clip);
        return;
    }
    const sp_stt_engine *e = sp_stt_whisper();
    if (!e) { SP_ASSERT(0); return; }
    float *pcm = NULL;
    size_t n = 0;
    SP_ASSERT(sp_pcm_load(clip, &pcm, &n) == 0 && n > 16000);
    atomic_bool abort_ = false;
    sp_stt_segment *segs = NULL;
    size_t ns = 0;
    char err[256] = {0};
    SP_ASSERT(e->transcribe(e->self, model, pcm, n, 2, &abort_,
                            &segs, &ns, err, sizeof err) == 0);
    bool found = false;
    for (size_t i = 0; i < ns; i++) {
        printf("  [%u-%u] %s\n", segs[i].start_ms, segs[i].end_ms,
               segs[i].text);
        if (strcasestr(segs[i].text, "bonjour"))
            found = true;
    }
    SP_ASSERT(found);
    sp_stt_segments_free(segs, ns);
    free(pcm);
}

int main(void) {
    SP_RUN(this_build_has_an_engine);
    SP_RUN(a_file_that_is_not_a_model_fails_cleanly);
    SP_RUN(french_speech_comes_back_as_french_words);
    return SP_TEST_RESULT();
}

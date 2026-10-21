#include "stt.h"
#include "transcript.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ggml-backend.h>
#include <whisper.h>

/* whisper prints model details and timings to stderr. The activity
   log is where this phone reports, and the outcome is enough there. */
static void quiet(enum ggml_log_level level, const char *text, void *user) {
    (void)level;
    (void)text;
    (void)user;
}

/* Debian builds ggml with its CPU code as plugins, one per instruction
   set, loaded at run time; without this whisper finds no device at all
   and aborts the process (GGML_ASSERT in ggml-backend.cpp). A static
   build has them linked in already, and this is then a no-op. */
static pthread_once_t g_backends_once = PTHREAD_ONCE_INIT;

static void load_backends(void) {
    ggml_backend_load_all();
}

static bool should_abort(void *user) {
    return atomic_load((const atomic_bool *)user);
}

static int whisper_transcribe(void *self, const char *model_path,
                              const float *pcm, size_t n, int n_threads,
                              const atomic_bool *abort,
                              sp_stt_segment **out, size_t *n_out,
                              char *err, size_t errlen) {
    (void)self;
    *out = NULL;
    *n_out = 0;
    whisper_log_set(quiet, NULL);
    pthread_once(&g_backends_once, load_backends);

    /* CPU only: the phone has no business waking a GPU for this, and
       a handset may not have one ggml can use. */
    struct whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    struct whisper_context *ctx =
        whisper_init_from_file_with_params(model_path, cp);
    if (!ctx) {
        snprintf(err, errlen, "the speech model is damaged or is not a "
                 "whisper model; delete it in Settings > Transcript and "
                 "download it again");
        return -1;
    }

    struct whisper_full_params p =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = n_threads;
    p.language = "auto";
    p.translate = false;
    /* Each side of each call stands alone: no text carried over from
       whatever was transcribed before. */
    p.no_context = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    /* Sounds are not words: no "[Bell]" or "(music)" in a transcript. */
    p.suppress_nst = true;
    p.abort_callback = should_abort;
    p.abort_callback_user_data = (void *)abort;

    if (whisper_full(ctx, p, pcm, (int)n) != 0) {
        snprintf(err, errlen, "%s", atomic_load(abort)
                 ? "stopped" : "the speech engine failed");
        whisper_free(ctx);
        return -1;
    }

    const int ns = whisper_full_n_segments(ctx);
    sp_stt_segment *s = ns > 0 ? calloc((size_t)ns, sizeof *s) : NULL;
    if (ns > 0 && !s) {
        whisper_free(ctx);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t kept = 0;
    for (int i = 0; i < ns; i++) {
        const char *t = whisper_full_get_segment_text(ctx, i);
        while (t && *t == ' ')
            t++;
        if (!t || sp_transcript_is_annotation(t))
            continue;
        /* t0 and t1 count 10 ms steps. */
        s[kept].start_ms = (uint32_t)(whisper_full_get_segment_t0(ctx, i) * 10);
        s[kept].end_ms = (uint32_t)(whisper_full_get_segment_t1(ctx, i) * 10);
        s[kept].text = strdup(t);
        if (!s[kept].text) {
            sp_stt_segments_free(s, kept);
            whisper_free(ctx);
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        kept++;
    }
    whisper_free(ctx);
    if (kept == 0) {
        free(s);
        s = NULL;
    }
    *out = s;
    *n_out = kept;
    return 0;
}

static const sp_stt_engine WHISPER = { whisper_transcribe, NULL };

const sp_stt_engine *sp_stt_whisper(void) {
    return &WHISPER;
}

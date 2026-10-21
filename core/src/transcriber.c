#include "transcriber.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "core_internal.h"
#include "modules.h"
#include "transcript.h"

typedef struct state_write {
    struct state_write *next;
    int64_t             history_id;
    sp_transcript_state state;
    const char         *why;
} state_write;

typedef struct job {
    struct job *next;
    int64_t     history_id;
    char       *far_label;
    char       *far_path;
    char       *mic_path;
} job;

struct sp_transcriber {
    sp_core             *core;
    pthread_t            thread;
    bool                 started;
    pthread_mutex_t      lock;
    pthread_cond_t       cv;
    job                 *head;
    job                 *tail;
    state_write         *states;
    bool                 stopping;
    bool                 busy;
    /* Set with abort when a call starts: the job goes back, not away. */
    bool                 requeue;
    atomic_bool          abort;
    const sp_stt_engine *engine;
    size_t               chunk;
};

/* Five minutes of one side at a time: about 19 MB of samples, rather
   than all of an hour-long call at once. */
#define SP_TRANSCRIBE_CHUNK (16000u * 300u)

typedef enum { RUN_DONE, RUN_REQUEUE } run_outcome;

static void drop_audio(const job *j) {
    if (j->far_path)
        unlink(j->far_path);
    if (j->mic_path)
        unlink(j->mic_path);
}

static void job_free(job *j) {
    if (!j)
        return;
    free(j->far_label);
    free(j->far_path);
    free(j->mic_path);
    free(j);
}

static void report(sp_transcriber *t, int64_t id, sp_transcript_state st,
                   const char *detail) {
    if (st == SP_TRANSCRIPT_FAILED)
        sp_history_transcript_failed(t->core->history, id, detail);
    else if (st != SP_TRANSCRIPT_READY)
        sp_history_transcript_state(t->core->history, id, st);
    sp_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SP_EV_TRANSCRIPT;
    ev.u.transcript.history_id = id;
    ev.u.transcript.state = st;
    ev.u.transcript.detail = detail;
    sp_core_post(t->core, &ev);
}

/* Half the cores and no more than four: whisper gains little past
   four threads, and the other half belong to whoever is at the
   keyboard. */
static int thread_budget(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN) / 2;
    if (n < 1)
        n = 1;
    if (n > 4)
        n = 4;
    return (int)n;
}

static bool calls_up(const sp_transcriber *t) {
    return t->core->calls && sp_ht_live_count(t->core->calls) > 0;
}

/* Appends to *segs, growing it. 0 on success, -1 on failure. */
static int append(sp_stt_segment **segs, size_t *n, size_t *cap,
                  const sp_stt_segment *s) {
    if (*n == *cap) {
        const size_t grow = *cap ? *cap * 2 : 32;
        sp_stt_segment *g = realloc(*segs, grow * sizeof *g);
        if (!g)
            return -1;
        *segs = g;
        *cap = grow;
    }
    (*segs)[(*n)++] = *s;
    return 0;
}

/* Whether any second the segment spans held speech. */
static bool spans_speech(const bool *mask, size_t nw, uint32_t start_ms,
                         uint32_t end_ms) {
    size_t a = start_ms / 1000u, b = end_ms / 1000u;
    if (b < a)
        b = a;
    for (size_t w = a; w <= b && w < nw; w++)
        if (mask[w])
            return true;
    return false;
}

/* One side of the call, a chunk at a time. Segments over stretches
   that held no speech are dropped: words whisper found in line noise
   are words nobody said. 0 on success, -1 on failure, 1 if stopped. */
static int transcribe_side(sp_transcriber *t, const sp_stt_engine *e,
                           const char *model, const char *path,
                           sp_stt_segment **segs, size_t *n,
                           char *err, size_t errlen) {
    *segs = NULL;
    *n = 0;
    size_t cap = 0;
    const size_t chunk = t->chunk;
    for (size_t from = 0;; from += chunk) {
        float *pcm = NULL;
        size_t samples = 0;
        if (sp_pcm_load_range(path, from, chunk, &pcm, &samples) != 0) {
            snprintf(err, errlen, "the captured audio could not be read");
            return -1;
        }
        if (samples == 0)
            return 0;
        bool *mask = NULL;
        size_t nw = 0;
        if (sp_pcm_speech_mask(pcm, samples, &mask, &nw) != 0) {
            free(pcm);
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        bool any = false;
        for (size_t w = 0; w < nw && !any; w++)
            any = mask[w];
        if (!any) {
            free(mask);
            free(pcm);
            continue;
        }
        sp_stt_segment *got = NULL;
        size_t n_got = 0;
        const int rc = e->transcribe(e->self, model, pcm, samples,
                                     thread_budget(), &t->abort,
                                     &got, &n_got, err, errlen);
        free(pcm);
        if (rc != 0) {
            free(mask);
            return atomic_load(&t->abort) ? 1 : -1;
        }
        const uint32_t offset = (uint32_t)(from / 16u);
        int fail = 0;
        for (size_t k = 0; k < n_got; k++) {
            if (fail || !spans_speech(mask, nw, got[k].start_ms,
                                      got[k].end_ms)) {
                free(got[k].text);
                continue;
            }
            sp_stt_segment s = got[k];
            s.start_ms += offset;
            s.end_ms += offset;
            if (append(segs, n, &cap, &s) != 0) {
                free(s.text);
                fail = 1;
            }
        }
        free(got);
        free(mask);
        if (fail) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        if (samples < chunk)
            return 0;
    }
}

static run_outcome run(sp_transcriber *t, const job *j,
                       const sp_stt_engine *e) {
    sp_core *core = t->core;

    pthread_mutex_lock(&core->lock);
    const bool on = sp_config_module_enabled(core->cfg, SP_MODULE_TRANSCRIPT);
    char *model = core->cfg->transcript_model
                  ? strdup(core->cfg->transcript_model) : NULL;
    pthread_mutex_unlock(&core->lock);

    /* Switched off meanwhile: the work stops, and nothing is left
       claiming a transcript is on its way. */
    if (!on) {
        drop_audio(j);
        sp_history_transcript_state(core->history, j->history_id,
                                    SP_TRANSCRIPT_NONE);
        free(model);
        return RUN_DONE;
    }
    if (!e) {
        drop_audio(j);
        report(t, j->history_id, SP_TRANSCRIPT_FAILED,
               "this build has no speech engine");
        free(model);
        return RUN_DONE;
    }
    if (!model || access(model, R_OK) != 0) {
        drop_audio(j);
        report(t, j->history_id, SP_TRANSCRIPT_FAILED,
               "no speech model; download one in Settings > Transcript");
        free(model);
        return RUN_DONE;
    }

    char err[256] = {0};
    sp_stt_segment *far = NULL, *mic = NULL;
    size_t n_far = 0, n_mic = 0;
    int rc = transcribe_side(t, e, model, j->far_path, &far, &n_far,
                             err, sizeof err);
    if (rc == 0)
        rc = transcribe_side(t, e, model, j->mic_path, &mic, &n_mic,
                             err, sizeof err);
    free(model);

    if (rc == 1) {
        sp_stt_segments_free(far, n_far);
        sp_stt_segments_free(mic, n_mic);
        /* Stopped by a call starting, or by Comlink stopping: either
           way the job waits, the second time for the next start. */
        pthread_mutex_lock(&t->lock);
        const bool requeue = t->requeue || t->stopping;
        pthread_mutex_unlock(&t->lock);
        if (requeue)
            return RUN_REQUEUE;
        drop_audio(j);
        if (!sp_module_is_enabled(core, SP_MODULE_TRANSCRIPT))
            sp_history_transcript_state(core->history, j->history_id,
                                        SP_TRANSCRIPT_NONE);
        else
            report(t, j->history_id, SP_TRANSCRIPT_FAILED, "stopped");
        return RUN_DONE;
    }
    drop_audio(j);

    if (rc == 0) {
        sp_transcript_line *lines =
            calloc(n_far + n_mic ? n_far + n_mic : 1, sizeof *lines);
        if (!lines) {
            rc = -1;
            snprintf(err, sizeof err, "out of memory");
        } else {
            const size_t n = sp_transcript_merge(far, n_far, mic, n_mic,
                                                 j->far_label, lines);
            if (sp_history_transcript_store(core->history, j->history_id,
                                            lines, n) != 0) {
                rc = -1;
                snprintf(err, sizeof err, "the transcript could not be saved");
            }
            free(lines);
        }
    }
    sp_stt_segments_free(far, n_far);
    sp_stt_segments_free(mic, n_mic);

    if (rc == 0)
        report(t, j->history_id, SP_TRANSCRIPT_READY, NULL);
    else
        report(t, j->history_id, SP_TRANSCRIPT_FAILED,
               err[0] ? err : "the speech engine failed");
    return RUN_DONE;
}

/* Called with t->lock held; returns with it held. */
static void apply_states(sp_transcriber *t) {
    while (t->states) {
        state_write *w = t->states;
        t->states = NULL;
        pthread_mutex_unlock(&t->lock);
        /* Pushed newest first; applied oldest first, so a later state
           for the same call wins. */
        state_write *rev = NULL;
        while (w) {
            state_write *next = w->next;
            w->next = rev;
            rev = w;
            w = next;
        }
        while (rev) {
            state_write *next = rev->next;
            if (rev->state == SP_TRANSCRIPT_FAILED)
                sp_history_transcript_failed(t->core->history,
                                             rev->history_id, rev->why);
            else
                sp_history_transcript_state(t->core->history,
                                            rev->history_id, rev->state);
            free(rev);
            rev = next;
        }
        pthread_mutex_lock(&t->lock);
    }
}

static void *worker(void *arg) {
    sp_transcriber *t = arg;
    /* Per thread on Linux and Android: the rest of the phone keeps its
       priority. Not worth stopping over if refused. */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19) != 0)
        fprintf(stderr, "comlink: transcriber priority not lowered: %s\n",
                strerror(errno));

    pthread_mutex_lock(&t->lock);
    for (;;) {
        /* Nothing while a call is up. Calls ending are not signalled
           here, so the wait is a short timed one. */
        while (!t->stopping && !t->states && (!t->head || calls_up(t))) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 500000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&t->cv, &t->lock, &until);
        }
        if (t->stopping)
            break;
        if (t->states) {
            apply_states(t);
            pthread_cond_broadcast(&t->cv);
            continue;
        }
        job *j = t->head;
        t->head = j->next;
        if (!t->head)
            t->tail = NULL;
        j->next = NULL;
        t->busy = true;
        t->requeue = false;
        atomic_store(&t->abort, false);
        const sp_stt_engine *e = t->engine;
        pthread_mutex_unlock(&t->lock);

        const run_outcome out = run(t, j, e);

        pthread_mutex_lock(&t->lock);
        if (out == RUN_REQUEUE) {
            j->next = t->head;
            t->head = j;
            if (!t->tail)
                t->tail = j;
        } else {
            job_free(j);
        }
        t->busy = false;
        pthread_cond_broadcast(&t->cv);
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

static const sp_stt_engine *g_default_engine;

void sp_transcriber_default_engine_for_tests(const sp_stt_engine *e) {
    g_default_engine = e;
}

sp_transcriber *sp_transcriber_create(sp_core *core) {
    if (!core)
        return NULL;
    sp_transcriber *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->core = core;
    t->engine = g_default_engine ? g_default_engine : sp_stt_whisper();
    t->chunk = SP_TRANSCRIBE_CHUNK;
    atomic_init(&t->abort, false);
    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t);
        return NULL;
    }
    if (pthread_cond_init(&t->cv, NULL) != 0) {
        pthread_mutex_destroy(&t->lock);
        free(t);
        return NULL;
    }
    if (pthread_create(&t->thread, NULL, worker, t) != 0) {
        pthread_cond_destroy(&t->cv);
        pthread_mutex_destroy(&t->lock);
        free(t);
        return NULL;
    }
    t->started = true;
    return t;
}

void sp_transcriber_destroy(sp_transcriber *t) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    t->stopping = true;
    atomic_store(&t->abort, true);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->lock);
    if (t->started)
        pthread_join(t->thread, NULL);
    pthread_mutex_lock(&t->lock);
    apply_states(t);
    pthread_mutex_unlock(&t->lock);

    /* What never ran keeps its audio and stays PENDING: the next
       start finds both and queues it again. */
    job *j = t->head;
    while (j) {
        job *next = j->next;
        job_free(j);
        j = next;
    }
    pthread_cond_destroy(&t->cv);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

void sp_transcriber_submit(sp_transcriber *t, int64_t history_id,
                           const char *far_label,
                           const char *far_path, const char *mic_path) {
    job *j = calloc(1, sizeof *j);
    if (j) {
        j->history_id = history_id;
        j->far_label = strdup(far_label && *far_label ? far_label : "Caller");
        j->far_path = far_path ? strdup(far_path) : NULL;
        j->mic_path = mic_path ? strdup(mic_path) : NULL;
    }
    if (!t || !j || !j->far_label || !j->far_path || !j->mic_path) {
        if (far_path)
            unlink(far_path);
        if (mic_path)
            unlink(mic_path);
        if (t)
            sp_history_transcript_failed(t->core->history, history_id,
                                         "out of memory");
        job_free(j);
        return;
    }

    pthread_mutex_lock(&t->lock);
    if (t->stopping) {
        /* Kept for the next start, like the rest of the queue. */
        pthread_mutex_unlock(&t->lock);
        job_free(j);
        return;
    }
    if (t->tail)
        t->tail->next = j;
    else
        t->head = j;
    t->tail = j;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->lock);
}

void sp_transcriber_set_state(sp_transcriber *t, int64_t history_id,
                              sp_transcript_state st, const char *why) {
    if (!t)
        return;
    state_write *w = calloc(1, sizeof *w);
    if (!w) {
        /* Late rather than lost. */
        if (st == SP_TRANSCRIPT_FAILED)
            sp_history_transcript_failed(t->core->history, history_id, why);
        else
            sp_history_transcript_state(t->core->history, history_id, st);
        return;
    }
    w->history_id = history_id;
    w->state = st;
    w->why = why;
    pthread_mutex_lock(&t->lock);
    w->next = t->states;
    t->states = w;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->lock);
}

void sp_transcriber_call_started(sp_transcriber *t) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    if (t->busy) {
        t->requeue = true;
        atomic_store(&t->abort, true);
    }
    pthread_mutex_unlock(&t->lock);
}

void sp_transcriber_module_off(sp_transcriber *t) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    if (t->busy) {
        t->requeue = false;
        atomic_store(&t->abort, true);
    }
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->lock);
}

bool sp_transcriber_has_engine(sp_transcriber *t) {
    if (!t)
        return false;
    pthread_mutex_lock(&t->lock);
    const bool has = t->engine != NULL;
    pthread_mutex_unlock(&t->lock);
    return has;
}

void sp_transcriber_disable_engine(sp_transcriber *t) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    t->engine = NULL;
    pthread_mutex_unlock(&t->lock);
}

void sp_transcriber_set_chunk_samples(sp_transcriber *t, size_t n) {
    if (!t || n == 0)
        return;
    pthread_mutex_lock(&t->lock);
    t->chunk = n;
    pthread_mutex_unlock(&t->lock);
}

void sp_transcriber_set_engine(sp_transcriber *t, const sp_stt_engine *e) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    t->engine = e ? e : sp_stt_whisper();
    pthread_mutex_unlock(&t->lock);
}

bool sp_transcriber_wait_idle(sp_transcriber *t, int timeout_ms) {
    if (!t)
        return false;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&t->lock);
    int rc = 0;
    while ((t->head || t->busy || t->states) && rc == 0)
        rc = pthread_cond_timedwait(&t->cv, &t->lock, &until);
    const bool idle = !t->head && !t->busy && !t->states;
    pthread_mutex_unlock(&t->lock);
    return idle;
}

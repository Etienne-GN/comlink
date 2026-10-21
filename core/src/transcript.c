#include "transcript.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float g_silence_rms = SP_SILENCE_RMS;

void sp_transcript_set_silence_rms(float rms) {
    g_silence_rms = rms;
}

int sp_pcm_load_range(const char *path, size_t from, size_t max,
                      float **out, size_t *n) {
    if (!path || !out || !n)
        return -1;
    *out = NULL;
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    const long bytes = ftell(f);
    if (bytes < 0) {
        fclose(f);
        return -1;
    }
    const size_t total = (size_t)bytes / 2;
    if (from >= total) {
        fclose(f);
        return 0;
    }
    size_t samples = total - from;
    if (samples > max)
        samples = max;
    if (fseek(f, (long)(from * 2), SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    float *pcm = malloc(samples * sizeof *pcm);
    if (!pcm) {
        fclose(f);
        return -1;
    }
    int16_t chunk[32768];
    size_t done = 0;
    while (done < samples) {
        size_t want = samples - done;
        if (want > sizeof chunk / sizeof chunk[0])
            want = sizeof chunk / sizeof chunk[0];
        const size_t got = fread(chunk, sizeof chunk[0], want, f);
        for (size_t i = 0; i < got; i++)
            pcm[done + i] = (float)chunk[i] / 32768.0f;
        done += got;
        if (got < want)
            break;
    }
    fclose(f);
    if (done != samples) {
        free(pcm);
        return -1;
    }
    *out = pcm;
    *n = samples;
    return 0;
}

int sp_pcm_load(const char *path, float **out, size_t *n) {
    return sp_pcm_load_range(path, 0, (size_t)-1, out, n);
}

static int by_value(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int sp_pcm_speech_mask(const float *pcm, size_t n, bool **mask,
                       size_t *n_windows) {
    if (!mask || !n_windows)
        return -1;
    *mask = NULL;
    *n_windows = 0;
    if (!pcm || n == 0)
        return 0;
    const size_t nw = (n + SP_SPEECH_WINDOW - 1) / SP_SPEECH_WINDOW;
    double *rms = malloc(nw * sizeof *rms);
    double *sorted = malloc(nw * sizeof *sorted);
    bool *m = malloc(nw * sizeof *m);
    if (!rms || !sorted || !m) {
        free(rms);
        free(sorted);
        free(m);
        return -1;
    }
    for (size_t w = 0; w < nw; w++) {
        const size_t a = w * SP_SPEECH_WINDOW;
        const size_t b = a + SP_SPEECH_WINDOW < n ? a + SP_SPEECH_WINDOW : n;
        double sum = 0.0;
        for (size_t i = a; i < b; i++)
            sum += (double)pcm[i] * (double)pcm[i];
        rms[w] = sqrt(sum / (double)(b - a));
        sorted[w] = rms[w];
    }
    qsort(sorted, nw, sizeof *sorted, by_value);
    /* The line's own noise is its quieter windows. Speech has to stand
       well clear of it -- unless nothing does, in which case the track
       is all one level, a short clip of steady speech or a steady hiss,
       and only the floor can tell which. */
    const double noise = sorted[nw / 10];
    const double loudest = sorted[nw - 1];
    double floor_ = (double)g_silence_rms;
    if (floor_ > 0.0 && loudest >= 4.0 * noise && 4.0 * noise > floor_)
        floor_ = 4.0 * noise;
    for (size_t w = 0; w < nw; w++)
        m[w] = g_silence_rms <= 0.0f || rms[w] >= floor_;
    free(rms);
    free(sorted);
    *mask = m;
    *n_windows = nw;
    return 0;
}

bool sp_pcm_is_silent(const float *pcm, size_t n) {
    bool *mask = NULL;
    size_t nw = 0;
    if (sp_pcm_speech_mask(pcm, n, &mask, &nw) != 0 || nw == 0)
        return true;
    bool any = false;
    for (size_t w = 0; w < nw && !any; w++)
        any = mask[w];
    free(mask);
    return !any;
}

bool sp_transcript_is_annotation(const char *text) {
    if (!text)
        return true;
    while (isspace((unsigned char)*text))
        text++;
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1]))
        n--;
    if (n == 0)
        return true;
    const char open = text[0], close = text[n - 1];
    if (!((open == '[' && close == ']') || (open == '(' && close == ')')))
        return false;
    /* One bracketed run, not "[a] and [b]". */
    for (size_t i = 1; i + 1 < n; i++)
        if (text[i] == open || text[i] == close)
            return false;
    return true;
}

static void put(sp_transcript_line *l, const sp_stt_segment *s, bool local,
                const char *speaker) {
    l->start_ms = s->start_ms;
    l->end_ms = s->end_ms;
    l->local = local;
    l->speaker = speaker;
    l->text = s->text;
}

size_t sp_transcript_merge(const sp_stt_segment *far, size_t n_far,
                           const sp_stt_segment *mic, size_t n_mic,
                           const char *far_label,
                           sp_transcript_line *out) {
    size_t i = 0, j = 0, k = 0;
    while (i < n_far || j < n_mic) {
        if (j >= n_mic || (i < n_far && far[i].start_ms <= mic[j].start_ms))
            put(&out[k++], &far[i++], false, far_label);
        else
            put(&out[k++], &mic[j++], true, "Me");
    }
    return k;
}

void sp_stt_segments_free(sp_stt_segment *s, size_t n) {
    if (!s)
        return;
    for (size_t i = 0; i < n; i++)
        free(s[i].text);
    free(s);
}

#include "sp_test.h"
#include "stt.h"
#include "transcript.h"

#include <math.h>
#include <stdlib.h>

static const char *write_pcm(const char *leaf, const int16_t *s, size_t n) {
    static char path[512];
    snprintf(path, sizeof path, "%s/%s", getenv("SP_TEST_TMP"), leaf);
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fwrite(s, 2, n, f);
    fclose(f);
    return path;
}

SP_TEST(pcm_loads_as_unit_floats) {
    const int16_t s[] = { 0, 16384, -32768 };
    float *pcm = NULL;
    size_t n = 0;
    SP_ASSERT(sp_pcm_load(write_pcm("a.pcm", s, 3), &pcm, &n) == 0);
    SP_ASSERT(n == 3);
    if (n == 3) {
        SP_ASSERT(pcm[0] == 0.0f);
        SP_ASSERT(fabsf(pcm[1] - 0.5f) < 1e-4f);
        SP_ASSERT(pcm[2] == -1.0f);
    }
    free(pcm);
}

SP_TEST(an_empty_file_is_no_samples_not_an_error) {
    float *pcm = NULL;
    size_t n = 99;
    SP_ASSERT(sp_pcm_load(write_pcm("empty.pcm", NULL, 0), &pcm, &n) == 0);
    SP_ASSERT(n == 0);
    free(pcm);
}

SP_TEST(a_missing_file_is_an_error) {
    float *pcm = NULL;
    size_t n = 0;
    SP_ASSERT(sp_pcm_load("/nonexistent/x.pcm", &pcm, &n) == -1);
    SP_ASSERT(pcm == NULL);
}

SP_TEST(zeros_and_hiss_are_silent_a_tone_is_not) {
    static float z[16000];
    SP_ASSERT(sp_pcm_is_silent(z, 16000));
    static float hiss[16000];
    for (int i = 0; i < 16000; i++)
        hiss[i] = (i & 1) ? 0.001f : -0.001f;
    SP_ASSERT(sp_pcm_is_silent(hiss, 16000));
    static float tone[16000];
    for (int i = 0; i < 16000; i++)
        tone[i] = 0.2f * sinf((float)i * 0.1f);
    SP_ASSERT(!sp_pcm_is_silent(tone, 16000));
    SP_ASSERT(sp_pcm_is_silent(NULL, 0));
}

SP_TEST(the_gate_can_be_turned_off) {
    static float hiss[16000];
    for (int i = 0; i < 16000; i++)
        hiss[i] = (i & 1) ? 0.001f : -0.001f;
    sp_transcript_set_silence_rms(0.0f);
    SP_ASSERT(!sp_pcm_is_silent(hiss, 16000));
    SP_ASSERT(sp_pcm_is_silent(NULL, 0));
    sp_transcript_set_silence_rms(SP_SILENCE_RMS);
    SP_ASSERT(sp_pcm_is_silent(hiss, 16000));
}

SP_TEST(merge_orders_by_time_far_end_first_on_a_tie) {
    sp_stt_segment far[] = { { 0, 900, "Bonjour." }, { 2000, 2500, "Merci." } };
    sp_stt_segment mic[] = { { 0, 400, "Allo." }, { 1000, 1800, "Oui." } };
    sp_transcript_line out[4];
    SP_ASSERT(sp_transcript_merge(far, 2, mic, 2, "Alice", out) == 4);
    SP_ASSERT_STR(out[0].text, "Bonjour.");
    SP_ASSERT_STR(out[0].speaker, "Alice");
    SP_ASSERT(out[0].local == false);
    SP_ASSERT_STR(out[1].text, "Allo.");
    SP_ASSERT_STR(out[1].speaker, "Me");
    SP_ASSERT(out[1].local == true);
    SP_ASSERT_STR(out[2].text, "Oui.");
    SP_ASSERT_STR(out[3].text, "Merci.");
    SP_ASSERT(out[3].start_ms == 2000 && out[3].end_ms == 2500);
}

SP_TEST(merge_with_one_empty_leg) {
    sp_stt_segment far[] = { { 10, 20, "x" } };
    sp_transcript_line out[1];
    SP_ASSERT(sp_transcript_merge(far, 1, NULL, 0, "Bob", out) == 1);
    SP_ASSERT(sp_transcript_merge(NULL, 0, far, 1, "Bob", out) == 1);
    SP_ASSERT(out[0].local == true);
    SP_ASSERT(sp_transcript_merge(NULL, 0, NULL, 0, "Bob", out) == 0);
}

SP_TEST(sound_descriptions_are_not_speech) {
    SP_ASSERT(sp_transcript_is_annotation("[Bell]"));
    SP_ASSERT(sp_transcript_is_annotation(" [Speaking in foreign language] "));
    SP_ASSERT(sp_transcript_is_annotation("(music)"));
    SP_ASSERT(sp_transcript_is_annotation("[BLANK_AUDIO]"));
    SP_ASSERT(sp_transcript_is_annotation(""));
    SP_ASSERT(sp_transcript_is_annotation("   "));
    SP_ASSERT(!sp_transcript_is_annotation("Bonjour [inaudible] merci"));
    SP_ASSERT(!sp_transcript_is_annotation("Hello, testing."));
    SP_ASSERT(!sp_transcript_is_annotation("[a] and [b]"));
}

SP_TEST(speech_is_found_against_the_line_s_own_noise) {
    /* 60 s of -45 dBFS hiss and 2 s of tone: only the tone counts. */
    const size_t n = 62 * 16000;
    float *pcm = malloc(n * sizeof *pcm);
    for (size_t i = 0; i < n; i++)
        pcm[i] = i < 60 * 16000 ? ((i & 1) ? 0.0056f : -0.0056f)
                                : 0.2f * sinf((float)i * 0.1f);
    bool *mask = NULL;
    size_t nw = 0;
    SP_ASSERT(sp_pcm_speech_mask(pcm, n, &mask, &nw) == 0);
    SP_ASSERT(nw == 62);
    size_t speech = 0;
    for (size_t w = 0; w < nw; w++)
        speech += mask[w];
    SP_ASSERT(speech == 2);
    SP_ASSERT(mask[60] && mask[61] && !mask[0] && !mask[59]);
    SP_ASSERT(!sp_pcm_is_silent(pcm, n));
    free(mask);
    free(pcm);
}

SP_TEST(a_little_speech_in_a_long_quiet_call_still_counts) {
    const size_t n = 300 * 16000;
    float *pcm = calloc(n, sizeof *pcm);
    for (size_t i = 100 * 16000; i < 103 * 16000; i++)
        pcm[i] = 0.1f * sinf((float)i * 0.1f);
    SP_ASSERT(!sp_pcm_is_silent(pcm, n));
    free(pcm);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(pcm_loads_as_unit_floats);
    SP_RUN(an_empty_file_is_no_samples_not_an_error);
    SP_RUN(a_missing_file_is_an_error);
    SP_RUN(zeros_and_hiss_are_silent_a_tone_is_not);
    SP_RUN(the_gate_can_be_turned_off);
    SP_RUN(merge_orders_by_time_far_end_first_on_a_tie);
    SP_RUN(merge_with_one_empty_leg);
    SP_RUN(sound_descriptions_are_not_speech);
    SP_RUN(speech_is_found_against_the_line_s_own_noise);
    SP_RUN(a_little_speech_in_a_long_quiet_call_still_counts);
    return SP_TEST_RESULT();
}

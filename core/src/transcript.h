/*
 * The parts of a transcript that are plain data: reading a captured
 * track, deciding whether it holds anything worth transcribing, and
 * putting the two sides of a call back into one conversation.
 */
#ifndef SP_TRANSCRIPT_H
#define SP_TRANSCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include "comlink/core.h"
#include "stt.h"

/* Raw mono s16le -> float in [-1, 1). 0 on success, -1 if the file
   cannot be read. An empty file is 0 with *n == 0. */
int  sp_pcm_load(const char *path, float **out, size_t *n);
/* At most max samples from sample `from` on; *n == 0 past the end.
   What keeps a long call from needing all of itself in memory. */
int  sp_pcm_load_range(const char *path, size_t from, size_t max,
                       float **out, size_t *n);

/* Whether each second of a track holds anything louder than the line
   itself: a window counts when its RMS is above both a floor and four
   times the line's noise (its 10th-percentile window), so hiss on a
   noisy line is not speech and a few seconds of speech in a long quiet
   call still are. A track all at one level is judged by the floor. One entry
   per started second, malloc'd. 0 on success. */
#define SP_SILENCE_RMS 0.003f
#define SP_SPEECH_WINDOW 16000u
int  sp_pcm_speech_mask(const float *pcm, size_t n, bool **mask,
                        size_t *n_windows);
/* No window in the mask counts. */
bool sp_pcm_is_silent(const float *pcm, size_t n);
/* Tests only: a self-call with no sound device carries near-silence,
   which the gate would rightly drop. 0 turns the gate off. */
void sp_transcript_set_silence_rms(float rms);

/* Whisper describes what it hears when it is not speech: "[Bell]",
   "(music)", "[Speaking in foreign language]". A segment that is one
   such description and nothing else is not something anybody said. */
bool sp_transcript_is_annotation(const char *text);

/* Both sides into one timeline: by start time, the far end first on a
   tie. out has room for n_far + n_mic lines; they point into the
   segments' text and far_label. Returns the count written. */
size_t sp_transcript_merge(const sp_stt_segment *far, size_t n_far,
                           const sp_stt_segment *mic, size_t n_mic,
                           const char *far_label,
                           sp_transcript_line *out);

#endif /* SP_TRANSCRIPT_H */

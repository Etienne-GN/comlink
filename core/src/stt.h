/*
 * Speech to text, behind one function, so the transcriber can be
 * tested without a 470 MB model and the engine can change without the
 * transcriber noticing.
 */
#ifndef SP_STT_H
#define SP_STT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    char    *text;      /* malloc'd */
} sp_stt_segment;

/* transcribe loads the model, runs, and frees it again: a model held
   between calls is memory a handset needs for other things. pcm is
   16 kHz mono in [-1, 1). On success, returns 0 and a malloc'd array
   the caller frees with sp_stt_segments_free. On failure, returns -1
   with err filled. Polls *abort and returns -1 soon after it turns
   true. */
typedef struct {
    int (*transcribe)(void *self, const char *model_path,
                      const float *pcm, size_t n_samples, int n_threads,
                      const atomic_bool *abort,
                      sp_stt_segment **out, size_t *n_out,
                      char *err, size_t errlen);
    void *self;
} sp_stt_engine;

void sp_stt_segments_free(sp_stt_segment *s, size_t n);

/* NULL when the core was built without whisper. */
const sp_stt_engine *sp_stt_whisper(void);

#endif /* SP_STT_H */

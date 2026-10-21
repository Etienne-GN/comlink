#include "stt.h"

/* Built without whisper: the transcriber says so instead of failing
   to link. */
const sp_stt_engine *sp_stt_whisper(void) {
    return NULL;
}

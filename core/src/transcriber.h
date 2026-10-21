/*
 * Turns a finished call's two captured sides into a stored transcript,
 * one call at a time, on one thread at the lowest priority.
 *
 * The phone is somebody's working machine first. Transcription can
 * wait; their music and their next call cannot.
 */
#ifndef SP_TRANSCRIBER_H
#define SP_TRANSCRIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comlink/core.h"
#include "stt.h"

typedef struct sp_transcriber sp_transcriber;

/* NULL on failure, in which case calls are simply not transcribed. */
sp_transcriber *sp_transcriber_create(sp_core *core);

/* Stops the job in progress and joins the thread. Unfinished jobs keep
   their audio and stay PENDING, for sp_core_start to queue again on
   the next run. */
void sp_transcriber_destroy(sp_transcriber *t);

/* Takes the two track files: they are deleted once the job is done,
   whatever its outcome, and kept only while it is still to run --
   including across a stop. Safe from any thread, the bridge's
   included. */
void sp_transcriber_submit(sp_transcriber *t, int64_t history_id,
                           const char *far_label,
                           const char *far_path, const char *mic_path);

/* Records a call's transcript state on the worker's thread instead of
   the caller's. The bridge's thread clocks every call's audio and
   must not wait on the disk. Safe from any thread. why is kept for a
   FAILED state and must outlive the write: a string literal. */
void sp_transcriber_set_state(sp_transcriber *t, int64_t history_id,
                              sp_transcript_state st, const char *why);

/* A call has started: whatever is running stops and goes back to the
   head of the queue, and nothing starts again until no call is left.
   Transcription loads a model of hundreds of megabytes; it does not do
   that while somebody is on the phone. */
void sp_transcriber_call_started(sp_transcriber *t);

/* The module was switched off: the running job stops and its audio
   goes. Queued jobs go the same way when their turn comes. */
void sp_transcriber_module_off(sp_transcriber *t);

/* Whether there is an engine to transcribe with at all. */
bool sp_transcriber_has_engine(sp_transcriber *t);

/* Tests only: the engine every transcriber created from now on starts
   with, so a queue resumed at startup runs on it. NULL restores the
   built-in one. */
void sp_transcriber_default_engine_for_tests(const sp_stt_engine *e);

/* Tests replace the engine; NULL restores the built-in one. */
void sp_transcriber_set_engine(sp_transcriber *t, const sp_stt_engine *e);

/* Tests only: no engine at all, as in a build without whisper. */
void sp_transcriber_disable_engine(sp_transcriber *t);
/* Tests only: how much of a side is transcribed at once. */
void sp_transcriber_set_chunk_samples(sp_transcriber *t, size_t n);

/* Tests only: blocks until the queue and the pending state writes are
   empty and nothing is running.
   false on timeout. */
bool sp_transcriber_wait_idle(sp_transcriber *t, int timeout_ms);

#endif /* SP_TRANSCRIBER_H */

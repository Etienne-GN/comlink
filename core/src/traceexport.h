/*
 * One call's signalling, as a file somebody can send.
 *
 * Plain text rather than pcap: what pjsip hands us is the message
 * text, and the IP and UDP framing a pcap needs is not ours to
 * invent. A fabricated capture is worse than no capture.
 *
 * Separate from the file writing, and free of pjproject, so the format
 * can be tested without touching a disk.
 */
#ifndef SP_TRACEEXPORT_H
#define SP_TRACEEXPORT_H

#include <stddef.h>

#include "comlink/core.h"

/* Formats msgs oldest-first into out. msgs is expected in the same
   newest-first order sp_siplog_read hands back; this reverses it,
   because sp_siplog_read's newest-first order is what a live view
   wants, and somebody reading a trace to understand a call reads it
   in the order it happened.

   Returns the number of bytes written, or -1 when they do not fit or
   there is nothing to write (a NULL or empty msgs, a NULL call_id, or
   a NULL/zero-length out). Never truncates: a trace that stops
   mid-message looks complete to whoever receives it. */
int sp_trace_format(char *out, size_t outlen, const sp_sip_message *msgs,
                    size_t n, const char *call_id);

/* Sanitises in for use as one filename component: anything outside
   [A-Za-z0-9._-] becomes '_', and a run of two dots (however it
   arose) is broken up so the result can never read as "..". A result
   that would otherwise be empty or made up entirely of dots -- a
   Call-ID of "" or "." -- becomes "_" instead, since a filename
   component with no material in it is as unsafe as one that walks
   out of the directory.

   0 on success. -1 for a NULL in, an empty in, a NULL out, or an
   outlen too small to hold the sanitised result -- refused rather
   than truncated, same as sp_trace_format. */
int sp_trace_safe_name(char *out, size_t outlen, const char *in);

#endif

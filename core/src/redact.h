/*
 * Credential-header redaction for SIP traces.
 *
 * Pure string work: no SQLite, no PJSIP, no I/O, so the behaviour is
 * provable on its own.
 */
#ifndef SP_REDACT_H
#define SP_REDACT_H

/* Returns a newly allocated copy of raw with the values of credential
   headers replaced by <redacted>. The caller frees it. NULL only for a
   NULL input or an allocation failure.
 *
 * Redacted: Authorization, Proxy-Authorization, WWW-Authenticate,
 * Proxy-Authenticate, including any folded continuation lines. The
 * whole value goes rather than just the digest response: the username
 * is personal data too, and a partial redaction that looks thorough is
 * worse than an obvious one.
 *
 * The body is copied through untouched. SDP is the most useful part of
 * a trace, and a body line that happens to look like a credential
 * header is body text, not a header.
 */
char *sp_redact_sip(const char *raw);

#endif /* SP_REDACT_H */

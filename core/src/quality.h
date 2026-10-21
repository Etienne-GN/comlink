/*
 * Audio quality: the arithmetic that turns RTCP counters into a figure
 * an operator can read at a glance.
 *
 * Deliberately free of pjsip and pjmedia types. Everything here takes
 * plain numbers, so the estimate is testable against known inputs with
 * no media stack, no PBX and no network.
 */
#ifndef SP_QUALITY_H
#define SP_QUALITY_H

#include <stdint.h>

/* Loss as a percentage of what should have arrived. Returns 0 when
   nothing was expected, which is not the same claim as "no loss": that
   distinction is what sp_call_quality.valid carries. */
double sp_qual_loss_pct(uint32_t lost, uint32_t received);

/* The transmission rating R and the MOS derived from it.
 *
 * The R-to-MOS conversion is the one ITU-T G.107 defines. The terms
 * that produce R from delay and loss here are NOT G.107's impairment
 * factors: they are the widely used simplification, and they are known
 * to be generous about heavy packet loss. Treat the result as a figure
 * for comparing one call against another on the same phone, not as a
 * certified G.107 score. The raw counters beside it are the
 * authoritative numbers.
 *
 * rtt_us and jitter_us are microseconds, as pjmedia reports them.
 * Either output pointer may be NULL. */
void sp_qual_estimate(double rx_loss_pct, uint32_t rtt_us,
                      uint32_t jitter_us, int *r_out, double *mos_out);

#endif /* SP_QUALITY_H */

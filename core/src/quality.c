#include "quality.h"

/* The unimpaired ceiling of the simplified model. */
#define SP_R_MAX 93.2

double sp_qual_loss_pct(uint32_t lost, uint32_t received) {
    uint64_t expected = (uint64_t)lost + (uint64_t)received;
    if (expected == 0)
        return 0.0;
    return (double)lost * 100.0 / (double)expected;
}

void sp_qual_estimate(double rx_loss_pct, uint32_t rtt_us,
                      uint32_t jitter_us, int *r_out, double *mos_out) {
    if (rx_loss_pct < 0.0)
        rx_loss_pct = 0.0;

    /* What the far ear waits for: half the round trip, plus a de-jitter
       allowance of twice the measured jitter, since a buffer has to
       hold about that much to absorb it, plus a flat 10 ms for codec
       and packetisation delay. */
    double one_way_ms = (double)rtt_us / 2000.0;
    double eff_ms = one_way_ms + ((double)jitter_us / 1000.0) * 2.0 + 10.0;

    double r = SP_R_MAX;
    if (eff_ms < 160.0)
        r -= eff_ms / 40.0;
    else
        r -= (eff_ms - 120.0) / 10.0;

    r -= rx_loss_pct * 2.5;

    if (r < 0.0)
        r = 0.0;
    if (r > SP_R_MAX)
        r = SP_R_MAX;

    /* G.107's conversion from the rating to a mean opinion score. */
    double mos = 1.0 + 0.035 * r + 7.10e-6 * r * (r - 60.0) * (100.0 - r);
    if (mos < 1.0)
        mos = 1.0;
    if (mos > 4.5)
        mos = 4.5;

    /* No libm: the core links pjproject statically and one rounding
       call is not worth a dependency. */
    if (r_out)
        *r_out = (int)(r + 0.5);
    if (mos_out)
        *mos_out = mos;
}

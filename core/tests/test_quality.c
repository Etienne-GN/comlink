#include "sp_test.h"
#include "quality.h"

SP_TEST(loss_pct_basics) {
    /* Five lost out of a hundred that should have arrived. */
    SP_ASSERT(sp_qual_loss_pct(5, 95) > 4.99);
    SP_ASSERT(sp_qual_loss_pct(5, 95) < 5.01);
    /* Nothing has arrived and nothing was lost: not 100% loss, and not
       a division by zero either. */
    SP_ASSERT(sp_qual_loss_pct(0, 0) == 0.0);
    SP_ASSERT(sp_qual_loss_pct(0, 100) == 0.0);
    /* Everything expected was lost. */
    SP_ASSERT(sp_qual_loss_pct(10, 0) > 99.99);
}

SP_TEST(perfect_conditions_rate_high) {
    int r = -1;
    double mos = -1.0;
    sp_qual_estimate(0.0, 0, 0, &r, &mos);
    /* An unimpaired narrowband call cannot reach 5.0 in this model;
       the ceiling is a little above 4.4. */
    SP_ASSERT(mos > 4.3);
    SP_ASSERT(mos <= 4.5);
    SP_ASSERT(r > 90 && r <= 94);
}

SP_TEST(loss_lowers_the_score) {
    double clean = 0.0, light = 0.0, heavy = 0.0;
    sp_qual_estimate(0.0, 0, 0, NULL, &clean);
    sp_qual_estimate(1.0, 0, 0, NULL, &light);
    sp_qual_estimate(10.0, 0, 0, NULL, &heavy);
    SP_ASSERT(clean > light);
    SP_ASSERT(light > heavy);
}

SP_TEST(delay_and_jitter_lower_the_score) {
    double near = 0.0, far = 0.0, jittery = 0.0;
    sp_qual_estimate(0.0, 0, 0, NULL, &near);
    sp_qual_estimate(0.0, 600000, 0, NULL, &far);      /* 600 ms rtt */
    sp_qual_estimate(0.0, 0, 60000, NULL, &jittery);   /*  60 ms jit */
    SP_ASSERT(near > far);
    SP_ASSERT(near > jittery);
}

SP_TEST(scores_are_clamped) {
    int r = -1;
    double mos = -1.0;
    /* Total loss: the rating floor, not a negative number. */
    sp_qual_estimate(100.0, 2000000, 500000, &r, &mos);
    SP_ASSERT(r == 0);
    SP_ASSERT(mos == 1.0);
    /* Nonsense input must not escape the range either. */
    sp_qual_estimate(-5.0, 0, 0, &r, &mos);
    SP_ASSERT(mos <= 4.5);
    SP_ASSERT(r <= 94);
}

SP_TEST(null_outputs_are_allowed) {
    /* A caller that wants only the MOS must not have to invent an int
       to throw away. */
    sp_qual_estimate(0.0, 0, 0, NULL, NULL);
    SP_ASSERT(1);
}

int main(void) {
    SP_RUN(loss_pct_basics);
    SP_RUN(perfect_conditions_rate_high);
    SP_RUN(loss_lowers_the_score);
    SP_RUN(delay_and_jitter_lower_the_score);
    SP_RUN(scores_are_clamped);
    SP_RUN(null_outputs_are_allowed);
    return SP_TEST_RESULT();
}

#include "sp_test.h"
#include "comlink/core.h"

#include <string.h>
#include <stdio.h>

/* The cases are the ones frontend/qt/avatar.cpp's comment argues
   about, so the rule is tested where it is explained. */
SP_TEST(initials_take_the_first_and_last_words) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("Sylvie Tremblay", out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "ST") == 0);
}

/* One word is a monogram. Two letters of one word would be a guess at
   a surname that is not there. */
SP_TEST(a_single_word_gives_one_letter) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("Repartition", out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "R") == 0);
}

SP_TEST(the_middle_of_three_words_is_ignored) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("Jean Claude Tremblay", out,
                                  sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "JT") == 0);
}

/* The reason this is not toupper() in a loop. A 911 centre in Quebec
   spells names with accents, and a byte-wise upper-case turns the
   first half of a two-byte character into something that is not a
   character at all. */
SP_TEST(accented_names_upper_case_rather_than_breaking) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("étienne gagné", out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "\xc3\x89\x47") == 0);   /* E-acute, G */

    SP_ASSERT(sp_contact_initials("Étienne Gagné", out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "\xc3\x89\x47") == 0);   /* already upper */
}

/* A script the rule does not know is passed through, not damaged. */
SP_TEST(an_unknown_script_is_left_alone) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("\xe4\xb8\xad\xe6\x96\x87", out,
                                  sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "\xe4\xb8\xad") == 0);
}

SP_TEST(extra_spaces_do_not_produce_empty_initials) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("   spaced    out   ", out,
                                  sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out, "SO") == 0);
}

/* Empty rather than an error: a contact with no name is not a fault,
   and the frontend draws the anonymous figure instead. */
SP_TEST(nothing_to_work_with_gives_an_empty_string) {
    char out[SP_INITIALS_MAX];
    SP_ASSERT(sp_contact_initials("", out, sizeof out) == SP_OK);
    SP_ASSERT(out[0] == '\0');
    SP_ASSERT(sp_contact_initials("     ", out, sizeof out) == SP_OK);
    SP_ASSERT(out[0] == '\0');
    SP_ASSERT(sp_contact_initials(NULL, out, sizeof out) == SP_OK);
    SP_ASSERT(out[0] == '\0');
}

SP_TEST(a_short_initials_buffer_is_refused) {
    char tiny[2];
    SP_ASSERT(sp_contact_initials("Sylvie Tremblay", tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
}

/* Word for word what mainwindow.cpp printed before this moved, so the
   two frontends cannot describe one call differently. */
SP_TEST(the_quality_line_reads_as_it_always_has) {
    sp_call_quality q;
    memset(&q, 0, sizeof q);
    q.mos = 4.41;
    q.rx_loss_pct = 0.0;
    q.rx_jitter.avg_us = 400;      /* 0.4 ms */
    q.rtt.avg_us = 12000;          /* 12 ms  */

    char out[SP_QUALITY_LINE_MAX];
    SP_ASSERT(sp_call_quality_line(&q, out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out,
        "MOS 4.41 \xc2\xb7 loss 0.0% \xc2\xb7 jit 0.4 \xc2\xb7 rtt 12 ms")
        == 0);
}

SP_TEST(a_quality_line_with_loss_keeps_one_decimal) {
    sp_call_quality q;
    memset(&q, 0, sizeof q);
    q.mos = 3.05;
    q.rx_loss_pct = 2.25;
    q.rx_jitter.avg_us = 15500;
    q.rtt.avg_us = 149600;

    char out[SP_QUALITY_LINE_MAX];
    SP_ASSERT(sp_call_quality_line(&q, out, sizeof out) == SP_OK);
    SP_ASSERT(strcmp(out,
        "MOS 3.05 \xc2\xb7 loss 2.2% \xc2\xb7 jit 15.5 \xc2\xb7 rtt 150 ms")
        == 0);
}

SP_TEST(a_short_quality_buffer_is_refused) {
    sp_call_quality q;
    memset(&q, 0, sizeof q);
    char tiny[8];
    SP_ASSERT(sp_call_quality_line(&q, tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_call_quality_line(NULL, tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
}

int main(void) {
    SP_RUN(initials_take_the_first_and_last_words);
    SP_RUN(a_single_word_gives_one_letter);
    SP_RUN(the_middle_of_three_words_is_ignored);
    SP_RUN(accented_names_upper_case_rather_than_breaking);
    SP_RUN(an_unknown_script_is_left_alone);
    SP_RUN(extra_spaces_do_not_produce_empty_initials);
    SP_RUN(nothing_to_work_with_gives_an_empty_string);
    SP_RUN(a_short_initials_buffer_is_refused);
    SP_RUN(the_quality_line_reads_as_it_always_has);
    SP_RUN(a_quality_line_with_loss_keeps_one_decimal);
    SP_RUN(a_short_quality_buffer_is_refused);
    return SP_TEST_RESULT();
}

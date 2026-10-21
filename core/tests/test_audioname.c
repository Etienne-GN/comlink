#include "sp_test.h"
#include "audioname.h"

#include <string.h>

/* The actual 64 names ALSA returned on a Debian laptop with three
   sound cards: a USB webcam, the built-in PCH, and a dock. Recorded
   verbatim from `comlink-cli audio`, so this test is about real
   enumeration rather than about names I invented. */
static const char *const REAL[] = {
    "sysdefault", "lavrate", "samplerate", "speexrate", "oss", "speex",
    "upmix", "vdownmix", "default", "pipewire", "pulse", "jack",
    "hw:CARD=camera,DEV=0", "plughw:CARD=camera,DEV=0",
    "sysdefault:CARD=camera", "front:CARD=camera,DEV=0",
    "dsnoop:CARD=camera,DEV=0", "usbstream:CARD=camera",
    "hw:CARD=PCH,DEV=0", "hw:CARD=PCH,DEV=3", "hw:CARD=PCH,DEV=7",
    "hw:CARD=PCH,DEV=8", "hw:CARD=PCH,DEV=9",
    "plughw:CARD=PCH,DEV=0", "plughw:CARD=PCH,DEV=3",
    "plughw:CARD=PCH,DEV=7", "plughw:CARD=PCH,DEV=8",
    "plughw:CARD=PCH,DEV=9", "sysdefault:CARD=PCH",
    "front:CARD=PCH,DEV=0", "surround21:CARD=PCH,DEV=0",
    "surround40:CARD=PCH,DEV=0", "surround41:CARD=PCH,DEV=0",
    "surround50:CARD=PCH,DEV=0", "surround51:CARD=PCH,DEV=0",
    "surround71:CARD=PCH,DEV=0", "hdmi:CARD=PCH,DEV=0",
    "hdmi:CARD=PCH,DEV=1", "hdmi:CARD=PCH,DEV=2", "hdmi:CARD=PCH,DEV=3",
    "dmix:CARD=PCH,DEV=0", "dmix:CARD=PCH,DEV=3", "dmix:CARD=PCH,DEV=7",
    "dmix:CARD=PCH,DEV=8", "dmix:CARD=PCH,DEV=9",
    "dsnoop:CARD=PCH,DEV=0", "usbstream:CARD=PCH",
    "hw:CARD=Dock,DEV=0", "hw:CARD=Dock,DEV=1",
    "plughw:CARD=Dock,DEV=0", "plughw:CARD=Dock,DEV=1",
    "sysdefault:CARD=Dock", "front:CARD=Dock,DEV=0",
    "surround21:CARD=Dock,DEV=0", "surround40:CARD=Dock,DEV=0",
    "surround41:CARD=Dock,DEV=0", "surround50:CARD=Dock,DEV=0",
    "surround51:CARD=Dock,DEV=0", "surround71:CARD=Dock,DEV=0",
    "iec958:CARD=Dock,DEV=0", "iec958:CARD=Dock,DEV=1",
    "dmix:CARD=Dock,DEV=0", "dmix:CARD=Dock,DEV=1",
    "dsnoop:CARD=Dock,DEV=0",
};
static const size_t N_REAL = sizeof REAL / sizeof REAL[0];

SP_TEST(the_real_list_collapses_to_a_usable_size) {
    size_t rec = 0;
    for (size_t i = 0; i < N_REAL; i++)
        if (sp_audio_is_recommended(REAL[i]))
            rec++;
    /* 64 entries is a wall, not a choice. The exact number matters
       less than the order of magnitude, but it must be small enough to
       read at a glance and large enough to cover every card. */
    SP_ASSERT(N_REAL == 64);
    SP_ASSERT(rec >= 8);
    SP_ASSERT(rec <= 16);
}

SP_TEST(every_card_is_still_reachable_from_the_short_list) {
    /* The point of filtering is to hide plumbing, not devices. Each of
       the three cards must survive. */
    const char *cards[] = { "camera", "PCH", "Dock" };
    for (size_t c = 0; c < 3; c++) {
        bool found = false;
        for (size_t i = 0; i < N_REAL && !found; i++) {
            if (!sp_audio_is_recommended(REAL[i]))
                continue;
            if (strstr(REAL[i], cards[c]) != NULL)
                found = true;
        }
        SP_ASSERT(found);
    }
}

SP_TEST(plumbing_is_classified_as_plumbing) {
    const char *junk[] = {
        "lavrate", "samplerate", "speexrate", "speex", "upmix", "vdownmix",
        "usbstream:CARD=PCH", "dmix:CARD=PCH,DEV=0", "dsnoop:CARD=PCH,DEV=0",
        "surround51:CARD=Dock,DEV=0", "front:CARD=PCH,DEV=0",
        "iec958:CARD=Dock,DEV=0", "hdmi:CARD=PCH,DEV=1",
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        SP_ASSERT(sp_audio_classify(junk[i]) == SP_AUDIO_PLUGIN);
        SP_ASSERT(sp_audio_is_recommended(junk[i]) == false);
    }
}

SP_TEST(servers_and_defaults_are_recognised) {
    SP_ASSERT(sp_audio_classify("default") == SP_AUDIO_DEFAULT);
    SP_ASSERT(sp_audio_classify("sysdefault") == SP_AUDIO_DEFAULT);
    SP_ASSERT(sp_audio_classify("pipewire") == SP_AUDIO_SERVER);
    SP_ASSERT(sp_audio_classify("pulse") == SP_AUDIO_SERVER);
    SP_ASSERT(sp_audio_classify("jack") == SP_AUDIO_SERVER);
    SP_ASSERT(sp_audio_is_recommended("default"));
    SP_ASSERT(sp_audio_is_recommended("pipewire"));
}

SP_TEST(the_android_device_is_a_device) {
    /* pjmedia's Android driver names its one device after itself. It
       carries every call on the handset, and an ALSA rule hid it. */
    SP_ASSERT(sp_audio_classify("Android JNI") == SP_AUDIO_SERVER);
    SP_ASSERT(sp_audio_is_recommended("Android JNI"));
}

SP_TEST(raw_and_converting_card_entries_are_told_apart) {
    /* hw: refuses a rate it cannot do, which for a phone at 8 kHz is
       most of them. plughw: converts. They must not be the same kind. */
    SP_ASSERT(sp_audio_classify("hw:CARD=PCH,DEV=0") == SP_AUDIO_RAW);
    SP_ASSERT(sp_audio_classify("plughw:CARD=PCH,DEV=0") == SP_AUDIO_CARD);
    SP_ASSERT(sp_audio_classify("sysdefault:CARD=PCH") == SP_AUDIO_CARD);
    SP_ASSERT(sp_audio_is_recommended("hw:CARD=PCH,DEV=0") == false);
    SP_ASSERT(sp_audio_is_recommended("plughw:CARD=PCH,DEV=0") == true);
    /* Later devices on a card are HDMI and digital outs; behind
       "show all" rather than in the short list. */
    SP_ASSERT(sp_audio_is_recommended("plughw:CARD=PCH,DEV=3") == false);
}

SP_TEST(labels_are_readable_and_never_empty) {
    char b[128];
    sp_audio_label("default", b, sizeof b);
    SP_ASSERT_STR(b, "System default");
    sp_audio_label("sysdefault", b, sizeof b);
    SP_ASSERT_STR(b, "Default sound card");
    sp_audio_label("pipewire", b, sizeof b);
    SP_ASSERT_STR(b, "PipeWire");
    sp_audio_label("sysdefault:CARD=PCH", b, sizeof b);
    SP_ASSERT_STR(b, "PCH");
    sp_audio_label("plughw:CARD=Dock,DEV=1", b, sizeof b);
    SP_ASSERT_STR(b, "Dock, device 1");
    /* Unrecognised names fall back to themselves: the raw name is
       still the operator's only handle on that device. */
    sp_audio_label("something-new", b, sizeof b);
    SP_ASSERT_STR(b, "something-new");

    for (size_t i = 0; i < N_REAL; i++) {
        sp_audio_label(REAL[i], b, sizeof b);
        SP_ASSERT(b[0] != '\0');
    }
}

SP_TEST(null_and_empty_do_not_crash) {
    char b[64];
    SP_ASSERT(sp_audio_classify(NULL) == SP_AUDIO_PLUGIN);
    SP_ASSERT(sp_audio_classify("") == SP_AUDIO_PLUGIN);
    SP_ASSERT(sp_audio_is_recommended(NULL) == false);
    sp_audio_label(NULL, b, sizeof b);
    SP_ASSERT(b[0] != '\0');
    sp_audio_label("default", NULL, 0);
    sp_audio_label("default", b, 0);
    SP_ASSERT(1);
}

int main(void) {
    SP_RUN(the_real_list_collapses_to_a_usable_size);
    SP_RUN(every_card_is_still_reachable_from_the_short_list);
    SP_RUN(plumbing_is_classified_as_plumbing);
    SP_RUN(servers_and_defaults_are_recognised);
    SP_RUN(the_android_device_is_a_device);
    SP_RUN(raw_and_converting_card_entries_are_told_apart);
    SP_RUN(labels_are_readable_and_never_empty);
    SP_RUN(null_and_empty_do_not_crash);
    return SP_TEST_RESULT();
}

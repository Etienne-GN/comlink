#include "audioname.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The sound servers, which are devices as far as a phone cares. The
   platform drivers pjmedia names after themselves belong here too:
   "Android JNI" is the handset's one device, and it carries every call
   there. */
static const char *const SERVERS[] = { "pipewire", "pulse", "jack", "oss",
                                       "Android JNI", NULL };

/* Prefixes that are plumbing rather than a device an operator would
   choose: rate conversion, channel remapping, software mixing and
   splitting, raw stream access. */
static const char *const PLUMBING[] = {
    "lavrate", "samplerate", "speexrate", "speex", "upmix", "vdownmix",
    "a52", "usbstream", "dmix", "dsnoop", "surround", "front", "rear",
    "center_lfe", "side", "iec958", "spdif", "hdmi", "modem", "phoneline",
    NULL
};

static bool in_list(const char *const *list, const char *name) {
    for (size_t i = 0; list[i]; i++)
        if (strcmp(list[i], name) == 0)
            return true;
    return false;
}

static bool starts_with_any(const char *const *list, const char *name) {
    for (size_t i = 0; list[i]; i++) {
        size_t n = strlen(list[i]);
        if (strncmp(name, list[i], n) == 0)
            return true;
    }
    return false;
}

sp_audio_kind sp_audio_classify(const char *name) {
    if (!name || !*name)
        return SP_AUDIO_PLUGIN;

    if (strcmp(name, "default") == 0)
        return SP_AUDIO_DEFAULT;
    if (in_list(SERVERS, name))
        return SP_AUDIO_SERVER;
    /* "sysdefault" with no card is the default card, which is a
       different thing from ALSA's "default" and worth keeping. */
    if (strcmp(name, "sysdefault") == 0)
        return SP_AUDIO_DEFAULT;

    if (starts_with_any(PLUMBING, name))
        return SP_AUDIO_PLUGIN;

    /* Everything real names a card. */
    if (strstr(name, "CARD=") != NULL) {
        if (strncmp(name, "hw:", 3) == 0)
            return SP_AUDIO_RAW;
        return SP_AUDIO_CARD;
    }
    return SP_AUDIO_PLUGIN;
}

bool sp_audio_is_recommended(const char *name) {
    switch (sp_audio_classify(name)) {
    case SP_AUDIO_DEFAULT:
    case SP_AUDIO_SERVER:
        return true;
    case SP_AUDIO_CARD:
        /* One usable entry per card, plus the card's first device.
           sysdefault:CARD=X is "this card, with format conversion",
           which is what a phone at 8 or 16 kHz needs; plughw's later
           devices are HDMI and digital outputs nobody places a call
           on, and they stay behind "show all". */
        if (strncmp(name, "sysdefault:", 11) == 0)
            return true;
        if (strncmp(name, "plughw:", 7) == 0 && strstr(name, "DEV=0"))
            return true;
        return false;
    case SP_AUDIO_RAW:
    case SP_AUDIO_PLUGIN:
        return false;
    }
    return false;
}

/* The card name out of "...:CARD=<name>[,DEV=<n>]", and the device
   number if there is one. Returns false if the name has no card. */
static bool split_card(const char *name, char *card, size_t cap,
                       int *dev_out) {
    const char *c = strstr(name, "CARD=");
    if (!c)
        return false;
    c += 5;
    size_t i = 0;
    while (c[i] && c[i] != ',' && i + 1 < cap) {
        card[i] = c[i];
        i++;
    }
    card[i] = '\0';
    *dev_out = -1;
    const char *d = strstr(name, "DEV=");
    if (d)
        *dev_out = atoi(d + 4);
    return i > 0;
}

void sp_audio_label(const char *name, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    if (!name || !*name) {
        snprintf(out, cap, "unknown");
        return;
    }

    if (strcmp(name, "default") == 0) {
        snprintf(out, cap, "System default");
        return;
    }
    if (strcmp(name, "sysdefault") == 0) {
        snprintf(out, cap, "Default sound card");
        return;
    }
    if (strcmp(name, "pipewire") == 0) {
        snprintf(out, cap, "PipeWire");
        return;
    }
    if (strcmp(name, "pulse") == 0) {
        snprintf(out, cap, "PulseAudio");
        return;
    }
    if (strcmp(name, "jack") == 0) {
        snprintf(out, cap, "JACK");
        return;
    }

    char card[96];
    int dev = -1;
    if (split_card(name, card, sizeof card, &dev)) {
        if (strncmp(name, "sysdefault:", 11) == 0)
            snprintf(out, cap, "%s", card);
        else if (dev >= 0)
            snprintf(out, cap, "%s, device %d", card, dev);
        else
            snprintf(out, cap, "%s", card);
        return;
    }

    /* Not recognised: the raw name is still the operator's only handle
       on it, so it is better than inventing something. */
    snprintf(out, cap, "%s", name);
}

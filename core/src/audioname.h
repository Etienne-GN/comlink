/*
 * Making sense of ALSA device names.
 *
 * ALSA's enumeration is not a list of devices. On an ordinary Debian
 * laptop it returns 64 entries for three sound cards: rate converters
 * (lavrate, samplerate, speexrate), channel maps (surround21 through
 * surround71), software mixers (dmix, dsnoop), raw USB streams, HDMI
 * and S/PDIF outputs, and one entry per card per plugin wrapper. A
 * list like that is not a choice, it is a wall.
 *
 * These functions are pure and know nothing about pjsip or pjmedia, so
 * they are tested against the real 64 names with no media stack.
 */
#ifndef SP_AUDIONAME_H
#define SP_AUDIONAME_H

#include <stdbool.h>
#include <stddef.h>

#include "comlink/core.h"

sp_audio_kind sp_audio_classify(const char *name);

/* True when the device is worth offering without the operator asking
   to see everything. */
bool sp_audio_is_recommended(const char *name);

/* A human-readable form of an ALSA device name. Never empty: falls
   back to the name itself, because a name we do not recognise is
   still the only handle the operator has on that device. */
void sp_audio_label(const char *name, char *out, size_t cap);

#endif /* SP_AUDIONAME_H */

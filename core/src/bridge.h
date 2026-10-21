/*
 * The conference bridge's format, for everything that adds a port
 * of its own to it.
 *
 * pjsua_media_config_default's own defaults, which core.c never
 * overrides (see core.c's pjsua_media_config_default call): the
 * bridge runs at this rate regardless of any call's own codec. A port
 * at another rate would be resampled, and the Opus writer's clock_rate
 * has to match it or a recording plays at the wrong pitch and seeks to
 * the wrong place.
 */
#ifndef SP_BRIDGE_H
#define SP_BRIDGE_H

#define SP_REC_BRIDGE_CLOCK_RATE 16000u
#define SP_REC_BRIDGE_CHANNELS   1u
#define SP_REC_BRIDGE_PTIME_MS   20u
#define SP_REC_BRIDGE_SAMPLES_PER_FRAME \
    (SP_REC_BRIDGE_CLOCK_RATE * SP_REC_BRIDGE_PTIME_MS / 1000u)

#endif /* SP_BRIDGE_H */

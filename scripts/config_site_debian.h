/* pjproject build configuration, Debian host.
 *
 * Only settings configure does not own belong here. SSL, Opus and the
 * ALSA audio backend are decided by configure autodetection; setting
 * them here would be a value that silently does nothing.
 */
#define PJMEDIA_HAS_VIDEO               0

/* The null audio device, so the audio subsystem still has a device
   when no real driver reports one. That is what lets
   "audio_device: none" run with ALSA disabled entirely: without it
   pjmedia_aud_subsys_init fails with PJMEDIA_EAUD_SYSERR and
   pjsua_init fails outright. Measured, not assumed. */
#define PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO 1

/* Upstream default is 4, which would cap concurrent calls. */
#define PJSUA_MAX_CALLS                 16
#define PJSUA_MAX_ACC                   8

#define PJSIP_MAX_TSX_COUNT             (1024 - 1)
#define PJSIP_MAX_DIALOG_COUNT          (512 - 1)

/* Codecs we do not ship, to keep the SDP offer honest. */
#define PJMEDIA_HAS_SPEEX_CODEC         0
#define PJMEDIA_HAS_ILBC_CODEC          0

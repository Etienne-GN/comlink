/*
 * Theme presets and override resolution.
 *
 * Pure C: no PJSIP, no SQLite, no I/O, so the resolution rules are
 * provable on their own. This lives in the core rather than in a
 * frontend because a Qt stylesheet cannot exist on Android, and a
 * theme implemented in a frontend is a theme written twice.
 *
 * The structure follows the operator's ThemeSettings.kt sketch:
 * presets carrying a base font and a default accent, nullable
 * overrides, and resolved accessors.
 */
#ifndef SP_THEME_H
#define SP_THEME_H

#include "comlink/core.h"

/* accent_hex and font_family may be NULL, meaning "use the preset's".
   An accent that is not #RRGGBB returns SP_ERR_CONFIG rather than
   quietly falling back to something that looks deliberate. */
sp_status sp_theme_resolve(sp_theme_preset preset, const char *accent_hex,
                           const char *font_family, sp_text_scale scale,
                           sp_theme *out);

/* 0 if hex is exactly #RRGGBB, case-insensitive; -1 otherwise. */
int sp_theme_validate_hex(const char *hex);

const char *sp_theme_preset_name(sp_theme_preset p);
/* Declared in comlink/core.h as well: both are public. */
const char *sp_text_scale_name(sp_text_scale s);
int sp_theme_preset_from_name(const char *name, sp_theme_preset *out);
int sp_text_scale_from_name(const char *name, sp_text_scale *out);

#endif /* SP_THEME_H */

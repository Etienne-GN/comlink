#include "theme.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    const char *label;
    bool        dark;
    const char *accent;
    const char *background;
    const char *surface;
    const char *foreground;
    const char *muted;
    const char *font;
    const char *emblem;
} preset_def;

/* Accents and fonts follow the operator's own sketch. Backgrounds and
   foregrounds are chosen to keep text legible against them, which is
   the part a frontend must not be left to guess. */
static const preset_def PRESETS[] = {
    [SP_THEME_DARK] = {
        "dark", "Dark", true,
        "#378ADD", "#12151A", "#1C2027", "#E6E9EF", "#8A93A3", "Cinzel", ""
    },
    [SP_THEME_GOLD] = {
        "gold", "Gold", true,
        "#EF9F27", "#14110B", "#201B12", "#F2E9D8", "#9C8C6E", "Cinzel", ""
    },
    [SP_THEME_LIGHT] = {
        "light", "Light", false,
        "#378ADD", "#FAFAFC", "#FFFFFF", "#1A1C20", "#5F6570", "Rajdhani", ""
    },
    [SP_THEME_HIGH_CONTRAST] = {
        "high_contrast", "High contrast", true,
        "#FFFFFF", "#000000", "#000000", "#FFFFFF", "#D0D0D0", "", ""
    },

    /* The rest carry no font: a preset that names a family nobody has
       installed does not fail, it silently falls back, which makes
       the setting a lie. The two above predate that lesson. */
    [SP_THEME_AMBER_CRT] = {
        "amber_crt", "Amber CRT", true,
        "#FFB000", "#1A1206", "#241A0A", "#F5D9A0", "#A88A54", "", ""
    },
    [SP_THEME_GREEN_PHOSPHOR] = {
        "green_phosphor", "Green Phosphor", true,
        "#33FF66", "#04120A", "#0A1E12", "#C8FFD9", "#6FA37F", "", ""
    },
    [SP_THEME_OSCILLOSCOPE] = {
        "oscilloscope", "Oscilloscope", true,
        "#7CFFB2", "#0A1410", "#12201A", "#D8F5E4", "#79A891", "", ""
    },
    [SP_THEME_COPPER_PAIR] = {
        "copper_pair", "Copper Pair", true,
        "#C87F4A", "#1B1512", "#261E19", "#F0DFD2", "#AD8B72", "", ""
    },
    [SP_THEME_PUNCHDOWN] = {
        "punchdown", "Punchdown", true,
        "#4A9FD8", "#14171C", "#1D2229", "#E4E9F0", "#8B95A5", "", ""
    },
    [SP_THEME_NEBULA] = {
        "nebula", "Nebula", true,
        "#B983FF", "#12101E", "#1B1830", "#E9E4FA", "#9188B8", "", ""
    },
    [SP_THEME_EVENT_HORIZON] = {
        "event_horizon", "Event Horizon", true,
        "#FF6B35", "#05060A", "#0E1016", "#E6E8EE", "#8A8F9C", "", ""
    },
    [SP_THEME_AURORA] = {
        "aurora", "Aurora", true,
        "#4EE8B0", "#06141A", "#0D2028", "#DCF2F0", "#7BA5A5", "", ""
    },
    [SP_THEME_REDSHIFT] = {
        "redshift", "Redshift", true,
        "#FF5C7A", "#170B0B", "#231212", "#F6E3E3", "#B08A8A", "", ""
    },
    [SP_THEME_KYBER] = {
        "kyber", "Kyber", true,
        "#4FC3F7", "#0A0F14", "#121A22", "#DCE9F2", "#8398A8", "", ""
    },
    [SP_THEME_PAPER_TAPE] = {
        "paper_tape", "Paper Tape", false,
        "#B26B2E", "#F5F0E6", "#FFFBF2", "#2A2620", "#6B6355", "", ""
    },
    [SP_THEME_WHITE_DWARF] = {
        "white_dwarf", "White Dwarf", false,
        "#2F7FD1", "#F4F7FA", "#FFFFFF", "#1B2028", "#5D6674", "", ""
    },
    /* Where the application's name comes from. Deep-space navy, the
       Republic's gold for the accent, and parchment for the text, so
       gold never has to be read as body copy. Gold rather than
       Republic red: red is what ends a call here. */
    [SP_THEME_REPUBLIC] = {
        "republic", "Republic", true,
        "#E3B341", "#070B18", "#0F1628", "#EFE6CF", "#8F93A8", "",
        "republic"
    },
    /* Gunmetal and a cold steel accent: an Empire that dresses in
       grey, with nothing warm in it. */
    [SP_THEME_EMPIRE] = {
        "empire", "Empire", true,
        "#AFC3D6", "#0A0B0D", "#16181C", "#E4E7EB", "#8C929C", "",
        "empire"
    },
    /* Robe browns and the blue of a lightsaber. */
    [SP_THEME_JEDI_ORDER] = {
        "jedi_order", "Jedi Order", true,
        "#4DA3FF", "#120D08", "#1E1710", "#F1E6D2", "#A8987F", "",
        "jedi"
    },
    /* The one preset whose accent is red, because the Sith are. It is
       the same family as the colour that ends a call and marks a
       recording, so here the accent and Stop sit closer than anywhere
       else: chosen by the operator knowing that. */
    [SP_THEME_SITH_ORDER] = {
        "sith_order", "Sith Order", true,
        "#E0283A", "#080506", "#140B0D", "#EDE0E0", "#9A8588", "",
        "sith"
    }
};

static const struct {
    const char *name;
    float       multiplier;
} SCALES[] = {
    [SP_TEXT_SMALL]  = { "small",       0.90f },
    [SP_TEXT_MEDIUM] = { "medium",      1.00f },
    [SP_TEXT_LARGE]  = { "large",       1.15f },
    [SP_TEXT_XLARGE] = { "extra_large", 1.30f }
};

#define N_PRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))
#define N_SCALES  ((int)(sizeof SCALES / sizeof SCALES[0]))

static int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int sp_theme_validate_hex(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) != 7)
        return -1;
    for (int i = 1; i < 7; i++)
        if (!is_hex_digit(hex[i]))
            return -1;
    return 0;
}

const char *sp_theme_preset_name(sp_theme_preset p) {
    if ((int)p < 0 || (int)p >= N_PRESETS)
        return PRESETS[SP_THEME_DARK].name;
    return PRESETS[p].name;
}

int sp_theme_preset_count(void) {
    return N_PRESETS;
}

const char *sp_theme_preset_label(sp_theme_preset p) {
    if ((int)p < 0 || (int)p >= N_PRESETS)
        return PRESETS[SP_THEME_DARK].label;
    return PRESETS[p].label;
}

const char *sp_text_scale_name(sp_text_scale s) {
    if ((int)s < 0 || (int)s >= N_SCALES)
        return SCALES[SP_TEXT_MEDIUM].name;
    return SCALES[s].name;
}

int sp_theme_preset_from_name(const char *name, sp_theme_preset *out) {
    if (!name || !out)
        return -1;
    for (int i = 0; i < N_PRESETS; i++)
        if (strcasecmp(PRESETS[i].name, name) == 0) {
            *out = (sp_theme_preset)i;
            return 0;
        }
    return -1;
}

int sp_text_scale_from_name(const char *name, sp_text_scale *out) {
    if (!name || !out)
        return -1;
    for (int i = 0; i < N_SCALES; i++)
        if (strcasecmp(SCALES[i].name, name) == 0) {
            *out = (sp_text_scale)i;
            return 0;
        }
    return -1;
}

sp_status sp_theme_resolve(sp_theme_preset preset, const char *accent_hex,
                           const char *font_family, sp_text_scale scale,
                           sp_theme *out) {
    if (!out)
        return SP_ERR_INVALID_ARG;
    if ((int)preset < 0 || (int)preset >= N_PRESETS)
        return SP_ERR_INVALID_ARG;
    if ((int)scale < 0 || (int)scale >= N_SCALES)
        return SP_ERR_INVALID_ARG;
    if (accent_hex && *accent_hex && sp_theme_validate_hex(accent_hex) != 0)
        return SP_ERR_CONFIG;

    const preset_def *p = &PRESETS[preset];
    out->preset = preset;
    out->text_scale = scale;
    out->text_multiplier = SCALES[scale].multiplier;
    out->dark = p->dark;
    out->background_hex = p->background;
    out->surface_hex = p->surface;
    out->foreground_hex = p->foreground;
    out->muted_hex = p->muted;

    /* Overrides win; an empty string counts as absent, so a config
       with accent: "" behaves like no accent rather than failing. */
    out->accent_hex = (accent_hex && *accent_hex) ? accent_hex : p->accent;
    out->font_family = (font_family && *font_family) ? font_family : p->font;
    out->emblem = p->emblem ? p->emblem : "";
    return SP_OK;
}

/* The public face of sp_theme_resolve. Kept as a separate symbol so
   the header needs no internal type and a frontend can preview a
   theme without a core. */
sp_status sp_theme_preview(sp_theme_preset preset, const char *accent_hex,
                           const char *font_family, sp_text_scale scale,
                           sp_theme *out) {
    return sp_theme_resolve(preset, accent_hex, font_family, scale, out);
}

/* --- derived colours -------------------------------------------------
 *
 * Two rules that used to live in frontend/qt/theme.cpp, and were
 * therefore duplicated by hand into the Android frontend the first
 * time it needed them. The comments explaining *why* each is shaped
 * this way are in comlink/core.h, beside the declarations.
 *
 * The arithmetic is Qt's, deliberately: QColor's HSL conversion is
 * fixed-point with its own rounding, and a port that used textbook
 * floating-point HSL would drift a channel here and there against
 * fifteen years of a working desktop. tests/test_theme.c pins all
 * sixteen presets to the values the Qt build produced before this
 * moved, so the port is checked against behaviour rather than
 * against a reading of the code.
 */

typedef struct { int r, g, b; } sp_rgb;

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Qt's qt_div_257: how QColor turns its sixteen-bit channels back
   into eight. Not a shift -- a shift is a unit low on roughly a third
   of the values, which is where six of the sixteen presets drifted
   when this was ported. */
static int div257(unsigned x) {
    const unsigned y = x + 128;
    return (int)((y - (y >> 8)) >> 8);
}

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static sp_rgb rgb_of_hex(const char *hex) {
    sp_rgb c = {0, 0, 0};
    if (!hex || sp_theme_validate_hex(hex) != 0)
        return c;
    c.r = hex_nib(hex[1]) * 16 + hex_nib(hex[2]);
    c.g = hex_nib(hex[3]) * 16 + hex_nib(hex[4]);
    c.b = hex_nib(hex[5]) * 16 + hex_nib(hex[6]);
    return c;
}

static int max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}
static int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

/* qrgb_to_hsl, in the units QColor reports: hue 0..359 or -1 when
   there is no hue at all, saturation and lightness 0..255. */
static void rgb_to_hsl(sp_rgb c, int *hue, int *sat, int *light) {
    const int max = max3(c.r, c.g, c.b), min = min3(c.r, c.g, c.b);
    const int delta = max - min, delta2 = max + min;
    const double l = 0.5 * delta2 / 255.0;

    if (delta == 0) {
        *hue = -1;
        *sat = 0;
        *light = div257((unsigned)round(l * 65535.0));
        return;
    }
    double s = (l < 0.5) ? (double)delta / delta2
                         : (double)delta / (510 - delta2);
    double h;
    if (c.r == max)      h = (c.g - c.b) / (double)delta;
    else if (c.g == max) h = 2.0 + (c.b - c.r) / (double)delta;
    else                 h = 4.0 + (c.r - c.g) / (double)delta;
    h *= 60.0;
    if (h < 0.0) h += 360.0;

    *hue = (int)round(h * 100.0) / 100;
    *sat = div257((unsigned)round(s * 65535.0));
    *light = div257((unsigned)round(l * 65535.0));
}

/* QColor::fromHsl, with QColor's own path back to eight-bit channels:
   the conversion lands in sixteen bits and the top byte is taken. */
static sp_rgb hsl_to_rgb(int hue, int sat, int light) {
    sp_rgb out = {0, 0, 0};
    const double h = (hue < 0 ? 0 : (hue * 100)) / 36000.0;
    const double s = (sat * 0x101) / 65535.0;
    const double l = (light * 0x101) / 65535.0;

    if (s == 0.0) {
        const int v = div257((unsigned)round(l * 65535.0));
        out.r = out.g = out.b = v;
        return out;
    }
    const double t2 = (l < 0.5) ? l * (1.0 + s) : l + s - (l * s);
    const double t1 = (2.0 * l) - t2;
    double t3[3] = { h + 1.0 / 3.0, h, h - 1.0 / 3.0 };
    int ch[3];
    for (int i = 0; i < 3; i++) {
        if (t3[i] < 0.0)      t3[i] += 1.0;
        else if (t3[i] > 1.0) t3[i] -= 1.0;
        const double six = t3[i] * 6.0;
        double c;
        if (six < 1.0)              c = t1 + (t2 - t1) * six;
        else if ((t3[i] * 2.0) < 1) c = t2;
        else if ((t3[i] * 3.0) < 2) c = t1 + (t2 - t1) * (2.0 / 3.0 - t3[i]) * 6.0;
        else                        c = t1;
        ch[i] = div257((unsigned)round(c * 65535.0));
    }
    out.r = ch[0];
    out.g = ch[1];
    out.b = ch[2];
    return out;
}

static double chan_lin(double v) {
    v /= 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(sp_rgb c) {
    return 0.2126 * chan_lin(c.r) + 0.7152 * chan_lin(c.g)
         + 0.0722 * chan_lin(c.b);
}

static double contrast(sp_rgb a, sp_rgb b) {
    const double la = luminance(a), lb = luminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

/* Walks a colour's lightness away from the surface until it clears a
   ratio. Which way to walk is decided by the surface, not by the
   theme's dark flag, because a custom accent can sit on either.
 *
 * The walk carries hue and saturation rather than re-deriving them
 * from RGB each step. QColor keeps a colour made by fromHsl in HSL
 * and hands those two straight back, so a port that round-tripped
 * through eight-bit RGB drifted a unit on six of the sixteen presets
 * -- visible in nothing, and enough to make "the same colour on both
 * platforms" untrue. */
static sp_rgb clear_of_hsl(int h, int s, int l, sp_rgb surface,
                           double want, int step) {
    sp_rgb c = hsl_to_rgb(h, s, l);
    for (int i = 0; i < 40 && contrast(c, surface) < want; i++) {
        const int next = clamp_int(l + step, 0, 255);
        if (next == l)
            break;
        l = next;
        c = hsl_to_rgb(h, s, l);
    }
    return c;
}

static void write_hex(sp_rgb c, char *out, size_t outlen) {
    if (!out || outlen < 8)
        return;
    static const char D[] = "0123456789ABCDEF";
    out[0] = '#';
    out[1] = D[(c.r >> 4) & 0xF]; out[2] = D[c.r & 0xF];
    out[3] = D[(c.g >> 4) & 0xF]; out[4] = D[c.g & 0xF];
    out[5] = D[(c.b >> 4) & 0xF]; out[6] = D[c.b & 0xF];
    out[7] = '\0';
}

sp_status sp_theme_channel(const sp_theme *t, bool receive,
                           char *out, size_t outlen) {
    if (!t || !out || outlen < 8)
        return SP_ERR_INVALID_ARG;
    const sp_rgb accent = rgb_of_hex(t->accent_hex);
    if (!receive) {
        write_hex(accent, out, outlen);
        return SP_OK;
    }
    const sp_rgb surface = rgb_of_hex(t->surface_hex);
    int h, s, l;
    rgb_to_hsl(accent, &h, &s, &l);

    if (h >= 0) {
        int nh = (h + 90) % 360;
        /* Red is spoken for. It ends calls and it marks a recording,
           and a trace wearing it would be reporting a fault. */
        if (nh > 335 || nh < 20)
            nh = (nh + 45) % 360;
        const int step = luminance(surface) < 0.18 ? 5 : -5;
        write_hex(clear_of_hsl(nh, s, l, surface, 3.0, step), out, outlen);
        return SP_OK;
    }

    /* No hue to turn. Receive becomes the accent stepped back towards
       the surface until it is plainly the quieter trace -- then held
       above 3:1 so "quieter" never becomes "invisible". Blending
       towards the surface rather than dimming keeps it right on light
       themes, where quieter means darker, not paler. */
    sp_rgb c = accent;
    for (int i = 0; i < 12; i++) {
        sp_rgb next;
        next.r = (c.r * 2 + surface.r) / 3;
        next.g = (c.g * 2 + surface.g) / 3;
        next.b = (c.b * 2 + surface.b) / 3;
        if (contrast(next, surface) < 3.0)
            break;
        c = next;
    }
    write_hex(c, out, outlen);
    return SP_OK;
}

sp_status sp_theme_semantic(const sp_theme *t, sp_semantic which,
                            char *out, size_t outlen) {
    if (!t || !out || outlen < 8)
        return SP_ERR_INVALID_ARG;
    const sp_rgb surface = rgb_of_hex(t->surface_hex);
    /* Green and red, at a saturation that survives being tinted
       towards a warm or a cool surface. */
    /* High contrast is black on black until the walk runs. Its
       direction comes from the theme's own dark flag here, not from
       the surface: that is what the Qt rule did and the two must not
       disagree. */
    const int h = which == SP_SEMANTIC_GO ? 142 : 4;
    const int step = t->dark ? 5 : -5;
    write_hex(clear_of_hsl(h, 165, t->dark ? 150 : 105, surface, 3.0, step),
              out, outlen);
    return SP_OK;
}

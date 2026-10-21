#include "sp_test.h"
#include "config.h"
#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char *write_tmp(const char *body) {
    static char path[512];
    snprintf(path, sizeof path, "%s/theme_test.yaml", getenv("SP_TEST_TMP"));
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(body, f);
    fclose(f);
    return path;
}

SP_TEST(each_preset_resolves_to_its_own_look) {
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        const sp_theme_preset preset = (sp_theme_preset)i;
        sp_theme t;
        SP_ASSERT(sp_theme_resolve(preset, NULL, NULL, SP_TEXT_MEDIUM, &t)
                  == SP_OK);
        SP_ASSERT(t.preset == preset);
        SP_ASSERT(t.accent_hex != NULL && t.accent_hex[0] == '#');
        SP_ASSERT(t.background_hex != NULL);
        SP_ASSERT(t.foreground_hex != NULL);
        SP_ASSERT(t.font_family != NULL);
    }
    /* The light preset must not claim to be dark, or a frontend picks
       the wrong contrast. */
    sp_theme dark, light;
    sp_theme_resolve(SP_THEME_DARK, NULL, NULL, SP_TEXT_MEDIUM, &dark);
    sp_theme_resolve(SP_THEME_LIGHT, NULL, NULL, SP_TEXT_MEDIUM, &light);
    SP_ASSERT(dark.dark == true);
    SP_ASSERT(light.dark == false);
    SP_ASSERT(strcmp(dark.background_hex, light.background_hex) != 0);
}

/* WCAG 2.1 relative luminance. A theme is allowed to be ugly; it is
   not allowed to be unreadable, and "looks fine on my monitor" is not
   a measurement. */
static double channel(int v) {
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(const char *hex) {
    char buf[3] = {0};
    double rgb[3];
    for (int i = 0; i < 3; i++) {
        buf[0] = hex[1 + i * 2];
        buf[1] = hex[2 + i * 2];
        rgb[i] = channel((int)strtol(buf, NULL, 16));
    }
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

static double contrast(const char *a, const char *b) {
    double la = luminance(a), lb = luminance(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

SP_TEST(every_preset_is_legible) {
    /* Body text clears AAA. Muted text is still text, so it clears AA.
       The accent only ever carries a highlight or a control, which is
       the 3:1 case. Every preset shipped has met these since they were
       written down; a new one that does not is a bug, not a taste. */
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        sp_theme t;
        SP_ASSERT(sp_theme_resolve((sp_theme_preset)i, NULL, NULL,
                                   SP_TEXT_MEDIUM, &t) == SP_OK);
        const char *name = sp_theme_preset_name((sp_theme_preset)i);

        SP_ASSERT(sp_theme_validate_hex(t.accent_hex) == 0);
        SP_ASSERT(sp_theme_validate_hex(t.background_hex) == 0);
        SP_ASSERT(sp_theme_validate_hex(t.surface_hex) == 0);
        SP_ASSERT(sp_theme_validate_hex(t.foreground_hex) == 0);
        SP_ASSERT(sp_theme_validate_hex(t.muted_hex) == 0);

        if (contrast(t.foreground_hex, t.background_hex) < 7.0)
            printf("  %s: foreground on background is %.2f\n", name,
                   contrast(t.foreground_hex, t.background_hex));
        SP_ASSERT(contrast(t.foreground_hex, t.background_hex) >= 7.0);

        if (contrast(t.foreground_hex, t.surface_hex) < 7.0)
            printf("  %s: foreground on surface is %.2f\n", name,
                   contrast(t.foreground_hex, t.surface_hex));
        SP_ASSERT(contrast(t.foreground_hex, t.surface_hex) >= 7.0);

        if (contrast(t.muted_hex, t.background_hex) < 4.5)
            printf("  %s: muted on background is %.2f\n", name,
                   contrast(t.muted_hex, t.background_hex));
        SP_ASSERT(contrast(t.muted_hex, t.background_hex) >= 4.5);

        if (contrast(t.accent_hex, t.background_hex) < 3.0)
            printf("  %s: accent on background is %.2f\n", name,
                   contrast(t.accent_hex, t.background_hex));
        SP_ASSERT(contrast(t.accent_hex, t.background_hex) >= 3.0);
    }
}

SP_TEST(every_preset_is_named_once) {
    /* Two presets sharing a config name means one of them can never be
       selected from a config file, and the picker shows a duplicate. */
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        const char *ni = sp_theme_preset_name((sp_theme_preset)i);
        const char *li = sp_theme_preset_label((sp_theme_preset)i);
        SP_ASSERT(ni && *ni);
        SP_ASSERT(li && *li);
        /* Config names are lowercase with underscores: they are typed
           into a YAML file by hand. */
        for (const char *c = ni; *c; c++)
            SP_ASSERT((*c >= 'a' && *c <= 'z') || *c == '_');
        for (int j = i + 1; j < sp_theme_preset_count(); j++) {
            SP_ASSERT(strcmp(ni, sp_theme_preset_name((sp_theme_preset)j)) != 0);
            SP_ASSERT(strcmp(li, sp_theme_preset_label((sp_theme_preset)j)) != 0);
        }
    }
}

SP_TEST(an_accent_override_wins_over_the_preset) {
    sp_theme base, over;
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, NULL, NULL, SP_TEXT_MEDIUM,
                               &base) == SP_OK);
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "#ABCDEF", NULL, SP_TEXT_MEDIUM,
                               &over) == SP_OK);
    SP_ASSERT_STR(over.accent_hex, "#ABCDEF");
    SP_ASSERT(strcmp(base.accent_hex, over.accent_hex) != 0);
    /* Overriding the accent must not disturb anything else. */
    SP_ASSERT_STR(over.background_hex, base.background_hex);
    SP_ASSERT_STR(over.font_family, base.font_family);
}

SP_TEST(a_font_override_wins_over_the_preset) {
    sp_theme t;
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, NULL, "Iosevka",
                               SP_TEXT_MEDIUM, &t) == SP_OK);
    SP_ASSERT_STR(t.font_family, "Iosevka");
}

SP_TEST(every_text_scale_has_a_distinct_multiplier) {
    const sp_text_scale all[] = {SP_TEXT_SMALL, SP_TEXT_MEDIUM,
                                 SP_TEXT_LARGE, SP_TEXT_XLARGE};
    float seen[4];
    for (size_t i = 0; i < 4; i++) {
        sp_theme t;
        SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, NULL, NULL, all[i], &t)
                  == SP_OK);
        SP_ASSERT(t.text_scale == all[i]);
        seen[i] = t.text_multiplier;
        SP_ASSERT(t.text_multiplier > 0.5f && t.text_multiplier < 2.0f);
    }
    for (size_t i = 0; i < 4; i++)
        for (size_t j = i + 1; j < 4; j++)
            SP_ASSERT(seen[i] != seen[j]);
    /* Ordered, or "large" could be smaller than "small". */
    SP_ASSERT(seen[0] < seen[1] && seen[1] < seen[2] && seen[2] < seen[3]);
}

/* A bad colour is a configuration error, not a silent fallback that
   looks deliberate. */
SP_TEST(an_invalid_accent_is_refused) {
    sp_theme t;
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "not-a-colour", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_ERR_CONFIG);
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "#12345", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_ERR_CONFIG);
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "378ADD", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_ERR_CONFIG);
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "#GGHHII", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_ERR_CONFIG);
    /* Lower case is fine. */
    SP_ASSERT(sp_theme_resolve(SP_THEME_DARK, "#abcdef", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_OK);
}

SP_TEST(preset_and_scale_names_round_trip) {
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        sp_theme_preset p;
        SP_ASSERT(sp_theme_preset_from_name(
            sp_theme_preset_name((sp_theme_preset)i), &p) == 0);
        SP_ASSERT(p == (sp_theme_preset)i);
    }
    for (int i = 0; i < 4; i++) {
        sp_text_scale sc;
        SP_ASSERT(sp_text_scale_from_name(
            sp_text_scale_name((sp_text_scale)i), &sc) == 0);
        SP_ASSERT(sc == (sp_text_scale)i);
    }
    sp_theme_preset bogus;
    SP_ASSERT(sp_theme_preset_from_name("chartreuse", &bogus) == -1);
    SP_ASSERT(sp_theme_preset_from_name(NULL, &bogus) == -1);
}

// --- config integration -------------------------------------------------

SP_TEST(theme_defaults_when_absent_from_config) {
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(write_tmp("accounts: []\n"), &c, err, sizeof err)
              == SP_OK);
    if (!c) { printf("  %s\n", err); return; }
    SP_ASSERT(c->theme_preset == SP_THEME_DARK);
    SP_ASSERT(c->theme_accent == NULL);
    SP_ASSERT(c->theme_font == NULL);
    SP_ASSERT(c->theme_scale == SP_TEXT_MEDIUM);
    sp_config_free(c);
}

SP_TEST(theme_is_read_from_config) {
    const char *p = write_tmp(
        "theme:\n"
        "  preset: light\n"
        "  accent: \"#EF9F27\"\n"
        "  font: Rajdhani\n"
        "  text_scale: large\n"
        "accounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    if (!c) { printf("  %s\n", err); return; }
    SP_ASSERT(c->theme_preset == SP_THEME_LIGHT);
    SP_ASSERT_STR(c->theme_accent, "#EF9F27");
    SP_ASSERT_STR(c->theme_font, "Rajdhani");
    SP_ASSERT(c->theme_scale == SP_TEXT_LARGE);
    sp_config_free(c);
}

SP_TEST(theme_survives_a_save) {
    const char *p = write_tmp(
        "theme:\n  preset: gold\n  accent: \"#112233\"\n"
        "  font: Cinzel\n  text_scale: extra_large\n"
        "accounts: []\n");
    sp_config *a = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &a, err, sizeof err) == SP_OK);
    if (!a) return;
    char out[512];
    snprintf(out, sizeof out, "%s/theme_rt.yaml", getenv("SP_TEST_TMP"));
    SP_ASSERT(sp_config_save(a, out, err, sizeof err) == SP_OK);
    sp_config *b = NULL;
    SP_ASSERT(sp_config_load(out, &b, err, sizeof err) == SP_OK);
    if (b) {
        SP_ASSERT(b->theme_preset == SP_THEME_GOLD);
        SP_ASSERT_STR(b->theme_accent, "#112233");
        SP_ASSERT_STR(b->theme_font, "Cinzel");
        SP_ASSERT(b->theme_scale == SP_TEXT_XLARGE);
        sp_config_free(b);
    }
    sp_config_free(a);
}

SP_TEST(unknown_theme_key_is_refused_naming_itself) {
    const char *p = write_tmp("theme:\n  presset: dark\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "presset") != NULL);
}

/* A preset a newer Comlink wrote must not stop this one starting: it
   is drawn in the default, and saving writes the name back as it was,
   so going forward a version again finds the theme still chosen. */
SP_TEST(unknown_preset_loads_as_default_and_is_written_back) {
    const char *p = write_tmp("theme:\n  preset: chartreuse\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_OK);
    SP_ASSERT(c->theme_preset == SP_THEME_DARK);
    SP_ASSERT(sp_config_save(c, p, err, sizeof err) == SP_OK);
    sp_config_free(c);

    FILE *f = fopen(p, "r");
    SP_ASSERT(f != NULL);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    SP_ASSERT(n > 0);
    SP_ASSERT(strstr(buf, "preset: chartreuse") != NULL);
}

SP_TEST(invalid_accent_in_config_is_refused) {
    const char *p = write_tmp("theme:\n  accent: blue\naccounts: []\n");
    sp_config *c = NULL; char err[256] = {0};
    SP_ASSERT(sp_config_load(p, &c, err, sizeof err) == SP_ERR_CONFIG);
    SP_ASSERT(strstr(err, "blue") != NULL);
}

/* Choosing a theme has to outlive the process, or it is not a
   setting. Needs a real core, since sp_theme_set persists. */
SP_TEST(setting_a_theme_persists_across_a_restart) {
    static char p[512];
    snprintf(p, sizeof p, "%s/theme_set.yaml", getenv("SP_TEST_TMP"));
    char db[512];
    snprintf(db, sizeof db, "%s/theme_set.db", getenv("SP_TEST_TMP"));
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\naccounts: []\n", db);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_theme_set(c, SP_THEME_LIGHT, "#AABBCC", "Iosevka",
                           SP_TEXT_LARGE) == SP_OK);
    sp_theme t;
    SP_ASSERT(sp_theme_get(c, &t) == SP_OK);
    SP_ASSERT(t.preset == SP_THEME_LIGHT);
    SP_ASSERT_STR(t.accent_hex, "#AABBCC");
    SP_ASSERT_STR(t.font_family, "Iosevka");
    SP_ASSERT(t.text_scale == SP_TEXT_LARGE);
    SP_ASSERT(t.dark == false);
    sp_core_destroy(c);

    sp_core *c2 = NULL;
    SP_ASSERT(sp_core_create(p, &c2) == SP_OK);
    if (!c2) return;
    sp_theme t2;
    SP_ASSERT(sp_theme_get(c2, &t2) == SP_OK);
    SP_ASSERT(t2.preset == SP_THEME_LIGHT);
    SP_ASSERT_STR(t2.accent_hex, "#AABBCC");
    SP_ASSERT_STR(t2.font_family, "Iosevka");
    SP_ASSERT(t2.text_scale == SP_TEXT_LARGE);
    sp_core_destroy(c2);
}

/* A rejected theme must leave the stored one untouched, not half
   applied. */
SP_TEST(a_rejected_theme_does_not_disturb_the_stored_one) {
    static char p[512];
    snprintf(p, sizeof p, "%s/theme_bad.yaml", getenv("SP_TEST_TMP"));
    char db[512];
    snprintf(db, sizeof db, "%s/theme_bad.db", getenv("SP_TEST_TMP"));
    unlink(db);
    FILE *f = fopen(p, "w");
    fprintf(f, "log_level: 0\nsip_port: 0\naudio_device: none\n"
               "db_file: %s\naccounts: []\n"
               "theme:\n  preset: gold\n", db);
    fclose(f);

    sp_core *c = NULL;
    SP_ASSERT(sp_core_create(p, &c) == SP_OK);
    if (!c) return;
    SP_ASSERT(sp_theme_set(c, SP_THEME_LIGHT, "lilac", NULL, SP_TEXT_SMALL)
              == SP_ERR_CONFIG);
    sp_theme t;
    SP_ASSERT(sp_theme_get(c, &t) == SP_OK);
    SP_ASSERT(t.preset == SP_THEME_GOLD);   /* unchanged */
    SP_ASSERT(t.text_scale == SP_TEXT_MEDIUM);
    sp_core_destroy(c);
}

SP_TEST(preview_matches_what_setting_would_produce) {
    sp_theme prev;
    SP_ASSERT(sp_theme_preview(SP_THEME_GOLD, "#010203", "Foo",
                               SP_TEXT_XLARGE, &prev) == SP_OK);
    SP_ASSERT_STR(prev.accent_hex, "#010203");
    SP_ASSERT_STR(prev.font_family, "Foo");
    SP_ASSERT(prev.preset == SP_THEME_GOLD);
    SP_ASSERT(sp_theme_preview(SP_THEME_GOLD, "nope", NULL, SP_TEXT_MEDIUM,
                               &prev) == SP_ERR_CONFIG);
}


/* Every preset's derived colours, pinned to what the Qt frontend
   produced before these rules moved into the core.
 *
 * The values were captured by running the Qt functions, not by
 * reading them: a test written from the new implementation would
 * agree with whatever the port happened to do. Six of these differed
 * by a unit on the first attempt, because QColor turns sixteen-bit
 * channels into eight with qt_div_257 rather than a shift. Nobody
 * would have seen it. Both frontends would have been wearing
 * different greens.
 */
SP_TEST(derived_colours_match_the_qt_frontend_for_every_preset) {
    static const struct {
        int preset;
        const char *go, *stop, *tx, *rx;
    } WANT[] = {
    { 0 , "#52DA84", "#DA5B52", "#378ADD", "#DD37DD" },  /* Dark */
    { 1 , "#52DA84", "#DA5B52", "#EF9F27", "#27EF3B" },  /* Gold */
    { 2 , "#23A553", "#AD2E25", "#378ADD", "#DD37DD" },  /* Light */
    { 3 , "#52DA84", "#DA5B52", "#FFFFFF", "#717171" },  /* High contrast */
    { 4 , "#52DA84", "#DA5B52", "#FFB000", "#01FF30" },  /* Amber CRT */
    { 5 , "#52DA84", "#DA5B52", "#33FF66", "#3366FF" },  /* Green Phosphor */
    { 6 , "#52DA84", "#DA5B52", "#7CFFB2", "#7D8AFF" },  /* Oscilloscope */
    { 7 , "#52DA84", "#DA5B52", "#C87F4A", "#55C84A" },  /* Copper Pair */
    { 8 , "#52DA84", "#DA5B52", "#4A9FD8", "#CA4AD8" },  /* Punchdown */
    { 9 , "#52DA84", "#DA5B52", "#B983FF", "#FFD883" },  /* Nebula */
    { 10, "#52DA84", "#DA5B52", "#FF6B35", "#64FF35" },  /* Event Horizon */
    { 11, "#52DA84", "#DA5B52", "#4EE8B0", "#634EE8" },  /* Aurora */
    { 12, "#52DA84", "#DA5B52", "#FF5C7A", "#CEFF5D" },  /* Redshift */
    { 13, "#52DA84", "#DA5B52", "#4FC3F7", "#D54FF7" },  /* Kyber */
    { 14, "#23A553", "#AD2E25", "#B26B2E", "#30A22A" },  /* Paper Tape */
    { 15, "#23A553", "#AD2E25", "#2F7FD1", "#D12FD1" },  /* White Dwarf */
    { 16, "#52DA84", "#DA5B52", "#E3B341", "#41E362" },  /* Republic */
    { 17, "#52DA84", "#DA5B52", "#AFC3D6", "#D6B0D6" },  /* Empire */
    { 18, "#52DA84", "#DA5B52", "#4DA3FF", "#FF4DFC" },  /* Jedi Order */
    { 19, "#52DA84", "#DA5B52", "#E0283A", "#96E028" },  /* Sith Order */
    };

    SP_ASSERT(sp_theme_preset_count() == 20);
    for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        sp_theme t = {0};
        SP_ASSERT(sp_theme_preview((sp_theme_preset)WANT[i].preset, NULL,
                                   NULL, SP_TEXT_MEDIUM, &t) == SP_OK);
        char go[8], stop[8], tx[8], rx[8];
        SP_ASSERT(sp_theme_semantic(&t, SP_SEMANTIC_GO, go, sizeof go) == SP_OK);
        SP_ASSERT(sp_theme_semantic(&t, SP_SEMANTIC_STOP, stop, sizeof stop) == SP_OK);
        SP_ASSERT(sp_theme_channel(&t, false, tx, sizeof tx) == SP_OK);
        SP_ASSERT(sp_theme_channel(&t, true, rx, sizeof rx) == SP_OK);
        SP_ASSERT(strcmp(go, WANT[i].go) == 0);
        SP_ASSERT(strcmp(stop, WANT[i].stop) == 0);
        SP_ASSERT(strcmp(tx, WANT[i].tx) == 0);
        SP_ASSERT(strcmp(rx, WANT[i].rx) == 0);
    }
}

/* Only the presets that have a mark name one; every other resolves to
   the empty string, never NULL, so a frontend can compare without a
   guard. */
SP_TEST(only_the_four_orders_carry_an_emblem) {
    for (int i = 0; i < sp_theme_preset_count(); i++) {
        sp_theme t = {0};
        SP_ASSERT(sp_theme_preview((sp_theme_preset)i, NULL, NULL,
                                   SP_TEXT_MEDIUM, &t) == SP_OK);
        SP_ASSERT(t.emblem != NULL);
        const char *want = i == SP_THEME_REPUBLIC   ? "republic"
                         : i == SP_THEME_EMPIRE     ? "empire"
                         : i == SP_THEME_JEDI_ORDER ? "jedi"
                         : i == SP_THEME_SITH_ORDER ? "sith" : "";
        SP_ASSERT(strcmp(t.emblem, want) == 0);
    }
}

/* Transmit is the accent itself, unmodified. If that ever stops
   being true the meter stops agreeing with everything else the theme
   colours. */
SP_TEST(the_transmit_trace_is_the_accent) {
    sp_theme t = {0};
    SP_ASSERT(sp_theme_preview(SP_THEME_DARK, "#123456", NULL,
                               SP_TEXT_MEDIUM, &t) == SP_OK);
    char tx[8];
    SP_ASSERT(sp_theme_channel(&t, false, tx, sizeof tx) == SP_OK);
    SP_ASSERT(strcmp(tx, "#123456") == 0);
}

/* A buffer too small is refused rather than half-filled. */
SP_TEST(a_short_buffer_is_refused) {
    sp_theme t = {0};
    SP_ASSERT(sp_theme_preview(SP_THEME_DARK, NULL, NULL,
                               SP_TEXT_MEDIUM, &t) == SP_OK);
    char tiny[4];
    SP_ASSERT(sp_theme_channel(&t, true, tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
    SP_ASSERT(sp_theme_semantic(&t, SP_SEMANTIC_GO, tiny, sizeof tiny)
              == SP_ERR_INVALID_ARG);
}

int main(void) {
    if (!getenv("SP_TEST_TMP")) { printf("SP_TEST_TMP unset\n"); return 1; }
    SP_RUN(each_preset_resolves_to_its_own_look);
    SP_RUN(every_preset_is_legible);
    SP_RUN(every_preset_is_named_once);
    SP_RUN(an_accent_override_wins_over_the_preset);
    SP_RUN(a_font_override_wins_over_the_preset);
    SP_RUN(every_text_scale_has_a_distinct_multiplier);
    SP_RUN(an_invalid_accent_is_refused);
    SP_RUN(preset_and_scale_names_round_trip);
    SP_RUN(theme_defaults_when_absent_from_config);
    SP_RUN(theme_is_read_from_config);
    SP_RUN(theme_survives_a_save);
    SP_RUN(unknown_theme_key_is_refused_naming_itself);
    SP_RUN(unknown_preset_loads_as_default_and_is_written_back);
    SP_RUN(invalid_accent_in_config_is_refused);
    SP_RUN(setting_a_theme_persists_across_a_restart);
    SP_RUN(a_rejected_theme_does_not_disturb_the_stored_one);
    SP_RUN(preview_matches_what_setting_would_produce);
    SP_RUN(derived_colours_match_the_qt_frontend_for_every_preset);
    SP_RUN(only_the_four_orders_carry_an_emblem);
    SP_RUN(the_transmit_trace_is_the_accent);
    SP_RUN(a_short_buffer_is_refused);
    return SP_TEST_RESULT();
}

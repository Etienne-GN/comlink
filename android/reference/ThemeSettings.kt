package com.questlog.settings.theme

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable

// --- Domain model -----------------------------------------------------

enum class ThemePreset(val baseFont: String, val defaultAccentHex: String) {
    HUNTER_DARK(baseFont = "cinzel", defaultAccentHex = "#378ADD"),
    GOLD_RANK(baseFont = "cinzel", defaultAccentHex = "#EF9F27"),
    LIGHT(baseFont = "rajdhani", defaultAccentHex = "#378ADD"),
    HIGH_CONTRAST(baseFont = "system_default", defaultAccentHex = "#FFFFFF")
}

enum class TextScale(val multiplier: Float) {
    SMALL(0.9f), MEDIUM(1.0f), LARGE(1.15f), EXTRA_LARGE(1.3f)
}

@Serializable
data class ThemeSettings(
    val presetId: ThemePreset = ThemePreset.HUNTER_DARK,
    val accentOverrideHex: String? = null,   // null => use preset default
    val fontId: String? = null,               // null => use preset's baseFont
    val textScale: TextScale = TextScale.MEDIUM
) {
    val resolvedAccentHex: String get() = accentOverrideHex ?: presetId.defaultAccentHex
    val resolvedFontId: String get() = fontId ?: presetId.baseFont
}

// --- Persistence (Jetpack DataStore) -----------------------------------

private object Keys {
    val PRESET = stringPreferencesKey("theme_preset")
    val ACCENT = stringPreferencesKey("theme_accent_override")
    val FONT = stringPreferencesKey("theme_font_id")
    val SCALE = intPreferencesKey("theme_text_scale")
}

class ThemeSettingsRepository(private val dataStore: DataStore<Preferences>) {

    val settingsFlow: Flow<ThemeSettings> = dataStore.data.map { prefs ->
        ThemeSettings(
            presetId = prefs[Keys.PRESET]?.let { runCatching { ThemePreset.valueOf(it) }.getOrNull() }
                ?: ThemePreset.HUNTER_DARK,
            accentOverrideHex = prefs[Keys.ACCENT],
            fontId = prefs[Keys.FONT],
            textScale = prefs[Keys.SCALE]?.let { TextScale.entries.getOrNull(it) } ?: TextScale.MEDIUM
        )
    }

    suspend fun setPreset(preset: ThemePreset) {
        dataStore.edit { it[Keys.PRESET] = preset.name }
    }

    suspend fun setAccentOverride(hex: String?) {
        dataStore.edit { prefs ->
            if (hex == null) prefs.remove(Keys.ACCENT) else prefs[Keys.ACCENT] = hex
        }
    }

    suspend fun setFont(fontId: String?) {
        dataStore.edit { prefs ->
            if (fontId == null) prefs.remove(Keys.FONT) else prefs[Keys.FONT] = fontId
        }
    }

    suspend fun setTextScale(scale: TextScale) {
        dataStore.edit { it[Keys.SCALE] = scale.ordinal }
    }
}

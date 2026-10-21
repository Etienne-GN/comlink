package com.questlog.settings.sound

import android.content.Context
import android.net.Uri
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.floatPreferencesKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import java.io.File
import java.util.UUID

enum class QuestEvent(val channelId: String) {
    QUEST_ASSIGNED("quest_assigned"),
    QUEST_COMPLETE("quest_complete"),
    OVERDUE_NUDGE("overdue_nudge")
}

sealed class SoundChoice {
    data class Builtin(val id: String) : SoundChoice()      // e.g. "quest_horn", "victory_fanfare"
    data class Custom(val filePath: String) : SoundChoice()  // app-private copy, not the original URI
    data object SystemDefault : SoundChoice()
}

data class SoundSettings(
    val perEvent: Map<QuestEvent, SoundChoice> = mapOf(
        QuestEvent.QUEST_ASSIGNED to SoundChoice.Builtin("quest_horn"),
        QuestEvent.QUEST_COMPLETE to SoundChoice.Builtin("victory_fanfare"),
        QuestEvent.OVERDUE_NUDGE to SoundChoice.Builtin("low_warning")
    ),
    val volume: Float = 0.8f
)

// --- Persistence --------------------------------------------------------
// Stored as "builtin:quest_horn" / "custom:/data/.../abc123.mp3" / "system"

private object Keys {
    fun soundKey(event: QuestEvent) = stringPreferencesKey("sound_${event.channelId}")
    val VOLUME = floatPreferencesKey("sound_volume")
}

class SoundSettingsRepository(
    private val context: Context,
    private val dataStore: DataStore<Preferences>
) {
    val settingsFlow: Flow<SoundSettings> = dataStore.data.map { prefs ->
        val perEvent = QuestEvent.entries.associateWith { event ->
            decode(prefs[Keys.soundKey(event)])
        }
        SoundSettings(perEvent = perEvent, volume = prefs[Keys.VOLUME] ?: 0.8f)
    }

    suspend fun setSound(event: QuestEvent, choice: SoundChoice) {
        dataStore.edit { it[Keys.soundKey(event)] = encode(choice) }
    }

    suspend fun setVolume(volume: Float) {
        dataStore.edit { it[Keys.VOLUME] = volume.coerceIn(0f, 1f) }
    }

    /**
     * Copies a user-picked audio file (from a SAF/content picker) into app-private
     * storage so it survives the source being moved or deleted, then returns the
     * SoundChoice to persist via setSound().
     */
    fun importCustomSound(sourceUri: Uri): SoundChoice {
        val soundsDir = File(context.filesDir, "custom_sounds").apply { mkdirs() }
        val destFile = File(soundsDir, "${UUID.randomUUID()}.mp3")
        context.contentResolver.openInputStream(sourceUri)?.use { input ->
            destFile.outputStream().use { output -> input.copyTo(output) }
        } ?: error("Could not open source audio for import")
        return SoundChoice.Custom(destFile.absolutePath)
    }

    private fun encode(choice: SoundChoice): String = when (choice) {
        is SoundChoice.Builtin -> "builtin:${choice.id}"
        is SoundChoice.Custom -> "custom:${choice.filePath}"
        SoundChoice.SystemDefault -> "system"
    }

    private fun decode(raw: String?): SoundChoice = when {
        raw == null -> SoundChoice.SystemDefault
        raw == "system" -> SoundChoice.SystemDefault
        raw.startsWith("builtin:") -> SoundChoice.Builtin(raw.removePrefix("builtin:"))
        raw.startsWith("custom:") -> SoundChoice.Custom(raw.removePrefix("custom:"))
        else -> SoundChoice.SystemDefault
    }
}

package com.questlog.notifications

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.media.AudioAttributes
import android.net.Uri
import androidx.core.content.FileProvider
import com.questlog.settings.sound.QuestEvent
import com.questlog.settings.sound.SoundChoice
import com.questlog.settings.sound.SoundSettings
import java.io.File

/**
 * Rebuilds notification channels to match the user's current sound settings.
 * Android channel sound/importance are LOCKED once created on API 26+, so any
 * change to sound picks means deleting and recreating the channel with a new id.
 * Call this once at app start and again immediately after any sound setting change.
 */
object QuestNotificationChannels {

    private const val AUTHORITY_SUFFIX = ".soundprovider"

    fun applySettings(context: Context, settings: SoundSettings) {
        val manager = context.getSystemService(NotificationManager::class.java)

        QuestEvent.entries.forEach { event ->
            val choice = settings.perEvent[event] ?: SoundChoice.SystemDefault
            // Versioning the channel id by choice forces Android to pick up the new sound
            val channelId = "${event.channelId}_${choiceVersionTag(choice)}"

            // Remove any previous version of this event's channel so stale ones don't linger
            manager.notificationChannels
                .filter { it.id.startsWith(event.channelId) && it.id != channelId }
                .forEach { manager.deleteNotificationChannel(it.id) }

            if (manager.getNotificationChannel(channelId) != null) return@forEach

            val channel = NotificationChannel(
                channelId,
                displayName(event),
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                val soundUri = resolveSoundUri(context, choice)
                if (soundUri != null) {
                    setSound(
                        soundUri,
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_NOTIFICATION_EVENT)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build()
                    )
                }
                // else: leave default, System channel default sound applies
            }
            manager.createNotificationChannel(channel)
        }
    }

    /** Returns the channel id to use for posting a notification for this event right now. */
    fun currentChannelId(event: QuestEvent, settings: SoundSettings): String {
        val choice = settings.perEvent[event] ?: SoundChoice.SystemDefault
        return "${event.channelId}_${choiceVersionTag(choice)}"
    }

    private fun choiceVersionTag(choice: SoundChoice): String = when (choice) {
        is SoundChoice.Builtin -> "b_${choice.id}"
        is SoundChoice.Custom -> "c_${File(choice.filePath).name.hashCode()}"
        SoundChoice.SystemDefault -> "sys"
    }

    private fun resolveSoundUri(context: Context, choice: SoundChoice): Uri? = when (choice) {
        SoundChoice.SystemDefault -> null
        is SoundChoice.Builtin -> Uri.parse(
            "android.resource://${context.packageName}/raw/${choice.id}"
        )
        is SoundChoice.Custom -> FileProvider.getUriForFile(
            context,
            context.packageName + AUTHORITY_SUFFIX,
            File(choice.filePath)
        )
    }

    private fun displayName(event: QuestEvent): String = when (event) {
        QuestEvent.QUEST_ASSIGNED -> "Quest assigned"
        QuestEvent.QUEST_COMPLETE -> "Quest complete"
        QuestEvent.OVERDUE_NUDGE -> "Overdue nudge"
    }
}

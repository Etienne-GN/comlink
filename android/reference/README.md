# Kotlin reference sketches

These three files are **not part of any build**. They came from a
different Android application (`com.questlog.*`) and are kept here as
prior art for the Android port, because they solve problems the port
will meet:

| File | Why it is worth keeping |
|---|---|
| `ThemeSettings.kt` | persisting a theme choice with DataStore, which is where the core's theme settings will land on Android |
| `SoundSettings.kt` | the same for sound selection, mirroring `sp_sound_set` |
| `NotificationChannels.kt` | Android locks a notification channel's sound and importance once the channel exists on API 26+, so changing a ringtone means deleting and recreating the channel under a new id. That is a real constraint on how `sp_sound_set` can behave on Android, and it is not obvious. |

They will be replaced, not adapted: the Android frontend calls the same
flat C ABI the Qt frontend does, so the logic these files contain
belongs in the core rather than in Kotlin. What they are useful for is
the platform plumbing around it.

They carry no licence header. If any of it survives into the port,
check its provenance first: this repository has no licence settled
yet, and code from elsewhere cannot be relicensed by being moved here.

# Roadmap

Requested after Phase 1 was verified working against a
real FreeSWITCH registrar with bidirectional PSTN audio.

## The constraint that shapes everything

**Debian and Android must be identical.** That is not a UI goal, it is
an architectural rule: every feature below lands in the **core** or in
the **shared YAML schema**, never in the Qt layer. A feature
implemented in Qt has to be written twice and will drift.

Concretely: theming cannot be Qt stylesheets, and call history cannot
be a Qt data model. Both belong in the core.

## Already done, needs no new core work

| Want | State |
|---|---|
| Recording | **Done.** Core, CLI, and a Record/Stop-rec button in the GUI writing timestamped WAVs to the user data dir. |
| Multiple registrations | **Done.** Core supports N simultaneous accounts; the main window now lists every account's registration state, not just the selected one. |
| Action log | The GUI activity pane exists. **Missing: persistence, and it is Qt-only so Android would not share it.** |

## New work, in dependency order

### 1. SIP log — cheap, because the module already sees everything

`hdr_module.c` already receives every tx and rx message; that is how
header rewriting works. A ring buffer of formatted messages plus an
ABI accessor is a small addition to an existing component.

- capture full message text, direction, timestamp, Call-ID
- bounded ring buffer, size from config, so a long-running phone
  cannot grow without limit
- new ABI: `sp_siplog_read`, plus an `SP_EV_SIP_MESSAGE` event for a
  live trace view
- **decided:** traces persist to disk attached to call records, with
  credential headers (`Authorization`, `Proxy-Authorization`, `WWW-`
  and `Proxy-Authenticate`) redacted by default, plus a config flag to
  temporarily disable redaction when debugging a PBX interaction.
  The flag is deliberately awkward to leave on: the core logs a
  warning on every start while redaction is off, so an unredacted
  capture cannot be forgotten about indefinitely. Dialled numbers and
  caller identities are still written in cleartext either way. In
  Quebec, Loi 25 applies to any organisation that deploys this to
  record real calls, and that deployment has to be reviewed with its
  privacy officer before it stores other people's numbers.

### 2. Call history — needs a storage decision

Depends on (1) so each record can carry its own SIP trace.

- per-call record: direction, remote party, account, start, answer,
  end, duration, disconnect cause, negotiated codec, recording path
- attach the SIP messages whose Call-ID matches
- **decided: SQLite.** Android ships it and Debian packages it, so
  one schema serves both platforms and history stays queryable.
  Consequences to plan for: a new core dependency, the NDK build must
  link it too, and schema migrations become a maintained concern from
  the first release rather than an afterthought.

### 3. Theming — must be core, not Qt

- theme described in the shared YAML: named presets plus overrides for
  accent, font and text scale
- core exposes the resolved values over the ABI; each frontend applies
  them natively (Qt palette/stylesheet, Android theme attributes)
- the schema, the presets and the resolution rules are shared, so the
  two platforms cannot drift

The QuestLog `ThemeSettings.kt` sketch already models this shape
correctly: presets with per-field overrides and a resolved accessor.
That structure is worth reusing; it just has to live in the core.

### 4. Android parity

The decisions below are settled; the spec and
the plan get written when M1 actually starts.

**What it is: a daily driver**, not a diagnostic instrument and not a
portability proof. That choice moves the centre of gravity off the UI
and onto staying reachable.

**Reachability: a foreground service**, not push. A persistent
registration behind an ongoing notification works against any PBX with
no server-side change, which is why it goes first. Push (RFC 8599 plus
FCM) is the better answer for battery and Doze, but it is a PBX-side
infrastructure commitment -- FreeSWITCH does not do it out of the box,
and it needs a relay holding FCM credentials -- rather than a Comlink
change, so it stays an open question rather than an assumption.

The foreground service's real weakness is named rather than hidden:
OEM battery managers kill it, and on a personal handset the
battery-optimisation exemption is a setting the user has to find. A
phone that is only reachable when Android feels like it is a fact to
design around, not a bug to be surprised by.

**Target: one sideloaded handset** to begin with. Fleet deployment is
a later question.

**Three milestones, each shippable on its own:**

1. **M1 -- it runs.** NDK cross-build, the JNI bridge, the foreground
   service, and enough UI to register, dial, talk and hang up.
   *Cross-build and bridge done; a call placed from the app
   to a desktop peer completed with media both ways, and the password
   is kept under an Android Keystore key. Registration against a real
   PBX is what is left -- see `android/README.md` in git history,
   removed with the Java frontend.*
2. **M2 -- it is a phone.** Incoming call over the lock screen,
   notifications, Telecom integration, and `sp_core_network_changed`
   finally implemented -- wifi/LTE handoff is testable here and
   nowhere else.
3. **M3 -- it is Comlink.** Accounts, codecs, contacts, history, SIP
   log, quality, theming, recordings.

**Build wiring:** mirror the Debian shape. A
`scripts/build-pjproject-android.sh` runs pjproject's own
`configure-android` per ABI, alongside cross-builds of libyaml, libogg
(for OggOpus recordings) and the sqlite3 amalgamation into
`third_party/prefix-android-<abi>/`. The
top-level `CMakeLists.txt` takes the prefix as a variable -- it
hardcoded `prefix-debian` -- and Gradle's `externalNativeBuild`
drives that same CMake. The core's source list stays defined once,
because a list maintained in two build systems drifts.

**Facts established while deciding, so they are not re-litigated:**

- The core is already portable. Scanned for `qsort_r`, `getline`,
  `memmem`, `timegm`, `strerror_r`, `execinfo` and `%m`: clean. The
  only Linux-specific line in the whole core is the
  `setenv("ALSA_CONFIG_PATH", ...)` pipewire workaround in `core.c`.

  Corrected: this used to say "no `localtime`, no
  `strftime`". Recording filenames, trace exports and the trace path
  all use `localtime_r` and `strftime` now (`recname.c`,
  `traceexport.c`, `core.c`). Both are POSIX and present in bionic, so
  the conclusion stands -- but the sentence had gone stale, and a
  portability claim nobody rechecks is how a port discovers a
  dependency late.
- `test_abi_purity` already proves the public header is PJSIP-free,
  which is what makes a hand-written JNI layer a bridge rather than a
  port.
- `MANAGE_OWN_CALLS` declared in the manifest is sufficient on its own
  to satisfy the `phoneCall` foreground-service-type prerequisite; a
  self-managed `ConnectionService` is not required for it. So M2's
  Telecom work does not get dragged into M1. Two constraints come
  with the type: `startForeground()` must be called within five
  seconds or the call is lost to
  `ForegroundServiceDidNotStartInTimeException`, and on Android 15+ a
  phone-call foreground service cannot be started from
  `BOOT_COMPLETED`.
- `configure-android` defaults `TARGET_ABI` to `arm64-v8a` and floors
  the platform at 23. Oboe is opt-in via `--with-oboe=<prefab path>`;
  without it the backend is OpenSL.
- Debian packages `android-sdk` and `gradle` but **not** the NDK, so
  the toolchain comes from Google's `sdkmanager`. That is a binary
  toolchain from outside the distribution, which is a provenance step
  up from the rest of this build: pin it to an exact version in the
  script rather than taking whatever `sdkmanager` last offered.
- An x86_64 emulator gives `adb` over localhost, so the cross-build
  and the JNI bridge are testable with no handset and no network
  exposure. Only registration and live audio wait for a phone.
- Android needs its own `config_site_android.h`; the Debian one
  disables video, speex and ilbc and enables the null audio device.

  Superseded by M1a: the Debian `config_site` was reused
  unchanged for both Android ABIs and nothing wanted to differ. A
  second config that starts as a copy is a second config that drifts,
  so there is still only one.

### M1a: the core cross-compiles (done)

`libcomlink_core.so` builds for `arm64-v8a` and `x86_64` against NDK
r27d. `scripts/build-deps-android.sh` cross-builds the dependencies
into `third_party/prefix-android-<abi>/`, `scripts/build-core-android.sh`
builds the core against it, and the source list is still the one in
`core/CMakeLists.txt`.

**Verified.** `file` and `readelf` on both `.so` files: ELF64,
AArch64 and x86-64 respectively, `DYN`, "for Android 23, built by NDK
r27d". `NEEDED` is `libc++_shared`, `libmediandk`, `libm`,
`libOpenSLES`, `liblog`, `libdl`, `libc` and nothing else -- no
`libasound`, no `libssl`, no host glibc. 245 exported `sp_*` symbols
on both, the same count as the desktop build.

**Superseded the same day: it runs.** An emulator was set up (see
`docs/android-emulator.md`) and the core's own test executables were
cross-compiled and run on it. **25 of 31 pass on Android 14, x86_64.**
That includes `store` -- the vendored SQLite amalgamation works --
`oggopus`, `speedport` and `sound`, so the recording path, the WSOLA
wrapper and the sound files are all exercised on the device rather
than assumed.

The other six all fail for one reason, described next.

### pjproject's Android backends need a JVM

Six tests fail on the emulator: `call`, `history`, `playback`,
`recording`, `siplog`, `traceexport`. Five abort on the same pjsip
assertion, and `call` segfaults. Both are the same cause.

pjlib on Android does not generate GUIDs itself. `guid_android.c`
calls `java.util.UUID.randomUUID()` through JNI, and with no JVM
attached it logs "Error generating UUID" and returns an empty string.
A probe on the device confirmed it directly: `PJ_GUID_STRING_LENGTH`
is 36, the generated string's length is 0 and its hash is 0 --
exactly what `pjsip_ua_register_dlg` asserts against. Every dialog
needs a local tag, so nothing can place or receive a call.

`test_call` gets one step further and segfaults in
`android_create_stream` (`android_jni_dev.c`), pjmedia's JNI audio
device, on a null `JNIEnv`. Note that this corrects an earlier note
here: "without Oboe the backend is OpenSL" is not what happens in
practice -- `libOpenSLES` is linked, but the JNI device is the one
pjsua opens.

**Confirmed the same day, and fixed.** `sp_core_set_jvm(void *vm)`
was appended to the ABI -- `void *` so the header stays free of both
pjsip and `jni.h`, and a no-op off Android so a frontend may call it
unconditionally. The JNI bridge calls it from `JNI_OnLoad`, and a
skeleton APK then placed a call on the emulator: `sp_core_create = 0`,
`sp_core_start = 0`, `sp_call_make(...) = success`. See
`android/README.md` in git history, removed with the Java frontend.

The open question about `JNI_OnLoad` is answered too: no collision.
pjlib's lives in `libcomlink_core.so` and ours in `libcomlink_jni.so`,
two separate objects, and the runtime only calls the one belonging to
the library `System.loadLibrary` names.

It also sets the ceiling on this harness: **25 of 31 is as far as
native test executables go.** The remaining six need an app process,
so they wait for the bridge.

### Credentials are written in cleartext, and now need not be

`sp_account_add` persists everything it is given into the
configuration file, password included. On the desktop that is the
established behaviour and arguably the right one: the file is the
operator's, mode 0600, and hand-editing it is supported. On Android
it meant that encrypting the password under a keystore key left an
identical cleartext copy in the app's own `comlink.yaml`.

`sp_account_add_transient` was appended to the ABI: the account exists
for the run and is never written, and the frontend owns putting it
back. The Android app adds its account that way. The regression test
looks for the password in the file the core wrote, and also asserts
that an ordinary add still persists -- so it fails for the right
reason rather than because the writer stopped working.

**The desktop is unchanged and still writes cleartext passwords to
`~/.config/comlink/comlink.yaml`.** Whether it should keep doing so is
a separate decision, not one this milestone took. The mechanism to
stop now exists.

### What the bridge found in the core

`sp_account_add` accepted a `reg_expires_s` the configuration loader
rejected. The loader enforces 30..86400 and defaults to 300; the API
took whatever it was given and wrote it out verbatim, so an account
added with the field left at zero -- which is what any C caller that
memsets the struct passes, and what the JNI bridge did -- produced a
file the core then refused to open. The only symptom was a
configuration error on the *next* start, nowhere near the cause.

Fixed by giving both the same constants: zero now means the default,
and anything outside the range is `SP_ERR_INVALID_ARG` rather than a
file that cannot be read back. The regression test asserts the
**reload**, not the stored value -- a test that checked the stored
value would have passed against the broken version, which stored
exactly what it was given.

**TLS was missing, and that was the finding that mattered.**
`aconfigure.ac` refuses to autodetect OpenSSL, Opus and most optional
dependencies when `ac_cross_compile` is set: without an explicit
`--with-ssl=DIR` it sets `enable_ssl=no` and says so in one line of
configure output that a successful build buries. Nothing about the
build failed; the Android pjproject simply had no TLS transport, and
for a client that registers over TLS that is not optional.

Resolved the same day. OpenSSL 3.6.4 is a submodule, pinned to the
tag matching the system version, cross-built into the prefix before
pjproject, and named by `--with-ssl`. configure now says "OpenSSL
library found, SSL support enabled" and, separately, "OpenSSL has AES
GCM support, SRTP will use OpenSSL". Both `.so` files carry
`pjsip_tls_transport_start` and `SSL_CTX_new`, and the `NEEDED` list
is unchanged because it links statically -- the core grew from 4.8 MB
to 12 MB, which is libcrypto. **That it negotiates a TLS handshake
against a real registrar is not shown**; only that the transport and
its crypto are present and linked.

**What had to change in the code:** one line. `store.c`'s migration
array concatenates two `ALTER TABLE` statements across two lines, and
clang's `-Wstring-concatenation` reads that as a forgotten comma. GCC
has no such warning, so the Debian build never mentioned it and the
cross-build refused to compile. Parenthesised, with the reason
written beside it. Everything else compiled unmodified -- including
`core.c`'s `setenv("ALSA_CONFIG_PATH", ...)`, which is a runtime
no-op on Android rather than a compile error, and still needs to go.

**Three build traps, all found the hard way:**

- **`PJ_PREFIX` was already taken.** `pkg_check_modules(PJ ...)`
  defines `<prefix>_PREFIX` from the `.pc` file's own `prefix=` and
  silently overwrites it. On Debian the two values are identical so
  the collision never showed. Renamed to `COMLINK_DEPS_PREFIX`.
- **The prefix is on the include path of the thing that installs into
  it.** pjproject compiles with `-I$PREFIX/include` and installs its
  own headers there, so a second run reads the previous run's
  `pjmedia-codec/config_auto.h` in preference to the one configure
  just generated. Enabling Opus produced a 744-byte `opus.o` because
  the stale header still said `PJMEDIA_HAS_OPUS_CODEC 0`.
  `build-deps-android.sh` now empties the prefix before it starts.
- **`PKG_CONFIG_PATH` is not enough when cross-compiling.** It only
  prepends; the system directories stay in the search list, so a
  missing Android library is answered by the host's copy. The core
  build sets `PKG_CONFIG_LIBDIR` to exactly one directory, which is
  what makes a missing dependency fail instead of linking x86-64
  objects into an aarch64 target.

**For M1b:** `libc++_shared.so` is a `NEEDED` entry, so the APK has to
ship the NDK's copy. The vendored sources are now four submodules
(pjproject, libyaml, libogg, libopus) plus the SQLite amalgamation;
libopus 1.6's `autogen.sh` downloads a 134 MB model archive over the
network, content-addressed by the SHA-256 in its URL,
which is worth knowing before anyone tries to build offline.

**Open: the "valid until the next call" accessors.** Roughly nine
accessors in `comlink/core.h` hand back a pointer into core-owned
storage documented as valid until the next setter call --
`sp_recording_get_dir` is only the newest. The getter copies the
pointer under the lock and releases it before returning; the setter
frees that same buffer under the lock. Two threads, and the reader
holds freed memory.

It is unreachable on the desktop, where one UI thread both reads and
writes these. M1 is where it becomes reachable, because the foreground
service and the UI hold the same core. Decide it there, once, for the
whole family rather than per accessor: either the getters copy into a
caller buffer, or the ABI grows a documented rule about which thread
may set. Found reviewing the recording directory work.

**Open, and deliberately not decided yet:** which foreground service
type the registration-holding service uses when no call is up.
`phoneCall` is honest during a call and a stretch while merely
registered; `specialUse` is accurate but carries its own declaration.
This gets resolved in M1's spec, with the answer written down.

## Closed since this was written

- Debian packaging: four packages build, install and are verified from
  the install tree rather than the build tree.
- TLS: proven on the wire, and a real defect fixed in the process.
- SRTP: implemented as a three-state mode with a negative test.
- Spec item 13.3: verified. rx rules take effect before pjsip parses,
  so they can influence the stack's own handling — and an rx rule that
  removes a needed header destroys calls silently. See
  `docs/verification-13-3-rx-ordering.md`.
- The three data races: fixed, ThreadSanitizer clean.

- Audio quality metrics (Plan 7): per-call RTCP counters, jitter,
  round-trip time, jitter-buffer state and an estimated MOS, live and
  in the call history. Verified against `tshark` on a real stream; see
  `docs/verification-plan-7-quality.md`.
- The test-suite hang, formerly filed as a teardown deadlock. Root
  cause found: ALSA's pipewire ctl plugin hanging inside `pjsua_init`
  when the pipewire socket is unreachable, which is the condition
  `dpkg-buildpackage` creates. Fixed for the test suite; see
  `docs/known-issues.md`.

- Audio device selection: enumerated in the core, chosen per
  direction, persisted, with an Audio window in the GUI and `audio` /
  `audio-set` in the CLI. `audio_device: none` now also makes startup
  skip ALSA entirely, which closes the product side of the hang above.

## Open bugs that gate "daily driver"

- **A phone that wants audio still enumerates ALSA**, so it still
  carries the pipewire hang risk at startup. There is no way to list
  audio devices without consulting ALSA. `audio_device: none` is the
  escape hatch, and it is now genuinely safe.
- **Hold, resume, recording playback and transfer are implemented but
  unproven on a live call.** The unit tests cover argument handling
  only. `tests/smoke/live-call-checklist.sh` drives them; see
  `docs/smoke-test-live-pbx.md`.
- **STUN/ICE is not covered by any plan.** It needs a NAT to be
  meaningful, and there is none in the test environment.

## Requested, not yet designed

Asked for after the module registry landed. Both are
call-handling behaviour, and both look like modules rather than
settings: each is something a plain SIP phone is complete without.

- **Do not disturb.** Refuse incoming calls without ringing. The
  questions worth settling before building it: does it reject with
  486 Busy Here or 480 Temporarily Unavailable, is it per-account or
  global, and does the call still reach the history as a missed call.
  The last one matters most -- a DND that hides the fact someone rang
  is a phone that lies to its operator.
- **Auto-answer.** Answer incoming calls without the operator. Needs
  a delay, and needs deciding whether it honours the `Call-Info:
  answer-after=` header that intercom and paging systems send, which
  is the usual reason to want it at all. Interacts with DND, and one
  of them has to win; DND should.

Neither is specified. Both want the same question asked first: what
happens to the call record, because a phone used for diagnostics that
quietly drops calls from its own history is worse than one without
the feature.


## Ideas, not committed

- **Getting at an incoming call without hunting for the window.** The
  need is real -- a phone spends most of the day not being looked at
  -- but the shape is not decided, and a tray icon is only one guess
  at it. A popup, or simply raising the window to the front when a
  call arrives, may be all it takes, and both are less machinery.

  If it ever is a tray: GNOME has no system tray of its own. What
  produces one is the AppIndicator extension, which is installed on
  the machine this was discussed on -- `org.kde.StatusNotifierWatcher`
  answers on the session bus there -- but is absent from a stock GNOME
  desktop, where `QSystemTrayIcon::isSystemTrayAvailable()` returns
  false and the icon silently does not appear. That is the same shape
  as the call log icon that looked fine on one desktop and was
  invisible on another, so it is written down now rather than
  discovered later.

  Desktop notifications carrying Answer and Decline need no extension
  and reach any desktop, which is worth weighing before any of it.

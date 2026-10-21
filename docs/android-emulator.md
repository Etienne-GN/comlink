# The Android emulator, one-time setup

What `scripts/start-emulator-android.sh` and
`scripts/run-core-tests-android.sh` expect to already exist. None of
it is scripted, because it downloads about 2.5 GB from Google and
accepts Google's SDK licence agreement -- neither of which a build
script should decide on someone's behalf.

## Why not the Debian SDK

`/usr/lib/android-sdk` is root-owned, so `sdkmanager` cannot write to
it, and Debian packages neither the emulator nor a system image. The
NDK is the one piece that does come from the distribution, and the
build scripts pin it to `27.3.13750724` (r27d).

So: the NDK from Debian, everything else in a user-writable SDK root.

## Setup

```sh
# A JDK, not a runtime: 11 is too old, and a 17 installed as a runtime
# only fails Gradle's Java compile with "does not provide the required
# capabilities: [JAVA_COMPILER]". The build scripts default to 21.
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
export ANDROID_SDK_ROOT="$HOME/android-sdk"
mkdir -p "$ANDROID_SDK_ROOT"

sdkmanager --sdk_root="$ANDROID_SDK_ROOT" --licenses
sdkmanager --sdk_root="$ANDROID_SDK_ROOT" \
    emulator platform-tools cmdline-tools\;latest \
    "system-images;android-34;default;x86_64"
```

`cmdline-tools;latest` is not optional. Debian's `avdmanager` is old
enough that it rejects the current system image with "Package path is
not valid", listing `null` as the alternative; the one that arrives
under `cmdline-tools/latest/bin` accepts it.

Then create the AVD:

```sh
"$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/avdmanager" create avd \
    -n comlink-x86_64 -k "system-images;android-34;default;x86_64" \
    -d pixel_6
```

## Hardware acceleration

`/dev/kvm` must be readable and writable. It is `root:kvm`, and
membership in the `kvm` group is the usual route; on this machine a
POSIX ACL grants the operator directly, which `getfacl /dev/kvm`
shows. Without it the emulator falls back to software emulation and
becomes too slow to be useful.

## The GPU mode is not a detail

Use `-gpu swangle_indirect`. `scripts/start-emulator-android.sh` does.

The emulator's SwiftShader rasteriser draws every quad Qt Quick
produces as a triangle tapering to nothing, while text renders
perfectly -- so the window looks like a Qt bug, or like a corrupt
build, and behaves identically under `swiftshader_indirect` and
`-gpu guest` because both of those *are* SwiftShader. ANGLE renders
the same APK correctly.

That cost a morning, a clean rebuild of Qt from source, and a
diagnostic pass through every Rectangle feature. Written down so it
costs nobody else one.

## Running

```sh
./scripts/start-emulator-android.sh
./scripts/run-core-tests-android.sh x86_64
```

The test runner pushes the core, the NDK's `libc++_shared.so` and
every `test_*` binary to `/data/local/tmp/comlink`, gives each one a
clean scratch directory, and reports per-test pass or fail.

## What cannot be tested this way

Six of the core's tests place a call, and a call cannot be placed from
a plain native executable. pjproject's Android backends go through
JNI -- `java.util.UUID.randomUUID()` for dialog tags,
`android.media.AudioRecord` for capture -- and a process with no JVM
gets an empty GUID and a null `JNIEnv`. See the M1a findings in
`docs/roadmap.md`. Those six need an app process, which means they
wait for the JNI bridge.

## The test peer

`comlink-cli` answering on the desktop is how an Android call is
tested. Put it on **5090**, not 5080: `tests/integration/loopback.sh`
uses 5080 and 5081, and a peer left running there fails the
integration tests with what looks like a regression in the core. It
cost three false alarms before the port was moved.

```sh
sed 's/@PORT@/5090/' tests/integration/bob.yaml > run/android/peer/bob.yaml
XDG_CONFIG_HOME="$PWD/build/alsa-home" \
    ./build/frontend/cli/comlink-cli --config run/android/peer/bob.yaml \
    --no-audio --auto-answer 200
```

Dial `sip:bob@<host LAN address>:5090` from the app. `10.0.2.2` is the
emulator's own NAT alias and answers pings itself, so it proves
nothing about whether the host is reachable.

## How the sounds get there

`packaging/sounds` is installed to `/usr/share/comlink/sounds` by the
Debian package and found there at runtime. An APK has no such place,
so the files ride in as Qt resources and `main.cpp` unpacks them
beside the configuration on each start, then points
`COMLINK_SOUND_DIR` at them.

Copied rather than read from the resource system because the core
opens a sound by path, and teaching pjmedia's WAV player to take a
stream would be a change to the media path for the sake of a
packaging detail. Overwritten every start, so an upgrade that ships a
new sound is not left waiting behind a copy of the old one.

Before this, every slot on Android read "none" and the core looked
like it had lost them.

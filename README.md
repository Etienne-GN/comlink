# Comlink

A SIP softphone and diagnostic instrument for telephony technicians.

It places and answers calls like any softphone. What it is actually for
is the other half: registering to several PBXes at once, rewriting SIP
headers from a declarative rule set, capturing the signalling, and
reporting what the audio path really did — packet loss, jitter,
round-trip time, jitter-buffer behaviour — per call, live and
afterwards.

```
comlink          the phone, one QML window on Debian and on Android
comlink-cli      the same core behind a line protocol, for scripting
libcomlink_core  a flat C ABI over pjsua-lib
```

## Why the core is a C library

Every feature lives in the core or in the shared YAML configuration,
never in the frontend. There is one frontend -- the same QML tree,
rendered by Qt on Debian and on Android -- so the two platforms are
not two views that have to be kept in step: they are one window with
two screen shapes. The same MOS figure, the same rule engine, the
same device classification, because there is only one of each.

The ABI exposes no PJSIP or SQLite types — a test compiles the public
header with no pjproject include path, so a leak breaks the build.
Handles come from a monotonic counter and are never reused, so a stale
handle resolves to "not found" rather than to whatever call took its
slot. Events are delivered from a single core-owned thread, which is
also what will let Android attach exactly one thread to the JVM.

## What it does

**Signalling**

- Multiple simultaneous registrations, each registerable and
  unregisterable on its own.
- UDP, TCP or TLS per account; SRTP disabled, optional or mandatory.
- Declarative header rewriting from YAML, matched on direction,
  message type, method and account, plus ad-hoc headers attached to a
  single call and named header presets.
- Full SIP trace per call, joined to the call record by Call-ID, with
  credential headers redacted by default.
- Blind and attended transfer, hold, DTMF, call recording.

**Measurement**

- Per-call audio quality from RTCP: packets, loss, discards, reorders,
  jitter and round-trip time as ranges, jitter-buffer delay and
  events, and an estimated MOS.
- Live readout during a call, full detail in a window, and the final
  figures stored with the call history.
- Verified against `tshark` on a real stream: packet counts match
  exactly, jitter agrees to within tens of microseconds.

**Audio**

- Per-direction device selection, with ALSA's device list classified
  down to the entries that are real devices — 64 becomes about 10 on
  an ordinary laptop.
- Independent transmit and receive volume with a mute each.
- Echo cancellation, noise suppression, gain control, silence
  suppression, comfort noise and packet loss concealment, each
  togglable. Defaults suit an instrument: anything that changes what
  you hear is off unless asked for.
- Ringtone, ringback and notification sounds, choosable and
  importable.

## Honest limits

These are the things worth knowing before trusting a number it shows.

- **MOS is an estimate.** The rating-to-MOS conversion is ITU-T
  G.107's. The terms producing that rating from delay and loss are the
  widely used simplification, not G.107's impairment factors, and they
  are generous about heavy loss: five per cent still scores near four.
  Use it to compare calls on the same phone. When it matters, read the
  counters beside it. It is not a certified G.107 score.
- **Round-trip time and every transmit-side figure come from the
  peer's RTCP receiver reports.** A peer that sends none leaves them at
  zero, which is not a measurement of zero. Receive-side figures are
  ours and are always real.
- **The call history stores dialled numbers and caller identities in
  cleartext.** Redaction covers credential headers only. That is
  deliberate — a call history without the numbers is not a call
  history — but in many jurisdictions those records are personal
  information with obligations attached. The database is a plain
  SQLite file under your configuration directory; point it elsewhere
  with `db_file`, or prune it.
- **The Android build is a sideloaded APK signed with the Android
  debug key**, arm64 only. An update installs only over an APK signed
  with the same key, so keep `~/.android/debug.keystore`. Its native
  libraries are aligned for 4 KB pages; a device booting with 16 KB
  pages will refuse it. There is no loudspeaker button: call audio
  plays through the earpiece. Swiping the app out of Recents ends it,
  registration included; Back keeps it running.
- **Hold and recording are verified on a live PBX call; blind
  transfer to an extension was fixed after that and wants the same
  check.** `tests/smoke/live-call-checklist.sh` drives them and wants a
  human listening.
- **STUN and ICE are untested.** No NAT in the test environment.
- **A phone that wants audio enumerates ALSA at startup**, and on a box
  with an unreachable PipeWire that can hang inside `pjsua_init`. See
  `docs/known-issues.md`. `audio_device: none` avoids it entirely and
  is safe.

## Building

Debian 13. pjproject is a pinned submodule and is built out of tree
first.

Developing on something newer is fine, but **a package for other
people has to be built on Debian 13**, not on the machine you happen
to develop on. `dpkg-shlibdeps` records whatever versions it finds
linked, so a package built on forky demands a glibc and a Qt that
Debian 13 cannot satisfy and apt refuses it outright. See
`scripts/build-deb-trixie.sh` below.

```sh
git clone --recurse-submodules git@github.com:Etienne-GN/comlink.git
cd comlink

sudo apt install cmake pkgconf libyaml-dev libssl-dev libasound2-dev \
                 uuid-dev libopus-dev qt6-base-dev qt6-declarative-dev \
                 qt6-shadertools-dev debhelper

./scripts/build-pjproject.sh      # once; installs into third_party/
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build            # 52 tests, 5 of them integration
```

`-fPIC` is mandatory for pjproject and the script sets it: the static
archives are linked into a shared library, and without it the link
fails. pjproject must be resolved with `pkg-config --static` or the
link fails on `fmod`.

### A package

To release one, build it on the target:

```sh
./scripts/build-deb-trixie.sh          # in a Debian 13 container
sudo apt install ./build/deb-trixie/comlink_*.deb
```

`apt install`, not `dpkg -i`: the package has a dozen dependencies and
`dpkg` resolves none of them.

On a Debian 13 machine the container is unnecessary:

```sh
./scripts/build-deb.sh
sudo apt install ./build/deb/comlink_*.deb
```

One package holds the phone, the CLI, the shared library, the C ABI
header and the bundled sounds. That is not the usual Debian library
split: the library has exactly two consumers and both ship here, so
splitting it meant three packages to install and nothing gained.

## Configuration

One YAML file, `~/.config/comlink/comlink.yaml`, and every frontend
reads it -- the window, the CLI and the Android build alike. `packaging/comlink.example.yaml` documents every key with the
reasoning. Unknown keys are a hard error rather than a warning — a
silently ignored typo in a header rule is the failure this project
will not tolerate.

A taste of it:

```yaml
accounts:
  - id: work
    user: "1001"
    domain: pbx.example.lan
    password: secret
    transport: tls
    srtp: mandatory

header_rules:
  - name: tag-outbound
    direction: tx
    message: request
    method: [INVITE]
    account: work
    actions:
      - remove: User-Agent
      - set: {name: X-Site, value: montreal}
```

**One warning about receive-side rules**, verified rather than assumed:
they run *before* pjsip parses the message, so a rule that strips a
header pjsip needs will break calls silently. Removing `Contact` from
an inbound INVITE means the callee never sees the call and nothing
reports an error. See `docs/verification-13-3-rx-ordering.md`.

## The CLI

`comlink-cli` is the same core behind a line protocol: one command per
line on stdin, one line per event on stdout. It is a phone, and it is
also what drives the integration tests.

```sh
comlink-cli --config ~/.config/comlink/comlink.yaml
```

```
call 1 sip:1002@pbx.example.lan X-Ticket: 4471
quality
audio
dsp
levels
history
siplog - 100
```

## Licence

Copyright © 2026 Étienne Gagné. Comlink is free software, licensed
under the GNU General Public License version 3 or (at your option) any
later version. See `LICENSE`.

pjproject is GPLv2-or-later or a commercial licence from Teluu, and it
is linked statically into `libcomlink_core`, so the shipped library
contains pjproject code. Its "or later" clause is what lets the
combined work be distributed under GPLv3+. Whoever distributes a built
package carries GPLv3+ obligations for it, including offering the
source. If you need different terms for a binary, that is a
conversation with Teluu, not with this repository.

Building it yourself for your own use raises none of that. It matters
when you hand the `.deb` to someone else.

The bundled sounds are covered by neither. See
`packaging/sounds/ATTRIBUTION.md`: they were supplied as royalty-free,
the specific terms were not supplied with them, and none is asserted
here. Confirm them before redistributing.

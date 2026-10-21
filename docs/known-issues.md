# Known issues

## The test-suite hang: root cause found

**Status: root cause identified and fixed. Not our deadlock, and not a
deadlock in teardown.**

It is a hang inside `pjsua_init`, in ALSA's pipewire ctl plugin, before
any Comlink code runs.

### The evidence

Caught live: a `dpkg-buildpackage` started the previous
afternoon had been wedged for 15h41m on `test_account`. `gdb` gave the
stack, and the bottom half of it settles
the question:

```
#5  __pthread_clockjoin_ex
#6  pw_thread_loop_stop            (libpipewire-0.3.so.0)
#7  ??                             (libasound_module_ctl_pipewire.so)
#8  _snd_ctl_pipewire_open         (libasound_module_ctl_pipewire.so)
#11 snd_ctl_open                   (libasound.so.2)
#12 snd_hctl_open
#13 snd_mixer_attach
#14 get_mixer_name                 (pjmedia)
#15 alsa_factory_refresh
#16 alsa_factory_init
#17 pjmedia_aud_driver_init
#18 pjmedia_aud_subsys_init
#19 pjmedia_endpt_create
#20 pjsua_media_subsys_init
#21 pjsua_init
#22 sp_core_start                  (core/src/core.c:341)
#23 booted                         (core/tests/test_account.c)
```

`test_call` was then caught in the identical stack minutes later, so
this is two independent reproductions with the same trace.

### Why it happens

`pjmedia`'s ALSA driver labels devices by calling
`snd_mixer_attach(handle, "default")` — cosmetic, just to name them. On
a desktop Debian box `default` is the pipewire ctl plugin. When the
pipewire socket is unreachable, that plugin's failure path calls
`pw_thread_loop_stop()`, which joins a loop thread nothing ever wakes.
The join never returns.

`dpkg-buildpackage` runs the suite without the operator's session, so
`XDG_RUNTIME_DIR` does not point at a live pipewire socket. That is the
missing ingredient, and it is why 100 clean runs in an interactive
session never reproduced it: in a healthy session the plugin connects
and returns normally.

### Reproduced on demand

One attempt, first try:

```
XDG_RUNTIME_DIR=<an empty directory> ./build/core/test_account
```

hangs, in the same stack. First attempt, no concurrency needed. This
is a reproduction rather than a rumour.

It is a race inside the plugin, not a certainty: with the socket
unreachable, some runs hang and some fail cleanly with "Host is down".
That is why a handful of passing runs proves nothing here, and why the
verification below uses ten.

### The fix

`tests/alsa-home/alsa/asoundrc` redirects ALSA's `default` device away
from any sound-server plugin, and every `ctest` test runs with
`XDG_CONFIG_HOME` pointing at `tests/alsa-home`. Wired in
`core/CMakeLists.txt` and the top-level `CMakeLists.txt`.

**There are two paths into that plugin, and fixing only the first is
what made this look fixed for three hours and then hang again:**

1. `pjmedia`'s `get_mixer_name()` calls
   `snd_mixer_attach(handle, "default")`, and `ctl.default` is the
   pipewire plugin on a desktop Debian box.
2. Device enumeration probes named PCMs, so it opens `pcm.pipewire`,
   `pcm.pulse` and `pcm.jack` **directly**, whatever `default` points
   at.

The first override made the suite pass ten runs in a row and cut the
run time to a flat 57s, which read as a fix. It was not: path 2 was
still open, and `test_call` wedged again inside the next
`dpkg-buildpackage`, in the same stack. Every sound-server namespace is
redirected now, not just `default`.

The lesson worth keeping: the plugin's failure is a race, so passing
runs are weak evidence. What settles it is that the plugin is never
loaded — see the verification below.

Three details, each of which cost an attempt:

- **`ALSA_CONFIG_PATH` does not work for this.** The obvious fix — a
  config file naming the overrides, pointed at by `ALSA_CONFIG_PATH` —
  is parsed *before* the load hook at the top of
  `/usr/share/alsa/alsa.conf` runs, so
  `/etc/alsa/conf.d/99-pipewire-default.conf` then overrides it and the
  whole exercise does nothing. That attempt was measured to be inert
  with `amixer -D default info`, which kept reporting the pipewire
  plugin's error. The hook reads `$XDG_CONFIG_HOME/alsa/asoundrc`
  **last**, so that is where an override wins.
- The system configuration must keep loading. With no configuration at
  all, ALSA reports no devices and `pjmedia_aud_subsys_init` fails with
  `PJMEDIA_EAUD_SYSERR`, so `pjsua_init` fails and nothing runs.
  Measured, not assumed.
- `ctl.!default` points at `hw:0`, which may not exist. That is fine:
  `pjmedia` gives up on the mixer label and carries on. A plugin that
  hangs is not fine.

Verified, on the stronger claim rather than the weak one. 15
consecutive `test_account` runs with the pipewire socket unreachable,
while sampling every thread name in the process:

```
runs=15 hangs=0 fails=0 alsa-pipewire-thread-sightings=0
```

The `alsa-pipewire` thread was present in every wedged process. It now
never gets created, so the code path that deadlocks does not run. That
is what makes this a fix rather than a run of luck.

Per-probe, with the socket unreachable:

```
amixer -D default info   -> Card default 'camera' ...   (hw:0, no plugin)
aplay -D pipewire        -> header read error           (null pcm opened)
aplay -D pulse           -> header read error           (null pcm opened)
aplay -D jack            -> header read error           (null pcm opened)
```

Before the second override, `aplay -D pipewire` answered "Host is
down", which is the plugin reporting that it ran and could not reach
the socket — the same call whose failure path joins forever.

Every test also carries `TIMEOUT 300` now. A test that hangs forever
is worse than a test that fails, and this one cost fifteen hours of
wall clock before anyone looked at it.

### The product had the same exposure. Now fixed.

Comlink itself could hang the same way: on a Debian box with an
unreachable or misconfigured pipewire, `sp_core_start` would hang
inside `pjsua_init`. `audio_device: none` did not save it, because
pjsua initialises the audio subsystem before any application setting
is read. A telephony tech's headless box is exactly where that bites.

Fixed in two parts:

1. **`PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO 1`** in
   `scripts/config_site_debian.h`. pjmedia's audio subsystem needs at
   least one device or `pjmedia_aud_subsys_init` fails with
   `PJMEDIA_EAUD_SYSERR` and `pjsua_init` fails outright — measured,
   twice, before this was added. The null device is what keeps the
   subsystem valid with no real driver present.
2. **When the operator asks for no audio, ALSA is pointed at a
   configuration that defines nothing** (`packaging/alsa-none.conf`,
   installed to `/usr/share/comlink/`), by `setenv` before
   `pjsua_create`. The ALSA driver then finds no devices and dlopens no
   plugin, so the hanging code path does not exist in the process.

Deliberate limits on that guard:

- It fires only when the operator asked for no audio — any of
  `audio_device`, `audio_capture_device` or `audio_playback_device` set
  to `none`. A phone that wants audio still enumerates normally, and
  still carries the risk, because there is no way to enumerate ALSA
  devices without consulting ALSA.
- It never overrides an `ALSA_CONFIG_PATH` the operator set
  themselves, and `COMLINK_ALSA_CONF` overrides the shipped path.
- If the shipped file is missing, ALSA finds nothing anyway, which is
  the same outcome the file exists to produce without a console error.
  Verified by running with the file absent.

Evidence that the code path is gone rather than merely less likely,
which matters because the plugin's failure is a race:

```
$ XDG_RUNTIME_DIR=<empty>  amixer -D default info
amixer: Control device default open error: Host is down      <- plugin ran

$ XDG_RUNTIME_DIR=<empty> ALSA_CONFIG_PATH=packaging/alsa-none.conf \
      amixer -D default info
ALSA lib control.c: Invalid CTL default                      <- never resolved
amixer: Control device default open error: No such file or directory
```

"Host is down" is the pipewire plugin reporting that it ran and could
not reach the socket — the same call whose failure path deadlocks.
"Invalid CTL default" is ALSA never resolving the name to any plugin at
all.

---

## Historical: the wrong diagnosis this replaced

Kept because the reasoning was plausible and wrong, and the way it was
wrong is instructive: a two-thread wedge was read as "the worker pool
is gone, so this is teardown", when the second thread was in fact
pipewire's loop thread and the wedge was in startup. The thread count
was real evidence; the inference from it was not.

**Status at the time: open, not reproduced on demand.**

Observed once, during a `dpkg-buildpackage` run whose
`ctest` step ran concurrently with two interactive `comlink-cli`
processes started by hand.

Evidence captured from the live process:

- `/proc/<pid>/stack`: `futex_do_wait` → `__futex_wait` → `futex_wait`
- `State: S (sleeping)`, `Threads: 2` (main + the event dispatch thread)
- **0** open sound-device file descriptors, so this is not audio
  contention despite that being the first suspicion
- normally completes in ~11s; was stuck for ~28 minutes

Six sequential clean runs afterwards all passed, so it needs
concurrency to surface.

Two threads parked on a futex points at the core's own locking rather
than pjsua: candidates are the `sp_evq_stop` join racing a dispatch
thread, or `pjsua_destroy` blocking on a worker while that worker is
inside a core callback. *(Both wrong: see above. The second thread was
not the event dispatch thread, it was `alsa-pipewire`.)* `sp_account_add_locked` also writes
`core->err_detail` without holding `core->lock`, which is a genuine
data race even if it is not this deadlock.

### Reproduction attempts

Built `tests/stress/deadlock-hunt.sh`: runs `test_lifecycle`,
`test_account` and `test_call` concurrently against a backdrop of
starting and stopping CLI instances, with a watchdog that attaches
`gdb` and dumps `thread apply all bt` from any process exceeding its
time budget.

| Build | Iterations | Concurrent test runs | Wedges |
|---|---|---|---|
| Debug (`build/`) | 40 | 120 | **0** |
| Release, `-O2` (`build-rel/`) | 60 | 180 | **0** |

The Release run was the strongest remaining lead, because the original
wedge happened inside `dpkg-buildpackage`, which builds
`-DCMAKE_BUILD_TYPE=Release`, and optimisation is what makes latent
races bite. It did not reproduce either.

**Status at the time: not reproduced, no root cause, therefore no
fix.** No speculative change was made in the name of curing it. That
turned out to be the right call: every candidate fix would have been
aimed at the wrong subsystem.

The stress harness looked for the wrong thing. It varied concurrency,
which was never the trigger. The trigger was a missing session
environment, which every interactive run has and `dpkg-buildpackage`
does not — so running the harness by hand could never have found it,
however many iterations it did.

### What was fixed on its own merits

Three genuine data races found by reading the teardown paths, now
fixed and verified. None is proven to be this deadlock; one of them
plausibly could have been.

- `sp_rules_eval` incremented `rules[i].hits` unsynchronised from
  pjsip worker threads while `sp_rules_stats` read it. Now atomic.
  Confirmed meaningful: with the fix reverted, ThreadSanitizer reports
  the race and the exact-count assertions fail on lost updates, so a
  firing rule could report a wrong hit count.
- `core->err_detail` was written without holding `core->lock` from
  twelve places, and `sp_last_error_detail` handed out a pointer to
  that live buffer. All writes now go through `sp_core_detail`, and
  the reader returns a per-thread snapshot taken under the lock.
- The handle table was enumerated by dense index while another thread
  could swap-remove a row, which can skip an entry or visit one twice.
  Replaced with `sp_ht_snapshot`, which copies every live handle under
  one lock.

`core/tests/test_races.c` covers the last two under ThreadSanitizer
and is clean. It is deliberately free of pjsua so the sanitizer
reports on this code rather than pjproject internals.

### A deadlock introduced and caught while doing that work

Worth recording as a near miss. Migrating the twelve `err_detail`
writes mechanically also rewrote the `snprintf` **inside**
`sp_core_fail`, which already held `core->lock`. The result was a
guaranteed self-deadlock on a non-recursive mutex on the first pjsua
error of any kind. Caught by the test suite immediately.

That is the same shape as the unreproduced bug: a lock taken twice on
one path. It is a reason to keep this issue open rather than assume
the remaining races were it.

### Next step if it recurs

Attach `gdb` and capture `thread apply all bt` while wedged; the
harness does this automatically. Read the whole stack before drawing a
conclusion from the thread count — that is the mistake recorded above.
Note also that attaching `gdb` broke both wedges loose: the
signal interrupts the join and it retries successfully. Convenient, but
it means the evidence has to be captured in the same breath as the
diagnosis.

---

## The MOS figure is an estimate

The rating-to-MOS conversion is the one ITU-T G.107 defines. The terms
that produce the rating from delay and loss are the widely used
simplification, not G.107's impairment factors, and they are generous
about heavy packet loss: five per cent loss still scores near four.

Use it to compare calls on this phone. When it matters, read the
counters beside it. Do not present it as a certified G.107 score.

## Quality figures partly depend on the peer

`rtt` and every `tx_` figure come from the peer's RTCP receiver
reports. A peer that sends none leaves them at zero, which is not a
measurement of zero. The `rx_` figures are ours and are always real.

`rx_packets == 0` means nothing was measured, which is not the same as
a flawless call. Both frontends and the history reader treat it that
way rather than showing a misleading 0.00% loss.

## Silence suppression is off by default

Unlike pjsua. A phone whose job is to measure a stream should keep
sending one, or the jitter and loss figures come from whatever leaked
through during speech. `silence_suppression: true` restores ordinary
softphone behaviour, at the cost of making the measurements much less
useful.

# Live-PBX smoke test

Some behaviour cannot be proven without a PBX, real credentials and
human ears. Hold, resume, recording playback, DTMF reaching the far end
and transfer all fall into that category: an automated test can prove
these functions reject bad arguments, and nothing more.

`tests/smoke/live-call-checklist.sh` drives them in a fixed order and
records a pass or fail for each, so what is unproven becomes a written
result instead of an assumption.

## Running it

```
tests/smoke/live-call-checklist.sh \
    ./build/frontend/cli/comlink-cli \
    ~/.config/comlink/comlink.yaml \
    <target> \
    smoke-result.txt
```

`<target>` should have **unambiguous audio**: hold music, an echo test,
or a speaking clock. Silence proves nothing in either direction, and a
ringing phone nobody answers proves less.

Results land in the output file; the raw CLI event stream lands beside
it with a `.log` suffix. The script exits non-zero if anything failed.

## What it covers

| Step | Automated | Needs your ears |
|---|---|---|
| registration | yes | no |
| call connects | yes | no |
| far-end audio | no | yes |
| hold stops audio | no | yes |
| resume restores audio | no | yes |
| recording writes a non-empty file | yes | no |
| recording plays back audibly | no | yes |
| DTMF reaches the far end | no | yes |
| blind transfer | no | yes |

It runs non-interactively too, which is how its own plumbing is
checked: without a terminal the prompts are recorded as `SKIP` rather
than blocking forever.

## Status of these features

Until this has been run against a real PBX, treat hold, resume,
recording playback and transfer as **implemented but unproven**. The
unit tests cover their argument handling only. Registration, call
setup, media, DTMF, codec negotiation and header rewriting are already
verified — see `docs/verification-phase1.md`.

## A timing detail worth knowing

A REGISTER sent to a host that never answers retransmits for about 32
seconds before pjsip gives up, so the script waits 35 before declaring
failure. Against a registrar that responds, registration resolves in
well under a second. If the script sits waiting, the registrar is
probably unreachable rather than slow.

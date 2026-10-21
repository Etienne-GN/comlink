# Phase 1 verification record

pjproject: 2.17, pinned submodule, built with `-fPIC`
Driver: `tests/integration/loopback.sh` (registered with CTest under
the `integration` label)

## What is proven, and how

Two `comlink-cli` instances place a **direct IP-to-IP call** on
loopback. No registrar is involved and no credentials exist anywhere
in the test, which is what makes this runnable by anyone who clones
the repo.

Acceptance evidence is a `tcpdump` capture read back with `tshark`.
A success return code was not accepted as evidence for anything that
happens on the wire.

All 17 checks pass:

### Call behaviour, from the event stream

- alice placed the call and saw it reach `CONFIRMED`
- bob saw `INCOMING_CALL` and auto-answered 200
- media went active on **both** legs
- alice saw `DISCONNECTED`
- the configured rule fired exactly once (`hits=1`)

### What actually went on the wire

- one INVITE sent
- the **per-call ad-hoc header** `X-Sp-Adhoc: from-alice` is present
- the **rule-injected header** `X-Sp-Rule: applied` is present
- `User-Agent`, removed by rule, is **absent** (0 INVITEs carry it)
- a 200 OK answered the call
- offer and answer **both lead with payload 0 (PCMU)**, which is what
  `codec_priority: [PCMU]` asked for
- a BYE ended the dialog
- 64 RTP packets decoded by tshark; 76 non-SIP UDP packets total

## A defect this test found

The SDP was offering a **text (T.140) stream** the phone does not
implement, because `pjsua_call_setting.txt_cnt` was left at its
default while `aud_cnt` and `vid_cnt` were set explicitly. Fixed by
setting `txt_cnt = 0` on both outgoing calls and answers. The test now
asserts no text stream is offered, so it cannot regress silently.

## Registration and live audio: VERIFIED by the operator

Confirmed by the operator, against a real FreeSWITCH
registrar, using the Qt GUI. This is what the automated loopback test
structurally cannot cover, since it needs a PBX and real credentials.

| Assertion | Result |
|---|---|
| REGISTER against a FreeSWITCH registrar | succeeded |
| Inbound media from a PBX test extension | audio heard |
| Outbound call egressing to the PSTN | connected |
| Bidirectional audio on the PSTN call | confirmed both directions |

No PBX hostnames, dialled numbers or credentials are recorded here,
and none were shared. Phase 1's acceptance criterion is met: the
shared core registers, places and receives calls, and passes audio
both ways against real infrastructure.

Reproducing it is still the operator's step, since it needs their own
credentials:

1. Copy `packaging/comlink.example.yaml`, set `id`, `user`,
   `domain`, `password` and `transport` for a real account, leave
   `register: true`.
2. `comlink --config <your.yaml>`, or the CLI equivalent.
3. Expect the status to read `registered`; the CLI prints
   `EV ACCOUNT_STATE acc=1 registered=1 code=200`.

A failure prints the SIP code and reason in the same line, and
`transport: tls` failing will emit `EV TRANSPORT_ERROR` rather than
quietly falling back to UDP.

Also still open: spec item 13.3, whether rx-path rewriting takes
effect before pjsua's dialog handling consumes the message. Not
observable in a capture of what we sent.

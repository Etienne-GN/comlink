# Spec item 13.3 — rx-path rewrite ordering: VERIFIED

pjproject: 2.17, module registered at `PJSIP_MOD_PRIORITY_TRANSPORT_LAYER`
Harness: `tests/verify/verify-13-3.sh`

## The question

Does a `direction: rx` rule take effect **before** pjsua's own dialog
handling consumes the message, or after? If after, rx rules would be
cosmetic for anything pjsua itself parses, and a user writing one to
influence call handling would be silently disappointed.

## Method

A received-message rewrite is not observable in a capture of what we
sent, so this is a **differential**. The same call is placed to a
callee twice:

- **control** — no rules
- **stripped** — one rx rule removing `Contact` from inbound INVITEs

`Contact` is required to form the dialog. If our rewrite runs first,
the two cases must behave differently.

## Result

| Case | caller CONFIRMED | callee INCOMING_CALL | callee answered |
|---|---|---|---|
| control | 1 | 1 | 1 |
| stripped | **0** | **0** | **0** |

**rx rules run before pjsua consumes the message.** Stripping
`Contact` prevented pjsua from forming the dialog at all: the callee's
`on_incoming_call` never fired, so the application never learned a
call had arrived.

## Consequence, and it cuts both ways

The capable half: an rx rule can influence pjsua's own handling of a
message, not merely decorate it. Rewriting an inbound `To`, `From` or
`Contact` genuinely changes how the call is treated.

The dangerous half, and it is the more important one: **an rx rule that
removes a header pjsua needs will silently destroy calls.** There is no
error, no event, and nothing in a capture of outbound traffic to
explain it — the call simply never arrives. The rule's hit counter will
happily report that it fired.

Anyone writing `direction: rx` rules should be told this. It is now
recorded in `packaging/comlink.example.yaml` next to the rule
syntax, and in `docs/roadmap.md`.

# Verification: audio quality metrics (Plan 7)

**Status: VERIFIED on a real RTP stream.**

Two Comlink instances placed a direct IP-to-IP call over loopback, held
it for 14 seconds, and the reported figures were compared against the
same call as `tshark` measured it independently. No PBX, no registrar,
no credentials. Host addresses are omitted here deliberately.

Reproduce with:

```
./tests/integration/loopback-quality.sh \
    ./build/frontend/cli/comlink-cli ./build ./
```

19 checks, all green.

## What Comlink reported

Mid-call, from `quality`:

```
OK mos 1 4.41
OK r-factor 1 93
OK rx-packets 1 609
OK tx-packets 1 608
OK rx-loss 1 0 0.00
OK rx-jitter-us 1 cur=... min=... max=750 avg=431
```

The final row written to the call history at teardown:

```
OK call-quality 1 mos=4.41 r=93 loss=0.00/0.00
   jitter=431/750 rtt=157/198 pkt=710/708
```

56 `EV CALL_QUALITY` events arrived over the call at a 250 ms sampling
interval.

## What tshark measured, independently

```
Src port  Dest port  Payload  Pkts  Lost      Mean Jitter(ms)  Max Jitter(ms)
4000      4002       g711U    708   0 (0.0%)  0.511            1.183
4002      4000       g711U    710   0 (0.0%)  0.496            0.730
```

RTCP packet types seen, 14 packets: 200 (sender report), 201 (receiver
report), 202 (SDES), 203 (BYE).

SDP media lines: `audio 4000 RTP/AVP 0 3 8 9 96 120 121` and
`audio 4002 RTP/AVP 0 120`. Audio only, which also confirms the T.140
text stream is still not being offered.

## Where the two agree

| Figure | Comlink | tshark |
|---|---|---|
| Packets received | 710 | 710 |
| Packets sent | 708 | 708 |
| Loss | 0 (0.00%) | 0 (0.0%) |
| Mean jitter, inbound | 0.431 ms | 0.496 ms |
| Max jitter, inbound | 0.750 ms | 0.730 ms |

The packet counts match exactly. The jitter figures agree to within a
few tens of microseconds, and they should not be expected to match
exactly: pjmedia computes jitter per RFC 3550 from RTP timestamps as
packets are received, and tshark computes it from capture timestamps at
the interface. They measure the same phenomenon at two different
points.

## What this proves about the design

- **RTP genuinely flowed and the counters are real.** 1432 non-SIP UDP
  packets on the wire, and Comlink's count of them is exact.
- **RTCP was exchanged**, which is what makes `rtt` and every `tx_`
  figure a measurement rather than a zero. Reported rtt: 157 us average,
  198 us peak — plausible for loopback.
- **The final snapshot is taken before the stream dies.** The history
  row shows `pkt=710/708`, while the mid-call sample eleven seconds
  earlier showed 609/608. If the snapshot were taken when
  DISCONNECTED reaches the application, the stream would already be
  gone and the row would read zero. This is the check that would catch
  that regression.
- **The estimate is bounded.** MOS 4.41 on a flawless loopback call is
  the model's ceiling, not a perfect 5.0, which is correct for the
  simplified E-model: see the caveat below.

## The test can fail

Verified by running the same script with `metrics_interval_ms: 0`:

```
FAIL sampling produced a stream of events: got '' want 'yes'
FAIL the events name the call: got '' want 'yes'
```

Two checks go red and the other 17 stay green, which is the correct
behaviour and worth stating: a zero interval turns off the periodic
*events*, not the measurement. `quality` still answers on demand, and
the final figures still reach the history, because that snapshot is
taken from `on_stream_destroyed` rather than from the sampler.

## Caveats, stated plainly

- **MOS here is an estimate.** The rating-to-MOS conversion is the one
  ITU-T G.107 defines. The terms producing that rating from delay and
  loss are the widely used simplification, not G.107's impairment
  factors, and they are generous about heavy loss: five per cent still
  scores near four. Use it to compare calls on this phone. When it
  matters, read the counters beside it. This analysis is not a
  certified G.107 implementation and should not be presented as one.
- **`rtt` and every `tx_` figure come from the peer's RTCP receiver
  reports.** A peer that sends none leaves them at zero, which is not a
  measurement of zero. The `rx_` figures are ours and are always real.
- **`rx_packets == 0` means nothing was measured**, which is a
  different statement from a flawless call. Both frontends and the
  history reader treat it that way.
- Loopback is the easiest possible network. Nothing here says anything
  about how the figures behave on a congested trunk; that needs
  `tests/smoke/live-call-checklist.sh` against a real PBX.

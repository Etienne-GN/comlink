# comlink-cli

A kept test instrument, not scaffolding. Its contract is the line
protocol on stdin/stdout, which is what makes signalling behaviour
scriptable and reproducible without a GUI. It is also the driver for
the loopback integration test.

## Usage

```
comlink-cli --config PATH [--no-audio] [--auto-answer [code]]
```

- `--no-audio` selects a null audio device. RTP still flows, so this
  is how the phone runs headless in tests and CI.
- `--auto-answer [code]` answers every incoming call, default 200.
  This is what lets a second instance act as a peer without shell
  gymnastics.

Exits 1 if the config cannot be loaded or the core cannot start, 2 on
a usage error.

## Commands

One per line on stdin.

| Command | Effect |
|---|---|
| `call <acc> <uri> [Name: value ...]` | place a call, with optional ad-hoc headers |
| `answer <call> [code]` | answer, default 200 |
| `hangup <call> [code]` | hang up, default 603 |
| `hold <call> on\|off` | hold or resume |
| `xfer <call> <uri>` | blind transfer |
| `xfer-att <call-a> <call-b>` | attended transfer |
| `dtmf <call> <digits>` | send DTMF (`0-9`, `*`, `#`, `A-D`) |
| `rec <call> <path>` | start recording to a WAV file |
| `rec-stop <call>` | stop recording |
| `siplog [call-id\|-] [limit]` | list captured SIP messages, newest first |
| `siplog-dump <index>` | print one captured message verbatim |
| `siplog-clear` | empty the SIP log |
| `actions [limit]` | list the action history |
| `actions-clear` | empty the action history |
| `history [limit]` | list past calls, newest first |
| `history-clear` | empty the call history |
| `accounts` | list accounts and registration state |
| `calls` | list active calls |
| `codecs` | list available codecs |
| `rules` | list header rules with hit counts |
| `quit` | stop and exit |

Handles are the numbers the core issues, printed by `call`, by
`EV INCOMING_CALL`, and by `accounts` / `calls`. They are never
reused, so a handle from an ended call reports
`ERR handle not found or retired` rather than acting on a different
call.

## Output

Every command prints one `OK ...` line per result, or
`ERR <status>: <detail>` on failure. Every event prints one `EV ` line.

```
OK ready
OK call 1
EV CALL_STATE call=1 state=CALLING code=0
EV CALL_STATE call=1 state=CONFIRMED code=200
EV CALL_MEDIA_STATE call=1 active=1
```

## Ad-hoc headers

Trailing `Name: value` or `Name:value` tokens on a `call` become
headers on that INVITE only. They are applied **after** the configured
rule set, so they always win. Values may not contain spaces — this is
a test instrument, not a shell.

```
call 1 sip:bob@127.0.0.1:5081 X-Sp-Adhoc: from-alice X-Trace:abc123
```

Verify what actually reached the wire rather than trusting the `OK`:

```
sudo tcpdump -i lo -s0 -w /tmp/c.pcap udp &
# ... place the call ...
tshark -r /tmp/c.pcap -Y 'sip.Method == "INVITE" && sip contains "X-Sp-Adhoc"'
```

## Checking a rule fired

`rules` prints hit counts, so a rule that silently never matched is
visible:

```
OK rule strip-ua hits=1
OK rules total=1
```

A count of 0 is reported, not treated as an error: a rule may
legitimately be waiting for a message type that has not occurred yet.

## Logs

Both logs live in a SQLite database owned by the core, so they survive
restarts and the Android build will read the same schema.

`siplog` lists messages newest first with an index; `siplog-dump
<index>` prints that message verbatim, which is the point of having a
trace at all:

```
siplog - 50
siplog-dump 0
```

Pass a Call-ID instead of `-` to see one dialog:

```
siplog 3f9a1c22-... 100
```

Credential headers are replaced with `<redacted>` unless
`sip_log_redact: false` is set, in which case the core warns on every
start. `redacted=0` in the listing tells you a message was stored
verbatim.

`actions` is the same history the GUI shows, formatted in the core so
both agree.

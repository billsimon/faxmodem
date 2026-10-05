# faxmodem

A command line fax modem. It registers to a SIP trunk with credentials, places
(or answers) an ordinary voice call, and runs a full T.30 fax session over the
G.711 audio stream using [spandsp](https://www.soft-switch.org/). No hardware
modem, no Asterisk, no FreeSWITCH — one binary.

Everything it does is logged to **stdout**, one line per event, so it drops
straight into `docker logs`, journald, or a pipe into `jq` (`--log-json`).

```
2026-09-14T17:06:42.927Z info  [sip] media active on call 0, modem connected (tag=out)
2026-09-14T17:07:26.034Z info  [fax] page complete tag=out pages_tx=2 pages_rx=0 bit_rate=9600 ecm=yes bad_rows=0 frame=MCF
2026-09-14T17:07:28.516Z info  [send] call finished tag=out to="sip:15551234567@sip.example.com" sip_status=200 connected=yes result="OK" pages=2 bit_rate=9600 ecm=yes remote_id="RX-STATION" duration_ms=45599 exit=0
```

## How it works

```
  TIFF-F  ──▶  spandsp T.30  ──▶  16-bit PCM  ──▶  pjmedia port
                                                        │
                                              conference bridge
                                                        │
                                        G.711 RTP  ◀────┴────▶  SIP trunk
                                                                (pjsip: REGISTER, INVITE, digest auth)
```

The fax engine is wired into pjsip as a custom `pjmedia_port`: the conference
bridge pulls 20 ms of modem output from it and pushes 20 ms of the far end back
in, 50 times a second, for the life of the call. Everything that would normally
improve *speech* is turned off, because all of it destroys modem carriers —
VAD, echo cancellation, packet loss concealment, perceptual enhancement, and
any jitter buffer that discards or stretches frames.

The offer is deliberately minimal — one `m=audio` stream carrying G.711 and
nothing else:

```
m=audio 4000 RTP/AVP 0 8
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
```

No video, no T.140 `m=text` stream, and no `telephone-event` payload. pjsua
offers all three by default; faxmodem drops them, because a fax call never
carries DTMF or text and offering a codec you will not use only gives a trunk
something else to negotiate over. Text and video go via the call settings;
telephone-event is compiled into pjmedia, so it is stripped from the SDP in the
`on_call_sdp_created` hook — for answers as well as offers.

**Transport is G.711 audio only (T.30), not T.38.** That works against trunks
that pass audio faithfully and against Asterisk/FreeSWITCH. If your carrier
insists on T.38, this build will not interoperate with it — see
[Limitations](#limitations).

## Build

Dependencies: spandsp, pjproject (pjsip), libtiff, CMake, a C11 compiler.

```sh
# macOS
brew install spandsp pjproject libtiff cmake pkg-config ghostscript

# Debian/Ubuntu (pjproject is not packaged; see the Dockerfile for building it)
sudo apt install libspandsp-dev libtiff-dev libssl-dev cmake pkg-config build-essential

cmake -S . -B build
cmake --build build
./build/faxmodem version
```

Or with Docker, which builds pjproject from source for you:

```sh
docker build -t faxmodem .
docker run --rm faxmodem help
```

### Production build

```sh
scripts/build-release.sh          # -> dist/bin/faxmodem
```

That configures `Release` with warnings as errors, `-O2`, LTO, and the usual
hardening (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, and PIE plus full
RELRO on Linux), strips the binary, then **refuses to stage it until it passes
both the in-memory selftest and the SIP loopback test**. It ends by printing
the SHA-256 and the artefact's runtime library dependencies. `SKIP_LOOPBACK=1`
skips the test that binds ports; `PREFIX=/usr/local` installs straight into
place instead of `dist/`.

Every binary knows what went into it, which is worth having in a production log:

```
$ faxmodem version
faxmodem 0.1.0 (spandsp T.30 over SIP/G.711)
  build      Release, 2026-09-14T17:29:12Z
  compiler   AppleClang 17.0.0.17000604 on Darwin arm64
  spandsp    0.0.6
  pjproject  2.17
  libtiff    4.7.2
  V.17       works (14400 available)
```

Set `SOURCE_DATE_EPOCH` for a reproducible build stamp. A native build links
dynamically against the spandsp, libtiff and OpenSSL on the build host, so it
is not portable to a machine without them — for deployment, build the container
image, which is self-contained apart from a handful of Debian runtime packages.

### V.17 on Apple Silicon

spandsp 0.0.6's `configure` builds it **fixed point** on any host it calls
`arm`, and its old `config.guess` calls an Apple Silicon Mac
`arm-apple-darwin`. Its fixed point V.17 modem does not work: it trains, then
demodulates nothing but noise. So Homebrew's spandsp tops out at 9600 (V.29).
faxmodem tries V.17 once at startup and, when it fails, stops offering it,
warns, and says so in `faxmodem version`. Faxes still go through, just at 9600
rather than 14400, which makes a page take about half as long again.

Linux builds — the container, and Debian's `libspandsp2` on x86-64 or aarch64 —
are floating point and unaffected. To get 14400 on a Mac, build a floating point
spandsp beside Homebrew's and point CMake at it:

```sh
curl -fLO https://deb.debian.org/debian/pool/main/s/spandsp/spandsp_0.0.6+dfsg.orig.tar.xz
tar xf spandsp_0.0.6+dfsg.orig.tar.xz && cd spandsp-0.0.6+dfsg
TIFF=$(brew --prefix libtiff)
./configure --prefix="$HOME/.local/spandsp" CPPFLAGS="-I$TIFF/include" LDFLAGS="-L$TIFF/lib" \
    "ac_cv_fixed_point_machine_$(sh config/config.guess | tr -c 'a-zA-Z0-9\n' _)=no"
grep -q '#undef SPANDSP_USE_FIXED_POINT' src/spandsp.h && echo "floating point: good"
make -C src && make -C src install
mkdir -p "$HOME/.local/spandsp/lib/pkgconfig" && cp spandsp.pc "$HOME/.local/spandsp/lib/pkgconfig/"
cd - && PKG_CONFIG_PATH="$HOME/.local/spandsp/lib/pkgconfig" cmake -S . -B build && cmake --build build
./build/faxmodem version    # V.17       works (14400 available)
```

## Quick start

```sh
export FAXMODEM_PASSWORD='...'          # keep the password out of argv

# is this document sendable?
faxmodem probe invoice.tif

# prove the T.30 engine works without touching the network at all
faxmodem selftest invoice.tif --output-dir /tmp/out

# send
faxmodem send +15551234567 invoice.tif \
    --server sip.example.com \
    --username 1001 \
    --station-id '+15550001111'

# answer inbound calls and write each fax to ./inbox
faxmodem receive --server sip.example.com --username 1001 --output-dir ./inbox
```

## Commands

| Command | What it does |
| --- | --- |
| `send <to> <file.tif>` | Registers, calls, sends one fax, exits with the outcome. |
| `receive` | Registers and answers inbound calls, writing each fax to `--output-dir`. Runs until SIGINT/SIGTERM. |
| `daemon` | Runs the spool queue; add `--serve-inbound` to answer inbound calls in the same process. |
| `enqueue <to> <file.tif>` | Writes a job into the spool for a daemon to pick up. Prints the job path. |
| `selftest <file.tif>` | Loops two T.30 engines back to back in memory. No SIP, no network. |
| `probe <file.tif>` | Reports page count, geometry and compression, and refuses non-bilevel images. |
| `version`, `help` | |

`<to>` is either a number (turned into `sip:<number>@<server>`) or a full SIP
URI (`sip:rx@192.0.2.10:5080`), so you can bypass the registrar entirely.

## Configuration

Three sources, in order of precedence: **command line flags → `FAXMODEM_*`
environment variables → `--config` file → defaults**. Every setting uses the
same name everywhere: the flag `--station-id` is `FAXMODEM_STATION_ID` in the
environment and `station-id = ...` in a config file. `faxmodem help` lists them
all; see [`examples/faxmodem.conf`](examples/faxmodem.conf).

In a config file a `#` after whitespace starts a trailing comment
(`media-timeout = 20  # seconds`); a `#` with no space before it, as in a
password like `abc#123`, is part of the value. A value that must contain ` #`
belongs in the environment or on the command line instead. Spool job files take
every value verbatim, so a `header` like `Acme #42` survives the queue.

The settings that matter most in practice:

| Flag | Default | Notes |
| --- | --- | --- |
| `--server` | — | Registrar/domain, `host[:port]`. |
| `--username` / `--password` | — | Digest credentials; `--auth-user` if the auth user differs. |
| `--register` / `--no-register` | see below | Whether to maintain a SIP registration. |
| `--proxy` | — | Outbound proxy (`sip:edge.example.com;lr`). |
| `--transport` | `udp` | `udp`, `tcp` or `tls`. |
| `--local-port` | ephemeral | SIP port. `--rtp-port` sets the RTP base port. |
| `--rtp-port-range` | `100` | Media stays within `[rtp-port, rtp-port+range]`. Open exactly this range. |
| `--media-timeout` | `20` | Clear the call after this long with no inbound RTP. |
| `--progress-timeout` | `180` | Clear the call after this long with no T.30 frame **and** no image data. |
| `--advance-timeout` | `300` | Clear the call after this long with no page or image-data advance. |
| `--public-addr` | — | Advertise this address when NAT is in the way. `--stun` also works. |
| `--caller-id` | — | Asserted caller ID; adds `P-Asserted-Identity` to outbound INVITEs. |
| `--station-id` | — | Your TSI/CSI, the number the far machine prints. Max 20 chars. |
| `--ecm` / `--no-ecm` | on | Error correction. Turn it off for very poor lines. |
| `--max-speed` | `14400` | Cap the modem: `14400` (V.17), `9600`/`7200` (V.29), `4800` (V.27ter). |
| `--v34` | off | Offer V.34 "Super G3", up to 33600. Falls back to the above with a G3 machine. See [V.34](#v34-super-g3). |
| `--v34-max-rate` | `33600` | Cap V.34's page rate: a multiple of 2400 from 2400 to 33600. |
| `--timeout` | `600` send / `28800` receive | Deadline for a single fax. Sending scales it to the page count; receiving cannot know the page count, so its default is a backstop measured in hours. |
| `--seconds-per-page` | `90` | Per-page budget used for that scaling. |
| `--log-level` | `info` | `error`/`warn`/`info`/`debug`/`trace`. `-v` bumps it. |
| `--log-json` | off | One JSON object per line. |

`--log-level debug` also turns up pjsip (full SIP message traces) and spandsp
(T.30 frame-by-frame) logging; `--pjsip-log-level` and `--spandsp-log-level`
override each independently.

Most T.30 logging happens on the media thread, in the middle of a 20 ms audio
frame, so lines from pjsip's and pjmedia's threads are queued and written by a
logger thread rather than in place: a stdout that stops accepting writes (a log
driver falling behind, a `| jq` that stopped reading) cannot stall the fax
carrier. If the 1 MB queue fills, lines are dropped and a
`N log lines dropped: stdout could not keep up` warning says how many.

### Registration

Registration exists so that somebody can call *you*; it is not what authorises
an outgoing call. So faxmodem only registers when it has to:

| Command | Registers by default |
| --- | --- |
| `send` | **no** — the INVITE is authenticated with the credentials instead |
| `receive` | yes |
| `daemon` | only with `--serve-inbound` |

Sending places the INVITE straight away; when the trunk answers `401` or `407`,
pjsip retries with a digest response built from `--username`/`--auth-user` and
`--password`. Nothing is registered, so nothing has to be torn down afterwards
and a one-shot `faxmodem send` costs exactly one call.

`--register` and `--no-register` override the default either way — use
`--register` for a PBX that refuses calls from unregistered accounts, and
`--no-register` on a `receive` that is fed by a static route or IP trust.

### Deadlines

Three limits protect a call, and they do different jobs:

| Limit | Catches |
| --- | --- |
| `--media-timeout` (20 s) | RTP never arrived, or stopped. A media path fault, not a fax fault. |
| `--progress-timeout` (180 s) | Nothing arriving: no T.30 frame **and** no image data growing. |
| `--advance-timeout` (300 s) | Frames arriving but no page completes and no image data grows — a far end looping on one signal. |
| `--timeout` (600 s send, 28800 s receive) | The backstop. Sending scales it to the page count; receiving keeps hours in hand. |

Both halves of the progress test matter. A non-ECM page sends no T.30 frames at
all between its first row and its last, so a frames-only test cuts off any page
slower than the timeout — and a dense halftone page at 9600 bps runs for nine
minutes. Meanwhile a far end looping on one signal keeps frames flowing while
delivering nothing, which is what `--advance-timeout` is for. Our own
retransmissions never count as progress either: a far end that has stopped
listening still makes us answer it.

Receiving deliberately keeps a long overall deadline. The page count,
resolution and content density are unknown until the fax arrives, a single
image-heavy page can take ten minutes, and the watchdogs above are what
actually clear a dead call — so the deadline should never be the thing that
ends a working transfer.

Because the first two clear a dead call in seconds, the overall deadline is free
to be generous — and it needs to be, since a fixed number is wrong at both ends
of the range. Sending knows the page count before it dials, so the deadline
becomes `60s + pages × --seconds-per-page` whenever that exceeds `--timeout`:

```
[send] deadline scaled to 1860s for 20 pages (tag=out, 60s setup + 90s per page)
```

Setting `--timeout` yourself (flag, environment or config file) disables the
scaling and uses your number exactly. Receiving cannot scale — the page count is
unknown until the fax arrives — so raise `--timeout` on inbound if you expect
long documents.

### Caller ID

`--caller-id` puts a `P-Asserted-Identity` header (RFC 3325) on every outbound
INVITE, which is how most trunks let you present a number other than the
registered account. What you pass through is what you get:

| `--caller-id` | Header sent | From header |
| --- | --- | --- |
| `+15550001111` | `<sip:+15550001111@sip.example.com>` | `"+15550001111" <sip:1001@sip.example.com>` |
| `sip:+15551112222@carrier.net` | `<sip:+15551112222@carrier.net>` | untouched |
| `tel:+15556667777` | `<tel:+15556667777>` | untouched |
| `"Acme Fax" <sip:+15553334444@carrier.net>` | passed through verbatim | untouched |

A bare number doubles as the From display name; anything that is already a URI
or a full name-addr goes only into the header, leaving From as the account
identity. Use `--from` if you need to control the From URI itself.

Note that P-Asserted-Identity is only honoured inside a trusted domain — a
carrier that has not agreed to trust your identity assertions will ignore it
and bill/present the account number instead.

## V.34 (Super G3)

`--v34` offers V.34 half duplex fax (T.30 Annex F, "Super G3"): pages at up to
33600 bit/s instead of V.17's 14400, so a page takes a third to half as long.
It is off by default. With it on:

- **Answering**, faxmodem sends ANSam in place of CED. A Super G3 caller
  answers with V.8 and the call runs on V.34; a G3 caller ignores ANSam and
  the call carries on as G3, exactly as without `--v34`.
- **Calling**, faxmodem listens for ANSam alongside its usual CNG and V.21
  receiver. A Super G3 answerer's ANSam leads to V.8 and V.34; a G3
  answerer's DIS settles it as G3.
- Under V.34 there is no TCF and **ECM is always on**, whatever `--ecm` says.
  T.30's frames go over V.34's 1200 bit/s control channel, and pages over the
  primary channel at the rate line probing and training picked, capped by
  `--v34-max-rate`. `--max-speed` only matters when the call falls back to G3.

The V.34 modem is faxmodem's own (`src/v34hdx.c`, `src/v34_*.c`), so unlike
V.17 it is unaffected by a fixed point spandsp and gives 33600 on Apple Silicon
too. T.30 is spandsp's, with the Annex F changes, vendored in
`third_party/spandsp-t30/` (its README lists them); it overrides the
installed library's copy, which must be spandsp 0.0.6.

The log says which way a call went. `v34=yes` and the page rate appear in the
`page complete`, `transfer finished`, `call finished` and `fax received` lines
and in spool `.result` files, and `--log-level debug` traces V.8, the
start-up phases, line probing, the rate chosen and each control and primary
channel turnaround under `[v34]`:

```
info  [fax] page complete tag=out pages_tx=1 pages_rx=0 bit_rate=33600 v34=yes ecm=yes bad_rows=0 frame=MCF
```

To try it against a real Super G3 machine:

```sh
# send to it
faxmodem send 15551234567 doc.tif --v34 --log-level debug
# have it call us
faxmodem receive --v34 --output-dir ./inbox --log-level debug
# a poor line: cap the rate rather than let it fail
faxmodem send 15551234567 doc.tif --v34 --v34-max-rate 24000
```

What is not implemented yet, and what it means in practice:

- **Retrains.** Neither channel retrains mid-call (V.34 12.7, and the control
  channel's response to AC). Each page's primary channel resynchronises
  from scratch, so drift and level changes between pages are handled, but a
  line that changes sharply within a page takes ECM retransmissions rather
  than a retrain, and the rate chosen at start-up holds for the call. If a
  line stalls, `--v34-max-rate` lower is the lever.
- **The answerer as the source** (12.2.2): polling, or an answerer with
  pages of its own to send at start-up. faxmodem declines V.34 in that case
  and the call runs as G3.
- **The 2400 bit/s control channel**, only used with asymmetric rates, which
  faxmodem never offers.
- V.34 has been tested modem-to-modem and over SIP/RTP between two faxmodems,
  not yet against another vendor's implementation.

## The spool daemon

```
<spool-dir>/queue/     jobs waiting
<spool-dir>/active/    the job being sent right now (claimed by rename)
<spool-dir>/done/      sent, plus <job>.result
<spool-dir>/failed/    out of attempts, plus <job>.result
<spool-dir>/tmp/       staging, so a job appears in queue/ only once complete
```

```sh
faxmodem daemon --config /etc/faxmodem.conf --serve-inbound
faxmodem enqueue +15551234567 invoice.tif --ref invoice-1043 --spool-dir /var/spool/faxmodem
```

A job is a key=value file accepting any faxmodem option, so a job can override
the daemon's station ID, header, ECM setting and so on for one fax — see
[`examples/example.job`](examples/example.job). Jobs are claimed with
`rename()`, which is atomic, so several daemons can share one spool. Failures
are retried `--max-attempts` times with a `--retry-backoff` delay that grows
with each attempt; a job left in `active/` by a crash is requeued at startup.
Each finished job gets a `.result` file:

```
status=sent
exit-code=0
sip-status=200
t30-result=0
t30-text=OK
pages=2
bit-rate=9600
ecm=yes
v34=no
remote-id=RX-STATION
duration-ms=45616
```

One call at a time: while a fax is in progress, inbound calls are answered with
486 Busy Here and the queue waits. Run several daemons (different SIP ports and
RTP ranges) if you need concurrency.

[`examples/faxmodem.service`](examples/faxmodem.service) is a systemd unit.

## Documents

Input must be a **bilevel (1 bit) TIFF**, ideally 1728 px wide at 204x196 dpi —
a TIFF-F fax page. `probe` tells you when it is not, and `send` refuses rather
than failing mid-call. Ghostscript converts anything else:

```sh
gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=tiffg4 -r204x196 -g1728x2156 \
   -sOutputFile=invoice.tif invoice.pdf
```

`scripts/make-test-page.sh out.tif 3` generates a multi-page test document.

## Testing without a trunk

`selftest` runs the whole T.30 conversation between two engines in memory in a
fraction of a second — it proves spandsp, your TIFF and the build, and never
touches the network.

`FAXMODEM_SELFTEST_LINE` puts a worse line between the two engines, for what a
perfect one cannot exercise: `delay=150` (ms, each way), `echo=-20` (each
end's own signal returned that many dB down, a round trip later, as a far-end
hybrid does), `noise=-50` (white noise, dBm0), and `ulaw` or `alaw` (G.711).
It is a test hook, not an option:

```sh
FAXMODEM_SELFTEST_LINE="delay=150,echo=-20,ulaw" faxmodem selftest invoice.tif --output-dir /tmp/out
```

`--v34` works with `selftest` too, alone or with a `FAXMODEM_SELFTEST_LINE`
(the `transfer finished` line should say `v34=yes`). The V.34 modem has
its own tests, built beside faxmodem and run by `ctest --test-dir build`:
`v34test` covers the coding, INFO and control channel layers, and
`v34hdxtest` runs two half duplex modems through V.8, start-up and pages
over a simulated line - `v34hdxtest [delay loss noise cap pages ppm seed]`
for one line, `v34hdxtest soak 100` for a hundred random ones (delay, loss,
noise, clock drift, G.711, echo).

`scripts/loopback-test.sh` goes further: it starts a receiver on 127.0.0.1,
sends it a fax from a second process over real SIP and real RTP, and compares
page counts. That exercises everything except your carrier.

```sh
cmake --build build && scripts/loopback-test.sh
V34=1 scripts/loopback-test.sh          # both ends with --v34; fails unless it ran as V.34
```

`DOC=file.tif` sends that document instead of building one with ghostscript,
and `RX_PORT`, `TX_PORT`, `RX_RTP` and `TX_RTP` move it off ports in use.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Fax transferred (T.30 finished with OK). |
| 2 | Bad command line. |
| 3 | Bad configuration, missing credentials, or an unusable document. |
| 4 | SIP transport or registration failure. |
| 5 | Call failed: rejected, busy, or never answered. |
| 6 | Call connected but T.30 failed. The log carries the spandsp reason. |
| 7 | `--timeout` hit. |
| 8 | Internal error. |

## Troubleshooting

**Registration fails (exit 4).** Run with `--log-level debug` to see the full
REGISTER/401 exchange. A 403 usually means `--auth-user` differs from
`--username`, or the trunk wants a specific `--realm`.

**Call connects but no fax (exit 6, "no answer from the remote fax").** Almost
always media: check the log for `media active`. If media never came up, the far
end may have offered only codecs we reject — faxmodem deliberately offers G.711
alone. If media is up but training never completes, the path is probably
transcoding or the carrier expects T.38.

**`no RTP from the far end for 20s`.** The call is up but nothing is arriving.
The `media active` line names the local RTP address and port actually in use —
check that the far end can reach it. The usual cause is a firewall or SBC that
permits a narrower range than the one media is negotiating: pin it with
`--rtp-port 4000 --rtp-port-range 100` and open exactly that. Behind NAT, the
address in the SDP must be the public one, so set `--public-addr` or `--stun`.
The watchdog clears the channel rather than holding it for the full `--timeout`,
so a redialling sender gets answered instead of a busy signal.

**Every call runs at 9600, never 14400.** Check `faxmodem version`: if V.17
"does not work in this spandsp build", see [V.17 on Apple Silicon](#v17-on-apple-silicon).

**Training keeps retraining down.** Try `--max-speed 9600`, then `4800`. Packet
loss, transcoding and one-way jitter all look like a bad phone line to a modem.

**Pages arrive corrupted.** Keep ECM on; with ECM off any lost packet becomes
speckle. `bad_rows` in the per-page log line tells you how bad the line is.

**A very noisy line never gets going.** spandsp's receivers treat anything
above -45.5 dBm0 as a carrier, so line noise near that level (it takes an
unusually bad analog leg; VoIP paths sit far below it) looks like a carrier
that never drops, and T.30 waits for the end of a frame that never comes.
Transfers become unreliable from about -50 dBm0 of noise and stop entirely at
-45. `--advance-timeout` clears such a call; there is no setting that rescues
it.

**Long-delay calls and echo.** On a call into the telephone network the far
end's line card can return our own signal a round trip later. faxmodem ignores
a received frame identical to one it sent in the last few seconds, so this no
longer ends calls; `own_echoes=` in the `transfer finished` line counts them.

**Nothing in the inbox.** `receive` writes only when a call actually completes
T.30; a partial transfer is logged as such and the TIFF flagged as incomplete.

## Limitations

- **No T.38.** Audio-path T.30 only. Carriers that force T.38 re-INVITE will
  not interoperate. Adding it means handling the re-INVITE and UDPTL against
  spandsp's `t38_terminal`; the engine boundary in `src/fax.c` is where that
  would plug in.
- **One call at a time per process.** Deliberate: two modems in one conference
  bridge share the same clock and tend to interfere.
- **No PDF conversion.** Use ghostscript, as above.
- **Sending is one document per call**, no polling, no subaddressing.
- **V.34 without retrains**, and never with the answerer sending; see
  [V.34](#v34-super-g3).

## Layout

```
src/main.c        command dispatch, signals
src/config.c      flags, environment, config files (one option table drives all three)
src/log.c         stdout logging, queued off the media thread; pjsip and spandsp are routed through it
src/fax.c         spandsp T.30 engine, phase B/D/E handlers, in-memory selftest
src/v34hdx.c      half duplex V.34 modem: V.8, start-up, control and primary channels
src/v34_cc.c      V.34's control channel modem
src/v34_*.c       V.34 coding, DSP and INFO/MP messages
third_party/spandsp-t30/  spandsp's T.30 and fax front end with T.30 Annex F
src/sip.c         pjsua setup, registration, calls, and the fax pjmedia port
src/spool.c       the queue: claim, send, retry, result files
src/tiff_probe.c  pre-flight document checks
```

# spandsp T.30 and fax front end, vendored

`t30.c`, `t30_api.c`, `t30_logging.c` and `fax.c` from spandsp 0.0.6 (Debian's
`spandsp_0.0.6+dfsg.orig.tar.xz`, the same code as the upstream tarball
Homebrew builds), compiled into faxmodem so that they **override the copies in
the installed libspandsp**. Everything else - the modems, HDLC, T.4 - still
comes from the library.

They are here because V.34 fax ("Super G3", T.30 Annex F) needs changes in
both: spandsp has no V.34 in its T.30 state machine, and its fax front end has
no way to drive a half-duplex V.34 modem. The `.orig` files are pristine;
`diff -u t30.c.orig t30.c` and `diff -u fax.c.orig fax.c` are the whole
change. Licence: LGPL 2.1, see `COPYING.LGPL`.

## Building against the installed library

The vendored files see spandsp's structures through the installed headers, so
the installed library must be 0.0.6, and the files must be compiled the way it
was. They include spandsp's headers one at a time, never `<spandsp.h>` - the
only header that says whether the library was built fixed point, which changes
the layout of the modem states inside `fax_state_t`. `CMakeLists.txt` reads
`SPANDSP_USE_FIXED_POINT` out of the installed `<spandsp.h>` and passes it on;
without that, against Homebrew's fixed point build, every fax failed.

`config.h` only declares the single-precision maths functions, so that
`floating_fudge.h` does not define shims that collide with `<math.h>`.

## The patches

All of them are for V.34 (T.30 Annex F), and all are inert unless V.34 is
offered with `fax_v34_enable()` (`include/faxmodem/fax_v34.h`): without it,
faxmodem behaves exactly as it did with the library's copies.

### fax.c - the front end

- `fax_init(NULL, ...)` allocates a wrapper with `fax_state_t` first and the
  V.34 state after it, since the installed library's `fax_state_t` cannot
  grow. `t30.c` reaches it through two hooks, `fm_t30_v8_capable()` and
  `fm_t30_v34_active()`.
- Phase A: the answerer's CED becomes the V.34 modem's ANSam and V.8; the
  caller's CNG and V.21 receiver keep running while the modem listens for
  ANSam, so a G3 answerer's DIS still settles it. If V.8 does not settle on
  V.34, everything carries on as G3.
- Under V.34, T.30's modem requests map onto the modem's channels: V.21 is
  the control channel, any fast modem with HDLC is the primary channel, and
  "send step complete" comes from the HDLC transmitter running dry, as it
  does for the G3 modems. The control channel always takes its bits from
  the HDLC transmitter, which idles on flags: switching between a flag
  generator of our own and it broke a flag where they met, and the far
  receiver, counting flags afresh, missed the frame behind it.
- Transmit requests under V.34 are acted on before spandsp's "same type as
  before" shortcut: a request swallowed by it once left T.30's frames
  unsent.

### t30.c - the state machine

- DIS carries the V.8 capability bit when V.34 is offered.
- No TCF: the sender waits for CFR straight after DCS; the receiver answers
  DCS with CFR at once. ECM is mandatory, and forced.
- DCS's data signalling rate bits are sent as zero under V.34.
- `queue_phase()` turns round at once under V.34. It waits for the far
  end's carrier to drop, which on V.21 it does after every frame; the V.34
  control channel never drops - it idles on flags - so T.30 waited for ever
  to send its DCS.
- HDLC flags do not start T2A or T4A under V.34: on the control channel
  flags say nothing about a frame being on its way, and a far end slow to
  answer would have been cut off after three seconds.

# spandsp, vendored in part

`t30.c`, `t30_api.c`, `t30_logging.c`, `fax.c` and `v17rx.c` from spandsp
0.0.6 (Debian's `spandsp_0.0.6+dfsg.orig.tar.xz`, the same code as the
upstream tarball Homebrew builds), compiled into faxmodem so that they
**override the copies in the installed libspandsp**. Everything else - the
other modems, HDLC, T.4 - still comes from the library.

- T.30 and the fax front end are here because V.34 fax ("Super G3", T.30
  Annex F) needs changes in both: spandsp has no V.34 in its T.30 state
  machine, and its fax front end has no way to drive a half-duplex V.34
  modem.
- The V.17 receiver is here because it does not work in a fixed point build
  of spandsp, which is what Homebrew installs on Apple Silicon, and because
  its short training depends on undefined behaviour in every build.

The `.orig` files are pristine; `diff -u t30.c.orig t30.c`, `diff -u fax.c.orig
fax.c` and `diff -u v17rx.c.orig v17rx.c` are the whole change (`t30_api.c` is
unchanged, and only compiled here because `t30.c` needs it to match).
`v17_v32bis_rx_floating_rrc.h` is generated when spandsp is built, by its
`make_modem_filter -m V.17 -r`; the two constellation map headers are as
shipped. Licence: LGPL 2.1, see `COPYING.LGPL`.

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

### v17rx.c - the V.17 receiver

spandsp 0.0.6 disabled the fixed point version of the V.17 receiver: the
file, and its state structure in `spandsp/private/v17rx.h`, test
`SPANDSP_USE_FIXED_POINTx`, which nothing defines, so both are floating
point in every build. Six `#if`s were missed and still test
`SPANDSP_USE_FIXED_POINT`. In a fixed point build those run the input
pulse-shaping filter with `vec_circular_dot_prodi16()` over what are really
float arrays (the filter buffer and coefficients), clear the float buffer
with `vec_zeroi16()` (half of it), and advance the carrier with the integer
`dds_advance()`. The receiver trains, then decodes noise at every rate.

The first fix is to make those six test `SPANDSP_USE_FIXED_POINTx` too. Since the
structure never depended on `SPANDSP_USE_FIXED_POINT`, the corrected file
fits the installed library's layout whichever way it was built, and in a
floating point build it is the library's own code. The library's fax front
end calls `v17_rx_init()` once from inside the library, which binds to the
library's copy, but our `fax.c` restarts the receiver with our
`v17_rx_restart()` before every use, so all receiving runs through the
fixed code.

With that alone, the in-memory selftest passed in a debug build and failed in
a release one: TCF's long training worked, every page's short training
failed. The second fix is for undefined behaviour that affects floating
point builds too. Carrier phases are `int32_t` DDS angles, and the receiver
subtracts them directly, as in the short training's choice of whether it
has just seen A or B:

    if ((uint32_t) (angle - s->start_angles[0]) < (uint32_t) DDS_PHASE(180.0f))

When the two angles lie either side of 180 degrees the subtraction
overflows. The author meant it to wrap, and at `-O0` it does. clang at `-O2`
may assume it cannot overflow and reduce the test to
`angle >= start_angles[0]`. The receiver then takes A for B, latches the
start of the scrambled segment one symbol out, and fails the short training.
Whether the angles straddle 180 degrees depends on the far transmitter, the
line and the level, not on timing: spandsp's receiver does not advance its
carrier while the line is silent. So on an unlucky line every page fails.
The stock floating point receiver failed 20 of 260 test cases this way (one
level, every carrier offset tried); the fixed one failed none.

All eight such subtractions go through `phase_diff()`, which subtracts as
`uint32_t`. The vendored files are also compiled with `-fwrapv`, since
spandsp assumes signed arithmetic wraps throughout. Under
`-fsanitize=signed-integer-overflow` the original reports overflows on every
run of the test harness and the fixed file on none.

Checked against a floating point spandsp: the fixed receiver decodes exactly
what the floating point one does, bit for bit, at every rate and through
noise, and the fixed point transmitter's output decodes without error on the
floating point receiver.

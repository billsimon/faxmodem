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

None yet: this commit vendors the files unchanged, and faxmodem behaves
exactly as it did with the library's copies.

#!/usr/bin/env bash
# Produces a release build in dist/ and refuses to ship it untested:
# it compiles with warnings as errors, strips the binary, then runs the
# in-memory T.30 selftest and the SIP loopback test against the artefact.
#
#   scripts/build-release.sh            # build, verify, stage into dist/
#   SKIP_LOOPBACK=1 scripts/build-release.sh   # skip the test that binds ports
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build-release}
PREFIX=${PREFIX:-dist}
JOBS=${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

echo "==> configuring Release in $BUILD_DIR"
rm -rf "$BUILD_DIR" "$PREFIX"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFAXMODEM_HARDENING=ON \
    -DFAXMODEM_LTO=ON \
    -DFAXMODEM_WERROR=ON

echo "==> building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j "$JOBS"

BIN="$BUILD_DIR/faxmodem"
echo "==> stripping"
if [ "$(uname -s)" = "Darwin" ]; then
    strip -x "$BIN"
else
    strip --strip-unneeded "$BIN"
fi

echo "==> verifying the artefact"
"$BIN" version
WORK=$(mktemp -d -t faxmodem-release)
trap 'rm -rf "$WORK"' EXIT
scripts/make-test-page.sh "$WORK/page.tif" 2 >/dev/null
"$BIN" probe "$WORK/page.tif" >/dev/null
"$BIN" selftest "$WORK/page.tif" --output-dir "$WORK/out" --log-level warn
echo "    selftest: T.30 engine OK"
"$BIN" selftest "$WORK/page.tif" --output-dir "$WORK/out34" --v34 --log-level info >"$WORK/selftest-v34.log" 2>&1 &&
    grep -q "transfer finished.* v34=yes" "$WORK/selftest-v34.log" || {
    echo "    V.34 selftest FAILED - see $WORK/selftest-v34.log"
    tail -20 "$WORK/selftest-v34.log"
    exit 1
}
echo "    selftest: V.34 OK"

if [ "${SKIP_LOOPBACK:-0}" != "1" ]; then
    scripts/loopback-test.sh "$BIN" >"$WORK/loopback.log" 2>&1 || {
        echo "    loopback test FAILED - see $WORK/loopback.log"
        tail -30 "$WORK/loopback.log"
        exit 1
    }
    echo "    loopback: SIP + RTP + T.30 OK"
fi

echo "==> staging into $PREFIX"
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null
cp README.md "$PREFIX/share/faxmodem/"

echo "==> artefact"
ls -l "$PREFIX/bin/faxmodem"
if command -v shasum >/dev/null; then
    shasum -a 256 "$PREFIX/bin/faxmodem" | tee "$PREFIX/bin/faxmodem.sha256"
fi
echo
echo "runtime dependencies:"
if [ "$(uname -s)" = "Darwin" ]; then
    otool -L "$PREFIX/bin/faxmodem" | tail -n +2 | sed 's/^/  /'
else
    ldd "$PREFIX/bin/faxmodem" | sed 's/^/  /'
fi
echo
echo "PASS - $PREFIX/bin/faxmodem is built, stripped and verified"

#!/usr/bin/env sh
# Builds a fax-ready TIFF-F test page (204x196 dpi, G4, 1728 px wide).
# Usage: scripts/make-test-page.sh [output.tif] [pages]
set -eu

OUT=${1:-testpage.tif}
PAGES=${2:-1}

command -v gs >/dev/null 2>&1 || {
    echo "ghostscript (gs) is required" >&2
    exit 1
}

PS=$(mktemp -t faxmodem-testpage.XXXXXX)
trap 'rm -f "$PS"' EXIT

i=1
while [ "$i" -le "$PAGES" ]; do
    cat >>"$PS" <<EOF
/Helvetica findfont 36 scalefont setfont
72 700 moveto (faxmodem test page $i of $PAGES) show
/Helvetica findfont 18 scalefont setfont
72 660 moveto (T.30 over SIP, spandsp) show
72 630 moveto (generated $(date -u +%Y-%m-%dT%H:%M:%SZ)) show
2 setlinewidth
72 100 moveto 468 0 rlineto 0 500 rlineto -468 0 rlineto closepath stroke
100 150 moveto
0 1 40 {
    pop
    10 0 rlineto
    0 20 rlineto
    0 -20 rmoveto
} for
stroke
showpage
EOF
    i=$((i + 1))
done

gs -q -dNOPAUSE -dBATCH -dSAFER \
    -sDEVICE=tiffg4 \
    -r204x196 \
    -g1728x2156 \
    -sOutputFile="$OUT" \
    "$PS"

echo "$OUT"

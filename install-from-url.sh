#!/bin/sh
# Download Yasem portable tarball and install on this CoreELEC box.
# Usage:
#   sh install-from-url.sh
#   sh install-from-url.sh http://www.example.com/mag/yasem.tar
#   YASEM_SKIP_CLEANUP=1 sh install-from-url.sh
set -eu

URL="${1:-http://www.example.com/mag/yasem.tar}"
WORKDIR="${YASEM_INSTALL_TMP:-/tmp/yasem-dl-$$}"

echo "Yasem install-from-url"
echo "  URL: $URL"
echo "  tmp: $WORKDIR"

rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

fetch() {
    if command -v wget >/dev/null 2>&1; then
        wget -O yasem.tar "$URL"
    elif command -v curl >/dev/null 2>&1; then
        curl -fL -o yasem.tar "$URL"
    else
        echo "ERROR: need wget or curl" >&2
        exit 1
    fi
}

fetch
if [ ! -s yasem.tar ]; then
    echo "ERROR: empty download" >&2
    exit 1
fi

tar xf yasem.tar

INSTALL=""
for candidate in \
    "$WORKDIR/yasem/install.sh" \
    "$WORKDIR/yasem-portable/install.sh" \
    "$WORKDIR/install.sh"
do
    if [ -x "$candidate" ] || [ -f "$candidate" ]; then
        INSTALL="$candidate"
        break
    fi
done

if [ -z "$INSTALL" ]; then
    echo "ERROR: install.sh not found in archive. Top level:" >&2
    ls -la "$WORKDIR" >&2
    exit 1
fi

echo "Running: sh $INSTALL"
sh "$INSTALL"

echo
echo "Download dir left at: $WORKDIR (safe to rm -rf)"
echo "Reboot recommended: reboot"

#!/bin/sh
# Install portable Yasem into /storage/yasem on this CoreELEC box.
# Usage (on the box):
#   sh /path/to/yasem/install.sh
# Or from USB/NFS after copying the folder:
#   sh ./install.sh
set -eu
SRC="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DST=/storage/yasem

echo "Installing Yasem from $SRC -> $DST"

if [ ! -x "$SRC/yasem" ]; then
    echo "ERROR: missing $SRC/yasem binary" >&2
    exit 1
fi
if [ ! -d "$SRC/lib" ] || [ ! -d "$SRC/plugins" ]; then
    echo "ERROR: incomplete package (need lib/ and plugins/)" >&2
    exit 1
fi

mkdir -p "$DST"
# Keep existing machine config if present; refresh binary/runtime.
for item in yasem start.sh run-instead-of-kodi.sh set-1080i50.sh install.sh \
            install-from-url.sh \
            enable-yasem-boot.sh cleanup-kodi-addons.sh yasem.service yasem.timer \
            yasem-standby.service standby-watch.sh led-status.sh \
            qt.conf lib plugins fonts launch.sh README.txt PROMPT.txt INSTALL.txt; do
    if [ -e "$SRC/$item" ]; then
        rm -rf "$DST/$item"
        cp -a "$SRC/$item" "$DST/$item"
    fi
done

mkdir -p "$DST/config"
if [ -f "$SRC/config/config.json.example" ] && [ ! -f "$DST/config/config.json" ]; then
    cp "$SRC/config/config.json.example" "$DST/config/config.json"
fi
# Install default remote map once; never overwrite a user-edited remote.conf
if [ -f "$SRC/config/remote.conf" ]; then
    cp -a "$SRC/config/remote.conf" "$DST/config/remote.conf.example"
    if [ ! -f "$DST/config/remote.conf" ]; then
        cp -a "$SRC/config/remote.conf" "$DST/config/remote.conf"
    fi
fi

chmod 755 "$DST/yasem" \
    "$DST/start.sh" \
    "$DST/run-instead-of-kodi.sh" \
    "$DST/set-1080i50.sh" \
    "$DST/install.sh" \
    "$DST/enable-yasem-boot.sh" \
    "$DST/cleanup-kodi-addons.sh" \
    "$DST/standby-watch.sh" \
    "$DST/led-status.sh" 2>/dev/null || true
[ -f "$DST/launch.sh" ] && chmod 755 "$DST/launch.sh"
[ -f "$DST/yasem-standby.service" ] && chmod 644 "$DST/yasem-standby.service"

# Force and persist 1080i50
"$DST/set-1080i50.sh" || true

# Remove other Kodi addons / aytos helpers, boot Yasem only
if [ "${YASEM_SKIP_CLEANUP:-0}" != "1" ]; then
    "$DST/cleanup-kodi-addons.sh" || true
fi
"$DST/enable-yasem-boot.sh" || true

ln -sfn "$DST/start.sh" /storage/yasem-start.sh 2>/dev/null || true

echo
echo "OK. Yasem-only mode."
echo "  boot: yasem.service (Kodi masked)"
echo "  config: $DST/config/config.json"
echo "  remote: $DST/config/remote.conf"
echo "  reboot recommended"

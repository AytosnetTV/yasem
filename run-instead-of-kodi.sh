#!/bin/sh
# Legacy launcher name — on Yasem-only boxes prefer: systemctl start yasem
# Stops Kodi if running and starts Yasem in foreground (used by yasem.service via start.sh).
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
rm -f /tmp/yasem.quit
if [ -x "$ROOT/set-1080i50.sh" ]; then
    "$ROOT/set-1080i50.sh" || true
fi
systemctl stop kodi.service >/dev/null 2>&1 || true
killall kodi.bin kodi.sh >/dev/null 2>&1 || true
sleep 1
# Do NOT restart Kodi on exit — Yasem is the only UI (systemd Restart=always).
exec "$ROOT/start.sh"

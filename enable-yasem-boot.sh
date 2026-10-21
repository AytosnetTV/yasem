#!/bin/sh
# Make Yasem the only UI on boot. Disables Kodi autostart.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
DST=/storage/yasem
CFG=/storage/.config
SD="$CFG/system.d"

if [ ! -x "$DST/yasem" ] && [ -x "$ROOT/yasem" ]; then
    DST="$ROOT"
fi
if [ ! -x "$DST/yasem" ]; then
    echo "ERROR: Yasem binary not found in $DST" >&2
    exit 1
fi

mkdir -p "$CFG" "$SD" "$DST/config" "$SD/multi-user.target.wants" "$SD/timers.target.wants"

# Display mode
if [ -x "$DST/set-1080i50.sh" ]; then
    "$DST/set-1080i50.sh" || true
elif [ -x "$CFG/yasem-1080i50.sh" ]; then
    "$CFG/yasem-1080i50.sh" || true
fi

# Minimal autostart (no aytos update.sh)
cat > "$CFG/autostart.sh" <<'EOF'
#!/bin/sh
[ -x /storage/.config/yasem-1080i50.sh ] && /storage/.config/yasem-1080i50.sh &
# Do not start Yasem while soft-standby watcher is waiting for Power
[ -f /storage/yasem/.standby ] && exit 0
if systemctl is-active yasem-standby.service >/dev/null 2>&1; then
    exit 0
fi
# Failsafe if systemd pull-in missed Yasem
if ! pidof yasem >/dev/null 2>&1; then
    systemctl start yasem.service >/dev/null 2>&1 || /storage/yasem/start.sh >/dev/null 2>&1 &
fi
EOF
chmod 755 "$CFG/autostart.sh"

# Install unit files (must NOT be executable — systemd warns and some CE builds skip)
cp -a "$DST/yasem.service" "$SD/yasem.service" 2>/dev/null || cp -a "$ROOT/yasem.service" "$SD/yasem.service"
cp -a "$DST/yasem.timer" "$SD/yasem.timer" 2>/dev/null || cp -a "$ROOT/yasem.timer" "$SD/yasem.timer"
cp -a "$DST/yasem-standby.service" "$SD/yasem-standby.service" 2>/dev/null \
    || cp -a "$ROOT/yasem-standby.service" "$SD/yasem-standby.service"
chmod 644 "$SD/yasem.service" "$SD/yasem.timer" "$SD/yasem-standby.service"

# Remove broken remote-config coupling (ordering cycle with multi-user)
rm -rf "$SD/remote-config.service.d" 2>/dev/null || true
rm -f "$SD/multi-user.target.wants/remote-config.service" 2>/dev/null || true

# Disable aytos leftovers
systemctl disable aytos-epg.timer 2>/dev/null || true
systemctl stop aytos-epg.timer 2>/dev/null || true
rm -f "$SD/timers.target.wants/aytos-epg.timer" 2>/dev/null || true

# Stop and mask Kodi
systemctl stop kodi.service 2>/dev/null || true
killall kodi.bin kodi.sh 2>/dev/null || true
systemctl disable kodi.service kodi.target 2>/dev/null || true
systemctl mask kodi.service 2>/dev/null || true

# Enable Yasem + boot timer failsafe
ln -sfn "$SD/yasem.service" "$SD/multi-user.target.wants/yasem.service"
ln -sfn "$SD/yasem.timer" "$SD/timers.target.wants/yasem.timer"
ln -sfn /usr/lib/systemd/system/multi-user.target "$SD/default.target" 2>/dev/null \
    || ln -sfn /lib/systemd/system/multi-user.target "$SD/default.target" 2>/dev/null \
    || true

systemctl daemon-reload 2>/dev/null || true
systemctl enable yasem.service yasem.timer 2>/dev/null || true
systemctl restart yasem.service 2>/dev/null || true
systemctl start yasem.timer 2>/dev/null || true

echo "Yasem boot enabled."
echo "  yasem.service + yasem.timer (OnBootSec=5 failsafe)"
echo "  kodi.service: masked"
echo "  remote-config: not tied to Yasem (IR via start.sh)"
echo "Reboot to verify splash → Yasem"

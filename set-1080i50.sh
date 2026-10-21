#!/bin/sh
# Force CoreELEC Amlogic HDMI output to 1080i50hz (required for Yasem UI/video).
MODE=1080i50hz
ok=0

if [ -w /sys/class/display/mode ]; then
    echo "$MODE" > /sys/class/display/mode 2>/dev/null && ok=1
fi

# Persist across reboot via CoreELEC /flash/config.ini
if mountpoint -q /flash 2>/dev/null || [ -d /flash ]; then
    mount -o remount,rw /flash 2>/dev/null || true
    cfg=/flash/config.ini
    if [ -e "$cfg" ] || touch "$cfg" 2>/dev/null; then
        if grep -q '^hdmimode=' "$cfg" 2>/dev/null; then
            sed -i "s/^hdmimode=.*/hdmimode=$MODE/" "$cfg"
        else
            printf '\nhdmimode=%s\n' "$MODE" >> "$cfg"
        fi
        ok=1
    fi
    mount -o remount,ro /flash 2>/dev/null || true
fi

# Boot-time enforce (idempotent)
boot=/storage/.config/yasem-1080i50.sh
mkdir -p /storage/.config
cat > "$boot" <<'EOF'
#!/bin/sh
if [ -w /sys/class/display/mode ]; then
    cur=$(cat /sys/class/display/mode 2>/dev/null)
    if [ "$cur" != "1080i50hz" ]; then
        echo 1080i50hz > /sys/class/display/mode 2>/dev/null || true
    fi
fi
EOF
chmod 755 "$boot"

auto=/storage/.config/autostart.sh
if [ ! -e "$auto" ]; then
    printf '#!/bin/sh\n' > "$auto"
    chmod 755 "$auto"
fi
if ! grep -q 'yasem-1080i50.sh' "$auto" 2>/dev/null; then
    printf '\n# Yasem: keep HDMI at 1080i50\n[ -x /storage/.config/yasem-1080i50.sh ] && /storage/.config/yasem-1080i50.sh &\n' >> "$auto"
fi

cur=$(cat /sys/class/display/mode 2>/dev/null)
echo "display mode now: $cur (want $MODE)"
[ "$cur" = "$MODE" ] && exit 0
[ "$ok" = "1" ] && exit 0
exit 1

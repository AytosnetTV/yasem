#!/bin/sh
# Launch Yasem MAG emulator on CoreELEC (Amlogic). Requires 1080i50 display.
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
LOG="$ROOT/yasem.log"

force_1080i50() {
    if [ -x "$ROOT/set-1080i50.sh" ]; then
        "$ROOT/set-1080i50.sh" || true
        return
    fi
    if [ -w /sys/class/display/mode ]; then
        cur=$(cat /sys/class/display/mode 2>/dev/null)
        if [ "$cur" != "1080i50hz" ]; then
            echo 1080i50hz > /sys/class/display/mode 2>/dev/null || true
        fi
    fi
}

ensure_ir() {
    # Without Kodi, remote-config.service (graphical.target) never runs.
    # Load Amlogic meson-remote + apply /storage/.config/remote.conf (same as Kodi).
    if grep -q 'Name="aml_keypad"' /proc/bus/input/devices 2>/dev/null && [ -e /dev/amremote ]; then
        return
    fi

    # meson-ir holds GPIOAO_7 — must unload before meson-remote can probe
    rmmod meson_ir 2>/dev/null || true
    rmmod meson-ir 2>/dev/null || true
    sleep 1

    modprobe meson-remote 2>/dev/null || true
    sleep 1

    if [ ! -e /sys/devices/platform/c8100580.meson-remote/driver ] \
        && [ -e /sys/bus/platform/drivers/meson-remote/bind ]; then
        echo c8100580.meson-remote > /sys/bus/platform/drivers/meson-remote/bind 2>/dev/null || true
        sleep 1
    fi

    if [ -e /dev/amremote ] && [ -f /storage/.config/remote.conf ]; then
        remotecfg /storage/.config/remote.conf >/tmp/yasem-remotecfg.log 2>&1 || true
    elif [ -x /usr/lib/coreelec/remote-config ]; then
        /usr/lib/coreelec/remote-config >/tmp/yasem-remote-config.log 2>&1 || true
    fi
    sleep 1
}

find_remote_event() {
    if [ -n "$YASEM_INPUT_EVENT" ] && [ -e "$YASEM_INPUT_EVENT" ]; then
        echo "$YASEM_INPUT_EVENT"
        return
    fi
    for ev in /sys/class/input/event*; do
        [ -e "$ev/device/name" ] || continue
        name=$(cat "$ev/device/name" 2>/dev/null)
        case "$name" in
            *cec*|*vkeypad*|*gpio_keypad*)
                continue
                ;;
            *aml_keypad*|*meson-remote*|*meson-ir*|*meson_ir*)
                echo "/dev/input/$(basename "$ev")"
                return
                ;;
        esac
    done
    for ev in /dev/input/event3 /dev/input/event4 /dev/input/event5; do
        [ -e "$ev" ] || continue
        echo "$ev"
        return
    done
    echo /dev/input/event0
}

wait_for_network() {
    # Yasem used to start before DHCP/DNS → "Host portal.example.com not found" (Qt error 3)
    portal_host="portal.example.com"
    if [ -f "$ROOT/config/config.json" ]; then
        h=$(sed -n 's/.*"portal"[[:space:]]*:[[:space:]]*"http[s]*:\/\/\([^"/]*\).*/\1/p' "$ROOT/config/config.json" | head -n 1)
        [ -n "$h" ] && portal_host="$h"
    fi
    i=0
    while [ "$i" -lt 60 ]; do
        # Prefer a real LAN address (not 127.x)
        ip=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(n=1;n<=NF;n++) if($n=="src"){print $(n+1); exit}}')
        case "$ip" in
            ""|127.*|0.0.0.0) ;;
            *)
                if ping -c 1 -W 1 "$portal_host" >/dev/null 2>&1 \
                    || ping -c 1 -W 1 8.8.8.8 >/dev/null 2>&1; then
                    echo "network ready ip=$ip host=$portal_host" >> "$LOG"
                    return 0
                fi
                ;;
        esac
        i=$((i + 1))
        sleep 1
    done
    echo "network wait timeout, starting anyway" >> "$LOG"
    return 0
}

force_1080i50
systemctl stop eventlircd.service >/dev/null 2>&1 || true
ensure_ir
wait_for_network

# Hide Linux FB console so IR keys never appear as text when UI is down
for b in /sys/class/vtconsole/vtcon*/bind; do
    [ -e "$b" ] || continue
    echo 0 > "$b" 2>/dev/null || true
done
echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true

export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM="linuxfb:fb=/dev/fb0"
export QT_QPA_FONTDIR="$ROOT/fonts"
export QT_PLUGIN_PATH="$ROOT/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$ROOT/plugins/platforms"
export QT_QPA_GENERIC_PLUGINS="evdevkeyboard"
REMOTE_EV=$(find_remote_event)
export QT_QPA_EVDEV_KEYBOARD_PARAMETERS="device=${REMOTE_EV}"
export HOME=/storage
export XDG_CACHE_HOME="$ROOT/config/cache"
mkdir -p "$ROOT/config" "$XDG_CACHE_HOME"
if [ -w /sys/class/video/disable_video ]; then
    echo 1 > /sys/class/video/disable_video
fi
# Keep panel blank until Yasem has started painting (unblank shortly after exec via subshell)
if [ -w /sys/class/graphics/fb0/blank ]; then
    echo 1 > /sys/class/graphics/fb0/blank
fi
if [ -w /sys/class/video/video_zorder ]; then
    echo 126 > /sys/class/video/video_zorde
fi
if [ -w /sys/class/graphics/fb0/order ]; then
    echo 2 > /sys/class/graphics/fb0/orde
fi
cd "$ROOT"
echo "---- $(date) ----" >> "$LOG"
echo "input $QT_QPA_EVDEV_KEYBOARD_PARAMETERS mode $(cat /sys/class/display/mode 2>/dev/null)" >> "$LOG"
echo "devices $(grep -E 'Name=|Handlers=' /proc/bus/input/devices | tr '\n' ' ')" >> "$LOG"
/storage/yasem/led-status.sh on 2>/dev/null || true
# Drop legacy userdata that could brick portal layout after a bad SaveUserData.
rm -f /storage/yasem/config/userdata.dat /storage/yasem/config/userdata.dat.tmp 2>/dev/null || true
# Unblank a bit after Yasem opens linuxfb (avoids console glyphs on wake).
# Clear power-guard after a few seconds so Power works again for standby.
( sleep 2
  echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
  echo 0 > /sys/class/graphics/fb1/blank 2>/dev/null || true
  echo 0 > /sys/devices/virtual/amhdmitx/amhdmitx0/avmute 2>/dev/null || true
  /storage/yasem/led-status.sh on 2>/dev/null || true
  sleep 3
  rm -f /storage/yasem/.power-guard /storage/yasem/.wake
) &
exec "$ROOT/yasem" >> "$LOG" 2>&1

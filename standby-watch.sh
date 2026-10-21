#!/bin/sh
# Soft standby: stop Yasem, black screen, wait for Power, start Yasem again.
# Wake runs in THIS process (foreground systemctl start). Background jobs
# get SIGKILL when the oneshot service exits (KillMode).
set -eu
FLAG=/storage/yasem/.standby
WAKE=/storage/yasem/.wake
GUARD=/storage/yasem/.power-guard
EV=/dev/input/event3
LOG=/storage/yasem/standby.log

echo "---- $(date) standby-watch start ----" >> "$LOG"
touch "$FLAG"
rm -f "$WAKE"

console_off() {
    for b in /sys/class/vtconsole/vtcon*/bind; do
        [ -e "$b" ] || continue
        echo 0 > "$b" 2>/dev/null || true
    done
    echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true
    clear >/dev/tty1 2>/dev/null || true
    clear >/dev/tty0 2>/dev/null || true
}

blank_on() {
    console_off
    echo 1 > /sys/class/video/disable_video 2>/dev/null || true
    echo 1 > /sys/class/graphics/fb0/blank 2>/dev/null || true
    echo 1 > /sys/class/graphics/fb1/blank 2>/dev/null || true
    echo 1 > /sys/devices/virtual/amhdmitx/amhdmitx0/avmute 2>/dev/null || true
    echo 1 > /sys/class/amhdmitx/amhdmitx0/avmute 2>/dev/null || true
    echo 0 > /sys/devices/virtual/amhdmitx/amhdmitx0/phy 2>/dev/null || true
    /storage/yasem/led-status.sh standby 2>/dev/null || true
}

blank_off() {
    echo 1 > /sys/devices/virtual/amhdmitx/amhdmitx0/phy 2>/dev/null || true
    echo 0 > /sys/devices/virtual/amhdmitx/amhdmitx0/avmute 2>/dev/null || true
    echo 0 > /sys/class/amhdmitx/amhdmitx0/avmute 2>/dev/null || true
    echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
    echo 0 > /sys/class/graphics/fb1/blank 2>/dev/null || true
    echo 0 > /sys/class/video/disable_video 2>/dev/null || true
    echo 1080i50hz > /sys/class/display/mode 2>/dev/null || true
    /storage/yasem/led-status.sh on 2>/dev/null || true
}

ensure_ir() {
    if ! grep -q 'Name="aml_keypad"' /proc/bus/input/devices 2>/dev/null; then
        rmmod meson_ir 2>/dev/null || true
        rmmod meson-ir 2>/dev/null || true
        sleep 1
        modprobe meson-remote 2>/dev/null || true
        sleep 1
    fi
    if [ -e /dev/amremote ] && [ -f /storage/.config/remote.conf ]; then
        remotecfg /storage/.config/remote.conf >/tmp/yasem-standby-rc.log 2>&1 || true
    fi
    for ev in /sys/class/input/event*; do
        [ -e "$ev/device/name" ] || continue
        name=$(cat "$ev/device/name" 2>/dev/null)
        case "$name" in
            *aml_keypad*|*meson-remote*)
                EV="/dev/input/$(basename "$ev")"
                break
                ;;
        esac
    done
}

blank_on
systemctl stop yasem.timer 2>/dev/null || true
systemctl stop yasem.service 2>/dev/null || true
killall yasem 2>/dev/null || true
blank_on
ensure_ir
echo "listening Power on $EV" >> "$LOG"

# Wait for Power press (KEY_S=31 from remote.conf, or KEY_POWER=116)
while true; do
    line=$(dd if="$EV" bs=16 count=1 2>/dev/null | hexdump -v -e '1/1 "%02x"')
    [ "${#line}" -eq 32 ] || continue
    t0=$(echo "$line" | cut -c17-18)
    t1=$(echo "$line" | cut -c19-20)
    c0=$(echo "$line" | cut -c21-22)
    c1=$(echo "$line" | cut -c23-24)
    v0=$(echo "$line" | cut -c25-26)
    v1=$(echo "$line" | cut -c27-28)
    v2=$(echo "$line" | cut -c29-30)
    v3=$(echo "$line" | cut -c31-32)
    type=$((0x${t1}${t0}))
    code=$((0x${c1}${c0}))
    value=$((0x${v3}${v2}${v1}${v0}))
    [ "$type" -eq 1 ] || continue
    [ "$value" -eq 1 ] || continue
    case "$code" in
        31|116) echo "Power pressed code=$code" >> "$LOG"; break ;;
    esac
done

# Do not read more IR here (no timeout binary on CoreELEC; bare dd blocks forever).
# Sleep so release/repeats settle; .power-guard blocks re-standby after Yasem starts.
echo "settling before wake..." >> "$LOG"
sleep 2

rm -f "$FLAG"
touch "$WAKE"
touch "$GUARD"
console_off
# Unblank before starting Yasem — if anything interrupts wake, screen is already on
blank_off

echo "starting yasem (foreground)..." >> "$LOG"
systemctl start yasem.service >>"$LOG" 2>&1 || true

i=0
while [ "$i" -lt 40 ]; do
    pidof yasem >/dev/null 2>&1 && break
    i=$((i + 1))
    sleep 0.25
done
echo "yasem pid=$(pidof yasem 2>/dev/null || echo none)" >> "$LOG"

blank_off
sleep 1
rm -f "$WAKE" "$GUARD"

echo "---- $(date) wake done ----" >> "$LOG"
exit 0

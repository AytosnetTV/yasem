#!/bin/sh
# Box front LED: only sys_led is software-controlled on this device.
# Many boxes use a dual-color LED (blue=on / red=standby) on that one GPIO.
# Usage: led-status.sh on|standby|off
set -eu
LED=/sys/class/leds/sys_led

led_set() {
    mode=$1
    [ -d "$LED" ] || return 0
    echo none > "$LED/trigger" 2>/dev/null || true
    case "$mode" in
        on|wake)
            # After DTB GPIO_ACTIVE_LOW invert: brightness 255 → blue (active)
            echo 255 > "$LED/brightness" 2>/dev/null || true
            ;;
        standby|off)
            # brightness 0 → red (standby) on dual-color panels
            echo 0 > "$LED/brightness" 2>/dev/null || true
            ;;
        blink)
            echo timer > "$LED/trigger" 2>/dev/null || true
            if [ -e "$LED/delay_on" ]; then
                echo 200 > "$LED/delay_on" 2>/dev/null || true
                echo 1800 > "$LED/delay_off" 2>/dev/null || true
            fi
            ;;
        *)
            echo "usage: $0 on|standby|off|blink" >&2
            return 1
            ;;
    esac
}

led_set "${1:-on}"

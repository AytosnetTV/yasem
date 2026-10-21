#!/bin/sh
# Remove Aytos/Kodi custom addons and helpers. Keep Yasem only.
set -eu

echo "Stopping Kodi / aytos helpers..."
systemctl stop kodi.service 2>/dev/null || true
systemctl stop aytos-epg.timer aytos-epg.service 2>/dev/null || true
killall kodi.bin kodi.sh 2>/dev/null || true

ADDONS=/storage/.kodi/addons
USERDATA=/storage/.kodi/userdata

# Custom Aytos / third-party UI addons
for a in \
    plugin.program.mag.portal \
    service.mag.activation \
    skin.mag.tv \
    skin.quartz \
    plugin.video.filmifen \
    plugin.video.youtube \
    pvr.iptvsimple \
    repository.mar33 \
    script.favourites \
    script.openweathermap.maps \
    weather.gismeteo \
    weather.multi \
    service.openvfd
do
    if [ -e "$ADDONS/$a" ]; then
        echo "remove addon $a"
        rm -rf "$ADDONS/$a"
    fi
    if [ -e "$USERDATA/addon_data/$a" ]; then
        echo "remove addon_data $a"
        rm -rf "$USERDATA/addon_data/$a"
    fi
done

# Aytos helper scripts / EPG / update pullers (keep yasem-1080i50)
rm -f /storage/.config/aytos-epg.sh \
      /storage/.config/system.d/aytos-epg.service \
      /storage/.config/system.d/aytos-epg.timer \
      /storage/.config/system.d/timers.target.wants/aytos-epg.timer \
      /storage/.config/not_v1.sh \
      /storage/.config/update.sh \
      /storage/.kodi/userdata/aytos-epg.xml 2>/dev/null || true

# Custom keymaps / skin leftovers
rm -f /storage/.kodi/userdata/keymaps/zzz_mag.xml \
      /storage/.kodi/userdata/keymaps/gen.xml \
      /storage/.kodi/userdata/keymaps/gen.xml.bak.nav 2>/dev/null || true

# Clear packages cache of custom zips if present
rm -rf /storage/.kodi/addons/packages/* 2>/dev/null || true
rm -rf /storage/.kodi/addons/temp/* 2>/dev/null || true

echo "Kodi custom addons/helpers removed. Yasem left at /storage/yasem"

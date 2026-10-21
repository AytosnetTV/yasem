#!/bin/bash
# Pull live Yasem from CoreELEC box в†’ yasem-portable/, dated backup, yasem.tar for upload.
set -euo pipefail
ROOT=/path/to/yasem-project
PORTABLE="$ROOT/yasem-portable"
SRC_TREE="$ROOT/yasem"
HOST="${YASEM_SSH_HOST:-root@YOUR_BOX_IP}"
SSH="sshpass -p ${YASEM_SSH_PASS:?YASEM_SSH_PASS empty} ssh -o StrictHostKeyChecking=no"
DATE=$(date +%Y-%m-%d)
STAMP=$(date +%Y-%m-%d-%H%M)
BACKUP="$ROOT/backup/yasem-$STAMP"
BACKUP_TODAY="$ROOT/backup/yasem-$DATE"
UPLOAD_TAR="$ROOT/yasem.tar"
UPLOAD_COPY="$ROOT/backup/yasem-$STAMP-upload.tar"

SCRIPTS=(
  start.sh run-instead-of-kodi.sh set-1080i50.sh install.sh install-from-url.sh
  enable-yasem-boot.sh cleanup-kodi-addons.sh
  yasem.service yasem.timer yasem-standby.service
  standby-watch.sh led-status.sh launch.sh
)

echo "== remote inventory =="
$SSH $HOST 'du -sh /storage/yasem; ls -la /storage/yasem; echo ---CONFIG---; ls -la /storage/yasem/config 2>/dev/null || true'

echo "== clean portable dir =="
mkdir -p "$PORTABLE"
rm -rf "$PORTABLE/lib" "$PORTABLE/plugins" "$PORTABLE/fonts" "$PORTABLE/config" \
       "$PORTABLE/yasem" "$PORTABLE/qt.conf"
for f in "${SCRIPTS[@]}"; do
  rm -f "$PORTABLE/$f"
done
rm -f "$PORTABLE/PROMPT.txt" "$PORTABLE/INSTALL.txt" "$PORTABLE/README.txt"

echo "== tar from box (binary + Qt runtime) =="
$SSH $HOST 'cd /storage/yasem && tar cf - yasem qt.conf lib plugins fonts' \
  | tar xf - -C "$PORTABLE"

mkdir -p "$PORTABLE/config"
$SSH $HOST 'test -f /storage/yasem/config/remote.conf && cat /storage/yasem/config/remote.conf' \
  > /tmp/yasem-remote.conf 2>/dev/null || true
if [ -s /tmp/yasem-remote.conf ]; then
  cp /tmp/yasem-remote.conf "$PORTABLE/config/remote.conf"
fi
rm -f /tmp/yasem-remote.conf

echo "== overlay local scripts + docs =="
for f in "${SCRIPTS[@]}"; do
  if [ -f "$SRC_TREE/$f" ]; then
    cp -a "$SRC_TREE/$f" "$PORTABLE/$f"
  fi
done
for f in PROMPT.txt INSTALL.txt README.txt; do
  [ -f "$SRC_TREE/$f" ] && cp -a "$SRC_TREE/$f" "$PORTABLE/$f"
done
if [ -f "$SRC_TREE/config/remote.conf" ]; then
  cp -a "$SRC_TREE/config/remote.conf" "$PORTABLE/config/remote.conf"
fi
if [ -f "$SRC_TREE/config/config.json.example" ]; then
  cp -a "$SRC_TREE/config/config.json.example" "$PORTABLE/config/config.json.example"
fi
if [ -f "$SRC_TREE/config/remote.conf.example" ]; then
  cp -a "$SRC_TREE/config/remote.conf.example" "$PORTABLE/config/remote.conf.example"
fi

find "$PORTABLE" -maxdepth 1 -type f \( -name '*.sh' -o -name '*.service' -o -name '*.timer' \) \
  -exec sed -i 's/\r$//' {} +
chmod 755 "$PORTABLE/yasem" "$PORTABLE"/*.sh 2>/dev/null || true
chmod 644 "$PORTABLE/yasem.service" "$PORTABLE/yasem.timer" \
  "$PORTABLE/yasem-standby.service" 2>/dev/null || true

echo "== portable size =="
du -sh "$PORTABLE" "$PORTABLE/lib" "$PORTABLE/plugins" "$PORTABLE/fonts" 2>/dev/null || true
ls -la "$PORTABLE"

echo "== backup $BACKUP =="
rm -rf "$BACKUP"
mkdir -p "$BACKUP/yasem" "$BACKUP/src" "$BACKUP/src-scripts"
cp -a "$PORTABLE"/. "$BACKUP/yasem/"
cp -a "$SRC_TREE/src" "$BACKUP/src/"
cp -a "$SRC_TREE/yasem.pro" "$BACKUP/"
for f in "${SCRIPTS[@]}"; do
  [ -f "$SRC_TREE/$f" ] && cp -a "$SRC_TREE/$f" "$BACKUP/src-scripts/"
done
for f in PROMPT.txt INSTALL.txt README.txt; do
  [ -f "$SRC_TREE/$f" ] && cp -a "$SRC_TREE/$f" "$BACKUP/"
done
[ -d "$SRC_TREE/config" ] && cp -a "$SRC_TREE/config" "$BACKUP/src-config"

rm -rf "$BACKUP_TODAY"
cp -a "$BACKUP" "$BACKUP_TODAY"

echo "== upload tarball yasem.tar (top-level yasem/) =="
# Structure for: wget вЂ¦/yasem.tar && tar xf yasem.tar && sh yasem/install.sh
rm -f "$UPLOAD_TAR" "$UPLOAD_COPY"
rm -rf /tmp/yasem-upload-stage
mkdir -p /tmp/yasem-upload-stage
cp -a "$PORTABLE" /tmp/yasem-upload-stage/yasem
# Do not ship machine-specific env/userdata in the public tarball
rm -rf /tmp/yasem-upload-stage/yasem/config/userdata \
       /tmp/yasem-upload-stage/yasem/config/env.json \
       /tmp/yasem-upload-stage/yasem/config/LocalStorage \
       /tmp/yasem-upload-stage/yasem/config/cache \
       /tmp/yasem-upload-stage/yasem/config/WebpageIcons.db 2>/dev/null || true
tar -C /tmp/yasem-upload-stage -cf "$UPLOAD_TAR" yasem
cp -a "$UPLOAD_TAR" "$UPLOAD_COPY"
rm -rf /tmp/yasem-upload-stage

echo "== full archive (portable + source backup) =="
TAR="$ROOT/backup/yasem-$STAMP.tar.gz"
(cd "$ROOT" && tar czf "$TAR" -C "$ROOT" "yasem-portable" "backup/yasem-$STAMP")
ls -lh "$TAR" "$UPLOAD_TAR" "$PORTABLE/yasem"

echo "DONE"
echo "  portable:     $PORTABLE"
echo "  backup:       $BACKUP"
echo "  today:        $BACKUP_TODAY"
echo "  full archive: $TAR"
echo "  UPLOAD FILE:  $UPLOAD_TAR"
echo "  upload copy:  $UPLOAD_COPY"
echo
echo "Upload to web:"
echo "  scp $UPLOAD_TAR user@host:/path/www/mag/yasem.tar"
echo "  # or copy D:\\Cursor\\KODI\/yasem.tar в†’ https://www.example.com/mag/yasem.tar"
echo
echo "Install on another box:"
echo "  ssh root@BOX 'wget -O /tmp/yasem.tar http://www.example.com/mag/yasem.tar && tar xf /tmp/yasem.tar -C /tmp && sh /tmp/yasem/install.sh && reboot'"

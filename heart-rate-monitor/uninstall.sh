#!/usr/bin/env bash
# Removes the BNTS heart rate app and gives you the normal Raspberry Pi desktop back.
set -uo pipefail
[ "$(id -u)" -eq 0 ] && { echo "Run as your normal user, not root." >&2; exit 1; }

sudo systemctl disable --now bnts-heart.service bnts-update.service 2>/dev/null
sudo rm -f /etc/systemd/system/bnts-heart.service /etc/systemd/system/bnts-update.service
sudo systemctl daemon-reload
pkill -f /opt/bnts-heart/kiosk.sh 2>/dev/null
sudo rm -rf /opt/bnts-heart /etc/default/bnts-heart

CFG="$HOME/.config/labwc"
if [ -f "$CFG/autostart.before-bnts" ]; then mv -f "$CFG/autostart.before-bnts" "$CFG/autostart"
else rm -f "$CFG/autostart"; fi          # no user file -> labwc falls back to the stock desktop

for f in /boot/firmware/cmdline.txt /boot/cmdline.txt /boot/firmware/config.txt /boot/config.txt; do
  [ -f "$f.before-bnts" ] && sudo mv -f "$f.before-bnts" "$f" && echo "Restored $f"
done
echo "Removed. Reboot to get the normal desktop back."

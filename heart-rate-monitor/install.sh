#!/usr/bin/env bash
# BNTS heart rate — Raspberry Pi installer.
#
#   ./install.sh                 install / update
#   ./install.sh --quiet-boot    ...and also hide boot text, logo and blinking cursor
#
# Run as your normal user (NOT root); it uses sudo where needed. Safe to re-run.
# Target: Raspberry Pi OS with desktop (Bookworm or newer).
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
  echo "Please run this as your normal user (e.g. 'pi'), not with sudo. It asks for sudo itself." >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
APP=/opt/bnts-heart
ME="$(id -un)"
QUIET_BOOT=0
[ "${1:-}" = "--quiet-boot" ] && QUIET_BOOT=1

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!  %s\033[0m\n' "$*"; }

say "Installing packages"
sudo apt-get update
CHROMIUM=chromium
apt-cache show chromium >/dev/null 2>&1 || CHROMIUM=chromium-browser
sudo apt-get install -y python3 python3-serial curl git "$CHROMIUM"
sudo apt-get install -y wlr-randr >/dev/null 2>&1 || true     # only used for the optional ROTATE setting

say "Installing the app to $APP"
sudo install -d "$APP"
sudo rm -rf "$APP/web"
sudo cp -r "$HERE/pi/web" "$APP/web"
sudo install -m 755 "$HERE/pi/server.py" "$APP/server.py"
sudo install -m 755 "$HERE/pi/kiosk.sh"  "$APP/kiosk.sh"
sudo install -m 755 "$HERE/pi/update.sh" "$APP/update.sh"
if [ ! -f /etc/default/bnts-heart ]; then
  sudo tee /etc/default/bnts-heart >/dev/null <<'EOF'
# BNTS heart rate settings (read by the server and the kiosk launcher)

# Extra server arguments, e.g.  BNTS_ARGS="--port /dev/ttyACM0"   (default: auto-detect the Arduino)
BNTS_ARGS=""

# Rotate the screen if it's mounted upside-down: 0, 90, 180 or 270. Empty = leave alone.
ROTATE=""

# Check GitHub for a new version at every boot and install it: 1 = yes, 0 = no.
AUTO_UPDATE="1"
EOF
fi
# set_conf KEY VALUE: add or replace KEY="VALUE" in the settings file
set_conf() {
  if sudo grep -q "^$1=" /etc/default/bnts-heart; then sudo sed -i "s|^$1=.*|$1=\"$2\"|" /etc/default/bnts-heart
  else echo "$1=\"$2\"" | sudo tee -a /etc/default/bnts-heart >/dev/null; fi
}

say "Setting up the background service (reads the Arduino)"
sudo usermod -aG dialout "$ME"
sudo tee /etc/systemd/system/bnts-heart.service >/dev/null <<EOF
[Unit]
Description=BNTS heart rate server (Arduino -> display)
After=network.target

[Service]
User=$ME
SupplementaryGroups=dialout
EnvironmentFile=-/etc/default/bnts-heart
ExecStart=/usr/bin/python3 $APP/server.py \$BNTS_ARGS
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable bnts-heart.service
sudo systemctl restart bnts-heart.service

say "Setting up automatic updates at every boot"
REPO_DIR="$(git -C "$HERE" rev-parse --show-toplevel 2>/dev/null || true)"
if [ -n "$REPO_DIR" ]; then
  set_conf REPO_DIR "$REPO_DIR"
  set_conf REPO_USER "$ME"
  set_conf APP_SUBDIR "$(realpath --relative-to="$REPO_DIR" "$HERE")"
  sudo grep -q '^AUTO_UPDATE=' /etc/default/bnts-heart || set_conf AUTO_UPDATE 1
  git -C "$REPO_DIR" rev-parse HEAD | sudo tee "$APP/.deployed-rev" >/dev/null
  sudo tee /etc/systemd/system/bnts-update.service >/dev/null <<EOF
[Unit]
Description=BNTS heart rate: install the latest version from GitHub
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=$APP/update.sh
TimeoutStartSec=300

[Install]
WantedBy=multi-user.target
EOF
  sudo systemctl daemon-reload
  sudo systemctl enable bnts-update.service
  echo "   At every boot the Pi will pull $REPO_DIR from GitHub and install anything new."
else
  warn "This folder isn't a git clone, so automatic updates are off. Clone the repo with git to turn them on."
fi

# setup_autologin: boot straight into the labwc (Wayland) desktop, logged in as this user. Raspberry Pi Connect
# screen sharing needs a live Wayland desktop session, and this brings the kiosk up with nobody touching the Pi.
# This edits the login screen's (lightdm's) settings directly instead of calling raspi-config, because raspi-config's
# option codes differ between OS versions ("Programmer error, unrecognised boot option").
setup_autologin() {
  local ETC="${LIGHTDM_ETC:-/etc/lightdm}" SESS="${WAYLAND_SESSIONS:-/usr/share/wayland-sessions}" session="" s
  sudo systemctl set-default graphical.target >/dev/null 2>&1 || warn "Couldn't set the boot target to the desktop."
  if [ ! -d "$ETC" ]; then
    warn "No lightdm login screen found. Set the Pi to start the desktop with auto-login, using the labwc session."
    return 0
  fi
  if ! command -v labwc >/dev/null 2>&1; then
    sudo apt-get install -y labwc >/dev/null 2>&1 || warn "Couldn't install labwc (the Wayland desktop that Raspberry Pi Connect screen sharing needs)."
  fi
  for s in "$SESS"/*labwc*.desktop; do
    [ -e "$s" ] && { session="$(basename "$s" .desktop)"; break; }
  done
  if [ -z "$session" ]; then
    warn "No labwc session found in $SESS: the Pi will log in automatically but may not use the labwc (Wayland) desktop."
  fi
  sudo install -d "$ETC/lightdm.conf.d"
  {
    echo "[Seat:*]"
    echo "autologin-user=$ME"
    echo "autologin-user-timeout=0"
    if [ -n "$session" ]; then echo "autologin-session=$session"; fi
  } | sudo tee "$ETC/lightdm.conf.d/50-bnts-autologin.conf" >/dev/null
  # lightdm.conf is read after the drop-in folder and wins, so make sure it doesn't say something different.
  if [ -f "$ETC/lightdm.conf" ]; then
    sudo sed -i "s|^autologin-user=.*|autologin-user=$ME|" "$ETC/lightdm.conf"
    if [ -n "$session" ]; then
      sudo sed -i "s|^autologin-session=.*|autologin-session=$session|; s|^user-session=.*|user-session=$session|" "$ETC/lightdm.conf"
    fi
  fi
  echo "   Auto-login as $ME into the ${session:-default} desktop session."
  return 0
}

say "Making the screen show ONLY the heart rate display"
# 1. Boot straight into the Wayland desktop as this user.
setup_autologin

# 2. labwc: a per-user autostart REPLACES the stock one (/etc/xdg/labwc/autostart), which is what normally
#    launches the taskbar, desktop icons/wallpaper and pop-up helpers. Ours starts only the kiosk.
CFG="$HOME/.config/labwc"
mkdir -p "$CFG"
if [ -f "$CFG/autostart" ] && ! grep -q "BNTS heart rate" "$CFG/autostart"; then
  cp "$CFG/autostart" "$CFG/autostart.before-bnts"
  echo "   (your previous autostart was saved as $CFG/autostart.before-bnts)"
fi
cat > "$CFG/autostart" <<'EOF'
# BNTS heart rate kiosk.
# This file replaces the stock Raspberry Pi desktop autostart (/etc/xdg/labwc/autostart), so there is
# no taskbar, no desktop icons and no pop-ups: Chromium showing the display is the only thing on screen.

# Let systemd user services (e.g. Raspberry Pi Connect) see this Wayland session.
dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=labwc >/dev/null 2>&1 || true

# Display profiles (keeps any saved screen layout).
command -v kanshi >/dev/null 2>&1 && /usr/bin/kanshi >/dev/null 2>&1 &

# The heart rate display. It restarts itself if Chromium ever quits.
/opt/bnts-heart/kiosk.sh >>/tmp/bnts-kiosk.log 2>&1 &

# If Raspberry Pi Connect is switched on, make sure its screen sharing is running in this session.
( sleep 8; systemctl --user is-enabled --quiet rpi-connect-wayvnc.service 2>/dev/null \
    && systemctl --user restart rpi-connect-wayvnc.service ) >/dev/null 2>&1 &
EOF

# 3. Optional: no boot text / logo / blinking cursor
if [ "$QUIET_BOOT" = 1 ]; then
  say "Hiding boot messages"
  CMD=/boot/firmware/cmdline.txt; [ -f "$CMD" ] || CMD=/boot/cmdline.txt
  CFGTXT=/boot/firmware/config.txt; [ -f "$CFGTXT" ] || CFGTXT=/boot/config.txt
  sudo cp -n "$CMD" "$CMD.before-bnts"
  for tok in quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0; do
    grep -qwF -- "$tok" "$CMD" || sudo sed -i "1s/\$/ $tok/" "$CMD"
  done
  sudo cp -n "$CFGTXT" "$CFGTXT.before-bnts"
  grep -q '^disable_splash=1' "$CFGTXT" || echo 'disable_splash=1' | sudo tee -a "$CFGTXT" >/dev/null
  echo "   (originals saved as *.before-bnts next to $CMD and $CFGTXT)"
fi

say "Done."
cat <<EOF

Next:  sudo reboot

After the reboot the Pi logs in by itself, and the touchscreen shows only the heart rate display.
Check the service any time with:   systemctl status bnts-heart
See what the last update did:      journalctl -u bnts-update -b
If the screen isn't right:         cat /tmp/bnts-kiosk.log
Raspberry Pi Connect:              see the 'Raspberry Pi Connect' section of README.md
EOF

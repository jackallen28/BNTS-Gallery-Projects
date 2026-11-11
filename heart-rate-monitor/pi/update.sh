#!/usr/bin/env bash
# BNTS heart rate - boot-time updater.
#
# Runs at every boot (bnts-update.service). It pulls the latest code from GitHub into your clone, copies
# it to /opt/bnts-heart, and restarts the display server if anything changed.
#
# It never stops the display from starting: with no internet, or if the pull fails, the version that is
# already installed keeps running. It always exits 0.
#
#   Run it by hand:   sudo /opt/bnts-heart/update.sh
#   Turn it off:      AUTO_UPDATE="0" in /etc/default/bnts-heart
#   See what it did:  journalctl -u bnts-update -b
#
# Note: if an update changes the Arduino firmware, it tells you in the log; flashing is still a manual
# step (./flash-arduino.sh), so an Arduino is never reprogrammed by surprise.
set -u

main() {
  local CONF="${BNTS_CONF:-/etc/default/bnts-heart}"
  local APP="${BNTS_APP_DIR:-/opt/bnts-heart}"
  [ -r "$CONF" ] && . "$CONF"
  local AUTO_UPDATE="${AUTO_UPDATE:-1}"
  local APP_SUBDIR="${APP_SUBDIR:-heart-rate-monitor}"
  local TRIES="${BNTS_ONLINE_TRIES:-6}"
  local SYSTEMCTL="${SYSTEMCTL:-systemctl}"

  log() { echo "[bnts-update] $*"; }

  [ "$AUTO_UPDATE" = "1" ] || { log "automatic updates are off (AUTO_UPDATE=$AUTO_UPDATE)"; return 0; }
  local REPO_DIR="${REPO_DIR:-}"
  [ -d "$REPO_DIR/.git" ] || { log "no git clone found at '$REPO_DIR' - run install.sh again from your clone"; return 0; }

  # Run git as the person who owns the clone (this script runs as root at boot).
  as_user() {
    if [ -z "${REPO_USER:-}" ] || [ "$(id -un)" = "$REPO_USER" ]; then "$@"; else runuser -u "$REPO_USER" -- "$@"; fi
  }
  git_u() { as_user env GIT_TERMINAL_PROMPT=0 git -C "$REPO_DIR" "$@"; }

  # Wi-Fi can take a while to come up at boot: try for about a minute.
  local online=0 i=0
  while [ "$i" -lt "$TRIES" ]; do
    if as_user timeout 20 env GIT_TERMINAL_PROMPT=0 git -C "$REPO_DIR" ls-remote --exit-code origin HEAD >/dev/null 2>&1; then
      online=1; break
    fi
    i=$((i + 1))
    [ "$i" -lt "$TRIES" ] && sleep 5
  done

  if [ "$online" = 1 ]; then
    if as_user timeout 120 env GIT_TERMINAL_PROMPT=0 git -C "$REPO_DIR" pull --ff-only --quiet; then
      log "checked GitHub: clone is up to date"
    else
      log "git pull failed (local edits in the clone?) - keeping the version already there"
    fi
  else
    log "no internet - keeping the version already installed"
  fi

  local NEW OLD SRC
  NEW="$(git_u rev-parse HEAD 2>/dev/null || true)"
  OLD="$(cat "$APP/.deployed-rev" 2>/dev/null || true)"
  SRC="$REPO_DIR/$APP_SUBDIR"
  [ -n "$NEW" ] || { log "couldn't read the clone's version"; return 0; }
  [ -d "$SRC/pi/web" ] || { log "can't find $SRC/pi/web"; return 0; }
  if [ "$NEW" = "$OLD" ]; then log "already running ${NEW:0:7}"; return 0; fi

  log "installing ${NEW:0:7} (was ${OLD:0:7})"
  mkdir -p "$APP" || return 0
  rm -rf "$APP/web.new" && cp -r "$SRC/pi/web" "$APP/web.new" && rm -rf "$APP/web" && mv "$APP/web.new" "$APP/web" \
    || { log "couldn't copy the display files - leaving things as they were"; return 0; }
  # Copy to a temporary name then rename, so replacing files that are in use (including this script) is safe.
  local f
  for f in server.py kiosk.sh update.sh; do
    [ -f "$SRC/pi/$f" ] || continue
    install -m 755 "$SRC/pi/$f" "$APP/$f.new" && mv -f "$APP/$f.new" "$APP/$f"
  done
  echo "$NEW" > "$APP/.deployed-rev"
  $SYSTEMCTL restart bnts-heart.service || log "couldn't restart bnts-heart.service"
  log "done - the display reloads itself when it is next idle"

  if [ -n "$OLD" ] && [ "$OLD" != "$NEW" ]; then
    local changed
    changed="$(git_u diff --name-only "$OLD" "$NEW" -- "$APP_SUBDIR/arduino/bnts_heartrate" 2>/dev/null || true)"
    [ -n "$changed" ] && log "NOTE: the Arduino firmware changed in this update. Run ./flash-arduino.sh to flash it."
    changed="$(git_u diff --name-only "$OLD" "$NEW" -- "$APP_SUBDIR/install.sh" 2>/dev/null || true)"
    [ -n "$changed" ] && log "NOTE: install.sh changed in this update. Re-run it when convenient."
  fi
  return 0
}

main "$@"
exit 0

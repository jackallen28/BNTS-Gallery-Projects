#!/bin/bash
# Shows the BNTS heart rate display full-screen in Chromium, and relaunches it if it ever quits.
# Started by ~/.config/labwc/autostart (installed by install.sh) inside the Wayland desktop session.

CONF=/etc/default/bnts-heart
[ -r "$CONF" ] && . "$CONF"
URL="http://127.0.0.1:8080/${KIOSK_QUERY:-}"      # e.g. KIOSK_QUERY="?measure=15&result=8" in /etc/default/bnts-heart
PROFILE=/tmp/bnts-kiosk-profile

BROWSER=$(command -v chromium || command -v chromium-browser) || { echo "Chromium not found"; exit 1; }

# Optional screen rotation (set ROTATE=180 in /etc/default/bnts-heart if the display is mounted upside-down)
if [ -n "${ROTATE:-}" ] && command -v wlr-randr >/dev/null 2>&1; then
  OUT=$(wlr-randr 2>/dev/null | awk '/^DSI/{print $1; exit}')
  [ -z "$OUT" ] && OUT=$(wlr-randr 2>/dev/null | awk '/^[^ ]/{print $1; exit}')
  T=$ROTATE; [ "$T" = "0" ] && T=normal
  [ -n "$OUT" ] && wlr-randr --output "$OUT" --transform "$T"
fi

# Wait (up to 2 min) for the Python server so Chromium never opens on an error page.
for _ in $(seq 1 120); do
  curl -sf -o /dev/null "$URL" && break
  sleep 1
done

while true; do
  rm -rf "$PROFILE"            # fresh profile every launch: no "restore pages?" bubble, no stale state
  "$BROWSER" \
    --kiosk \
    --user-data-dir="$PROFILE" \
    --ozone-platform=wayland \
    --noerrdialogs \
    --disable-infobars \
    --disable-session-crashed-bubble \
    --hide-crash-restore-bubble \
    --no-first-run \
    --no-default-browser-check \
    --password-store=basic \
    --disable-sync \
    --disable-translate \
    --disable-features=Translate,TranslateUI,MediaRouter \
    --disable-pinch \
    --overscroll-history-navigation=0 \
    --disable-component-update \
    --check-for-update-interval=31536000 \
    "$URL"
  sleep 2
done

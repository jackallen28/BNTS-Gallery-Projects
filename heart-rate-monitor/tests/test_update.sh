#!/usr/bin/env bash
# Tests pi/update.sh against a fake "GitHub" (a local bare repo), a fake Pi clone and a fake install folder.
# No root, network or Pi needed:   bash tests/test_update.sh [workdir]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${1:-$(mktemp -d)}"; rm -rf "$W"; mkdir -p "$W"; cd "$W" || exit 1
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
fails=0
check() { if eval "$2"; then echo "  ok:   $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }

# fake systemctl that just records what it was asked
mkdir -p bin; printf '#!/bin/sh\necho "$@" >> "%s/systemctl.log"\n' "$W" > bin/systemctl; chmod +x bin/systemctl

# "GitHub": a bare repo with the project in heart-rate-monitor/
git init -q --bare -b main origin.git
git clone -q origin.git seed 2>/dev/null; mkdir -p seed/heart-rate-monitor
cp -r "$HERE/pi" "$HERE/arduino" seed/heart-rate-monitor/; cp "$HERE/install.sh" seed/heart-rate-monitor/
( cd seed && git add -A && git commit -qm v1 && git branch -M main && git push -q origin main 2>/dev/null )
git clone -q origin.git piclone                                  # the clone that lives on the Pi

# config like install.sh writes it
cat > conf <<EOF
REPO_DIR="$W/piclone"
REPO_USER=""
APP_SUBDIR="heart-rate-monitor"
AUTO_UPDATE="1"
EOF
mkdir -p "$W/usrbin"
run() { BNTS_CONF="$W/conf" BNTS_APP_DIR="$W/app" BNTS_BIN_DIR="$W/usrbin" SYSTEMCTL="$W/bin/systemctl" BNTS_ONLINE_TRIES=1 bash "$HERE/pi/update.sh" 2>&1; }

echo "1. first run: nothing deployed yet -> installs the files and restarts the server"
out=$(run); echo "$out" | sed 's/^/      /'
check "web, server.py, kiosk.sh, update.sh and serial-monitor.sh installed" '[ -f app/web/index.html ] && [ -x app/server.py ] && [ -x app/kiosk.sh ] && [ -x app/update.sh ] && [ -x app/serial-monitor.sh ]'
check "bnts-monitor command linked" '[ -L usrbin/bnts-monitor ]'
check "deployed revision recorded" '[ "$(cat app/.deployed-rev)" = "$(git -C piclone rev-parse HEAD)" ]'
check "bnts-heart restarted once" '[ "$(grep -c "restart bnts-heart" systemctl.log)" = 1 ]'

echo "2. second run, nothing new -> does nothing"
out=$(run); echo "$out" | sed 's/^/      /'
check "says it is already up to date" 'echo "$out" | grep -q "already running"'
check "no extra restart" '[ "$(grep -c "restart bnts-heart" systemctl.log)" = 1 ]'

echo "3. a new version appears on GitHub (display file + Arduino firmware changed)"
( cd seed && echo "/* v2 */" >> heart-rate-monitor/pi/web/app.js && echo "// v2" >> heart-rate-monitor/arduino/bnts_heartrate/bnts_heartrate.ino \
  && git commit -qam v2 && git push -q origin main 2>/dev/null )
out=$(run); echo "$out" | sed 's/^/      /'
check "new display file deployed" 'grep -q "v2" app/web/app.js'
check "server restarted" '[ "$(grep -c "restart bnts-heart" systemctl.log)" = 2 ]'
check "tells you the firmware changed" 'echo "$out" | grep -q "firmware changed"'
check "deployed revision moved forward" '[ "$(cat app/.deployed-rev)" = "$(git -C seed rev-parse HEAD)" ]'

echo "4. no internet (GitHub unreachable) -> keeps what is installed and still exits 0"
git -C piclone remote set-url origin "$W/does-not-exist.git"
before=$(cat app/.deployed-rev)
out=$(run); rc=$?; echo "$out" | sed 's/^/      /'
check "exit code 0" '[ $rc = 0 ]'
check "says it is keeping the installed version" 'echo "$out" | grep -q "no internet"'
check "nothing changed" '[ "$(cat app/.deployed-rev)" = "$before" ] && [ "$(grep -c "restart bnts-heart" systemctl.log)" = 2 ]'
git -C piclone remote set-url origin "$W/origin.git"

echo "5. local edits in the clone block the pull -> keeps going, exit 0"
( cd seed && echo "/* v3 */" >> heart-rate-monitor/pi/web/app.js && git commit -qam v3 && git push -q origin main 2>/dev/null )
echo "local tweak" >> piclone/heart-rate-monitor/pi/web/app.js
out=$(run); rc=$?; echo "$out" | sed 's/^/      /'
check "exit code 0" '[ $rc = 0 ]'
check "reports the failed pull" 'echo "$out" | grep -q "pull failed"'
git -C piclone checkout -q -- .

echo "6. AUTO_UPDATE=0 -> does nothing"
sed -i 's/AUTO_UPDATE="1"/AUTO_UPDATE="0"/' conf
out=$(run); echo "$out" | sed 's/^/      /'
check "reports that updates are off" 'echo "$out" | grep -q "are off"'

echo; if [ $fails = 0 ]; then echo "ALL PASSED"; else echo "FAILED ($fails)"; fi
exit $fails

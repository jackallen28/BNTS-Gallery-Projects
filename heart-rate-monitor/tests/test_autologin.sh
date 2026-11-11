#!/usr/bin/env bash
# Tests install.sh's setup_autologin against fake login-screen (lightdm) folders. No root or Pi needed:
#   bash tests/test_autologin.sh
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fails=0
check() { if eval "$2"; then echo "  ok:   $1"; else echo "  FAIL: $1"; fails=$((fails+1)); fi; }

# pull the function out of install.sh
sed -n '/^setup_autologin() {/,/^}/p' "$HERE/install.sh" > "$W/fn.sh"
[ -s "$W/fn.sh" ] || { echo "FAIL: setup_autologin not found in install.sh"; exit 1; }

# fakes: sudo runs the command itself, systemctl/apt-get/labwc are no-ops that record what they were called with
mkdir -p "$W/bin"
for c in systemctl apt-get; do printf '#!/bin/sh\necho "%s $*" >> "%s/calls.log"\n' "$c" "$W" > "$W/bin/$c"; done
printf '#!/bin/sh\nexit 0\n' > "$W/bin/labwc"; printf '#!/bin/sh\nexec "$@"\n' > "$W/bin/sudo"
chmod +x "$W/bin"/*

run() {  # run setup_autologin with the fake environment; $1 = lightdm dir, $2 = sessions dir, $3 = PATH extra
  ( export PATH="$W/bin:$3:$PATH" LIGHTDM_ETC="$1" WAYLAND_SESSIONS="$2"; ME=bnts
    warn() { echo "WARN: $*"; }; . "$W/fn.sh"; setup_autologin; echo "returned $?" ) 2>&1
}

echo "1. fresh Pi: no lightdm.conf yet, labwc sessions installed"
mkdir -p "$W/a/etc" "$W/a/sess"; touch "$W/a/sess/labwc.desktop" "$W/a/sess/rpd-x.desktop"
out=$(run "$W/a/etc" "$W/a/sess" ""); echo "$out" | sed 's/^/      /'
f="$W/a/etc/lightdm.conf.d/50-bnts-autologin.conf"
check "auto-login user set to bnts"      'grep -q "^autologin-user=bnts$" "$f"'
check "labwc session chosen"             'grep -q "^autologin-session=labwc$" "$f"'
check "no login timeout"                 'grep -q "^autologin-user-timeout=0$" "$f"'
check "boot target set to the desktop"   'grep -q "set-default graphical.target" "$W/calls.log"'
check "function returned 0 (set -e safe)" 'echo "$out" | grep -q "returned 0"'

echo "2. an old auto-login was set by raspi-config in lightdm.conf: it must not override ours"
mkdir -p "$W/b/etc" "$W/b/sess"; touch "$W/b/sess/labwc-pi.desktop"
printf '[LightDM]\n[Seat:*]\n#autologin-user=\nautologin-user=olduser\nautologin-session=rpd-x\nuser-session=rpd-x\nxserver-command=X\n' > "$W/b/etc/lightdm.conf"
out=$(run "$W/b/etc" "$W/b/sess" ""); echo "$out" | sed 's/^/      /'
check "lightdm.conf now names bnts"        'grep -q "^autologin-user=bnts$" "$W/b/etc/lightdm.conf"'
check "lightdm.conf now uses labwc-pi"     'grep -q "^autologin-session=labwc-pi$" "$W/b/etc/lightdm.conf" && grep -q "^user-session=labwc-pi$" "$W/b/etc/lightdm.conf"'
check "unrelated lines untouched"          'grep -q "^xserver-command=X$" "$W/b/etc/lightdm.conf" && grep -q "^#autologin-user=$" "$W/b/etc/lightdm.conf"'

echo "3. no labwc session installed: still logs in automatically, warns about the session"
mkdir -p "$W/c/etc" "$W/c/sess"; touch "$W/c/sess/rpd-x.desktop"
out=$(run "$W/c/etc" "$W/c/sess" ""); echo "$out" | sed 's/^/      /'
check "warns about the missing session"    'echo "$out" | grep -q "No labwc session"'
check "auto-login user still set"          'grep -q "^autologin-user=bnts$" "$W/c/etc/lightdm.conf.d/50-bnts-autologin.conf"'
check "no session line written"            '! grep -q "autologin-session" "$W/c/etc/lightdm.conf.d/50-bnts-autologin.conf"'

echo "4. no lightdm at all: warns, does not crash"
out=$(run "$W/missing" "$W/a/sess" ""); echo "$out" | sed 's/^/      /'
check "warns and returns 0" 'echo "$out" | grep -q "No lightdm" && echo "$out" | grep -q "returned 0"'

echo "5. labwc missing: it gets installed"
rm "$W/bin/labwc"; : > "$W/calls.log"
out=$(run "$W/a/etc" "$W/a/sess" "/nonexistent"); echo "$out" | sed 's/^/      /'
check "apt-get install labwc was run" 'grep -q "apt-get install -y labwc" "$W/calls.log"'

echo; [ $fails = 0 ] && echo "ALL PASSED" || echo "FAILED ($fails)"; exit $fails

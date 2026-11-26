#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Installs fcitx5 + a Japanese engine on a CardputerZero and starts it as a
# systemd user service next to APPLaunch.
#
# APPLaunch runs as a user service of the UID 1000 account. That account is
# created as "pi" by pi-gen and renamed by LaunchWizard / Raspberry Pi Imager,
# so its name differs per device. Run this script either
#   - as that user (over SSH):   sh install_fcitx5_ime.sh [mozc|anthy|skk]
#   - or as root (adb shell):     sh install_fcitx5_ime.sh [mozc|anthy|skk]
# In both cases fcitx5 is configured for the UID 1000 account.
#
# mozc  : best conversion, heaviest (mozc_server stays resident)
# anthy : lighter, phrase conversion
# skk   : lightest, SKK-style input (different key operation)

set -eu

engine=${1:-mozc}
case "$engine" in
mozc) packages="fcitx5-mozc" ;;
anthy) packages="fcitx5-anthy" ;;
skk) packages="fcitx5-skk skkdic" ;;
*)
    echo "usage: $0 [mozc|anthy|skk]" >&2
    exit 2
    ;;
esac

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
unit=fcitx5-cardputer.service
uid=1000

entry=$(getent passwd "$uid" || true)
if [ -z "$entry" ]; then
    echo "!!  No UID $uid account found (APPLaunch runs as that user)." >&2
    exit 1
fi
user=$(printf '%s' "$entry" | cut -d: -f1)
home=$(printf '%s' "$entry" | cut -d: -f6)
runtime_dir=/run/user/$uid

if [ "$(id -u)" -eq 0 ]; then
    as_root() { "$@"; }
    as_user() {
        runuser -u "$user" -- env HOME="$home" XDG_RUNTIME_DIR="$runtime_dir" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime_dir/bus" "$@"
    }
elif [ "$(id -u)" -eq "$uid" ]; then
    as_root() { sudo "$@"; }
    as_user() { "$@"; }
else
    echo "!!  Run as '$user' (UID $uid) or as root; current user is $(id -un)." >&2
    exit 1
fi
echo "==> Target account: $user (UID $uid, home $home)"

echo "==> Installing fcitx5 and $engine"
as_root apt-get update
# --no-install-recommends keeps GTK/Qt front-ends and config GUIs off the
# 512 MB device; dbus-user-session provides the per-user session bus.
# shellcheck disable=SC2086
as_root apt-get install -y --no-install-recommends fcitx5 $packages dbus-user-session

if [ ! -S "$runtime_dir/bus" ]; then
    echo "!!  $runtime_dir/bus does not exist: the user session bus is not running."
    echo "!!  If dbus-user-session was just installed, reboot and run this script again."
    exit 1
fi

echo "==> Writing $home/.config/fcitx5/profile (keyboard-us + $engine)"
as_user systemctl --user stop "$unit" 2>/dev/null || true
as_user mkdir -p "$home/.config/fcitx5" "$home/.config/systemd/user"
if [ -f "$home/.config/fcitx5/profile" ]; then
    as_user cp "$home/.config/fcitx5/profile" "$home/.config/fcitx5/profile.bak"
fi
as_user sh -c 'cat > "$1"' sh "$home/.config/fcitx5/profile" <<EOF
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=$engine

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=$engine
Layout=

[GroupOrder]
0=Default
EOF

echo "==> Installing $unit"
# Read by the calling shell, so it also works when the script lives in /root.
as_user sh -c 'cat > "$1"' sh "$home/.config/systemd/user/$unit" < "$here/$unit"
as_user systemctl --user daemon-reload
as_user systemctl --user enable "$unit"
as_user systemctl --user restart "$unit"

printf '==> Waiting for org.fcitx.Fcitx5 '
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if as_user busctl --user status org.fcitx.Fcitx5 >/dev/null 2>&1; then
        echo "ok"
        echo "==> Restarting APPLaunch"
        as_user systemctl --user restart APPLaunch.service || true
        echo "Done. In a text field press Ctrl+Space to switch to Japanese."
        exit 0
    fi
    printf '.'
    sleep 1
done
echo
echo "!!  fcitx5 did not appear on the session bus. Check:"
echo "      journalctl --user -u $unit -b     (as $user)"
exit 1

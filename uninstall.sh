#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Remove the Acer ALG RGB keyboard stack installed by install.sh.
#
# Run with:
#   sudo bash /path/to/alg-rgb/uninstall.sh
#
# User configuration (~/.config/alg-rgb) is intentionally left in place.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(sed -n 's/^#define ALG_RGB_VERSION_STRING "\([^"]*\)"/\1/p' "$ROOT/include/version.h" | head -n1)"
DKMS_NAME="alg-rgb"
EXT_UUID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["uuid"])' "$ROOT/extension/metadata.json" 2>/dev/null || echo 'alg-rgb@chiragk.local')"
SRC_DIR="/usr/src/${DKMS_NAME}-${VERSION}"
EXT_DIR="/usr/share/gnome-shell/extensions/${EXT_UUID}"

log()  { printf '[alg-rgb] %s\n' "$*"; }
warn() { printf '[alg-rgb] WARNING: %s\n' "$*" >&2; }
die()  { printf '[alg-rgb] ERROR: %s\n' "$*" >&2; exit 1; }

[ "${EUID:-$(id -u)}" -eq 0 ] || die "run this uninstaller with sudo: sudo bash $ROOT/uninstall.sh"
[ -n "$VERSION" ] || die "could not read the version from include/version.h"

TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER=""
fi
TARGET_HOME=""
if [ -n "$TARGET_USER" ]; then
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
fi

log "Removing ALG RGB ${VERSION}"

# ---------------------------------------------------------------------------
# Stop user services and disable the extension
# ---------------------------------------------------------------------------

if [ -n "$TARGET_USER" ]; then
    TARGET_UID="$(id -u "$TARGET_USER")"
    RUNTIME_DIR="/run/user/${TARGET_UID}"

    if [ -S "${RUNTIME_DIR}/bus" ] && command -v runuser >/dev/null 2>&1; then
        run_user() {
            runuser -u "$TARGET_USER" -- env \
                HOME="$TARGET_HOME" \
                XDG_RUNTIME_DIR="$RUNTIME_DIR" \
                DBUS_SESSION_BUS_ADDRESS="unix:path=${RUNTIME_DIR}/bus" \
                "$@"
        }

        if command -v gnome-extensions >/dev/null 2>&1; then
            run_user gnome-extensions disable "$EXT_UUID" >/dev/null 2>&1 || true
        fi
        run_user systemctl --user disable --now alg-rgb-web.service >/dev/null 2>&1 || true
    fi
fi

# ---------------------------------------------------------------------------
# System services, module and DKMS
# ---------------------------------------------------------------------------

log "Stopping system services"
systemctl disable --now alg-rgb-restore.service >/dev/null 2>&1 || true

log "Removing kernel module"
modprobe -r alg_rgb >/dev/null 2>&1 || true

log "Removing DKMS module"
dkms remove -m "$DKMS_NAME" -v "$VERSION" --all >/dev/null 2>&1 || true
rm -rf "$SRC_DIR"

# ---------------------------------------------------------------------------
# Installed files
# ---------------------------------------------------------------------------

log "Removing installed files"
rm -f /etc/udev/rules.d/70-alg-rgb.rules
rm -f /etc/udev/rules.d/99-alg-rgb.rules
rm -f /etc/modules-load.d/alg_rgb.conf
rm -f /etc/systemd/system/alg-rgb-restore.service
rm -f /usr/lib/systemd/system-sleep/alg-rgb
rm -f /usr/local/bin/alg-rgb
rm -f /usr/local/bin/alg-rgb-web
rm -rf /usr/local/lib/alg-rgb
rm -f /usr/lib/systemd/user/alg-rgb-web.service
rm -f /usr/share/applications/alg-rgb-dashboard.desktop
rm -f /usr/share/icons/hicolor/scalable/apps/lumen-rgb.svg
rm -rf "$EXT_DIR"
rm -rf /var/lib/alg-rgb

systemctl daemon-reload
udevadm control --reload-rules
udevadm trigger --subsystem-match=misc --action=remove >/dev/null 2>&1 || true

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi

# ---------------------------------------------------------------------------
# Group membership
# ---------------------------------------------------------------------------

if [ -n "$TARGET_USER" ] && getent group alg-rgb >/dev/null 2>&1; then
    gpasswd -d "$TARGET_USER" alg-rgb >/dev/null 2>&1 || true
fi

if getent group alg-rgb >/dev/null 2>&1; then
    MEMBERS="$(getent group alg-rgb | cut -d: -f4)"
    if [ -z "$MEMBERS" ]; then
        groupdel alg-rgb >/dev/null 2>&1 || true
    fi
fi

cat <<EOF

================================================================================
ALG RGB ${VERSION} removed
================================================================================

User configuration was kept at ~/.config/alg-rgb.
A reboot or logout is recommended so the kernel module and GNOME extension
are fully unloaded.  If the keyboard is left in a custom state, the firmware
will retain it until the next power cycle or another RGB command.
================================================================================
EOF

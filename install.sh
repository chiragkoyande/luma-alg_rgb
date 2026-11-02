#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Install the Acer ALG RGB keyboard stack:
#   - DKMS kernel module
#   - alg-rgb CLI
#   - udev / modules-load / systemd integration
#   - GNOME Shell extension
#   - local Lumen RGB Studio dashboard
#
# Run from anywhere:
#   sudo bash /path/to/alg-rgb/install.sh
#
# The installer is idempotent and safe to re-run.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(sed -n 's/^#define ALG_RGB_VERSION_STRING "\([^"]*\)"/\1/p' "$ROOT/include/version.h" | head -n1)"
DKMS_NAME="alg-rgb"
EXT_UUID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["uuid"])' "$ROOT/extension/metadata.json")"
KREL="$(uname -r)"
SRC_DIR="/usr/src/${DKMS_NAME}-${VERSION}"
EXT_DIR="/usr/share/gnome-shell/extensions/${EXT_UUID}"

log()  { printf '[alg-rgb] %s\n' "$*"; }
warn() { printf '[alg-rgb] WARNING: %s\n' "$*" >&2; }
die()  { printf '[alg-rgb] ERROR: %s\n' "$*" >&2; exit 1; }

[ "${EUID:-$(id -u)}" -eq 0 ] || die "run this installer with sudo: sudo bash $ROOT/install.sh"
[ -n "$VERSION" ] || die "could not read the version from include/version.h"
[ -f "$ROOT/kernel/alg_rgb.c" ] || die "kernel source not found under $ROOT"

TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER=""
fi
TARGET_HOME=""
if [ -n "$TARGET_USER" ]; then
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
fi

log "Installing ALG RGB ${VERSION} for kernel ${KREL}"

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

if [ ! -e "/lib/modules/${KREL}/build" ]; then
    if command -v dnf >/dev/null 2>&1; then
        log "Installing kernel headers for ${KREL}"
        dnf install -y "kernel-devel-${KREL}" || die "could not install kernel-devel-${KREL}"
    else
        die "kernel headers for ${KREL} are missing (install kernel-devel)"
    fi
fi

if ! command -v dkms >/dev/null 2>&1; then
    if command -v dnf >/dev/null 2>&1; then
        log "Installing DKMS"
        dnf install -y dkms || die "could not install dkms"
    else
        die "dkms is required but is not installed"
    fi
fi

command -v make >/dev/null 2>&1 || die "make is required"
command -v gcc >/dev/null 2>&1 || die "gcc is required"

# Flask is needed for the dashboard.  Check the way the service will run:
# as the desktop user when possible, otherwise as root.
flask_ok=false
if [ -n "$TARGET_USER" ] && command -v runuser >/dev/null 2>&1; then
    if runuser -u "$TARGET_USER" -- env HOME="$TARGET_HOME" python3 -c 'import flask' >/dev/null 2>&1; then
        flask_ok=true
    fi
elif python3 -c 'import flask' >/dev/null 2>&1; then
    flask_ok=true
fi

if [ "$flask_ok" != true ]; then
    if command -v dnf >/dev/null 2>&1; then
        log "Installing python3-flask for the dashboard"
        dnf install -y python3-flask || warn "could not install python3-flask; the dashboard will not start until Flask is available"
    else
        warn "Flask is missing; install it before using the dashboard"
    fi
fi

# ---------------------------------------------------------------------------
# Build and install the CLI
# ---------------------------------------------------------------------------

log "Building the CLI"
make -C "$ROOT/cli" clean all
install -Dm755 "$ROOT/cli/alg-rgb" /usr/local/bin/alg-rgb

# ---------------------------------------------------------------------------
# Install and build the DKMS module
# ---------------------------------------------------------------------------

log "Installing DKMS source to ${SRC_DIR}"
if [ -d /sys/module/alg_rgb ]; then
    log "Unloading the running alg_rgb module"
    if ! modprobe -r alg_rgb; then
        die "could not unload alg_rgb; close any program using /dev/alg_rgb and run the installer again"
    fi
fi
dkms remove -m "$DKMS_NAME" -v "$VERSION" --all >/dev/null 2>&1 || true
make -C "$ROOT/kernel" clean >/dev/null 2>&1 || true
rm -rf "$SRC_DIR"
install -d "$SRC_DIR"
cp -a "$ROOT/kernel" "$ROOT/include" "$ROOT/dkms.conf" "$ROOT/LICENSE" "$ROOT/README.md" "$SRC_DIR/"
if [ -d "$ROOT/docs" ]; then
    cp -a "$ROOT/docs" "$SRC_DIR/"
fi

log "Registering and building the DKMS module"
dkms add -m "$DKMS_NAME" -v "$VERSION"
dkms build -m "$DKMS_NAME" -v "$VERSION"
dkms install --force -m "$DKMS_NAME" -v "$VERSION"

# ---------------------------------------------------------------------------
# Users, permissions and persistence directories
# ---------------------------------------------------------------------------

log "Creating the alg-rgb group and state directory"
groupadd -f alg-rgb
if [ -n "$TARGET_USER" ]; then
    usermod -aG alg-rgb "$TARGET_USER"
    log "Added ${TARGET_USER} to the alg-rgb group"
fi
install -d -m 2775 -g alg-rgb /var/lib/alg-rgb
chgrp alg-rgb /var/lib/alg-rgb
chmod 2775 /var/lib/alg-rgb
if [ -f /var/lib/alg-rgb/state ]; then
    chgrp alg-rgb /var/lib/alg-rgb/state
    chmod 0664 /var/lib/alg-rgb/state
fi

# ---------------------------------------------------------------------------
# udev, module autoload and systemd units
# ---------------------------------------------------------------------------

log "Installing udev, module autoload and systemd integration"
# Rules must be numbered before 73-seat-late.rules so the uaccess builtin
# sees our tag and applies the seat ACL.
rm -f /etc/udev/rules.d/99-alg-rgb.rules
install -Dm644 "$ROOT/udev/70-alg-rgb.rules" /etc/udev/rules.d/70-alg-rgb.rules
install -Dm644 "$ROOT/modules-load/alg_rgb.conf" /etc/modules-load.d/alg_rgb.conf
install -Dm644 "$ROOT/systemd/alg-rgb-restore.service" /etc/systemd/system/alg-rgb-restore.service
install -Dm755 "$ROOT/systemd/alg-rgb-sleep" /usr/lib/systemd/system-sleep/alg-rgb

systemctl daemon-reload
systemctl enable alg-rgb-restore.service >/dev/null 2>&1 || warn "could not enable alg-rgb-restore.service"

udevadm control --reload-rules
udevadm trigger --subsystem-match=misc --action=add >/dev/null 2>&1 || true

log "Loading the kernel module"
if modprobe alg_rgb; then
    log "alg_rgb loaded"
else
    warn "modprobe alg_rgb failed; run 'sudo dmesg | tail -40' after checking Secure Boot and DKMS status"
fi

for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -e /dev/alg_rgb ] && break
    sleep 0.3
done
if [ -e /dev/alg_rgb ]; then
    chgrp alg-rgb /dev/alg_rgb 2>/dev/null || true
    chmod 0660 /dev/alg_rgb 2>/dev/null || true
    log "Device ready: /dev/alg_rgb"
else
    warn "/dev/alg_rgb was not created; check 'sudo dmesg | tail -40'"
fi

# Apply the saved system state now if a module is present.  The udev add
# event may already have done this; the operation is idempotent.
if [ -e /dev/alg_rgb ]; then
    systemctl start alg-rgb-restore.service >/dev/null 2>&1 || true
fi

# ---------------------------------------------------------------------------
# Dashboard (user service) and desktop entry
# ---------------------------------------------------------------------------

log "Installing the local dashboard"
install -d /usr/local/lib/alg-rgb/web
install -m644 "$ROOT/web/app.py" /usr/local/lib/alg-rgb/web/app.py
rm -rf /usr/local/lib/alg-rgb/web/static
cp -a "$ROOT/web/static" /usr/local/lib/alg-rgb/web/static
install -Dm755 "$ROOT/web/alg-rgb-web.sh" /usr/local/bin/alg-rgb-web

install -Dm644 "$ROOT/systemd/alg-rgb-web.service" /usr/lib/systemd/user/alg-rgb-web.service
install -Dm644 "$ROOT/systemd/alg-rgb-dashboard.desktop" /usr/share/applications/alg-rgb-dashboard.desktop
install -Dm644 "$ROOT/web/static/favicon.svg" /usr/share/icons/hicolor/scalable/apps/lumen-rgb.svg

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi

# ---------------------------------------------------------------------------
# GNOME Shell extension
# ---------------------------------------------------------------------------

log "Installing the GNOME Shell extension (${EXT_UUID})"
rm -rf "$EXT_DIR"
install -d "$EXT_DIR"
cp -a "$ROOT/extension/." "$EXT_DIR/"

# ---------------------------------------------------------------------------
# Start per-user services when a live session bus is available
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

        log "Starting the dashboard user service"
        if run_user systemctl --user daemon-reload; then
            run_user systemctl --user enable --now alg-rgb-web.service \
                || warn "could not start alg-rgb-web.service; run 'systemctl --user start alg-rgb-web' after logging in"
        else
            warn "could not contact the user systemd manager; the dashboard will start at next login"
        fi

        if command -v gnome-extensions >/dev/null 2>&1; then
            log "Enabling the GNOME extension"
            run_user gnome-extensions enable "$EXT_UUID" \
                || warn "could not enable the extension automatically; enable 'ALG RGB' in the Extensions app"
        fi
    else
        warn "no live user session bus for ${TARGET_USER}; the dashboard and extension will be available after login"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

cat <<EOF

================================================================================
ALG RGB ${VERSION} installed
================================================================================

CLI:        /usr/local/bin/alg-rgb
Device:     /dev/alg_rgb $( [ -e /dev/alg_rgb ] && echo '(present)' || echo '(missing)' )
Dashboard:  http://127.0.0.1:8722/
Module:     $(dkms status -m "$DKMS_NAME" -v "$VERSION" 2>/dev/null | head -n1 || echo 'check dkms status')

Try now (as root, or after your seat ACL is applied):
  alg-rgb status
  alg-rgb color cyan 3
  alg-rgb effect wave
  alg-rgb off
  alg-rgb probe

EOF

if [ -n "$TARGET_USER" ]; then
    if id -nG "$TARGET_USER" | grep -qw alg-rgb; then
        cat <<EOF
${TARGET_USER} is a member of the alg-rgb group.  The current graphical
session already receives a logind ACL for /dev/alg_rgb.  For permanent
group access in new terminals, log out and back in (or run: newgrp alg-rgb).

EOF
    fi
    cat <<EOF
If the GNOME extension does not appear immediately, log out and back in.
On Wayland, GNOME Shell only scans newly installed extensions at startup.

EOF
fi

cat <<EOF
The dashboard is local-only (127.0.0.1:8722) and has no authentication.
Per-key RGB is not enabled: HID 048D:8910 is not enumerated, so this stack
controls the verified CLV0001 zone-RGB path only.

If the keyboard does not respond:
  sudo dmesg | tail -40
  sudo dkms status
  alg-rgb probe
================================================================================
EOF

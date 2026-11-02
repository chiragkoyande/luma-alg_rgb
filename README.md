# ALG RGB

Native Linux control for the Acer Aspire Lite **AL15G-52** RGB keyboard.

This project talks directly to the keyboard firmware through Acer's
`CLV0001` ACPI interface. It does not need Windows, Acer Control Center, a
background vendor service, or an RGB suite. The stack includes:

- a small DKMS kernel module;
- an `alg-rgb` command-line tool;
- a GNOME Shell panel extension;
- a local, SaaS-style web dashboard called **Lumen RGB Studio**;
- boot, suspend/resume and udev integration.

The only firmware function used is the verified `_DSM` function `0x67`.
Functions `0x68`-`0x6A` are deliberately not implemented.

---

## Status

| Area | Status |
| --- | --- |
| Zone RGB, colours, off/on | Implemented |
| Brightness (0-4) | Implemented |
| Zones: main, mid, right, light bar, logo | Implemented |
| Firmware effects | Implemented |
| Boot and suspend/resume restore | Implemented |
| CLI, JSON API, GNOME extension | Implemented |
| Local web dashboard | Implemented |
| Per-key RGB | **Not available on this machine** |

Per-key RGB is investigated honestly in [`docs/PERKEY.md`](docs/PERKEY.md).
Acer's Windows per-key stack uses HID `048D:8910`; that controller is not
enumerated on this Linux installation, so only the verified CLV0001
zone-RGB path is exposed. The dashboard and `alg-rgb probe` report this
rather than pretending per-key control works.

---

## Hardware and protocol

The target machine is the Acer Aspire Lite AL15G-52. Linux exposes the
keyboard controller as:

```
ACPI device:  CLV0001:00
Namespace:    \_SB.DCHU
UUID:         93f224e4-fbdc-4bbf-add6-db71bdc0afad
Function:     0x67
Payload:      4 bytes, [green, red, blue, zone/command]
```

The firmware uses **GRB** byte order, not RGB. The kernel driver performs
that conversion so userspace only deals with normal RGB values.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the packet layout, zone
values, brightness steps and effect packets.

---

## Requirements

- Acer ALG-class laptop with the `CLV0001` ACPI device (target: AL15G-52)
- Fedora or a comparable systemd distribution
- Kernel headers matching the running kernel
- `dkms`, `gcc`, `make`
- Python 3 and Flask for the web dashboard
- GNOME Shell 45-50 for the extension

The installer installs `dkms`, missing kernel headers and `python3-flask`
with `dnf` when needed. Secure Boot must allow the DKMS module, or the
module must be signed through your normal MOK flow.

---

## Install

From this directory:

```bash
sudo bash /home/chiragk/cyber/tools/alg-rgb/install.sh
```

or, in general:

```bash
sudo bash ./install.sh
```

The installer is idempotent. It:

1. builds and installs `/usr/local/bin/alg-rgb`;
2. registers, builds and installs the `alg_rgb` DKMS module;
3. creates the `alg-rgb` group and `/var/lib/alg-rgb`;
4. installs the udev rule, module autoload file, restore service and sleep hook;
5. installs the dashboard, launcher and user service;
6. installs and tries to enable the GNOME extension;
7. loads the module and verifies `/dev/alg_rgb`.

The udev rule gives the active seat user a logind ACL (`uaccess`) and
grants group `alg-rgb` access. Add your user to the group for permanent
access in new terminals:

```bash
sudo usermod -aG alg-rgb "$USER"
```

Then log out and back in, or run `newgrp alg-rgb` for the current shell.
The graphical session normally receives the ACL immediately.

---

## CLI

```bash
alg-rgb red                  # named colour
alg-rgb '#22d3ee' 3          # hex colour, brightness 3
alg-rgb color cyan 4 --zone main
alg-rgb brightness 2
alg-rgb effect wave
alg-rgb effect none
alg-rgb off
alg-rgb on
alg-rgb status
alg-rgb status --json
alg-rgb probe
alg-rgb probe --json
alg-rgb list
```

### Colours

`off`, `red`, `orange`, `yellow`, `lime`, `light-green`, `green`,
`green-cyan`, `cyan`, `light-blue`, `blue`, `violet`, `magenta`, `pink`,
`flesh`, `bluish-white`, `white`, or any `#RRGGBB` value.

### Brightness

`0` (off), `1` (low), `2` (medium), `3` (high), `4` (maximum).

### Zones

| Name | Value | Meaning |
| --- | --- | --- |
| `main` / `left` | `0xF0` | Main keyboard zone |
| `mid` | `0xF1` | Middle zone |
| `right` | `0xF2` | Right zone |
| `bar` | `0xF3` | Light bar |
| `logo` | `0xF6` | Logo LED |
| `all` | | Apply the colour to every zone above |

### Effects

`random`, `flash`, `tempo`, `dance`, `wave`, `cycle`, `breath`, and
`none` (static colour). `static` is accepted as an alias for `none`.

### State

Each successful command saves the current state to:

- `~/.config/alg-rgb/state`
- `/var/lib/alg-rgb/state` (when the user has group access)

The systemd restore service and sleep hook use the system state. Manual
control is available too:

```bash
alg-rgb save
alg-rgb restore
sudo alg-rgb save --system
sudo alg-rgb restore --system
alg-rgb save --file /tmp/my-lighting.state
alg-rgb restore --file /tmp/my-lighting.state
```

### JSON output

`--json` makes the CLI suitable for the extension, dashboard and scripts:

```bash
alg-rgb status --json
alg-rgb color '#ff6600' 4 --json
```

---

## Web dashboard

The dashboard is a local Flask application with a dependency-free
frontend. It provides colour picking, brightness, zones, effects, power,
presets, an activity log, system health and a read-only hardware probe.

```bash
# Already running as a user service after install:
systemctl --user status alg-rgb-web
systemctl --user restart alg-rgb-web

# Or start it manually:
alg-rgb-web --open
```

Open <http://127.0.0.1:8722/>.

The service binds to loopback only. There is no authentication because it
is intended for a single-user desktop. Binding to a non-loopback address
requires an explicit flag and should only be done on a trusted network:

```bash
alg-rgb-web --host 0.0.0.0 --allow-lan
```

API endpoints:

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/status` | Current keyboard state and health |
| GET | `/api/health` | Runtime diagnostics |
| GET | `/api/probe` | Read-only hardware probe |
| GET | `/api/activity` | In-memory activity history |
| GET | `/api/meta` | App and protocol metadata |
| POST | `/api/color` | `{ "color": "#22d3ee", "brightness": 3, "zone": "main" }` |
| POST | `/api/brightness` | `{ "brightness": 2 }` |
| POST | `/api/effect` | `{ "effect": "wave" }` |
| POST | `/api/power` | `{ "on": false }` |
| POST | `/api/preset` | Colour + brightness + effect + zone |

The dashboard invokes `/usr/local/bin/alg-rgb`; it does not touch
`/dev/alg_rgb` or ACPI directly.

---

## GNOME extension

The extension is installed system-wide as
`alg-rgb@chiragk.local`. It adds an `input-keyboard-symbolic` panel icon
with:

- 16 colour swatches;
- a brightness slider;
- an effect submenu;
- power on/off;
- status refresh;
- a shortcut to the dashboard.

Enable it from the Extensions app or with:

```bash
gnome-extensions enable alg-rgb@chiragk.local
```

On Wayland, GNOME Shell only scans newly installed extensions at startup,
so log out and back in if the icon does not appear. The extension calls
the same `/usr/local/bin/alg-rgb` CLI as the dashboard.

---

## Safety

- Only `_DSM` function `0x67` is called.
- The four-byte payload is fixed to colour/zone/effect/brightness commands
  captured from the vendor implementation.
- No ACPI fuzzing and no speculative capability probes.
- `_DSM` functions `0x68`, `0x69` and `0x6A` are intentionally absent.
  One of them hard-locked an AL15G-53 during reverse engineering.
- The `probe` command is read-only. It reads sysfs and the HID bus; it does
  not issue unknown firmware calls.
- The web dashboard is loopback-only by default and has no remote auth.

---

## Troubleshooting

### `/dev/alg_rgb` is missing

```bash
sudo dkms status
sudo modprobe alg_rgb
sudo dmesg | tail -40
```

Check that `CLV0001:00` exists:

```bash
ls /sys/bus/acpi/devices/CLV0001:00
```

### Permission denied

```bash
ls -l /dev/alg_rgb
id
getent group alg-rgb
```

The udev rule should set group `alg-rgb`, mode `0660`, and a `uaccess`
ACL. Log out/in if group membership was just added. As a temporary check:

```bash
sudo alg-rgb status
```

If `getfacl /dev/alg_rgb` shows no ACL, verify that the installed rule is
`/etc/udev/rules.d/70-alg-rgb.rules`. An older `99-alg-rgb.rules` runs after
`73-seat-late.rules` and adds the `uaccess` tag too late; re-run the
installer, which removes the old rule and applies the correct one.

### DKMS build fails

```bash
sudo dkms status
sudo dkms build -m alg-rgb -v 1.0.0
```

Make sure `/lib/modules/$(uname -r)/build` points at matching kernel
headers. On Fedora:

```bash
sudo dnf install "kernel-devel-$(uname -r)"
```

### Secure Boot

If `modprobe` fails with a signature error, either enroll a MOK key for
DKMS or disable Secure Boot. Secure Boot is disabled on the development
machine, but this is machine-specific.

### Dashboard does not start

```bash
systemctl --user status alg-rgb-web
journalctl --user -u alg-rgb-web -n 80 --no-pager
python3 -c 'import flask; print(flask.__version__)'
```

If Flask is missing:

```bash
sudo dnf install python3-flask
```

### Extension does not appear

```bash
gnome-extensions list | grep alg-rgb
gnome-extensions info alg-rgb@chiragk.local
journalctl --user -f -o cat /usr/bin/gnome-shell
```

Log out and back in on Wayland after installation.

### The keyboard is left in an odd state

```bash
alg-rgb reset        # white, brightness 2, main zone
# or
alg-rgb off
```

A full power cycle also returns the firmware to its default lighting.

---

## Uninstall

```bash
sudo bash /home/chiragk/cyber/tools/alg-rgb/uninstall.sh
```

This removes the DKMS module, CLI, udev/systemd/udev files, dashboard,
extension and system state. User state in `~/.config/alg-rgb` is kept.

---

## Development

```bash
make -C kernel          # build the module against the running kernel
make -C cli             # build the CLI
python3 web/app.py --port 8722
node --check web/static/app.js
node --check extension/extension.js
python3 -m py_compile web/app.py
```

Project layout:

```
alg-rgb/
├── cli/                 alg-rgb CLI
├── docs/                protocol, per-key and testing notes
├── extension/           GNOME Shell extension
├── include/             shared version and protocol constants
├── kernel/              ACPI misc driver
├── modules-load/        /etc/modules-load.d snippet
├── systemd/             restore, sleep hook, dashboard, desktop entry
├── udev/                device permissions
├── web/                 Flask app and static dashboard
├── dkms.conf
├── install.sh
└── uninstall.sh
```

The kernel driver and CLI are GPL-2.0. See [`LICENSE`](LICENSE).

---

## Credits

The original CLV0001 reverse engineering and driver work came from
[`24kaushik/alg-cli`](https://github.com/24kaushik/alg-cli). This project
adapts that work for the AL15G-52, adds zone/effect/brightness state,
persistence, the web dashboard, the GNOME extension and packaging.

The Acer firmware and Windows driver remain the property of their
respective owners. This is an independent interoperability project.

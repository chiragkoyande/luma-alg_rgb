# Hardware and integration test plan

Run these checks after `sudo bash install.sh`. Stop immediately if the
machine locks up, the keyboard becomes unresponsive, or `dmesg` shows
ACPI errors. The only firmware function exercised is the verified
`_DSM 0x67`; the probe is read-only.

Record the kernel version first:

```bash
uname -r
```

The development machine used `6.17.13-200.fc42.x86_64` with GNOME Shell
49.2 on Wayland and Secure Boot disabled.

---

## 1. Preflight

```bash
# ACPI device should exist with status 15
ls /sys/bus/acpi/devices/CLV0001:00
cat /sys/bus/acpi/devices/CLV0001:00/status

# Module should be loaded
lsmod | grep alg_rgb
dkms status

# Device node and permissions
ls -l /dev/alg_rgb
getfacl /dev/alg_rgb 2>/dev/null || true

# Verified capabilities
cat /sys/class/misc/alg_rgb/capabilities
```

Expected:

- `CLV0001:00` present, status `15`.
- `alg_rgb` loaded.
- `/dev/alg_rgb` is a character device, group `alg-rgb`, mode `0660`,
  with a `uaccess` ACL for the seat user.
- Capabilities mention `function=0x67`, `color_order=GRB`, the five
  zones, the brightness steps and `perkey=unavailable`.

```bash
alg-rgb probe
alg-rgb probe --json
```

Expected: ACPI present, module loaded, device accessible, HID
`048D:8910` not present, and a clear per-key explanation. No firmware
writes occur during `probe`.

---

## 2. CLI basics

```bash
alg-rgb status
alg-rgb list
alg-rgb red
alg-rgb green
alg-rgb blue
alg-rgb white
```

Expected: the keyboard changes colour immediately and `status` reports
the same colour, `power=on`, and the current brightness.

```bash
alg-rgb '#ff6600' 4
alg-rgb '#22d3ee' 1
alg-rgb color '#7c4dff' 2 --zone main
```

Expected: arbitrary hex colours are accepted, brightness visibly changes,
and invalid values are rejected:

```bash
alg-rgb '#12345'          # should fail, not crash
alg-rgb brightness 9      # should fail
alg-rgb effect nope       # should fail
```

Check JSON:

```bash
alg-rgb status --json | python3 -m json.tool
```

Expected keys include `ok`, `power`, `color`, `brightness`, `effect`,
`zone`, `zone_name`, `r`, `g`, `b`.

Check exit codes:

```bash
alg-rgb red; echo $?              # 0
alg-rgb not-a-color; echo $?      # 1
alg-rgb status --json >/dev/null; echo $?  # 0
```

---

## 3. Brightness

```bash
alg-rgb brightness 0
alg-rgb brightness 1
alg-rgb brightness 2
alg-rgb brightness 3
alg-rgb brightness 4
```

Expected: five distinct brightness levels, with `0` dark. `status`
reports the level. Test `alg-rgb off` / `alg-rgb on`:

```bash
alg-rgb off
alg-rgb status        # power=off, brightness=0
alg-rgb on
alg-rgb status        # power=on, previous colour restored
```

The firmware brightness step values are `0x00/0x2F/0x5F/0x8F/0xBF`;
userspace should only ever use `0..4`.

---

## 4. Zones

```bash
alg-rgb color red 3 --zone main
alg-rgb color green 3 --zone mid
alg-rgb color blue 3 --zone right
alg-rgb color cyan 3 --zone bar
alg-rgb color magenta 3 --zone logo
alg-rgb color white 3 --zone all
```

Expected: each zone the hardware actually has responds. Some AL15G-52
units may not have a separate mid/right/light-bar/logo LED; the firmware
may accept the command without a visible change. That is a hardware
limitation, not a CLI failure. `all` should return the default zone to
`main`.

```bash
alg-rgb zone mid
alg-rgb status
alg-rgb zone main
```

Expected: the default zone changes without writing LEDs; subsequent
colour commands use it.

---

## 5. Effects

Test one at a time:

```bash
alg-rgb effect random
alg-rgb effect flash
alg-rgb effect tempo
alg-rgb effect dance
alg-rgb effect wave
alg-rgb effect cycle
alg-rgb effect breath
alg-rgb effect none
```

Expected: each effect is applied through `_DSM 0x67`. Some effects may
look similar or may not be visible on every panel revision. `none`
returns to the last static colour. `status` reports the effect name.

If an effect appears to hang the lighting controller, immediately run:

```bash
alg-rgb effect none
alg-rgb off
```

Do **not** try to "fix" it by calling unknown ACPI functions.

---

## 6. State persistence

### User state

```bash
alg-rgb color '#22d3ee' 3
alg-rgb save
cat ~/.config/alg-rgb/state
alg-rgb color red 1
alg-rgb restore
alg-rgb status       # should be back to #22d3ee, 3
```

### System state

```bash
sudo alg-rgb color '#ff6600' 4
sudo alg-rgb save --system
cat /var/lib/alg-rgb/state
sudo alg-rgb color white 1
sudo alg-rgb restore --system
alg-rgb status       # should be #ff6600, 4
```

Expected: state files contain `power`, `color`, `brightness`, `effect` and
`zone` lines. `restore --system` with no file should be harmless and use
safe defaults.

### Module reload (udev restore)

```bash
sudo modprobe -r alg_rgb
sudo modprobe alg_rgb
sleep 2
alg-rgb status
systemctl status alg-rgb-restore.service --no-pager
```

Expected: the module reloads, udev creates `/dev/alg_rgb`, and the restore
service reapplies the system state.

### Boot

```bash
sudo alg-rgb color '#22d3ee' 3
sudo systemctl start alg-rgb-restore.service
sudo reboot
```

After reboot and login:

```bash
alg-rgb status
```

Expected: the saved colour/brightness are restored. If not, check:

```bash
systemctl status alg-rgb-restore.service --no-pager
journalctl -u alg-rgb-restore.service -b --no-pager
```

### Suspend/resume

```bash
alg-rgb color magenta 3
systemctl suspend
# resume
alg-rgb status
journalctl -b -u systemd-sleep --no-pager | tail -30
```

Expected: the sleep hook saves before suspend and restores after resume.
The kernel module's in-memory state may reset, so the system state file is
the source of truth.

---

## 7. Permissions

As the desktop user (after logout/login if the group was just added):

```bash
id
ls -l /dev/alg_rgb
alg-rgb status
```

Expected: `id` includes `alg-rgb`, and `status` works without `sudo`.
The logind `uaccess` ACL should make it work even before the next login
for the active seat.

Negative check:

```bash
sudo -u nobody alg-rgb status
```

Expected: permission denied. Do not weaken the udev rule to `0666`.

---

## 8. Web dashboard

```bash
systemctl --user status alg-rgb-web --no-pager
ss -ltnp | grep 8722
curl -s http://127.0.0.1:8722/api/health | python3 -m json.tool
```

Expected: the service is active and listening only on `127.0.0.1:8722`.
Open <http://127.0.0.1:8722/> and verify:

- connection pill says Connected;
- keyboard visual matches the current colour/brightness;
- colour picker and hex input apply a colour;
- swatches apply colours;
- brightness slider changes levels;
- zone selector changes the zone;
- each effect card applies an effect;
- Static returns to the chosen colour;
- power switch turns the backlight off/on;
- presets apply and custom presets persist across reload;
- activity list updates;
- per-key lab shows "Zone RGB only";
- "Run hardware probe" returns checks and JSON;
- layout works at desktop and mobile widths.

API checks:

```bash
curl -s http://127.0.0.1:8722/api/status | python3 -m json.tool
curl -s -X POST http://127.0.0.1:8722/api/color \
  -H 'Content-Type: application/json' \
  -d '{"color":"#22d3ee","brightness":3,"zone":"main"}' | python3 -m json.tool
curl -s -X POST http://127.0.0.1:8722/api/brightness \
  -H 'Content-Type: application/json' -d '{"brightness":2}'
curl -s -X POST http://127.0.0.1:8722/api/effect \
  -H 'Content-Type: application/json' -d '{"effect":"wave"}'
curl -s -X POST http://127.0.0.1:8722/api/power \
  -H 'Content-Type: application/json' -d '{"on":false}'
curl -s http://127.0.0.1:8722/api/probe | python3 -m json.tool
```

Validation checks (expect HTTP 400):

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST \
  http://127.0.0.1:8722/api/color \
  -H 'Content-Type: application/json' -d '{"color":"not-a-color"}'

curl -s -o /dev/null -w '%{http_code}\n' -X POST \
  http://127.0.0.1:8722/api/brightness \
  -H 'Content-Type: application/json' -d '{"brightness":9}'
```

LAN refusal:

```bash
alg-rgb-web --host 0.0.0.0
# should exit with "refusing non-loopback bind without --allow-lan"
```

---

## 9. GNOME extension

```bash
gnome-extensions list | grep alg-rgb
gnome-extensions info alg-rgb@chiragk.local
```

Expected: UUID `alg-rgb@chiragk.local`, enabled. Click the keyboard icon
and verify:

- status line shows current colour/brightness;
- colour swatches apply;
- slider changes brightness;
- effect submenu applies effects;
- Turn Off / Turn On works;
- Open dashboard opens the web UI.

On Wayland, log out/in after installation if the extension is not found.
Shell errors:

```bash
journalctl --user -f -o cat /usr/bin/gnome-shell
```

---

## 10. Reboot and reinstall

```bash
sudo reboot
```

After login, verify CLI, dashboard and extension again. Then test the
installer's idempotence:

```bash
sudo bash install.sh
```

Expected: no duplicate DKMS registration, no errors, module reloads, and
the keyboard responds.

Finally, test the uninstaller in a maintenance window:

```bash
sudo bash uninstall.sh
ls /dev/alg_rgb                 # should be gone
dkms status | grep alg-rgb      # should be empty
```

Reinstall afterward if needed.

---

## 11. Failure handling

| Symptom | Check | Action |
| --- | --- | --- |
| No `/dev/alg_rgb` | `sudo dmesg \| tail -40`, `dkms status` | Rebuild DKMS, check headers, Secure Boot |
| Permission denied | `ls -l`, `id`, `getfacl` | Log out/in, check udev rule |
| No ACPI device | `ls /sys/bus/acpi/devices/CLV0001:00` | Unsupported model or firmware |
| Module loads but writes fail | `dmesg`, `alg-rgb probe` | ACPI handle not found; do not fuzz |
| Effect hangs lighting | `alg-rgb effect none`, `alg-rgb off` | Power cycle; report firmware behaviour |
| Dashboard 502 | `journalctl --user -u alg-rgb-web` | Check CLI path and `/dev/alg_rgb` access |
| Extension missing | `gnome-extensions list` | Log out/in on Wayland |

Do not use `_DSM 0x68`-`0x6A` as a debugging tool.

# Changelog

All notable changes to this project are documented here.

## 1.0.0 - 2026-11-02

First packaged release for the Acer Aspire Lite AL15G-52.

### Added

- Kernel module for the Acer `CLV0001` ACPI RGB keyboard interface.
  - Verified `_DSM` function `0x67`, UUID
    `93f224e4-fbdc-4bbf-add6-db71bdc0afad`.
  - GRB colour packets, zone selection, mapped brightness steps and the
    captured firmware effect packets.
  - `/dev/alg_rgb` command device plus read-only `state` and
    `capabilities` sysfs attributes.
  - Deliberate exclusion of `_DSM` functions `0x68`-`0x6A`.
- `alg-rgb` command-line utility.
  - Named colours, `#RRGGBB` colours, brightness, zones, effects and
    power control.
  - `status`, `probe`, `list`, `save` and `restore` commands.
  - JSON output for the GNOME extension and dashboard.
  - User and system state persistence for boot and resume restore.
- DKMS packaging and an idempotent installer/uninstaller.
- systemd boot restore service and suspend/resume hook.
- udev rule for device permissions and restore-on-add.
- GNOME Shell extension with colour swatches, brightness slider, effects,
  power control and dashboard shortcut.
- Local Flask dashboard (`Lumen RGB Studio`) with a dependency-free
  frontend, presets, activity log and read-only hardware probe.
- Documentation for the protocol, per-key investigation, testing and
  troubleshooting.

### Fixed

- The kernel now treats a non-NULL `_DSM` `0x67` result as delivered, even
  when the AL15G-52 returns a non-zero vendor status. Previously a
  successful red command reported `EIO` even though the keyboard changed.
- Failed writes now roll back the driver's in-memory state instead of
  reporting a colour that was never applied.
- The udev rule is installed as `70-alg-rgb.rules`, before
  `73-seat-late.rules`, so the `uaccess` ACL is actually applied on first
  device creation. The old `99-alg-rgb.rules` is removed during upgrade.
- Brightness commands now send the firmware step values
  (`0x00/0x2F/0x5F/0x8F/0xBF`) instead of the user-facing 0-4 index.
- State-file directory creation handles paths without a slash and
  absolute paths correctly.

### Known limitations

- Per-key RGB is not implemented. The Windows per-key stack uses HID
  `048D:8910`, which is not enumerated on this Linux installation.
- Only the zone-RGB ACPI path is exposed. No unverified firmware
  functions are called.
- The web dashboard is local-only and unauthenticated; use
  `--allow-lan` only on a trusted network.

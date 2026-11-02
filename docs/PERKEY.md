# Per-key RGB investigation

## Short answer

Per-key RGB is **not implemented** and is **not available on the current
Linux installation**. The keyboard works through the verified CLV0001
zone-RGB ACPI path. The per-key controller that Acer's Windows software
uses is HID device `048D:8910` (an ITE 829x-class controller), and that
device is not enumerated on this machine.

This is not a missing feature flag in `alg-rgb`. Linux cannot see the
transport that would carry per-key reports.

## Evidence

### Windows stack

Static analysis of Acer's Windows keyboard components found that the
per-key path uses a HID device with:

```
Vendor ID:   0x048D  (ITE Tech)
Product ID:  0x8910
Usage:       per-key RGB keyboard
```

The Windows `perkey_api.dll` communicates with that HID interface rather
than the CLV0001 ACPI zone interface. The ITE 829x family is also
supported by OpenRGB's per-key keyboard driver, which uses 6-byte HID
feature reports with report ID `0xCC` and LED IDs encoded as:

```
((row & 0x07) << 5) | (col & 0x1F)
```

OpenRGB's merge request for ITE 829x PID `0x8910` documents direct mode,
firmware effects (wave, breathing, flashing, random, scan, snake,
spectrum cycle, rainbow wave, Fn highlight), brightness and speed
control. That is the protocol family a future per-key implementation
would need to speak.

### Linux enumeration

On the AL15G-52, `/sys/bus/hid/devices` contains only the FocalTech
touchpad:

```
0018:2808:0101.0001
```

There is no `048D:8910` entry, and no USB device with VID `048D`, PID
`8910` appears in `/sys/bus/usb/devices`. `alg-rgb probe` and the web
dashboard report this explicitly.

### ACPI type hints (static only)

The vendor ACPI stack contains a keyboard-type query:

```
_DSM 70  ->  _DSM 13
```

A byte in the WMI13 response (offset 15) appears to select a keyboard
type. The static mapping observed in the vendor code is:

| Type values | Interpretation |
| --- | --- |
| `3`, `19`, `35`, `51`, `0xF3` | Per-key capable |
| `2`, `6`, `22` | Zone RGB |

Capability bits were also mapped statically:

- `_DSM 96`, bit 8;
- `_DSM 122`, bits 12, 14 and 18.

**None of those queries are implemented in this project.** They are
unverified firmware calls outside the known-safe `0x67` path. The mapping
is recorded only as research context. We do not know the AL15G-52's
actual type/capability response because we have not issued those calls.

## Why the HID device might be absent

Possible explanations, in rough order of likelihood:

1. **The AL15G-52 uses zone RGB, not per-key RGB.** The Windows driver
   package may contain per-key code for other ALG models, while this
   panel is a zone-lit keyboard. The static presence of the DLL is not
   proof that this exact machine has per-key LEDs.
2. **The ITE controller is present but its HID interface is not
   published** in the current firmware configuration. Some vendors hide
   or re-enumerate the interface depending on a BIOS setting, boot mode
   or ACPI mode switch.
3. **The controller is connected through a path Linux does not
   enumerate** (for example, a firmware-owned HID interface that is not
   exposed to the OS).
4. **The device is present only after a vendor-specific initialization**
   that we have not performed. We will not guess at that initialization:
   the ACPI functions that could trigger it include the unsafe
   `0x68`-`0x6A` range.

There is no safe userspace action that can conjure a HID device into
existence.

## What was deliberately not done

- No calls to `_DSM 0x68`, `0x69` or `0x6A`.
- No ACPI fuzzing.
- No writes to unknown ACPI objects.
- No forcing or unbinding of unrelated HID devices.
- No claim that "per-key should work" without the HID transport.

The probe in the CLI and dashboard is read-only: it inspects sysfs and
the HID/USB buses. It does not call firmware.

## If you want to continue the investigation

Safe next steps:

1. Run `alg-rgb probe --json` and save the output.
2. Check the USB and HID trees:
   ```bash
   lsusb -t
   ls /sys/bus/usb/devices
   ls /sys/bus/hid/devices
   ```
3. Check whether the ITE device appears in Windows on the same machine
   (Device Manager -> Human Interface Devices, hardware IDs). If Windows
   does not see `VID_048D&PID_8910` either, the hardware is not per-key.
4. Compare a Windows USB capture of the per-key controller with the
   OpenRGB ITE 829x implementation. A capture is useful only if the
   device is actually present.
5. If the HID device appears later, implement per-key control through
   HID feature reports, not through new ACPI functions. OpenRGB's ITE
   829x driver is the appropriate reference.

Unsafe next steps to avoid:

- Calling `_DSM 0x68`-`0x6A` to "see what happens".
- Fuzzing the CLV0001 UUID with random functions.
- Writing arbitrary packages to the ACPI device.
- Assuming the Windows DLL's presence means this panel has per-key LEDs.

## Current interface contract

The CLI and dashboard expose only zone RGB. `alg-rgb probe` returns:

```json
{
  "perkey": {
    "available": false,
    "hid_id": "048D:8910",
    "reason": "HID 048D:8910 (ITE 829x per-key controller) is not enumerated. Only the CLV0001 zone-RGB path is exposed."
  }
}
```

If the HID device is ever detected, the reason changes to say that the
transport is present but no verified per-key protocol is implemented.
The tool will not silently pretend per-key output is available.

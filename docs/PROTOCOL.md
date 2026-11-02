# CLV0001 RGB protocol notes

This document records the ACPI interface used by the AL15G-52 keyboard and
the subset implemented by `alg_rgb`. It is a reverse-engineering reference,
not a firmware specification.

## Device

| Item | Value |
| --- | --- |
| ACPI HID | `CLV0001` |
| Linux device | `CLV0001:00` |
| ACPI namespace | `\_SB.DCHU` (Linux: `\_SB_.DCHU`) |
| Purpose | Acer DCHU keyboard/lighting control |

Linux ACPI status on the development machine is `15` (present, enabled,
functional).

## `_DSM` call

```
UUID:     93f224e4-fbdc-4bbf-add6-db71bdc0afad
Revision: 0
Function: 0x67
```

The function-specific argument is an ACPI package containing one
four-byte buffer:

```
Byte 0   green   0x00-0xFF
Byte 1   red     0x00-0xFF
Byte 2   blue    0x00-0xFF
Byte 3   zone or command
```

The colour order is **GRB**. For example, pure red is `00 FF 00 F0`, not
`FF 00 00 F0`.

The kernel passes the package through `acpi_evaluate_dsm()`. A NULL result
means the AML method could not be evaluated, and the write fails. A
non-NULL result — including the non-zero vendor status returned by the
AL15G-52 — is treated as delivered. The AL15G-52 was observed to apply a
red packet while returning a non-zero integer status. The driver never calls
functions outside the verified subset.

## Zones

| Value | Name | Notes |
| --- | --- | --- |
| `0xF0` | main / left | Main keyboard lighting; the default |
| `0xF1` | mid | Middle keyboard zone |
| `0xF2` | right | Right keyboard zone |
| `0xF3` | light bar | Chassis light bar |
| `0xF4` | brightness | Special command, not a colour zone |
| `0xF6` | logo | Logo LED |

`0xF0` is aliased as both `main` and `left` in the CLI. On the AL15G-52
the main zone is the practical lighting surface; the mid/right zones may
be absent or may address the same physical LEDs depending on the model.
The CLI exposes them because the firmware accepts them.

## Colour packets

| Colour | Packet |
| --- | --- |
| Red | `00 FF 00 F0` |
| Green | `FF 00 00 F0` |
| Blue | `00 00 FF F0` |
| Yellow | `FF FF 00 F0` |
| Cyan | `FF 00 C8 F0` |
| White | `FF FF FF F0` |
| Off | `00 00 00 F0` |

The same packets are valid for the other zones by replacing byte 3.

## Brightness

Brightness is a separate command with zone byte `0xF4`:

```
[step, 0x00, 0x00, 0xF4]
```

The firmware steps are:

| Level | Payload byte |
| --- | --- |
| 0 (off) | `0x00` |
| 1 (low) | `0x2F` |
| 2 (medium) | `0x5F` |
| 3 (high) | `0x8F` |
| 4 (maximum) | `0xBF` |

The userspace interface uses the 0-4 index; the kernel maps it to the
firmware step. Sending the index directly is wrong.

The driver sends the colour/effect first and the brightness command
afterwards. `off` first blacks the current zone, then sends step `0x00`.

## Effects

All effect packets use the same `_DSM 0x67` call. They were captured from
Acer's Windows keyboard module.

| Effect | Packet | Behaviour |
| --- | --- | --- |
| Random | `00 00 00 70` | Random colour changes |
| Flash | `00 00 00 A0` | Flashing keys |
| Tempo | `00 00 00 90` | Tempo/beat animation |
| Dance | `00 00 00 80` | Dancing pattern |
| Wave | `00 00 00 B0` | Wave across the keyboard |
| Cycle | `00 00 01 33` | Spectrum cycle |
| Breath | `00 A0 02 10` | Breathing fade |
| None | colour packet | Static colour |

The `cycle` and `breath` packets are not simple zone commands; their
fourth byte is part of the effect parameter. The driver treats them as
opaque captured packets and does not reinterpret them.

Effects and static colour are mutually exclusive in the driver state.
Selecting a colour clears the effect. Selecting `none` or `static`
re-sends the last colour.

## Power

There is no dedicated "power" ACPI command in the verified subset. The
driver implements power as:

- **off**: black colour packet for the current zone, then brightness `0`;
- **on**: re-send the saved colour/effect, then the saved brightness.

This keeps the firmware in a known state and avoids unverified power
functions. The trade-off is that `on` restores the driver's saved state,
not necessarily a state changed by another OS or by firmware shortcuts.

## State model

The kernel keeps only a small in-memory snapshot:

```
r, g, b        last static colour (0-255 RGB)
brightness     0-4
zone           current default zone
power          on/off
effect         pointer to a captured effect packet or NULL
```

It deliberately does **not** persist anything. Persistence is handled in
userspace by the CLI so that the ACPI surface stays small and auditable.

## What is not implemented

`_DSM` functions `0x68`, `0x69` and `0x6A` exist in the vendor stack but
are not implemented. During reverse engineering, a call in that range
hard-locked an AL15G-53. They are treated as unsafe until their semantics
are fully understood. No fuzzing or speculative capability probes are
performed.

The per-key path is separate and is documented in
[`PERKEY.md`](PERKEY.md).

## Linux interface

The driver exposes:

```
/dev/alg_rgb
/sys/class/misc/alg_rgb/state
/sys/class/misc/alg_rgb/capabilities
```

`/dev/alg_rgb` is line-oriented:

```
color R G B [ZONE]
brightness N
effect NAME
off
on
zone ZONE
reset
status
```

Reads return the state snapshot:

```
version=1.0.0;power=on;color=#ffffff;brightness=2;effect=none;zone=0xf0
```

The sysfs `state` attribute exposes the same snapshot read-only. The
`capabilities` attribute records the verified protocol facts and the
per-key limitation.

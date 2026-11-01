# xonesxs-winxpdrv

A Windows XP driver for wired **Xbox One** and **Xbox Series X|S** controllers.

Microsoft never released Xbox One controller support for XP. This driver adds it for XP SP2 and later, on both **32-bit (x86)** and **XP Professional x64**. The controller shows up two ways, like an Xbox 360 pad does on XP:

* As a standard **HID game controller**: it appears in Control Panel → Game Controllers and works in any **DirectInput** game.
* As an **XInput** controller: it works in games that use `xinput*.dll` (`xinput1_3`, `xinput9_1_0`, …).

> **Unofficial project.** It isn't affiliated with or endorsed by Microsoft. "Xbox" is a Microsoft trademark. This is an unsigned kernel-mode driver, so use it at your own risk and keep a restore point.

## Status

| | |
|---|---|
| Tested hardware | Xbox Series X\|S controller (`045E:0B12`, firmware 5.17) on a real XP SP3 laptop (Intel USB 2.0) |
| Driver install and load | ✅ Works |
| Buttons, sticks, triggers, D-pad in Game Controllers | ✅ Works (r05) once the pad is switched on with the Xbox button |
| Powering on by itself when plugged in | 🧪 r06 adds Windows' identify handshake for this; not yet tested on hardware |
| XInput interface | ✅ Detected by `xusbtest`; input and rumble through XInput not yet tested on hardware |
| Rumble | 🧪 Implemented; not yet tested on hardware |
| Xbox Wireless Adapter / Bluetooth | ❌ Not supported (wired USB only) |

See [CHANGELOG.md](CHANGELOG.md) for what changed in each release and what was found along the way.

## Supported controllers

| Hardware ID | Controller |
|---|---|
| `USB\VID_045E&PID_02D1` | Xbox One (original) |
| `USB\VID_045E&PID_02DD` | Xbox One (2015 firmware) |
| `USB\VID_045E&PID_02E3` | Xbox One Elite |
| `USB\VID_045E&PID_02EA` | Xbox One S |
| `USB\VID_045E&PID_0B00` | Xbox Elite Series 2 |
| `USB\VID_045E&PID_0B12` | Xbox Series X\|S (tested) |

All of these use the same GIP protocol over USB. Only the Series X|S pad has been tested so far.

## Installing on XP

You'll need **Windows XP SP2 or later** (the KMDF 1.9 runtime requires it) and a **data** USB cable, not a charge-only one.

1. Download the newest release from the [Releases page](https://github.com/dalves408/xonesxs-winxpdrv/releases), or [build it yourself](#building). Copy the `x86` folder to the XP PC (`amd64` for XP x64). The `-chk` zips are debug builds that log to DebugView.
2. Plug in the controller. In the **Found New Hardware** wizard, choose **Install from a list or specific location** and point it at the folder.
   If XP already installed it as an unknown device, use Device Manager → right-click → **Update Driver** → **Don't search, I will choose** → **Have Disk**.
3. Accept the "has not passed Windows Logo testing" warning. XP doesn't require signed drivers on either x86 or x64.
4. Open **Control Panel → Game Controllers**. "Xbox Series X|S Controller (GIP)" should be listed. Use **Properties** to test it.

For XInput games, install the **DirectX 9.0c (June 2010) runtime**, which provides `xinput1_3.dll`.

**If XP blue-screens on boot after installing:** start in **Safe Mode** (F8 at boot), uninstall the controller in Device Manager, and reboot, or use System Restore.

## Controller mapping

### DirectInput (HID)

| HID | Controller |
|---|---|
| X / Y | Left stick |
| Rx / Ry | Right stick |
| Z / Rz | Left / right trigger (separate axes, 0–1023) |
| Hat switch | D-pad |
| Buttons 1–12 | A, B, X, Y, LB, RB, View, Menu, LS, RS, Xbox (Guide), Share |
| Output report 2 | Rumble: left, right, left trigger, right trigger (0–100 each) |

### XInput

Standard `XINPUT_GAMEPAD` layout. The Guide button comes through `XInputGetStateEx` (bit `0x0400`), and the left/right motors are mapped to the pad's main rumble motors.

## How it works

```
 DirectInput / joy.cpl                          XInput games (xinput1_3.dll etc.)
        │                                               │  XUSB IOCTLs
 HID game controller (input.inf)                XboxGip\XUsbInterface (raw PDO)
        │   ← HID child PDO from hidclass               │   ← child of xboxgip.sys
 hidclass.sys + xgiphid.sys   (FDO)                     │
        │   every IRP passed straight down              │
 xboxgip.sys                  (lower filter)  ◄─────────┘
        │   KMDF 1.9: USB + GIP + HID IOCTLs + XUSB IOCTLs
 usbhub PDO  USB\VID_045E&PID_xxxx
```

* **`xgiphid.sys`** (`xgiphid/`) is a tiny WDM HID minidriver shim. `hidclass.sys` takes over a minidriver's dispatch table, which a KMDF driver can't allow, so this shim registers with hidclass and passes every IRP down. Windows 7 ships `mshidkmdf.sys` for this job; XP doesn't.
* **`xboxgip.sys`** (`xboxgip/`) is a KMDF 1.9 lower filter that does the real work:
  * `usb.c`: selects the configuration, then **switches interface 0 to alternate setting 1** (as Windows does; on setting 0 the pad stays silent). It finds the interrupt pipes, runs a continuous reader and sends packets asynchronously.
  * `gip.c`: the GIP protocol. It handles announce → identify (chunked, ACKed) → power-on, LED and auth-done, then parses input and sends rumble. Startup follows what Windows sends on the wire and the [xone](https://github.com/medusalix/xone) driver; packet layouts follow Linux [xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c).
  * `hid.c`: the HID report descriptor and the `IOCTL_HID_*` handlers.
  * `xinput.c`: creates a raw child device that registers the XUSB interface `{EC87F1E3-C13B-4100-B5F7-8B84D54260CB}` and answers the IOCTLs that Microsoft's Xbox 360 driver would. The INF installs it with a null driver, so it needs no extra setup.
* **`xusbtest.exe`** (`tools/xusbtest/`) is a console tool that talks to any XUSB (XInput) device interface. It works against this driver on XP and against Microsoft's drivers on Windows 7–11.

### XInput (XUSB) protocol

The XUSB protocol is undocumented. The IOCTL codes and buffer layouts come from the reverse-engineered [OpenXinput](https://github.com/Nemirtingas/OpenXinput) and [SharpXusb](https://github.com/TheNathannator/SharpXusb) projects. They were checked field by field against Microsoft's own XUSB stack on Windows 11 with `xusbtest`.

The driver reports **XUSB 1.0**, the oldest version, so every XInput DLL on XP can use it. With 1.0, XInput fills in capabilities and battery information itself, so only three IOCTLs are needed:

| IOCTL | Code | Notes |
|---|---|---|
| GET_INFORMATION | `0x80006000` | 12 bytes: version, slot count, VID/PID |
| GET_GAMEPAD_STATE | `0x8000E00C` | 1-byte request → 20-byte 1.0 state; 3-byte request → 29-byte 1.1 state |
| SET_GAMEPAD_STATE | `0x8000A010` | Rumble; LED (player number) requests are accepted and ignored |

## Building

You need the **WDK 7.1.0** (`7600.16385.1`), the last Windows Driver Kit that targets XP. It's the free ISO `GRMWDK_EN_7600_1.ISO` from Microsoft (SHA-1 `DE6ABDB8EB4E08942ADD4AA270C763ED4E3D8242`). Install it to `C:\WinDDK\7600.16385.1`, or pass its path to `build.cmd`.

<details>
<summary>Installing the WDK without KitSetup</summary>

On a modern Windows PC you can lay the kit out with administrative extracts instead of running its installer. Use `msiexec /a <msi> /qn TARGETDIR=<dir>` on these packages from the ISO's `WDK\` folder:

`buildtools_x86fre`, `headers`, `libs_x86fre`, `libs_x64fre`, `wxplibs_x86fre`, `wnetlibs_x64fre`, `wcoinstallers`, `tools_x86fre`, `drvtools_x86fre`, `wdftools_x86fre`, `generaltools_x86fre`

The files land in two dated folders under `<dir>\WinDDK\`; merge both into `C:\WinDDK\7600.16385.1`. OACR (code analysis) isn't needed, and `build.cmd` turns it off.
</details>

Then run:

```bat
build.cmd
```

Each run creates the next numbered release folder, which is never overwritten:

```
releases\rNN\x86\        XP 32-bit
releases\rNN\x86-chk\    XP 32-bit, debug build with logging
releases\rNN\amd64\      XP x64
releases\rNN\amd64-chk\  XP x64, debug build with logging
```

Each folder contains `xboxgip.inf`, `xboxgip.sys`, `xgiphid.sys`, `WdfCoInstaller01009.dll`, `xusbtest.exe` and symbols (`.pdb`). The revision number is compiled into the driver and stamped into the INF version (`0.1.NN.0`).

The project path may contain spaces: `build.cmd` maps it to a drive letter for the duration of the build, because WDK 7.1's `build.exe` can't handle spaces.

## Testing and diagnostics

**`xusbtest`** (in every release folder):

```bat
xusbtest            :: list XUSB interfaces, device info, one state dump
xusbtest poll 10    :: print state changes for 10 seconds
xusbtest state10    :: force the XUSB 1.0 request format
xusbtest rumble     :: run both motors for one second
```

**`rumbletest`** (in every release folder) is an interactive test of all four rumble motors. Hold a button to pick a motor, and pull either trigger to set the strength:

| Button | Motor |
|---|---|
| A | Left grip motor (heavy, low frequency) |
| B | Right grip motor (light, high frequency) |
| X | Left trigger impulse motor |
| Y | Right trigger impulse motor |
| LB | All four |
| RB | Both grip motors, pulsing on and off |
| View | Switch route: HID output report (all four motors) ↔ XInput (grip motors only) |
| Menu | Quit |

**Driver logs:** the `-chk` builds log every step: USB setup, every GIP packet sent (`->`) and received (`<-`), the startup handshake, and the XInput requests. On XP you can read them with **Sysinternals DebugView 4.81** (the last version that runs on XP) with *Capture → Capture Kernel* enabled. No debugger cable is needed. The first line shows the running revision: `xboxgip: revision NN`.

**Kernel debugging:** add `/debug /debugport=COM1 /baudrate=115200` to `boot.ini`, connect a null-modem cable, and run `windbg -k com:port=COMx,baud=115200` on another PC.

**Watching Windows' own handshake:** [`trace/capture-usb.ps1`](trace/capture-usb.ps1) records a USB trace on a modern Windows PC while the controller is plugged in (run it from an Administrator PowerShell). That trace is how the alternate-setting requirement and the identify step were found.

## Roadmap

- [ ] Confirm automatic power-on (r06) on hardware
- [ ] Confirm XInput input and rumble with a real game on XP
- [ ] Confirm the Share button position on the Series X|S pad
- [ ] Elite Series 2 paddles
- [ ] Battery/status reporting (GIP `0x03`)

## Credits

This driver stands on reverse-engineering work by others:

* [xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c) (Linux kernel): GIP input, rumble and init packet layouts.
* [xone](https://github.com/medusalix/xone) by medusalix: GIP framing, chunked messages, acknowledgements and the identify handshake.
* [OpenXinput](https://github.com/Nemirtingas/OpenXinput) and [SharpXusb](https://github.com/TheNathannator/SharpXusb): the XUSB (XInput) IOCTL interface.

No code was copied from these projects; the driver is an independent implementation for the Windows XP driver model.

## License

Copyright (C) 2026 dalves408

This program is free software. You can redistribute it and/or modify it under the terms of the **GNU General Public License, version 2 only** (`GPL-2.0-only`), as published by the Free Software Foundation. See [LICENSE](LICENSE) for the full text.

It is distributed in the hope that it will be useful, but **without any warranty**, without even the implied warranty of merchantability or fitness for a particular purpose.

`WdfCoInstaller01009.dll` in the release packages isn't covered by this license. It is Microsoft's KMDF co-installer, redistributed under the WDK 7.1 redistribution terms.

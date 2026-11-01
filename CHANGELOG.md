# Changelog

Each release is built into its own folder, `releases\rNN\`, and is never overwritten. Every release folder contains:

| Folder | Contents |
|---|---|
| `x86` | XP 32-bit release build |
| `x86-chk` | XP 32-bit debug build, logs to DebugView |
| `amd64` | XP x64 release build |
| `amd64-chk` | XP x64 debug build |

The debug build's first log line is `xboxgip: revision N`. The INF version is `0.1.N.0`.

## r08 (2026-11-01): Windows-style USB setup, rumbletest strength inverted, power-on findings

**Hardware-tested on XP** (Xbox Series X|S controller, HP laptop, XP SP3):
* Buttons, sticks, triggers and D-pad work through DirectInput (Game Controllers) and XInput.
* All four rumble motors work through both the HID and the XInput route.
* **Known limitation:** the controller does **not** power on by itself on XP. Plug it in, then **press the Xbox button once**; after that the driver completes the startup automatically.

### Driver

* **USB setup now copies what Windows 11 sends,** taken from a USB trace of Windows 11 with the same controller:
  1. Ask for the Microsoft OS descriptors (the `0xEE` string and the `XGIP10` compat ID) and GET_STATUS before SET_CONFIGURATION, as Windows 11 does on every connection. XP itself only asks once per device model.
  2. Switch interface 0 to alternate setting 1, then send **SET_INTERFACE(1, 0)** and **SET_INTERFACE(2, 0)**. KMDF skips those because the interfaces are already on setting 0, so the driver sends the standard requests itself.
  3. Reset the port and repeat the setup, as Windows does (and as xone does when attaching): check interface 0 is back on setting 1, repeat the SET_INTERFACE requests and OS queries, read the string descriptors, and reset both GIP pipes.
* **Nothing is sent before the controller announces itself**, matching Windows. The blind power-on attempts from r05–r07 never woke the pad.
* **Optional Linux xpad startup** behind a compile-time switch, `XGIP_XPAD_MODE` in `xboxgip.h`; it's off by default. It leaves interface 0 on setting 0 (4 ms input polling instead of 2 ms) and sends power-on straight away, as xpad does. It may help older pads; it didn't wake the Series X|S pad either.

### Power-on investigation

Debug logs on XP showed the startup handshake works perfectly once the controller announces itself: identify request → chunked identify reply with ACKs → power-on, LED, auth-done → status heartbeat. But the controller only announces itself when its Xbox button is pressed. On Windows 11 it announces 0.3 s after setup. Unreleased test builds tried, in turn:

| Test build | What was added | Result |
|---|---|---|
| 9 | SET_INTERFACE(1, 0) and (2, 0) | Accepted, pad stayed off |
| 10 | Port reset and full reconfiguration | Accepted, pad stayed off |
| 11 | MS OS descriptor queries, GET_STATUS, strings, no blind init | Accepted, pad stayed off |
| 12 | Linux xpad startup (setting 0, immediate power-on) | Pad stayed off |

Every request Windows 11 and Linux xpad send has now been reproduced on XP, so the remaining difference is likely outside what a driver controls, for example the USB host controller hardware (USB 3 xHCI on the Windows 11 PC, USB 2.0 on the XP laptop).

### rumbletest

* A held button now runs its motor at **100 % with no trigger input**, and pulling either trigger **turns it down**; fully pulled is 0 %.

## r07 (2026-11-01): rumble test tool

The driver is unchanged from r06.

* New **`rumbletest.exe`**: an interactive test of all four rumble motors. Hold a button to pick a motor and pull either trigger to set the strength:
  * **A:** left (heavy) motor
  * **B:** right (light) motor
  * **X:** left trigger motor
  * **Y:** right trigger motor
  * **LB:** all four
  * **RB:** main motors pulsing
  * **View:** switch between the HID route (all four motors) and the XInput route (main motors only)
  * **Menu:** quit
* `LICENSE` (GPL-2.0-only) is now included in every release folder.

## r06 (2026-11-01): power on by itself, Windows-style startup

r05 on XP: the pad worked (buttons and sticks correct) but only after pressing the Xbox button. Windows 11 powers it on automatically.

* **GIP sequence numbers never use 0.** xone notes they are "always greater than zero". In r05 the power-on packet was the one numbered 0, which the pad likely dropped.
* **Identify handshake, as Windows does it:** announce → identify request (4 bytes) → identify reply in chunks, each ACKed with the bytes received and the bytes remaining → power-on, LED, auth-done. The packet sizes match the Windows 11 trace (4, 13…, 5, 7, 6).
* Proper GIP framing: varint lengths, chunked messages, and several packets in one USB transfer.
* Retries continue until real input (or the Guide button) arrives, not just any packet. This covers the identify step stalling.
* The Guide button reads both low bits, as xpad does.

## r05 (2026-11-01): select alternate setting 1

* **Fix:** switch the GIP interface (interface 0) to **alternate setting 1** after SET_CONFIGURATION, as Windows does. A USB trace of Windows 11 showed this. (Later testing, r08, showed the r02/r03 silence was really the pad waiting for its Xbox button; setting 1 matches Windows and gives 2 ms input polling.)
* Wait for the pad's announce before sending the init sequence, like Windows. A 1-second timer sends it anyway if nothing arrives.
* ACK every packet that requests it, not just Guide.
* Removed the r04 port reset.
* First release with the revision number stamped into the driver and INF.

## r04 (not tested, binaries not kept)

* Reset the USB port before configuring, as the xone Linux driver does. Superseded by r05.

## r03 (tested on XP: pad still silent, binaries not kept)

* Init sequence matches Linux `xpad`: power-on → LED → **auth-done**. Packets are numbered from 0 and sent one at a time.
* Retry the sequence every second, up to 10 times, until the pad answers.
* Share button read from `len - 26` (xpad `MAP_SHARE_OFFSET` for 0x0B12).
* The log lists every alternate setting and endpoint.

## r02 (tested on XP: pad silent, binaries not kept)

* Added the debug (`chk`) builds with USB/GIP logging for DebugView.

## r01 (tested on XP: driver loads, no input, binaries not kept)

* First version: HID game controller via xgiphid.sys + xboxgip.sys, XInput (XUSB 1.0) raw PDO, `xusbtest` tool.

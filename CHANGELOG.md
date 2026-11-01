# Changelog

Each release is built into its own folder, `releases\rNN\`, and is never overwritten. Every release folder contains:

| Folder | Contents |
|---|---|
| `x86` | XP 32-bit release build |
| `x86-chk` | XP 32-bit debug build, logs to DebugView |
| `amd64` | XP x64 release build |
| `amd64-chk` | XP x64 debug build |

The debug build's first log line is `xboxgip: revision N`. The INF version is `0.1.N.0`.

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

* **Fix:** switch the GIP interface (interface 0) to **alternate setting 1** after SET_CONFIGURATION, as Windows does. A USB trace of Windows 11 showed this. On setting 0 the pad accepts writes but never sends anything, which matched the r02/r03 logs on XP.
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

/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * rumbletest - interactive test of the controller's four rumble motors.
 *
 * Hold a button to pick a motor. It runs at full strength; pull either
 * trigger to turn it down (fully pulled = off):
 *
 *   A     left motor (heavy, low frequency)
 *   B     right motor (light, high frequency)
 *   X     left trigger motor
 *   Y     right trigger motor
 *   LB    all four motors
 *   RB    main motors pulsing on and off
 *   View  switch between the HID route (all four motors) and the
 *         XInput route (main motors only)
 *   Menu  quit
 *
 * Buttons and triggers are read through the XUSB (XInput) interface.
 * Rumble goes out either as xboxgip's HID output report 2 (left, right,
 * left trigger, right trigger, 0..100 each) or as an XUSB SET_STATE request.
 */
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(GuidXusbInterface,
    0xEC87F1E3, 0xC13B, 0x4100, 0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB);

#define IOCTL_XUSB_GET_STATE        0x8000E00C
#define IOCTL_XUSB_SET_STATE        0x8000A010

#define BTN_START           0x0010      /* Menu */
#define BTN_BACK            0x0020      /* View */
#define BTN_LSHOULDER       0x0100
#define BTN_RSHOULDER       0x0200
#define BTN_A               0x1000
#define BTN_B               0x2000
#define BTN_X               0x4000
#define BTN_Y               0x8000

#define RUMBLE_REPORT_ID    2
#define MICROSOFT_VID       0x045E
#define XBOXGIP_PRODUCT     L"Xbox Controller (GIP)"    /* xboxgip hid.c, IOCTL_HID_GET_STRING */

#define TRIGGER_DEADZONE    5           /* percent */
#define PULSE_MS            200
#define RESEND_MS           1000        /* refresh unchanged rumble now and then */

typedef enum _ROUTE {
    RouteHid,
    RouteXinput
} ROUTE;

typedef struct _MOTORS {
    BYTE Left;              /* 0..100 */
    BYTE Right;
    BYTE LeftTrigger;
    BYTE RightTrigger;
} MOTORS;

static volatile LONG g_Quit;

static BOOL WINAPI
CtrlHandler(
    DWORD CtrlType
    )
{
    UNREFERENCED_PARAMETER(CtrlType);
    InterlockedExchange(&g_Quit, 1);
    return TRUE;
}

/* Opens the device behind the Index-th interface of a class; returns its path in Path. */
static HANDLE
OpenInterface(
    const GUID *Guid,
    DWORD Index,
    char *Path,
    DWORD PathSize,
    BOOL *More
    )
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD size = 0;
    HANDLE h = INVALID_HANDLE_VALUE;

    *More = FALSE;
    Path[0] = '\0';

    devs = SetupDiGetClassDevsA(Guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        return h;
    }

    ifData.cbSize = sizeof(ifData);
    if (SetupDiEnumDeviceInterfaces(devs, NULL, Guid, Index, &ifData)) {
        *More = TRUE;
        SetupDiGetDeviceInterfaceDetailA(devs, &ifData, NULL, 0, &size, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(size);
        if (detail != NULL) {
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailA(devs, &ifData, detail, size, NULL, NULL)) {
                strncpy(Path, detail->DevicePath, PathSize - 1);
                Path[PathSize - 1] = '\0';
                h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
            }
            free(detail);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return h;
}

/* First XUSB (XInput) device; used for reading buttons and triggers. */
static HANDLE
OpenXusb(
    void
    )
{
    char path[512];
    BOOL more;
    DWORD i;
    HANDLE h;

    for (i = 0; ; i++) {
        h = OpenInterface(&GuidXusbInterface, i, path, sizeof(path), &more);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
        if (!more) {
            return INVALID_HANDLE_VALUE;
        }
    }
}

/*
 * The xboxgip HID game controller: Microsoft VID, Generic Desktop / Game Pad
 * top-level collection, and an output report big enough for report 2.
 */
static HANDLE
OpenRumbleHid(
    USHORT *OutputLength
    )
{
    GUID hidGuid;
    char path[512];
    WCHAR product[64];
    HIDD_ATTRIBUTES attributes;
    PHIDP_PREPARSED_DATA preparsed;
    HIDP_CAPS caps;
    BOOL more;
    BOOL match;
    DWORD i;
    HANDLE h;

    HidD_GetHidGuid(&hidGuid);

    for (i = 0; ; i++) {
        h = OpenInterface(&hidGuid, i, path, sizeof(path), &more);
        if (!more) {
            return INVALID_HANDLE_VALUE;
        }
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        /* Only xboxgip's own collection; other gamepads use other report layouts. */
        match = FALSE;
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(h, &attributes) &&
            attributes.VendorID == MICROSOFT_VID &&
            HidD_GetProductString(h, product, sizeof(product)) &&
            wcscmp(product, XBOXGIP_PRODUCT) == 0 &&
            HidD_GetPreparsedData(h, &preparsed)) {
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == 0x01 && caps.Usage == 0x05 &&
                caps.OutputReportByteLength >= 5) {
                *OutputLength = caps.OutputReportByteLength;
                match = TRUE;
            }
            HidD_FreePreparsedData(preparsed);
        }

        if (match) {
            return h;
        }
        CloseHandle(h);
    }
}

/* XUSB 1.0 state request: 1-byte index in, 20 bytes out, gamepad at offset 8. */
static BOOL
ReadPad(
    HANDLE Xusb,
    WORD *Buttons,
    BYTE *LeftTrigger,
    BYTE *RightTrigger
    )
{
    BYTE in = 0;
    BYTE out[20];
    DWORD returned = 0;

    memset(out, 0, sizeof(out));
    if (!DeviceIoControl(Xusb, IOCTL_XUSB_GET_STATE, &in, sizeof(in),
                         out, sizeof(out), &returned, NULL) ||
        returned < sizeof(out) || out[0] != 1) {
        return FALSE;
    }

    *Buttons = (WORD)(out[8] | (out[9] << 8));
    *LeftTrigger = out[10];
    *RightTrigger = out[11];
    return TRUE;
}

static BOOL
RumbleHid(
    HANDLE Hid,
    USHORT OutputLength,
    const MOTORS *Motors
    )
{
    BYTE report[64];

    if (OutputLength > sizeof(report)) {
        return FALSE;
    }

    memset(report, 0, sizeof(report));
    report[0] = RUMBLE_REPORT_ID;
    report[1] = Motors->Left;
    report[2] = Motors->Right;
    report[3] = Motors->LeftTrigger;
    report[4] = Motors->RightTrigger;

    return HidD_SetOutputReport(Hid, report, OutputLength);
}

/* XUSB SET_STATE: index, LED, left, right (0..255), flags (0x02 = vibration). */
static BOOL
RumbleXusb(
    HANDLE Xusb,
    const MOTORS *Motors
    )
{
    BYTE in[5];
    DWORD returned;

    in[0] = 0;
    in[1] = 0;
    in[2] = (BYTE)(Motors->Left * 255 / 100);
    in[3] = (BYTE)(Motors->Right * 255 / 100);
    in[4] = 0x02;

    return DeviceIoControl(Xusb, IOCTL_XUSB_SET_STATE, in, sizeof(in), NULL, 0, &returned, NULL);
}

static void
PrintHelp(
    BOOL HaveHid
    )
{
    printf("rumbletest - Xbox controller rumble test\n\n");
    printf("  Hold a button: it runs at full strength. Pull either trigger to turn it\n");
    printf("  down (fully pulled = off):\n");
    printf("    A     left motor (heavy, low frequency)\n");
    printf("    B     right motor (light, high frequency)\n");
    printf("    X     left trigger motor\n");
    printf("    Y     right trigger motor\n");
    printf("    LB    all four motors\n");
    printf("    RB    main motors pulsing (%u ms on / %u ms off)\n", PULSE_MS, PULSE_MS);
    if (HaveHid) {
        printf("    View  switch route: HID (all 4 motors) / XInput (main motors only)\n");
    }
    printf("    Menu  quit (or Ctrl+C)\n\n");
}

int __cdecl
main(
    void
    )
{
    HANDLE xusb;
    HANDLE hid;
    USHORT hidOutputLength = 0;
    ROUTE route;
    MOTORS motors;
    MOTORS sent;
    MOTORS off;
    WORD buttons = 0;
    WORD previousButtons = 0;
    BYTE lt = 0;
    BYTE rt = 0;
    BYTE pull;
    BYTE strength;
    DWORD lastSend = 0;
    BOOL connected = TRUE;
    BOOL sendOk = TRUE;
    BOOL triggerMotorsWanted;

    xusb = OpenXusb();
    if (xusb == INVALID_HANDLE_VALUE) {
        printf("No XInput (XUSB) controller found. Is the xboxgip driver installed\n");
        printf("and the controller plugged in?\n");
        return 1;
    }

    hid = OpenRumbleHid(&hidOutputLength);
    route = (hid != INVALID_HANDLE_VALUE) ? RouteHid : RouteXinput;

    PrintHelp(hid != INVALID_HANDLE_VALUE);
    if (hid == INVALID_HANDLE_VALUE) {
        printf("Note: the xboxgip HID rumble report wasn't found, so only the XInput route\n");
        printf("(main motors) is available. Trigger motors need the xboxgip driver.\n\n");
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    memset(&sent, 0xFF, sizeof(sent));     /* force the first send */
    memset(&off, 0, sizeof(off));

    while (!g_Quit) {
        if (!ReadPad(xusb, &buttons, &lt, &rt)) {
            if (connected) {
                printf("\nController not responding; waiting...\n");
                connected = FALSE;
            }
            Sleep(500);
            continue;
        }
        connected = TRUE;

        if (buttons & BTN_START) {
            break;
        }

        /* View (on press, not hold) switches route; stop the old route's motors first. */
        if ((buttons & BTN_BACK) && !(previousButtons & BTN_BACK) &&
            hid != INVALID_HANDLE_VALUE) {
            if (route == RouteHid) {
                RumbleHid(hid, hidOutputLength, &off);
                route = RouteXinput;
            } else {
                RumbleXusb(xusb, &off);
                route = RouteHid;
            }
            memset(&sent, 0xFF, sizeof(sent));
        }
        previousButtons = buttons;

        /*
         * Strength: full power at rest, turned down by pulling either trigger
         * (the further-pulled one wins); fully pulled is 0 %. The deadzone at
         * the resting end keeps a slightly loose trigger at a clean 100 %.
         */
        pull = (BYTE)(((lt > rt) ? lt : rt) * 100 / 255);
        if (pull < TRIGGER_DEADZONE) {
            pull = 0;
        }
        strength = (BYTE)(100 - pull);

        memset(&motors, 0, sizeof(motors));
        if (buttons & BTN_A) motors.Left = strength;
        if (buttons & BTN_B) motors.Right = strength;
        if (buttons & BTN_X) motors.LeftTrigger = strength;
        if (buttons & BTN_Y) motors.RightTrigger = strength;
        if (buttons & BTN_LSHOULDER) {
            motors.Left = motors.Right = motors.LeftTrigger = motors.RightTrigger = strength;
        }
        if ((buttons & BTN_RSHOULDER) && ((GetTickCount() / PULSE_MS) % 2) == 0) {
            motors.Left = motors.Right = strength;
        }

        triggerMotorsWanted = (motors.LeftTrigger || motors.RightTrigger);
        if (route == RouteXinput) {
            motors.LeftTrigger = motors.RightTrigger = 0;
        }

        if (memcmp(&motors, &sent, sizeof(motors)) != 0 ||
            GetTickCount() - lastSend >= RESEND_MS) {
            sendOk = (route == RouteHid) ?
                RumbleHid(hid, hidOutputLength, &motors) :
                RumbleXusb(xusb, &motors);
            sent = motors;
            lastSend = GetTickCount();
        }

        printf("\r[%-6s] strength %3u%% | L %3u  R %3u  LT %3u  RT %3u %-28s",
               (route == RouteHid) ? "HID" : "XInput", strength,
               motors.Left, motors.Right, motors.LeftTrigger, motors.RightTrigger,
               !sendOk ? "| SEND FAILED" :
               (route == RouteXinput && triggerMotorsWanted) ? "| trigger motors: HID only" : "");

        Sleep(16);
    }

    /* Leave the controller still. */
    if (hid != INVALID_HANDLE_VALUE) {
        RumbleHid(hid, hidOutputLength, &off);
        CloseHandle(hid);
    }
    RumbleXusb(xusb, &off);
    CloseHandle(xusb);

    printf("\nMotors stopped. Bye.\n");
    return 0;
}

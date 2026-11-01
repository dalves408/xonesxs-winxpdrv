/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * xusbtest - talks to XUSB (XInput) device interfaces directly.
 *
 *   xusbtest            list interfaces and dump GET_INFORMATION
 *   xusbtest poll [n]   print state from the first interface for n seconds (default 10)
 *   xusbtest rumble     run both motors for one second on the first interface
 *   xusbtest state10    dump state using the XUSB 1.0 request format
 *
 * Works against xboxgip.sys on XP and against Microsoft's XUSB drivers
 * (xusb22 / xinputhid) on newer Windows, which makes it a reference check.
 */
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(GuidXusbInterface,
    0xEC87F1E3, 0xC13B, 0x4100, 0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB);

#define IOCTL_XUSB_GET_INFORMATION  0x80006000
#define IOCTL_XUSB_GET_STATE        0x8000E00C
#define IOCTL_XUSB_SET_STATE        0x8000A010

static HANDLE
OpenInterface(
    DWORD Index,
    char *Path,
    DWORD PathSize
    )
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    DWORD size = 0;
    HANDLE h = INVALID_HANDLE_VALUE;

    devs = SetupDiGetClassDevsA(&GuidXusbInterface, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        return h;
    }

    ifData.cbSize = sizeof(ifData);
    if (SetupDiEnumDeviceInterfaces(devs, NULL, &GuidXusbInterface, Index, &ifData)) {
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
                if (h == INVALID_HANDLE_VALUE) {
                    printf("  open failed: %lu\n", GetLastError());
                }
            }
            free(detail);
        }
    } else {
        Path[0] = '\0';
    }

    SetupDiDestroyDeviceInfoList(devs);
    return h;
}

static BOOL
GetInformation(
    HANDLE h,
    BYTE Info[12]
    )
{
    DWORD returned = 0;

    memset(Info, 0, 12);
    if (!DeviceIoControl(h, IOCTL_XUSB_GET_INFORMATION, NULL, 0, Info, 12, &returned, NULL)) {
        printf("  GET_INFORMATION failed: %lu\n", GetLastError());
        return FALSE;
    }
    printf("  GET_INFORMATION (%lu bytes): version %04X, slots %u, b3 %02X, status %02X, VID %04X PID %04X\n",
           returned, *(WORD *)&Info[0], Info[2], Info[3], Info[4],
           *(WORD *)&Info[8], *(WORD *)&Info[10]);
    return TRUE;
}

static void
DumpHex(
    const BYTE *Data,
    DWORD Length
    )
{
    DWORD i;

    for (i = 0; i < Length; i++) {
        printf("%02X ", Data[i]);
    }
}

/* Returns the byte offset of the gamepad block, or -1 on failure. */
static int
GetState(
    HANDLE h,
    WORD Version,
    BYTE *Out,
    DWORD *Returned
    )
{
    BYTE in[3];
    DWORD inSize;
    DWORD outSize;

    if (Version == 0x0100) {
        in[0] = 0;
        inSize = 1;
        outSize = 20;
    } else {
        in[0] = 0x01;           /* request as 1.1 */
        in[1] = 0x01;
        in[2] = 0;
        inSize = 3;
        outSize = 29;
    }

    memset(Out, 0, 29);
    if (!DeviceIoControl(h, IOCTL_XUSB_GET_STATE, in, inSize, Out, outSize, Returned, NULL)) {
        printf("  GET_STATE failed: %lu\n", GetLastError());
        return -1;
    }
    return (Version == 0x0100) ? 8 : 11;
}

static void
Poll(
    HANDLE h,
    WORD Version,
    int Seconds
    )
{
    BYTE out[29];
    DWORD returned;
    DWORD lastPacket = 0xFFFFFFFF;
    DWORD packet;
    DWORD end = GetTickCount() + Seconds * 1000;
    int pad;
    const BYTE *g;

    while (GetTickCount() < end) {
        pad = GetState(h, Version, out, &returned);
        if (pad < 0) {
            return;
        }
        packet = *(DWORD *)&out[(Version == 0x0100) ? 3 : 5];
        if (packet != lastPacket) {
            g = &out[pad];
            printf("  [%lu bytes] status %u pkt %-6lu btn %04X LT %3u RT %3u L(%6d,%6d) R(%6d,%6d)",
                   returned, out[(Version == 0x0100) ? 0 : 2], packet,
                   *(WORD *)&g[0], g[2], g[3],
                   *(SHORT *)&g[4], *(SHORT *)&g[6], *(SHORT *)&g[8], *(SHORT *)&g[10]);
            if (Version != 0x0100) {
                printf(" extra %02X", out[28]);
            }
            printf("\n");
            lastPacket = packet;
        }
        Sleep(16);
    }
}

static void
Rumble(
    HANDLE h
    )
{
    BYTE in[5] = { 0, 0, 0xFF, 0xFF, 0x02 };
    DWORD returned;

    if (!DeviceIoControl(h, IOCTL_XUSB_SET_STATE, in, sizeof(in), NULL, 0, &returned, NULL)) {
        printf("  SET_STATE failed: %lu\n", GetLastError());
        return;
    }
    Sleep(1000);
    in[2] = in[3] = 0;
    DeviceIoControl(h, IOCTL_XUSB_SET_STATE, in, sizeof(in), NULL, 0, &returned, NULL);
    printf("  rumble sent\n");
}

int __cdecl
main(
    int argc,
    char **argv
    )
{
    char path[512];
    BYTE info[12];
    BYTE out[29];
    DWORD returned;
    DWORD i;
    HANDLE h;
    int found = 0;
    const char *mode = (argc > 1) ? argv[1] : "list";

    for (i = 0; ; i++) {
        h = OpenInterface(i, path, sizeof(path));
        if (path[0] == '\0') {
            break;
        }
        found++;
        printf("[%lu] %s\n", i, path);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        if (GetInformation(h, info)) {
            WORD version = *(WORD *)&info[0];

            if (strcmp(mode, "state10") == 0) {
                version = 0x0100;
            }

            if (strcmp(mode, "poll") == 0 && i == 0) {
                Poll(h, version, (argc > 2) ? atoi(argv[2]) : 10);
            } else if (strcmp(mode, "rumble") == 0 && i == 0) {
                Rumble(h);
            } else if (GetState(h, version, out, &returned) >= 0) {
                printf("  GET_STATE (%lu bytes): ", returned);
                DumpHex(out, returned);
                printf("\n");
            }
        }
        CloseHandle(h);
    }

    if (found == 0) {
        printf("No XUSB interfaces found.\n");
        return 1;
    }
    return 0;
}

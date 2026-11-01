/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * xboxgip.sys - KMDF lower filter that drives an Xbox One / Series controller
 * over USB (GIP protocol) and answers hidclass requests forwarded by xgiphid.sys.
 *
 * Stack:  hidclass + xgiphid.sys (FDO)
 *           xboxgip.sys        (lower filter, owns the USB pipes)
 *         usbhub PDO
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usbdi.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <hidport.h>

#define XGIP_POOL_TAG 'piGX'

/*
 * Startup style:
 *   0 = Windows: interface 0 on alternate setting 1, OS-descriptor queries,
 *       port reset and reconfigure, wait for the pad's announce.
 *   1 = Linux xpad: interface 0 left on setting 0, SET_INTERFACE for the
 *       other interfaces, then power-on/LED/auth-done straight away,
 *       numbered from 0, retried until input arrives.
 * Both answer an announce with the full identify handshake. Windows mode is
 * the default: setting 1 polls input every 2 ms instead of 4 ms, and it's the
 * mode tested end to end. On XP neither makes a Series X|S pad power on by
 * itself; it needs its Xbox button pressed once after plugging in.
 */
#ifndef XGIP_XPAD_MODE
#define XGIP_XPAD_MODE 0
#endif

/* Release number from build.cmd (releases\rNN); 0 for a manual build. */
#ifndef XGIP_REVISION
#define XGIP_REVISION 0
#endif

/* ------------------------------------------------------------------------- */
/* GIP (Gaming Input Protocol) - header is cmd, options, sequence, length.   */
/* ------------------------------------------------------------------------- */

#define GIP_CMD_ACK         0x01
#define GIP_CMD_ANNOUNCE    0x02
#define GIP_CMD_STATUS      0x03
#define GIP_CMD_IDENTIFY    0x04
#define GIP_CMD_POWER       0x05
#define GIP_CMD_GUIDE       0x07
#define GIP_CMD_RUMBLE      0x09
#define GIP_CMD_LED         0x0A
#define GIP_CMD_EXTRA_INPUT 0x4D
#define GIP_CMD_INPUT       0x20

#define GIP_OPT_ACK         0x10
#define GIP_OPT_INTERNAL    0x20

#define GIP_CMD_AUTHENTICATE 0x06

#define GIP_HEADER_SIZE     4
#define GIP_MAX_PACKET      64

/* Minimum payload of a 0x20 input packet: buttons, triggers, sticks. */
#define GIP_INPUT_PAYLOAD_MIN 14

/* Resend the init sequence this often until the pad sends something. */
#define XGIP_INIT_RETRY_MS     1000
#define XGIP_INIT_MAX_ATTEMPTS 10

/* ------------------------------------------------------------------------- */
/* Normalized controller state. Laid out like XINPUT_GAMEPAD so the XInput   */
/* interface (phase 2) can hand it out directly.                             */
/* ------------------------------------------------------------------------- */

#define XGIP_BTN_DPAD_UP    0x0001
#define XGIP_BTN_DPAD_DOWN  0x0002
#define XGIP_BTN_DPAD_LEFT  0x0004
#define XGIP_BTN_DPAD_RIGHT 0x0008
#define XGIP_BTN_START      0x0010
#define XGIP_BTN_BACK       0x0020
#define XGIP_BTN_LTHUMB     0x0040
#define XGIP_BTN_RTHUMB     0x0080
#define XGIP_BTN_LSHOULDER  0x0100
#define XGIP_BTN_RSHOULDER  0x0200
#define XGIP_BTN_GUIDE      0x0400
#define XGIP_BTN_A          0x1000
#define XGIP_BTN_B          0x2000
#define XGIP_BTN_X          0x4000
#define XGIP_BTN_Y          0x8000

typedef struct _XGIP_STATE {
    USHORT  Buttons;
    UCHAR   LeftTrigger;        /* 0..255, XInput scale */
    UCHAR   RightTrigger;
    SHORT   ThumbLX;            /* Y axes are up-positive, as in XInput */
    SHORT   ThumbLY;
    SHORT   ThumbRX;
    SHORT   ThumbRY;
    USHORT  LeftTriggerRaw;     /* 0..1023, as reported by GIP */
    USHORT  RightTriggerRaw;
    BOOLEAN Share;
} XGIP_STATE, *PXGIP_STATE;

/* ------------------------------------------------------------------------- */
/* HID reports - must match XgipReportDescriptor in hid.c.                   */
/* ------------------------------------------------------------------------- */

#define XGIP_REPORT_ID_INPUT  1
#define XGIP_REPORT_ID_RUMBLE 2

#include <pshpack1.h>
typedef struct _XGIP_HID_INPUT_REPORT {
    UCHAR  ReportId;
    USHORT X;                   /* left stick, 0..65535, down-positive */
    USHORT Y;
    USHORT Rx;                  /* right stick */
    USHORT Ry;
    USHORT Z;                   /* left trigger, 0..1023 */
    USHORT Rz;                  /* right trigger */
    UCHAR  Hat;                 /* low nibble: 1..8 clockwise from up, 0 = centered */
    USHORT Buttons;             /* bits 0..11 */
} XGIP_HID_INPUT_REPORT, *PXGIP_HID_INPUT_REPORT;

typedef struct _XGIP_HID_RUMBLE_REPORT {
    UCHAR ReportId;
    UCHAR LeftMotor;            /* 0..100 */
    UCHAR RightMotor;
    UCHAR LeftTriggerMotor;
    UCHAR RightTriggerMotor;
} XGIP_HID_RUMBLE_REPORT, *PXGIP_HID_RUMBLE_REPORT;
#include <poppack.h>

/* ------------------------------------------------------------------------- */
/* Device context                                                            */
/* ------------------------------------------------------------------------- */

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE             Device;
    WDFUSBDEVICE          UsbDevice;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    WDFUSBPIPE            InPipe;
    WDFUSBPIPE            OutPipe;

    WDFQUEUE              ReadQueue;     /* pending IOCTL_HID_READ_REPORTs */

    WDFSPINLOCK           StateLock;     /* protects State, ReportDirty, PacketNumber */
    XGIP_STATE            State;
    BOOLEAN               ReportDirty;   /* State changed since the last completed read */
    ULONG                 PacketNumber;  /* bumped on every state change (XInput dwPacketNumber) */

    LONG                  Sequence;      /* GIP output sequence counter */
    ULONG                 RxCount;       /* USB transfers received since D0 */
    ULONG                 ChunkTotal;    /* length of the chunked message being received */
    BOOLEAN               GotInput;      /* pad is powered on and sending input */

    /* Init sequence: sent one packet at a time, retried until the pad answers. */
    WDFTIMER              InitTimer;
    LONG                  InitActive;    /* a sequence is in flight */
    LONG                  InitRestart;   /* announce arrived mid-sequence: run it again */
    ULONG                 InitStep;      /* next entry in the init table */
    ULONG                 InitAttempts;
    BOOLEAN               HasShareButton;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* Raw child PDO that exposes the XUSB (XInput) interface. */
typedef struct _PDO_CONTEXT {
    WDFDEVICE Parent;
} PDO_CONTEXT, *PPDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PDO_CONTEXT, GetPdoContext)

/* driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD XgipEvtDeviceAdd;

/* usb.c */
EVT_WDF_DEVICE_PREPARE_HARDWARE XgipEvtPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY XgipEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT XgipEvtD0Exit;
EVT_WDF_USB_READER_COMPLETION_ROUTINE XgipEvtReadComplete;
EVT_WDF_USB_READERS_FAILED XgipEvtReadersFailed;

NTSTATUS
XgipUsbWrite(
    PDEVICE_CONTEXT Context,
    const UCHAR *Data,
    ULONG Length,
    BOOLEAN Init
    );

/* gip.c */
EVT_WDF_TIMER XgipEvtInitTimer;

VOID
GipStartInit(
    PDEVICE_CONTEXT Context
    );

VOID
GipInitPacketDone(
    PDEVICE_CONTEXT Context
    );

NTSTATUS
GipSendRumble(
    PDEVICE_CONTEXT Context,
    UCHAR LeftMotor,
    UCHAR RightMotor,
    UCHAR LeftTriggerMotor,
    UCHAR RightTriggerMotor
    );

VOID
GipProcessTransfer(
    PDEVICE_CONTEXT Context,
    const UCHAR *Data,
    ULONG Length
    );

/* hid.c */
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL XgipEvtInternalDeviceControl;

VOID
XgipHidCompletePendingRead(
    PDEVICE_CONTEXT Context
    );

/* xinput.c */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL XgipEvtXusbDeviceControl;

NTSTATUS
XgipCreateXusbPdo(
    WDFDEVICE Device
    );

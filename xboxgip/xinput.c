/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * XInput support: a raw child PDO that registers the XUSB device interface
 * and answers the IOCTLs xinput*.dll sends to Microsoft's Xbox 360 driver.
 *
 * The XUSB protocol is undocumented. Codes and layouts follow the
 * reverse-engineered OpenXinput and SharpXusb projects. We report XUSB 1.0,
 * the oldest version, so every XInput DLL on XP (9.1.0 through 1.3) can use
 * it. With 1.0, xinput itself fills in capabilities and battery info, and
 * only GET_INFORMATION, GET_GAMEPAD_STATE and SET_GAMEPAD_STATE are needed.
 */
#include "xboxgip.h"
#include <initguid.h>

/* {EC87F1E3-C13B-4100-B5F7-8B84D54260CB} - XUSB device interface */
DEFINE_GUID(XgipGuidXusbInterface,
    0xEC87F1E3, 0xC13B, 0x4100, 0xB5, 0xF7, 0x8B, 0x84, 0xD5, 0x42, 0x60, 0xCB);

/* {745A17A0-74D3-11D0-B6FE-00A0C90F57DA} - HIDClass, same class as the parent */
DEFINE_GUID(XgipGuidDevclassHid,
    0x745A17A0, 0x74D3, 0x11D0, 0xB6, 0xFE, 0x00, 0xA0, 0xC9, 0x0F, 0x57, 0xDA);

#define XGIP_XUSB_HARDWARE_ID   L"XboxGip\\XUsbInterface"

#define IOCTL_XUSB_GET_INFORMATION  CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_XUSB_GET_STATE        CTL_CODE(0x8000, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_XUSB_SET_STATE        CTL_CODE(0x8000, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define XUSB_VERSION_1_0            0x0100
#define XUSB_VERSION_1_1            0x0101

#define XUSB_SET_STATE_FLAG_VIBRATION 0x02

#define XUSB_STATUS_CONNECTED       1
#define XUSB_INPUT_ID_NONE          0xFF   /* no headset/audio device */

#include <pshpack1.h>
typedef struct _XUSB_INFORMATION {
    USHORT Version;
    UCHAR  DeviceCount;     /* slots on this interface; xinput polls indexes 0..n-1 */
    UCHAR  Unknown1;
    UCHAR  Status;          /* bit 0x80 makes xinput skip the interface */
    UCHAR  Unknown2;
    USHORT Unknown3;
    USHORT VendorId;
    USHORT ProductId;
} XUSB_INFORMATION;

typedef struct _XUSB_GAMEPAD {
    USHORT Buttons;
    UCHAR  LeftTrigger;
    UCHAR  RightTrigger;
    SHORT  ThumbLX;
    SHORT  ThumbLY;
    SHORT  ThumbRX;
    SHORT  ThumbRY;
} XUSB_GAMEPAD;

/* GET_STATE request for 1.1+; 1.0 sends just the index byte. */
typedef struct _XUSB_REQUEST {
    USHORT Version;
    UCHAR  DeviceIndex;
} XUSB_REQUEST;

typedef struct _XUSB_STATE_1_0 {
    UCHAR        Status;
    UCHAR        Unknown1;
    UCHAR        InputId;
    ULONG        PacketNumber;
    UCHAR        Unknown2;
    XUSB_GAMEPAD Gamepad;
} XUSB_STATE_1_0;

typedef struct _XUSB_STATE_1_1 {
    USHORT       Version;
    UCHAR        Status;
    UCHAR        Unknown1;
    UCHAR        InputId;
    ULONG        PacketNumber;
    UCHAR        Unknown2;
    UCHAR        Unknown3;
    XUSB_GAMEPAD Gamepad;
    UCHAR        Unknown4[5];
    UCHAR        ExtraButtons;  /* 0x01 = Share */
} XUSB_STATE_1_1;

typedef struct _XUSB_SET_STATE {
    UCHAR DeviceIndex;
    UCHAR LedState;
    UCHAR LeftMotor;        /* 0..255, low-frequency (heavy) motor */
    UCHAR RightMotor;       /* 0..255, high-frequency (light) motor */
    UCHAR Flags;
} XUSB_SET_STATE;
#include <poppack.h>

C_ASSERT(sizeof(XUSB_INFORMATION) == 12);
C_ASSERT(sizeof(XUSB_STATE_1_0) == 20);
C_ASSERT(sizeof(XUSB_STATE_1_1) == 29);
C_ASSERT(sizeof(XUSB_SET_STATE) == 5);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, XgipCreateXusbPdo)
#endif

NTSTATUS
XgipCreateXusbPdo(
    WDFDEVICE Device
    )
{
    DECLARE_CONST_UNICODE_STRING(hardwareId, XGIP_XUSB_HARDWARE_ID);
    DECLARE_CONST_UNICODE_STRING(instanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(deviceText, L"Xbox GIP XInput Interface");
    /* Games run as normal users: everyone gets read/write. */
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)");
    PWDFDEVICE_INIT pdoInit;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE pdo;
    NTSTATUS status;

    PAGED_CODE();

    pdoInit = WdfPdoInitAllocate(Device);
    if (pdoInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Raw: starts without a function driver; we handle its I/O ourselves. */
    status = WdfPdoInitAssignRawDevice(pdoInit, &XgipGuidDevclassHid);
    if (NT_SUCCESS(status)) {
        status = WdfDeviceInitAssignSDDLString(pdoInit, &sddl);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAssignDeviceID(pdoInit, &hardwareId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAddHardwareID(pdoInit, &hardwareId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAssignInstanceID(pdoInit, &instanceId);
    }
    if (NT_SUCCESS(status)) {
        status = WdfPdoInitAddDeviceText(pdoInit, &deviceText, &deviceText, 0x409);
    }
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }
    WdfPdoInitSetDefaultLocale(pdoInit, 0x409);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PDO_CONTEXT);
    status = WdfDeviceCreate(&pdoInit, &attributes, &pdo);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(pdoInit);
        return status;
    }

    GetPdoContext(pdo)->Parent = Device;

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
    pnpCaps.SurpriseRemovalOK = WdfTrue;
    pnpCaps.NoDisplayInUI = WdfTrue;
    WdfDeviceSetPnpCapabilities(pdo, &pnpCaps);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = XgipEvtXusbDeviceControl;
    status = WdfIoQueueCreate(pdo, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);

    if (NT_SUCCESS(status)) {
        status = WdfDeviceCreateDeviceInterface(pdo, &XgipGuidXusbInterface, NULL);
    }
    if (NT_SUCCESS(status)) {
        status = WdfFdoAddStaticChild(Device, pdo);
    }
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: XUSB PDO setup failed 0x%08X\n", status));
        WdfObjectDelete(pdo);
    } else {
        KdPrint(("xboxgip: XUSB PDO created\n"));
    }

    return status;
}

static NTSTATUS
XusbGetInformation(
    PDEVICE_CONTEXT Context,
    WDFREQUEST Request,
    PULONG_PTR Information
    )
{
    XUSB_INFORMATION *info;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), (PVOID *)&info, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(info, sizeof(*info));
    info->Version = XUSB_VERSION_1_0;
    info->DeviceCount = 1;
    info->Unknown1 = 1;
    info->VendorId = Context->DeviceDescriptor.idVendor;
    info->ProductId = Context->DeviceDescriptor.idProduct;

    *Information = sizeof(*info);
    return STATUS_SUCCESS;
}

static VOID
XusbFillGamepad(
    const XGIP_STATE *State,
    XUSB_GAMEPAD *Gamepad
    )
{
    Gamepad->Buttons = State->Buttons;
    Gamepad->LeftTrigger = State->LeftTrigger;
    Gamepad->RightTrigger = State->RightTrigger;
    Gamepad->ThumbLX = State->ThumbLX;
    Gamepad->ThumbLY = State->ThumbLY;
    Gamepad->ThumbRX = State->ThumbRX;
    Gamepad->ThumbRY = State->ThumbRY;
}

static NTSTATUS
XusbGetState(
    PDEVICE_CONTEXT Context,
    WDFREQUEST Request,
    size_t InputLength,
    PULONG_PTR Information
    )
{
    PUCHAR input;
    XUSB_STATE_1_0 *out10;
    XUSB_STATE_1_1 *out11;
    XGIP_STATE state;
    ULONG packetNumber;
    USHORT version;
    UCHAR index;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID *)&input, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* 1.0 callers send only the index; 1.1+ send version + index. */
    if (InputLength >= sizeof(XUSB_REQUEST)) {
        version = (USHORT)(input[0] | (input[1] << 8));
        index = input[2];
    } else {
        version = XUSB_VERSION_1_0;
        index = input[0];
    }

    if (index != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Input and output share the system buffer; read the request before writing. */
    WdfSpinLockAcquire(Context->StateLock);
    state = Context->State;
    packetNumber = Context->PacketNumber;
    WdfSpinLockRelease(Context->StateLock);

    if (version == XUSB_VERSION_1_0) {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out10), (PVOID *)&out10, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        RtlZeroMemory(out10, sizeof(*out10));
        out10->Status = XUSB_STATUS_CONNECTED;
        out10->InputId = XUSB_INPUT_ID_NONE;
        out10->PacketNumber = packetNumber;
        XusbFillGamepad(&state, &out10->Gamepad);
        *Information = sizeof(*out10);
    } else {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out11), (PVOID *)&out11, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        RtlZeroMemory(out11, sizeof(*out11));
        out11->Version = XUSB_VERSION_1_1;
        out11->Status = XUSB_STATUS_CONNECTED;
        out11->InputId = XUSB_INPUT_ID_NONE;
        out11->PacketNumber = packetNumber;
        XusbFillGamepad(&state, &out11->Gamepad);
        out11->ExtraButtons = state.Share ? 0x01 : 0x00;
        *Information = sizeof(*out11);
    }

    return STATUS_SUCCESS;
}

/* XInput motor speed 0..255 -> GIP 0..100 */
static UCHAR
XusbScaleMotor(
    UCHAR Value
    )
{
    return (UCHAR)(((ULONG)Value * 100 + 127) / 255);
}

static NTSTATUS
XusbSetState(
    PDEVICE_CONTEXT Context,
    WDFREQUEST Request
    )
{
    XUSB_SET_STATE *in;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in->DeviceIndex != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* LED requests (player number) are accepted and ignored: GIP pads have no player LEDs. */
    if (in->Flags & XUSB_SET_STATE_FLAG_VIBRATION) {
        return GipSendRumble(Context,
                             XusbScaleMotor(in->LeftMotor),
                             XusbScaleMotor(in->RightMotor),
                             0, 0);
    }

    return STATUS_SUCCESS;
}

VOID
XgipEvtXusbDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    WDFDEVICE parent = GetPdoContext(WdfIoQueueGetDevice(Queue))->Parent;
    PDEVICE_CONTEXT context = GetDeviceContext(parent);
    ULONG_PTR information = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    KdPrint(("xboxgip: XUSB IOCTL %08X in %u out %u (rx so far %u)\n",
             IoControlCode, (ULONG)InputBufferLength, (ULONG)OutputBufferLength,
             context->RxCount));

    switch (IoControlCode) {
    case IOCTL_XUSB_GET_INFORMATION:
        status = XusbGetInformation(context, Request, &information);
        break;

    case IOCTL_XUSB_GET_STATE:
        status = XusbGetState(context, Request, InputBufferLength, &information);
        break;

    case IOCTL_XUSB_SET_STATE:
        status = XusbSetState(context, Request);
        break;

    default:
        /* Capabilities, LED, battery, audio, guide wait, power-off: not used with XUSB 1.0. */
        KdPrint(("xboxgip: unhandled XUSB IOCTL 0x%08X\n", IoControlCode));
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * HID side: answers the internal IOCTLs hidclass sends through xgiphid.sys.
 * hidclass passes buffers for these METHOD_NEITHER IOCTLs in Irp->UserBuffer
 * (kernel memory), so we read them from the IRP directly.
 */
#include "xboxgip.h"

static const UCHAR XgipReportDescriptor[] = {
    0x05, 0x01,                     /* Usage Page (Generic Desktop) */
    0x09, 0x05,                     /* Usage (Game Pad) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, XGIP_REPORT_ID_INPUT,     /*   Report ID (1) */

    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical) - left stick */
    0x09, 0x30,                     /*     Usage (X) */
    0x09, 0x31,                     /*     Usage (Y) */
    0x15, 0x00,                     /*     Logical Minimum (0) */
    0x27, 0xFF, 0xFF, 0x00, 0x00,   /*     Logical Maximum (65535) */
    0x75, 0x10,                     /*     Report Size (16) */
    0x95, 0x02,                     /*     Report Count (2) */
    0x81, 0x02,                     /*     Input (Data,Var,Abs) */
    0xC0,                           /*   End Collection */

    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical) - right stick */
    0x09, 0x33,                     /*     Usage (Rx) */
    0x09, 0x34,                     /*     Usage (Ry) */
    0x81, 0x02,                     /*     Input (Data,Var,Abs) */
    0xC0,                           /*   End Collection */

    0x09, 0x32,                     /*   Usage (Z) - left trigger */
    0x09, 0x35,                     /*   Usage (Rz) - right trigger */
    0x26, 0xFF, 0x03,               /*   Logical Maximum (1023) */
    0x81, 0x02,                     /*   Input (Data,Var,Abs) */

    0x09, 0x39,                     /*   Usage (Hat switch) */
    0x15, 0x01,                     /*   Logical Minimum (1) */
    0x25, 0x08,                     /*   Logical Maximum (8) */
    0x35, 0x00,                     /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01,               /*   Physical Maximum (315) */
    0x65, 0x14,                     /*   Unit (Degrees) */
    0x75, 0x04,                     /*   Report Size (4) */
    0x95, 0x01,                     /*   Report Count (1) */
    0x81, 0x42,                     /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,                     /*   Unit (None) */
    0x45, 0x00,                     /*   Physical Maximum (0) - reset */
    0x81, 0x03,                     /*   Input (Const) - 4 bit pad */

    0x05, 0x09,                     /*   Usage Page (Button) */
    0x19, 0x01,                     /*   Usage Minimum (1) */
    0x29, 0x0C,                     /*   Usage Maximum (12) */
    0x15, 0x00,                     /*   Logical Minimum (0) */
    0x25, 0x01,                     /*   Logical Maximum (1) */
    0x75, 0x01,                     /*   Report Size (1) */
    0x95, 0x0C,                     /*   Report Count (12) */
    0x81, 0x02,                     /*   Input (Data,Var,Abs) */
    0x75, 0x04,                     /*   Report Size (4) */
    0x95, 0x01,                     /*   Report Count (1) */
    0x81, 0x03,                     /*   Input (Const) - pad */

    0x06, 0x00, 0xFF,               /*   Usage Page (Vendor 0xFF00) */
    0x85, XGIP_REPORT_ID_RUMBLE,    /*   Report ID (2) */
    0x09, 0x01,                     /*   Usage (1) - rumble */
    0x15, 0x00,                     /*   Logical Minimum (0) */
    0x25, 0x64,                     /*   Logical Maximum (100) */
    0x75, 0x08,                     /*   Report Size (8) */
    0x95, 0x04,                     /*   Report Count (4) */
    0x91, 0x02,                     /*   Output (Data,Var,Abs) */
    0xC0                            /* End Collection */
};

static const HID_DESCRIPTOR XgipHidDescriptor = {
    sizeof(HID_DESCRIPTOR),
    0x21,                           /* HID descriptor type */
    0x0111,                         /* HID 1.11 */
    0x00,
    0x01,
    { { 0x22, sizeof(XgipReportDescriptor) } }
};

/* D-pad bits (up=1, down=2, left=4, right=8) -> hat value, 0 = centered. */
static const UCHAR XgipHatTable[16] = {
    0, 1, 5, 0,
    7, 8, 6, 0,
    3, 2, 4, 0,
    0, 0, 0, 0
};

/* HID button N is bit N-1; this table lists the XGIP_BTN_* flag for each. */
static const USHORT XgipHidButtonMap[] = {
    XGIP_BTN_A,
    XGIP_BTN_B,
    XGIP_BTN_X,
    XGIP_BTN_Y,
    XGIP_BTN_LSHOULDER,
    XGIP_BTN_RSHOULDER,
    XGIP_BTN_BACK,                  /* View */
    XGIP_BTN_START,                 /* Menu */
    XGIP_BTN_LTHUMB,
    XGIP_BTN_RTHUMB,
    XGIP_BTN_GUIDE
};                                  /* button 12 is Share, handled separately */

static VOID
XgipBuildInputReport(
    const XGIP_STATE *State,
    PXGIP_HID_INPUT_REPORT Report
    )
{
    ULONG i;
    USHORT buttons = 0;

    Report->ReportId = XGIP_REPORT_ID_INPUT;

    /* HID Y is down-positive; GIP is up-positive. */
    Report->X  = (USHORT)((LONG)State->ThumbLX + 32768);
    Report->Y  = (USHORT)(32767 - (LONG)State->ThumbLY);
    Report->Rx = (USHORT)((LONG)State->ThumbRX + 32768);
    Report->Ry = (USHORT)(32767 - (LONG)State->ThumbRY);
    Report->Z  = State->LeftTriggerRaw;
    Report->Rz = State->RightTriggerRaw;

    Report->Hat = XgipHatTable[State->Buttons & 0x000F];

    for (i = 0; i < RTL_NUMBER_OF(XgipHidButtonMap); i++) {
        if (State->Buttons & XgipHidButtonMap[i]) {
            buttons |= (USHORT)(1 << i);
        }
    }
    if (State->Share) {
        buttons |= (USHORT)(1 << 11);
    }
    Report->Buttons = buttons;
}

static NTSTATUS
XgipCopyToIrpBuffer(
    PIRP Irp,
    size_t OutputLength,
    const VOID *Source,
    ULONG SourceLength,
    PULONG_PTR Information
    )
{
    if (Irp->UserBuffer == NULL || OutputLength < SourceLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(Irp->UserBuffer, Source, SourceLength);
    *Information = SourceLength;
    return STATUS_SUCCESS;
}

/* Completes one queued IOCTL_HID_READ_REPORT if the state changed since the last one. */
VOID
XgipHidCompletePendingRead(
    PDEVICE_CONTEXT Context
    )
{
    XGIP_HID_INPUT_REPORT report;
    WDFREQUEST request;
    WDF_REQUEST_PARAMETERS params;
    ULONG_PTR information = 0;
    NTSTATUS status;

    WdfSpinLockAcquire(Context->StateLock);
    if (!Context->ReportDirty ||
        !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->ReadQueue, &request))) {
        WdfSpinLockRelease(Context->StateLock);
        return;
    }
    Context->ReportDirty = FALSE;
    XgipBuildInputReport(&Context->State, &report);
    WdfSpinLockRelease(Context->StateLock);

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(request, &params);

    status = XgipCopyToIrpBuffer(WdfRequestWdmGetIrp(request),
                                 params.Parameters.DeviceIoControl.OutputBufferLength,
                                 &report, sizeof(report), &information);

    WdfRequestCompleteWithInformation(request, status, information);
}

static NTSTATUS
XgipHidGetInputReport(
    PDEVICE_CONTEXT Context,
    PIRP Irp,
    PULONG_PTR Information
    )
{
    PHID_XFER_PACKET packet = (PHID_XFER_PACKET)Irp->UserBuffer;
    XGIP_HID_INPUT_REPORT report;

    if (packet == NULL || packet->reportBuffer == NULL ||
        packet->reportBufferLen < sizeof(report)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (packet->reportId != XGIP_REPORT_ID_INPUT) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(Context->StateLock);
    XgipBuildInputReport(&Context->State, &report);
    WdfSpinLockRelease(Context->StateLock);

    RtlCopyMemory(packet->reportBuffer, &report, sizeof(report));
    *Information = sizeof(report);
    return STATUS_SUCCESS;
}

static UCHAR
XgipClampMotor(
    UCHAR Value
    )
{
    return (Value > 100) ? 100 : Value;
}

static NTSTATUS
XgipHidWriteReport(
    PDEVICE_CONTEXT Context,
    PIRP Irp,
    PULONG_PTR Information
    )
{
    PHID_XFER_PACKET packet = (PHID_XFER_PACKET)Irp->UserBuffer;
    PXGIP_HID_RUMBLE_REPORT rumble;
    NTSTATUS status;

    if (packet == NULL || packet->reportBuffer == NULL ||
        packet->reportBufferLen < sizeof(XGIP_HID_RUMBLE_REPORT)) {
        return STATUS_INVALID_PARAMETER;
    }

    rumble = (PXGIP_HID_RUMBLE_REPORT)packet->reportBuffer;
    if (rumble->ReportId != XGIP_REPORT_ID_RUMBLE) {
        return STATUS_INVALID_PARAMETER;
    }

    status = GipSendRumble(Context,
                           XgipClampMotor(rumble->LeftMotor),
                           XgipClampMotor(rumble->RightMotor),
                           XgipClampMotor(rumble->LeftTriggerMotor),
                           XgipClampMotor(rumble->RightTriggerMotor));
    if (NT_SUCCESS(status)) {
        *Information = packet->reportBufferLen;
    }
    return status;
}

static NTSTATUS
XgipHidGetString(
    PIRP Irp,
    size_t OutputLength,
    PULONG_PTR Information
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG_PTR argument = (ULONG_PTR)stack->Parameters.DeviceIoControl.Type3InputBuffer;
    const WCHAR *string;

    /* Low word: string id, high word: language id (ignored). */
    switch (argument & 0xFFFF) {
    case HID_STRING_ID_IMANUFACTURER:
        string = L"Microsoft";
        break;
    case HID_STRING_ID_IPRODUCT:
        string = L"Xbox Controller (GIP)";
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    return XgipCopyToIrpBuffer(Irp, OutputLength, string,
                               (ULONG)((wcslen(string) + 1) * sizeof(WCHAR)),
                               Information);
}

VOID
XgipEvtInternalDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    PIRP irp = WdfRequestWdmGetIrp(Request);
    HID_DEVICE_ATTRIBUTES attributes;
    ULONG_PTR information = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = XgipCopyToIrpBuffer(irp, OutputBufferLength, &XgipHidDescriptor,
                                     sizeof(XgipHidDescriptor), &information);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = XgipCopyToIrpBuffer(irp, OutputBufferLength, XgipReportDescriptor,
                                     sizeof(XgipReportDescriptor), &information);
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        RtlZeroMemory(&attributes, sizeof(attributes));
        attributes.Size = sizeof(attributes);
        attributes.VendorID = context->DeviceDescriptor.idVendor;
        attributes.ProductID = context->DeviceDescriptor.idProduct;
        attributes.VersionNumber = context->DeviceDescriptor.bcdDevice;
        status = XgipCopyToIrpBuffer(irp, OutputBufferLength, &attributes,
                                     sizeof(attributes), &information);
        break;

    case IOCTL_HID_READ_REPORT:
        /* Park it until the pad reports something (or now, if a report is waiting). */
        status = WdfRequestForwardToIoQueue(Request, context->ReadQueue);
        if (NT_SUCCESS(status)) {
            XgipHidCompletePendingRead(context);
            return;
        }
        break;

    case IOCTL_HID_GET_INPUT_REPORT:
        status = XgipHidGetInputReport(context, irp, &information);
        break;

    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = XgipHidWriteReport(context, irp, &information);
        break;

    case IOCTL_HID_GET_STRING:
        status = XgipHidGetString(irp, OutputBufferLength, &information);
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

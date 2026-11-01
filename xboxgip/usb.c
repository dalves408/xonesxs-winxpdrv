/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
#include "xboxgip.h"

/* Per-write context, so a completed init packet can kick off the next one. */
typedef struct _WRITE_CONTEXT {
    PDEVICE_CONTEXT Device;
    UCHAR           Command;
    BOOLEAN         Init;
} WRITE_CONTEXT, *PWRITE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WRITE_CONTEXT, GetWriteContext)

EVT_WDF_REQUEST_COMPLETION_ROUTINE XgipWriteComplete;

static NTSTATUS
XgipSetInterfaceRaw(
    PDEVICE_CONTEXT Context,
    UCHAR InterfaceNumber,
    UCHAR AlternateSetting
    );

static VOID
XgipResetOtherInterfaces(
    PDEVICE_CONTEXT Context
    );

VOID
XgipResetAndRestore(
    PDEVICE_CONTEXT Context
    );

VOID
XgipQueryOsDescriptors(
    PDEVICE_CONTEXT Context
    );

VOID
XgipReadStrings(
    PDEVICE_CONTEXT Context
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, XgipEvtPrepareHardware)
#pragma alloc_text(PAGE, XgipSetInterfaceRaw)
#pragma alloc_text(PAGE, XgipResetOtherInterfaces)
#pragma alloc_text(PAGE, XgipResetAndRestore)
#pragma alloc_text(PAGE, XgipQueryOsDescriptors)
#pragma alloc_text(PAGE, XgipReadStrings)
#endif

#define MS_OS_STRING_INDEX          0xEE
#define MS_OS_COMPAT_ID_INDEX       0x0004
#define XGIP_MS_OS_VENDOR_CODE      0x90    /* what Windows 11 used for this pad */

/* Synchronous device-to-host control request; returns bytes received in *Transferred. */
NTSTATUS
XgipControlIn(
    PDEVICE_CONTEXT Context,
    WDF_USB_BMREQUEST_TYPE Type,
    UCHAR Request,
    USHORT Value,
    USHORT Index,
    PVOID Buffer,
    ULONG Length,
    PULONG Transferred
    )
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR memory;

    WDF_USB_CONTROL_SETUP_PACKET_INIT(&setup, BmRequestDeviceToHost, BmRequestToDevice,
                                      Request, Value, Index);
    setup.Packet.bm.Request.Type = Type;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, Buffer, Length);
    *Transferred = 0;
    return WdfUsbTargetDeviceSendControlTransferSynchronously(Context->UsbDevice,
                                                              WDF_NO_HANDLE, NULL,
                                                              &setup, &memory, Transferred);
}

/*
 * Windows 11 asks the pad for its Microsoft OS compat ID (vendor request,
 * index 4, which returns "XGIP10") on every connection before configuring
 * it. XP asks only on the first-ever connection of a VID/PID/REV and caches
 * the answer. The pad may use this query to tell a Windows GIP host from a
 * charger and stay silent without it, so ask the way Windows 11 does, plus
 * GET_STATUS, which Windows sends just before SET_CONFIGURATION.
 */
VOID
XgipQueryOsDescriptors(
    PDEVICE_CONTEXT Context
    )
{
    UCHAR buffer[0x40];
    UCHAR vendorCode = XGIP_MS_OS_VENDOR_CODE;
    ULONG transferred;
    NTSTATUS status;

    PAGED_CODE();

    /* "MSFT100" string descriptor; byte 16 is the vendor request code. */
    RtlZeroMemory(buffer, sizeof(buffer));
    status = XgipControlIn(Context, BmRequestStandard, USB_REQUEST_GET_DESCRIPTOR,
                           (USB_STRING_DESCRIPTOR_TYPE << 8) | MS_OS_STRING_INDEX, 0,
                           buffer, 0x12, &transferred);
    if (NT_SUCCESS(status) && transferred >= 17) {
        vendorCode = buffer[16];
    }
    KdPrint(("xboxgip: MS OS string: 0x%08X, %u bytes, vendor code %02X\n",
             status, transferred, vendorCode));

    /* Compat ID: header first (16 bytes), then the whole descriptor, as Windows does. */
    status = XgipControlIn(Context, BmRequestVendor, vendorCode, 0, MS_OS_COMPAT_ID_INDEX,
                           buffer, 0x10, &transferred);
    KdPrint(("xboxgip: MS OS compat ID header: 0x%08X, %u bytes\n", status, transferred));

    RtlZeroMemory(buffer, sizeof(buffer));
    status = XgipControlIn(Context, BmRequestVendor, vendorCode, 0, MS_OS_COMPAT_ID_INDEX,
                           buffer, 0x28, &transferred);
    KdPrint(("xboxgip: MS OS compat ID: 0x%08X, %u bytes, id \"%.8s\"\n",
             status, transferred, (transferred >= 0x20) ? (char *)&buffer[0x12] : ""));

    status = XgipControlIn(Context, BmRequestStandard, USB_REQUEST_GET_STATUS, 0, 0,
                           buffer, 2, &transferred);
    KdPrint(("xboxgip: GET_STATUS: 0x%08X, %02X %02X\n", status, buffer[0], buffer[1]));
}

/* Windows reads the language list and strings 1-3 after its final SET_INTERFACE. */
VOID
XgipReadStrings(
    PDEVICE_CONTEXT Context
    )
{
    UCHAR buffer[0xFF];
    ULONG transferred;
    NTSTATUS status;
    UCHAR index;

    PAGED_CODE();

    for (index = 0; index <= 3; index++) {
        status = XgipControlIn(Context, BmRequestStandard, USB_REQUEST_GET_DESCRIPTOR,
                               (USB_STRING_DESCRIPTOR_TYPE << 8) | index,
                               (index == 0) ? 0 : 0x0409, buffer, sizeof(buffer), &transferred);
        KdPrint(("xboxgip: string %u: 0x%08X, %u bytes\n", index, status, transferred));
    }
}

/* Standard SET_INTERFACE sent as-is (KMDF's select-setting skips unchanged settings). */
static NTSTATUS
XgipSetInterfaceRaw(
    PDEVICE_CONTEXT Context,
    UCHAR InterfaceNumber,
    UCHAR AlternateSetting
    )
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    NTSTATUS status;

    PAGED_CODE();

    WDF_USB_CONTROL_SETUP_PACKET_INIT(&setup, BmRequestHostToDevice, BmRequestToInterface,
                                      USB_REQUEST_SET_INTERFACE, AlternateSetting, InterfaceNumber);
    status = WdfUsbTargetDeviceSendControlTransferSynchronously(Context->UsbDevice,
                                                                WDF_NO_HANDLE, NULL,
                                                                &setup, NULL, NULL);
    KdPrint(("xboxgip: SET_INTERFACE(%u, %u): 0x%08X\n", InterfaceNumber, AlternateSetting, status));
    return status;
}

/*
 * Windows sends SET_INTERFACE(1, 0) and SET_INTERFACE(2, 0) right after
 * switching interface 0 to setting 1, and xpad/xone do the same for the audio
 * interface. Those interfaces are already on setting 0, so KMDF's
 * WdfUsbInterfaceSelectSetting would skip the request; send it directly.
 * If XP's stack refuses the raw request, toggle through setting 1 instead.
 */
static VOID
XgipResetOtherInterfaces(
    PDEVICE_CONTEXT Context
    )
{
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS settingParams;
    USB_INTERFACE_DESCRIPTOR ifDesc;
    WDFUSBINTERFACE iface;
    UCHAR count = WdfUsbTargetDeviceGetNumInterfaces(Context->UsbDevice);
    UCHAR i;
    NTSTATUS status;

    PAGED_CODE();

    for (i = 1; i < count; i++) {
        iface = WdfUsbTargetDeviceGetInterface(Context->UsbDevice, i);
        WdfUsbInterfaceGetDescriptor(iface, 0, &ifDesc);

        status = XgipSetInterfaceRaw(Context, ifDesc.bInterfaceNumber, 0);

        if (!NT_SUCCESS(status) && WdfUsbInterfaceGetNumSettings(iface) > 1) {
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 1);
            status = WdfUsbInterfaceSelectSetting(iface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
            KdPrint(("xboxgip:   fallback: interface %u alt 1: 0x%08X\n", ifDesc.bInterfaceNumber, status));

            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 0);
            status = WdfUsbInterfaceSelectSetting(iface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
            KdPrint(("xboxgip:   fallback: interface %u alt 0: 0x%08X\n", ifDesc.bInterfaceNumber, status));
        }
    }
}

/*
 * The Windows 11 trace shows Windows configuring the pad, then resetting its
 * port and configuring it again; the pad announces itself 0.3 s after the
 * second round. xone also resets the device when it attaches. Configured
 * once and left alone, the pad stays silent on XP until its Xbox button is
 * pressed. So: reset the port, check the configuration XP restored, and
 * repeat Windows' SET_INTERFACE requests.
 */
VOID
XgipResetAndRestore(
    PDEVICE_CONTEXT Context
    )
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_REQUEST_SEND_OPTIONS options;
    UCHAR alternateSetting = 0xFF;
    ULONG transferred = 0;
    NTSTATUS status;

    PAGED_CODE();

    status = WdfUsbTargetDeviceResetPortSynchronously(Context->UsbDevice);
    KdPrint(("xboxgip: port reset: 0x%08X\n", status));
    if (!NT_SUCCESS(status)) {
        return;
    }

    /* XP's hub restores address and configuration; ask the pad what interface 0 is on now. */
    WDF_USB_CONTROL_SETUP_PACKET_INIT(&setup, BmRequestDeviceToHost, BmRequestToInterface,
                                      USB_REQUEST_GET_INTERFACE, 0, 0);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, &alternateSetting, sizeof(alternateSetting));
    status = WdfUsbTargetDeviceSendControlTransferSynchronously(Context->UsbDevice,
                                                                WDF_NO_HANDLE, NULL,
                                                                &setup, &memory, &transferred);
    KdPrint(("xboxgip: GET_INTERFACE(0) after reset: 0x%08X, setting %u\n",
             status, alternateSetting));

    if (!NT_SUCCESS(status) || alternateSetting != 1) {
        XgipSetInterfaceRaw(Context, 0, 1);
    }

    XgipResetOtherInterfaces(Context);

    /* The reset may have cleared what the pad learned from the first query; ask again. */
    XgipQueryOsDescriptors(Context);
    XgipReadStrings(Context);

    /* CLEAR_FEATURE(ENDPOINT_HALT): host and pad restart both pipes' data toggles together. */
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_IGNORE_TARGET_STATE);
    status = WdfUsbTargetPipeResetSynchronously(Context->InPipe, WDF_NO_HANDLE, &options);
    KdPrint(("xboxgip: reset IN pipe: 0x%08X\n", status));
    status = WdfUsbTargetPipeResetSynchronously(Context->OutPipe, WDF_NO_HANDLE, &options);
    KdPrint(("xboxgip: reset OUT pipe: 0x%08X\n", status));
}

NTSTATUS
XgipEvtPrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selectParams;
    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDFUSBINTERFACE gipInterface;
    WDFUSBPIPE pipe;
    UCHAR pipeCount;
    UCHAR i;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    /* Already configured (e.g. resource rebalance). */
    if (context->UsbDevice != NULL) {
        return STATUS_SUCCESS;
    }

    status = WdfUsbTargetDeviceCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &context->UsbDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: WdfUsbTargetDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(context->UsbDevice, &context->DeviceDescriptor);
    KdPrint(("xboxgip: VID_%04X PID_%04X REV_%04X\n",
             context->DeviceDescriptor.idVendor,
             context->DeviceDescriptor.idProduct,
             context->DeviceDescriptor.bcdDevice));

    /* Series X|S pads have a Share button. */
    context->HasShareButton = (context->DeviceDescriptor.idProduct == 0x0B12);

    KdPrint(("xboxgip: startup mode: %s\n", XGIP_XPAD_MODE ? "xpad (Linux)" : "Windows"));

#if !XGIP_XPAD_MODE
    /* What Windows 11 asks before SET_CONFIGURATION on every connection. */
    XgipQueryOsDescriptors(context);
#endif

    /* Selects alternate setting 0 on every interface; only interface 0 has GIP endpoints. */
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&selectParams, 0, NULL);
    status = WdfUsbTargetDeviceSelectConfig(context->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES,
                                            &selectParams);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: SelectConfig failed 0x%08X\n", status));
        return status;
    }

#if DBG
    {
        USB_INTERFACE_DESCRIPTOR ifDesc;
        UCHAR ifCount = WdfUsbTargetDeviceGetNumInterfaces(context->UsbDevice);

        KdPrint(("xboxgip: %u interface(s)\n", ifCount));
        for (i = 0; i < ifCount; i++) {
            WDFUSBINTERFACE iface = WdfUsbTargetDeviceGetInterface(context->UsbDevice, i);

            UCHAR alt, ep;

            WdfUsbInterfaceGetDescriptor(iface, 0, &ifDesc);
            KdPrint(("xboxgip:   if[%u] number %u class %02X/%02X/%02X, %u alt setting(s), %u pipe(s) configured\n",
                     i, ifDesc.bInterfaceNumber, ifDesc.bInterfaceClass,
                     ifDesc.bInterfaceSubClass, ifDesc.bInterfaceProtocol,
                     WdfUsbInterfaceGetNumSettings(iface),
                     WdfUsbInterfaceGetNumConfiguredPipes(iface)));

            for (alt = 0; alt < WdfUsbInterfaceGetNumSettings(iface); alt++) {
                for (ep = 0; ep < WdfUsbInterfaceGetNumEndpoints(iface, alt); ep++) {
                    WDF_USB_PIPE_INFORMATION epInfo;

                    WDF_USB_PIPE_INFORMATION_INIT(&epInfo);
                    WdfUsbInterfaceGetEndpointInformation(iface, alt, ep, &epInfo);
                    KdPrint(("xboxgip:     alt %u ep %02X type %u maxpacket %u interval %u\n",
                             alt, epInfo.EndpointAddress, epInfo.PipeType,
                             epInfo.MaximumPacketSize, epInfo.Interval));
                }
            }
        }
    }
#endif

    gipInterface = WdfUsbTargetDeviceGetInterface(context->UsbDevice, 0);

#if !XGIP_XPAD_MODE
    /*
     * Windows switches the GIP interface to alternate setting 1 right after
     * SET_CONFIGURATION (seen in a USB trace of Windows 11 with a Series X|S
     * pad). xpad leaves it on setting 0 and powers the pad on directly.
     */
    if (WdfUsbInterfaceGetNumSettings(gipInterface) > 1) {
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS settingParams;

        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 1);
        status = WdfUsbInterfaceSelectSetting(gipInterface, WDF_NO_OBJECT_ATTRIBUTES,
                                              &settingParams);
        KdPrint(("xboxgip: select interface 0 alt 1: 0x%08X\n", status));
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
#endif

    /* Windows and xpad both send SET_INTERFACE for the audio interface. */
    XgipResetOtherInterfaces(context);

    pipeCount = WdfUsbInterfaceGetNumConfiguredPipes(gipInterface);

    for (i = 0; i < pipeCount; i++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(gipInterface, i, &pipeInfo);

        KdPrint(("xboxgip:   pipe[%u] EP %02X type %u maxpacket %u interval %u\n",
                 i, pipeInfo.EndpointAddress, pipeInfo.PipeType,
                 pipeInfo.MaximumPacketSize, pipeInfo.Interval));

        if (pipeInfo.PipeType != WdfUsbPipeTypeInterrupt) {
            continue;
        }
        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            context->InPipe = pipe;
        } else if (WdfUsbTargetPipeIsOutEndpoint(pipe)) {
            context->OutPipe = pipe;
        }
    }

    if (context->InPipe == NULL || context->OutPipe == NULL) {
        KdPrint(("xboxgip: interrupt IN/OUT pipes not found on interface 0\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

#if !XGIP_XPAD_MODE
    /* Second round of setup, as Windows does it. */
    XgipResetAndRestore(context);
#endif

    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(context->InPipe);

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerConfig, XgipEvtReadComplete, context,
                                          GIP_MAX_PACKET);
    readerConfig.EvtUsbTargetPipeReadersFailed = XgipEvtReadersFailed;
    /* One read in flight keeps GIP packets in order; gip.c relies on that. */
    readerConfig.NumPendingReads = 1;

    status = WdfUsbTargetPipeConfigContinuousReader(context->InPipe, &readerConfig);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: ConfigContinuousReader failed 0x%08X\n", status));
    }

    return status;
}

NTSTATUS
XgipEvtD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    NTSTATUS status;

    KdPrint(("xboxgip: D0Entry from D%u\n", PreviousState - WdfPowerDeviceD0));
    UNREFERENCED_PARAMETER(PreviousState);

    /* Start from a centered, nothing-pressed state and report it once. */
    WdfSpinLockAcquire(context->StateLock);
    RtlZeroMemory(&context->State, sizeof(context->State));
    context->ReportDirty = TRUE;
    WdfSpinLockRelease(context->StateLock);

    context->Sequence = 0;
    context->RxCount = 0;
    context->ChunkTotal = 0;
    context->GotInput = FALSE;
    context->InitActive = 0;
    context->InitRestart = 0;
    context->InitAttempts = 0;

    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(context->OutPipe));
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: OUT target start failed 0x%08X\n", status));
        return status;
    }

    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(context->InPipe));
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: IN target start (continuous reader) failed 0x%08X\n", status));
        return status;
    }

#if XGIP_XPAD_MODE
    /*
     * Like xpad: power on straight away, without waiting for an announce,
     * and resend every second until input arrives.
     */
    GipStartInit(context);
    WdfTimerStart(context->InitTimer, WDF_REL_TIMEOUT_IN_MS(XGIP_INIT_RETRY_MS));
#else
    /*
     * Like Windows, send nothing until the pad announces itself; the announce
     * starts the identify + init sequence (gip.c). Blind init packets before
     * the announce were tried with setting 1 and the pad ignored every one.
     */
#endif

    return STATUS_SUCCESS;
}

NTSTATUS
XgipEvtD0Exit(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfTimerStop(context->InitTimer, TRUE);
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(context->InPipe), WdfIoTargetCancelSentIo);
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(context->OutPipe), WdfIoTargetCancelSentIo);

    return STATUS_SUCCESS;
}

VOID
XgipEvtReadComplete(
    WDFUSBPIPE Pipe,
    WDFMEMORY Buffer,
    size_t NumBytesTransferred,
    WDFCONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred == 0) {
        KdPrint(("xboxgip: <- zero-length read\n"));
        return;
    }

    GipProcessTransfer((PDEVICE_CONTEXT)Context,
                     (const UCHAR *)WdfMemoryGetBuffer(Buffer, NULL),
                     (ULONG)NumBytesTransferred);
}

BOOLEAN
XgipEvtReadersFailed(
    WDFUSBPIPE Pipe,
    NTSTATUS Status,
    USBD_STATUS UsbdStatus
    )
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);     /* KdPrint compiles out in fre builds */
    UNREFERENCED_PARAMETER(UsbdStatus);

    KdPrint(("xboxgip: continuous reader failed 0x%08X usbd 0x%08X\n", Status, UsbdStatus));

    /* Let the framework reset the pipe and restart the readers. */
    return TRUE;
}

/*
 * Sends one GIP packet on the interrupt OUT pipe without waiting.
 * Callable at <= DISPATCH_LEVEL, so it works from the reader completion.
 */
NTSTATUS
XgipUsbWrite(
    PDEVICE_CONTEXT Context,
    const UCHAR *Data,
    ULONG Length,
    BOOLEAN Init
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFIOTARGET target = WdfUsbTargetPipeGetIoTarget(Context->OutPipe);
    PWRITE_CONTEXT writeContext;
    WDFREQUEST request;
    WDFMEMORY memory;
    PVOID buffer;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WRITE_CONTEXT);
    attributes.ParentObject = Context->Device;
    status = WdfRequestCreate(&attributes, target, &request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    writeContext = GetWriteContext(request);
    writeContext->Device = Context;
    writeContext->Command = Data[0];
    writeContext->Init = Init;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = request;
    status = WdfMemoryCreate(&attributes, NonPagedPool, XGIP_POOL_TAG, Length,
                             &memory, &buffer);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(request);
        return status;
    }
    RtlCopyMemory(buffer, Data, Length);

    status = WdfUsbTargetPipeFormatRequestForWrite(Context->OutPipe, request, memory, NULL);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(request);
        return status;
    }

    KdPrint(("xboxgip: -> %02X %02X %02X %02X ... (%u bytes)\n",
             Data[0], Data[1], Data[2], (Length > 3) ? Data[3] : 0, Length));

    WdfRequestSetCompletionRoutine(request, XgipWriteComplete, NULL);

    if (!WdfRequestSend(request, target, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(request);
        KdPrint(("xboxgip: write send failed 0x%08X\n", status));
        WdfObjectDelete(request);
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
XgipWriteComplete(
    WDFREQUEST Request,
    WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params,
    WDFCONTEXT Context
    )
{
    PWRITE_CONTEXT writeContext = GetWriteContext(Request);
    PDEVICE_CONTEXT device = writeContext->Device;
    BOOLEAN init = writeContext->Init;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    KdPrint(("xboxgip:    write cmd %02X done 0x%08X usbd 0x%08X\n",
             writeContext->Command, Params->IoStatus.Status,
             Params->Parameters.Usb.Completion->UsbdStatus));
    UNREFERENCED_PARAMETER(Params);

    WdfObjectDelete(Request);

    /* Init packets go out strictly one after another, like xpad. */
    if (init) {
        GipInitPacketDone(device);
    }
}

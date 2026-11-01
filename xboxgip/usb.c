#include "xboxgip.h"

/* Per-write context, so a completed init packet can kick off the next one. */
typedef struct _WRITE_CONTEXT {
    PDEVICE_CONTEXT Device;
    UCHAR           Command;
    BOOLEAN         Init;
} WRITE_CONTEXT, *PWRITE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WRITE_CONTEXT, GetWriteContext)

EVT_WDF_REQUEST_COMPLETION_ROUTINE XgipWriteComplete;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, XgipEvtPrepareHardware)
#endif

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

    /*
     * Windows switches the GIP interface to alternate setting 1 right after
     * SET_CONFIGURATION (seen in a USB trace of Windows 11 with a Series X|S
     * pad). On setting 0 the pad accepts writes but never sends anything.
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

    /*
     * Like Windows, wait for the pad to announce itself; the announce starts
     * the init sequence (gip.c). If nothing arrives, the timer sends it anyway.
     */
    WdfTimerStart(context->InitTimer, WDF_REL_TIMEOUT_IN_MS(XGIP_INIT_RETRY_MS));

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

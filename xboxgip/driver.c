/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
#include "xboxgip.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, XgipEvtDeviceAdd)
#endif

NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    KdPrint(("xboxgip: revision %u (built %s %s)\n", XGIP_REVISION, __DATE__, __TIME__));

    WDF_DRIVER_CONFIG_INIT(&config, XgipEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}

NTSTATUS
XgipEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_TIMER_CONFIG timerConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    /* xgiphid.sys is the function driver; anything we don't handle is forwarded down. */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = XgipEvtPrepareHardware;
    pnpCallbacks.EvtDeviceD0Entry = XgipEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = XgipEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    context = GetDeviceContext(device);
    context->Device = device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->StateLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, XgipEvtInitTimer, XGIP_INIT_RETRY_MS);
    timerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &attributes, &context->InitTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = XgipEvtInternalDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: default queue create failed 0x%08X\n", status));
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &context->ReadQueue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("xboxgip: read queue create failed 0x%08X\n", status));
        return status;
    }

    /* XInput is optional; the HID controller works without it. */
    if (!NT_SUCCESS(XgipCreateXusbPdo(device))) {
        KdPrint(("xboxgip: XInput interface unavailable\n"));
    }

    return STATUS_SUCCESS;
}

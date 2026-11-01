/*
 * xgiphid.sys - HID minidriver shim.
 *
 * hidclass.sys only talks to drivers that call HidRegisterMinidriver, and it
 * takes over their dispatch table, which a KMDF driver can't allow. So this
 * tiny WDM driver registers with hidclass as the function driver and passes
 * every IRP down to xboxgip.sys (KMDF lower filter), which does the real work.
 *
 * Windows 7 ships mshidkmdf.sys for this; XP does not, so we bring our own.
 */
#include <wdm.h>
#include <hidport.h>

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE XgipHidAddDevice;
DRIVER_UNLOAD XgipHidUnload;
__drv_dispatchType_other DRIVER_DISPATCH XgipHidPassThrough;
__drv_dispatchType(IRP_MJ_POWER) DRIVER_DISPATCH XgipHidPower;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, XgipHidAddDevice)
#pragma alloc_text(PAGE, XgipHidUnload)
#endif

#define GET_NEXT_DEVICE_OBJECT(fdo) \
    (((PHID_DEVICE_EXTENSION)(fdo)->DeviceExtension)->NextDeviceObject)

NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    HID_MINIDRIVER_REGISTRATION registration;
    ULONG i;

    /* hidclass saves these and calls them for IRPs aimed at our FDO. */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = XgipHidPassThrough;
    }
    DriverObject->MajorFunction[IRP_MJ_POWER] = XgipHidPower;
    DriverObject->DriverExtension->AddDevice = XgipHidAddDevice;
    DriverObject->DriverUnload = XgipHidUnload;

    RtlZeroMemory(&registration, sizeof(registration));
    registration.Revision = HID_REVISION;
    registration.DriverObject = DriverObject;
    registration.RegistryPath = RegistryPath;
    registration.DeviceExtensionSize = 0;
    registration.DevicesArePolled = FALSE;

    return HidRegisterMinidriver(&registration);
}

NTSTATUS
XgipHidAddDevice(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT FunctionalDeviceObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();

    /* hidclass already created and attached the FDO. */
    FunctionalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS
XgipHidPassThrough(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(GET_NEXT_DEVICE_OBJECT(DeviceObject), Irp);
}

NTSTATUS
XgipHidPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    /* Pre-Vista power IRPs must go through PoStartNextPowerIrp/PoCallDriver. */
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(GET_NEXT_DEVICE_OBJECT(DeviceObject), Irp);
}

VOID
XgipHidUnload(
    PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    PAGED_CODE();
}

//#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>
#include "DriverInit.h"
#include "FilteringEngine.h"
#include "CommonStructures.h"

DECLARE_CONST_UNICODE_STRING(
SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_R_RES_R,
L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GR;;;WD)(A;;GR;;;RC)"
);

VOID
kkVPNUnload(
	_In_ PDRIVER_OBJECT pDriverObject
	)
{
	UNREFERENCED_PARAMETER(pDriverObject);

	StopFilterEngine();

	DbgPrint(_DRVNAME "Unloaded\n");
}

NTSTATUS
DriverEntry(
	_In_ PDRIVER_OBJECT  pDriverObject,
	_In_ PUNICODE_STRING pRegistryPath
	)
{
	NTSTATUS status;
	WDF_DRIVER_CONFIG config;
	
	DbgPrint(_DRVNAME "Started (Version " _DRVVER ")\n");
	WDF_DRIVER_CONFIG_INIT(&config, kkdrvDriverDeviceAdd);
	status = WdfDriverCreate(
			pDriverObject, 
			pRegistryPath, 
			WDF_NO_OBJECT_ATTRIBUTES, 
			&config, 
			&gDriver
			);

	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfDriverCreate, status);
		goto Exit;
	}

	pDriverObject->DriverUnload = kkVPNUnload;

Exit:
	return status;
}

NTSTATUS 
kkdrvDriverDeviceAdd(
	_In_     WDFDRIVER Driver,
	_Inout_  PWDFDEVICE_INIT DeviceInit
	)
{
	UNREFERENCED_PARAMETER(Driver);

	NTSTATUS status = STATUS_SUCCESS;

	//WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
	WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_NETWORK);
	WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);
	status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &gDevice);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(WdfDeviceCreate, status);
		WdfDeviceInitFree(DeviceInit);
		goto Exit;
	}
	DbgPrint(_DRVNAME "Device created\n");

	status = CreateQueue(&gDevice, &gQueue);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(CreateQueue, status);
		goto Exit;
	}
	DbgPrint(_DRVNAME "Device I/O queue created\n");

	DECLARE_CONST_UNICODE_STRING(dosDeviceName, DOS_DEVICE_NAME);
	status = WdfDeviceCreateSymbolicLink(
		gDevice,
		&dosDeviceName
		);
	if (!NT_SUCCESS(status)) {
		REPORT_ERROR(WdfDeviceCreateSymbolicLink, status);
		goto Exit;
	}
	DbgPrint(_DRVNAME "Symbolic link created\n");

	UNICODE_STRING ref;
	RtlInitUnicodeString(&ref, L"kkdrvrefstring");
	status = WdfDeviceCreateDeviceInterface(
		gDevice,
		(LPGUID)&GUID_KKDRV_INTERFACE,
		NULL //&ref
		);
	if (!NT_SUCCESS(status)) {
		REPORT_ERROR(WdfDeviceCreateDeviceInterface, status);
		goto Exit;
	}
	DbgPrint(_DRVNAME "Device interface created\n");

	WdfControlFinishInitializing(gDevice);
	DbgPrint(_DRVNAME "Device initialization finished\n");

	status = StartFilterEngine(&gDevice);

Exit:
	return status;
}

NTSTATUS 
CreateQueue(
	_In_ WDFDEVICE *hDevice,
	_Out_ WDFQUEUE *hQueue
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG  ioQueueConfig;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
		&ioQueueConfig,
		WdfIoQueueDispatchSequential
		);

	ioQueueConfig.EvtIoDeviceControl = kkdrvIoDeviceControl;

	status = WdfIoQueueCreate(
		*hDevice,
		&ioQueueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		hQueue
		);
	if (!NT_SUCCESS(status)) {
		REPORT_ERROR(WdfIoQueueCreate, status);
		goto Exit;
	}

Exit:
	return status;
}

VOID kkdrvIoDeviceControl(
	_In_  WDFQUEUE Queue,
	_In_  WDFREQUEST Request,
	_In_  size_t OutputBufferLength,
	_In_  size_t InputBufferLength,
	_In_  ULONG IoControlCode
	)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	NTSTATUS status = STATUS_SUCCESS;
	FILTER_IP_RANGE *buf = NULL;
	size_t bytes_read = 0;

	switch (IoControlCode) 
	{
		case IOCTL_REGISTER:
			status = WdfRequestRetrieveInputBuffer(
				Request,
				sizeof(FILTER_IP_RANGE),
				(void*) &buf,
				&bytes_read
				);
			if (!NT_SUCCESS(status))
			{
				REPORT_ERROR(WdfRequestRetrieveInputBuffer, status);
				goto Complete;
			}

			DbgPrint(_DRVNAME "Device I/O Control recieved (low: 0x%08x, high: 0x%08x)\n", 
				buf->low,
				buf->high
				);

			status = RegisterFilter(
				gDevice,
				buf,
				&gCalloutID
				);
			if (!NT_SUCCESS(status))
			{
				REPORT_ERROR(RegisterFilter, status);
				goto Complete;
			}

			DbgPrint(_DRVNAME "Filter registered\n");
			break;

		case IOCTL_RESTART:
			status = RestartEngine();

			DbgPrint(_DRVNAME "Filter unregistered\n");
			break;
		/*case IOCTL_SET_EVENT_HANDLE:
			status = WdfRequestRetrieveInputBuffer(
				Request,
				sizeof(gPacketEvent),
				(void*)&gPacketEvent,
				&bytes_read
				);
			if (!NT_SUCCESS(status))
			{
				REPORT_ERROR(WdfRequestRetrieveInputBuffer, status);
				goto Complete;
			}

			DbgPrint(_DRVNAME "Event handle received\n");
			break;*/
		default:
			status = STATUS_INVALID_DEVICE_REQUEST;
			DbgPrint(_DRVNAME "Device I/O Control recieved (invalid IoControlCode)\n");
			break;
	}

Complete:
	WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)0);
}
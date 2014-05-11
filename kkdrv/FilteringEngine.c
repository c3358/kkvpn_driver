//#include <ntifs.h>
#include <ntddk.h>
#include <wdf.h>

#pragma warning(push)
#pragma warning(disable:4201)       // unnamed struct/union
#include <fwpsk.h>
#pragma warning(pop)

#include <fwpmk.h>

#include "FilteringEngine.h"
#include "DriverInit.h"
#include "Callout.h"

NTSTATUS StartFilterEngine(
	_Inout_ WDFDEVICE *device
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	BOOL engineOpened = FALSE;
	BOOL transactionStarted = FALSE;

	FWPM_SESSION session = { 0 };
	
	session.flags = FWPM_SESSION_FLAG_DYNAMIC;

	status = FwpmEngineOpen(
		NULL,
		RPC_C_AUTHN_WINNT,
		NULL,
		&session,
		&gEngineHandle
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmEngineOpen, status);
		goto Exit;
	}
	engineOpened = TRUE;
	DbgPrint(_DRVNAME "Filter Engine opened\n");

	status = FwpmTransactionBegin(gEngineHandle, 0);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmTransactionBegin, status);
		goto Exit;
	}
	transactionStarted = TRUE;
	DbgPrint(_DRVNAME "Filter transaction started\n");

	//register callout

	status = RegisterCallout(
		device,
		&gCalloutID
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(RegisterCallout, status);
		goto Exit;
	}
	DbgPrint(_DRVNAME "Callout registered\n");

	gActiveFilter = 0;

	status = FwpmTransactionCommit(gEngineHandle);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmTransactionCommit, status);
		goto Exit;
	}
	transactionStarted = FALSE;
	DbgPrint(_DRVNAME "Filter transaction commit\n");

Exit:

	if (!NT_SUCCESS(status))
	{
		if (transactionStarted)
		{
			FwpmTransactionAbort(gEngineHandle);
		}

		if (engineOpened)
		{
			FwpmEngineClose(gEngineHandle);
			gEngineHandle = NULL;
		}
	}

	return status;
}

VOID StopFilterEngine()
{
	if (gCalloutID)
		FwpsCalloutUnregisterById(gCalloutID);

	if (gActiveFilter)
		FwpmFilterDeleteById(gEngineHandle, gActiveFilter);

	if (gEngineHandle)
	{
		FwpmEngineClose(gEngineHandle);
		gEngineHandle = NULL;
	}
}

NTSTATUS
RegisterCallout(
	_Inout_ WDFDEVICE* deviceObject,
	_Out_ UINT32* calloutId
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	BOOLEAN calloutRegistered = FALSE;

	FWPS_CALLOUT sCallout = { 0 };
	FWPM_CALLOUT mCallout = { 0 };
	FWPM_DISPLAY_DATA displayData = { 0 };

	sCallout.calloutKey = GUID_KKDRV_CALLOUT;
	sCallout.classifyFn = CalloutClasifyFunction;
	sCallout.notifyFn = CalloutNotifyFunction;

	status = FwpsCalloutRegister(
		deviceObject,
		&sCallout,
		calloutId
		);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpsCalloutRegister, status);
		goto Exit;
	}
	calloutRegistered = TRUE;

	displayData.name = L"kkdrv IPv4 Callout";
	displayData.description = L"Callout that extracts network data.";

	mCallout.calloutKey = GUID_KKDRV_CALLOUT;
	mCallout.displayData = displayData;
	mCallout.applicableLayer = FWPM_LAYER_INBOUND_IPPACKET_V4;
	status = FwpmCalloutAdd(
		gEngineHandle,
		&mCallout,
		NULL,
		NULL
		);

	if (!NT_SUCCESS(status))
	{
		goto Exit;
	}

Exit:
	if (!NT_SUCCESS(status))
	{
		if (calloutRegistered)
		{
			FwpsCalloutUnregisterById(*calloutId);
		}
	}

	return status;
}

NTSTATUS
RegisterFilter(
	_Inout_ WDFDEVICE* deviceObject,
	_In_ FILTER_IP_RANGE *ipRange,
	_Out_ UINT32* calloutId
	)
{
	NTSTATUS status = STATUS_SUCCESS;

	FWPM_FILTER filterInbound = { 0 };
	FWPM_FILTER filterOutbound = { 0 };
	FWPM_FILTER_CONDITION filterInboundCondition[1] = { 0 };
	FWPM_FILTER_CONDITION filterOutboundCondition[1] = { 0 };
	FWP_RANGE filterConditionRange = { 0 };

	BOOL transactionStarted = FALSE;

	UNREFERENCED_PARAMETER(deviceObject);
	UNREFERENCED_PARAMETER(calloutId);

	filterConditionRange.valueLow.type = FWP_UINT32;
	filterConditionRange.valueLow.uint32 = (UINT32)(ipRange->low);

	filterConditionRange.valueHigh.type = FWP_UINT32;
	filterConditionRange.valueHigh.uint32 = (UINT32)(ipRange->high);

	status = FwpmTransactionBegin(gEngineHandle, 0);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmTransactionBegin, status);
		goto Exit;
	}
	transactionStarted = TRUE;

	if (gActiveFilter)
	{
		FwpmFilterDeleteById(gEngineHandle, gActiveFilter);
		if (!NT_SUCCESS(status))
		{
			REPORT_ERROR(FwpmFilterDeleteById, status);
			goto Exit;
		}
	}

	filterInbound.layerKey = FWPM_LAYER_INBOUND_IPPACKET_V4;
	filterInbound.displayData.name = L"kkdrv IPv4 Inbound Filter";
	filterInbound.displayData.description = L"Filter that checks for packets addressed inside\
											  VPN subnetwork.";

	filterInbound.action.type = FWP_ACTION_CALLOUT_TERMINATING;
	filterInbound.action.calloutKey = GUID_KKDRV_CALLOUT;
	filterInbound.filterCondition = filterInboundCondition;
	filterInbound.numFilterConditions = 1;
	filterInbound.subLayerKey = FWPM_SUBLAYER_UNIVERSAL;
	filterInbound.weight.type = FWP_EMPTY;

	filterInboundCondition[0].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
	filterInboundCondition[0].matchType = FWP_MATCH_RANGE;
	filterInboundCondition[0].conditionValue.type = FWP_RANGE_TYPE;
	filterInboundCondition[0].conditionValue.rangeValue = &filterConditionRange;

	filterOutbound.layerKey = FWPM_LAYER_OUTBOUND_IPPACKET_V4;
	filterOutbound.displayData.name = L"kkdrv IPv4 Outbound Filter";
	filterOutbound.displayData.description = L"Filter that checks for packets addressed inside\
											   VPN subnetwork.";

	filterOutbound.action.type = FWP_ACTION_BLOCK;
	//filterOutbound.action.type = FWP_ACTION_CALLOUT_TERMINATING;
	//filterOutbound.action.calloutKey = *calloutKey;
	filterOutbound.filterCondition = filterOutboundCondition;
	filterOutbound.numFilterConditions = 1;
	filterOutbound.subLayerKey = FWPM_SUBLAYER_UNIVERSAL;
	filterOutbound.weight.type = FWP_EMPTY;

	filterOutboundCondition[0].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
	filterOutboundCondition[0].matchType = FWP_MATCH_RANGE;
	filterOutboundCondition[0].conditionValue.type = FWP_RANGE_TYPE;
	filterOutboundCondition[0].conditionValue.rangeValue = &filterConditionRange;

	status = FwpmFilterAdd(
		gEngineHandle,
		&filterInbound,
		NULL,
		&gActiveFilter);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmFilterAdd(inbound), status);
		goto Exit;
	}

	status = FwpmFilterAdd(
		gEngineHandle,
		&filterOutbound,
		NULL,
		&gActiveFilter);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmFilterAdd(outbound), status);
		goto Exit;
	}

	DbgPrint(_DRVNAME "Filter ID: 0x%x.\n", gActiveFilter);

	status = FwpmTransactionCommit(gEngineHandle);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(FwpmTransactionCommit, status);
		goto Exit;
	}
	transactionStarted = FALSE;

Exit:

	if (transactionStarted)
	{
		FwpmTransactionAbort(gEngineHandle);
	}

	return status;
}

NTSTATUS
RestartEngine()
{
	NTSTATUS status = STATUS_SUCCESS;

	StopFilterEngine();

	StartFilterEngine(&gDevice);
	if (!NT_SUCCESS(status))
	{
		REPORT_ERROR(StartFilterEngine, status);
		goto Exit;
	}

Exit:
	return status;
}


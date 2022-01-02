#pragma once

#include "pch.h"
#include "MelodyPublic.h"
#include "PlaybackState.h"
#include "KMelody.h"
#include "Memory.h"

void MelodyUnload(PDRIVER_OBJECT);
NTSTATUS MelodyCreateClose(PDEVICE_OBJECT, PIRP);
NTSTATUS MelodyDeviceControl(PDEVICE_OBJECT, PIRP);
NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status = STATUS_SUCCESS, ULONG_PTR info = 0);

PlaybackState* g_State;

extern "C" NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	g_State = new (PagedPool) PlaybackState;
	if (g_State == nullptr)
		return STATUS_INSUFFICIENT_RESOURCES;

	auto status = STATUS_SUCCESS;
	PDEVICE_OBJECT DeviceObject = nullptr;
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\KMelody");
	bool symLinkCreated = false;

	do {
		UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\KMelody");
		status = IoCreateDevice(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN, 0, TRUE, &DeviceObject);
		if (!NT_SUCCESS(status))
			break;

		status = IoCreateSymbolicLink(&symLink, &name);
		if (!NT_SUCCESS(status))
			break;

		symLinkCreated = true;

	} while (false);

	if (!NT_SUCCESS(status)) {
		KdPrint((DRIVER_PREFIX "Error (0x%08X)\n", status));

		delete g_State;
		if (DeviceObject)
			IoDeleteDevice(DeviceObject);
		if (symLinkCreated)
			IoDeleteSymbolicLink(&symLink);
		return status;
	}

	DriverObject->DriverUnload = MelodyUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = DriverObject->MajorFunction[IRP_MJ_CLOSE] = MelodyCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MelodyDeviceControl;

	return status;
}

void MelodyUnload(PDRIVER_OBJECT DriverObject) {
	delete g_State;
	UNICODE_STRING symLink = RTL_CONSTANT_STRING(L"\\??\\KMelody");
	IoDeleteSymbolicLink(&symLink);
	IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS MelodyCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	if (IoGetCurrentIrpStackLocation(Irp)->MajorFunction == IRP_MJ_CREATE) {
		//
		// create the "playback" thread
		//
		auto status = g_State->Start(DeviceObject);
		return CompleteRequest(Irp, status);
	}
	else {
		g_State->Stop();
		return CompleteRequest(Irp);
	}
}

NTSTATUS MelodyDeviceControl(PDEVICE_OBJECT, PIRP Irp) {
	auto irpSp = IoGetCurrentIrpStackLocation(Irp);
	auto& dic = irpSp->Parameters.DeviceIoControl;
	auto status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG info = 0;

	switch (dic.IoControlCode) {
		case IOCTL_MELODY_PLAY:
		{
			if (dic.InputBufferLength == 0 || dic.InputBufferLength % sizeof(Note) != 0) {
				status = STATUS_INVALID_BUFFER_SIZE;
				break;
			}
			auto data = (Note*)Irp->AssociatedIrp.SystemBuffer;
			if (data == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			status = g_State->AddNotes(data, dic.InputBufferLength / sizeof(Note));
			if (!NT_SUCCESS(status))
				break;
			info = dic.InputBufferLength;
			break;
		}

		case IOCTL_MELODY_WAIT_ON_CLOSE:
			if (dic.InputBufferLength == 0) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			auto wait = (bool*)Irp->AssociatedIrp.SystemBuffer;
			if (wait == nullptr) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			NT_ASSERT(g_State);
			g_State->WaitOnClose(*wait);
			info = 1;
			status = STATUS_SUCCESS;
			break;
	}
	return CompleteRequest(Irp, status, info);
}

NTSTATUS CompleteRequest(PIRP Irp, NTSTATUS status, ULONG_PTR info) {
	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

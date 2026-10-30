#include "Driver.h"

// EPROCESS.ActiveProcessLinks moves between Windows builds. The offset is
// detected at load time (see DetectLinksOffset); this is only the fallback.
//   Win7 SP1        : 0x188
//   Win8.1          : 0x2e0
//   Win10 1607-1902 : 0x2e8
//   Win10 1903-22H2 : 0x2f0
//   Win11 22H2+     : 0x448
#ifndef OFF_EPROCESS_ACTIVE_LINKS
#define OFF_EPROCESS_ACTIVE_LINKS 0x448
#endif

#define FFPANEL_MAX_MODULES   512
#define FFPANEL_MAX_PROTECTED 16
#define FFPANEL_MAX_HIDDEN    8

// Undocumented but exported by ntoskrnl on every Windows 7+ build.
NTKERNELAPI PVOID NTAPI PsGetProcessPeb(_In_ PEPROCESS Process);

// The kernel's module list entry only guarantees InLoadOrderLinks; the other
// two list members exist in the user-mode LDR_DATA_TABLE_ENTRY, not here.
// Using the user-mode layout here is what corrupts the list.
typedef struct _FF_KLDR_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
} FF_KLDR_ENTRY, *PFF_KLDR_ENTRY;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct _FF_PROTECTED_PROCESS {
    ULONG pid;
    PEPROCESS process;
} FF_PROTECTED_PROCESS;

typedef struct _FF_HIDDEN_MODULE {
    ULONG pid;
    PVOID entry;   // LDR_DATA_TABLE_ENTRY in the target, for IOCTL_UNHIDE_MODULE
} FF_HIDDEN_MODULE;

static FF_PROTECTED_PROCESS gProtected[FFPANEL_MAX_PROTECTED];
static FF_HIDDEN_MODULE gHidden[FFPANEL_MAX_HIDDEN];
static KSPIN_LOCK gProtectedLock;
static ULONG gClientPid;
static ULONG gActiveLinksOffset;
static PVOID gObRegistration;
static UNICODE_STRING gAltitude;
static OB_OPERATION_REGISTRATION gOperations[1];

// ---------------------------------------------------------------------------
// Hiding the driver itself
// ---------------------------------------------------------------------------

// Takes this driver out of PsLoadedModuleList, so NtQuerySystemInformation no
// longer reports it. The image stays mapped and callable; only the list entry
// goes away.
static VOID HideDriver(_In_ PDRIVER_OBJECT driver) {
    PFF_KLDR_ENTRY entry;
    if (driver == NULL || driver->DriverSection == NULL) return;

    entry = (PFF_KLDR_ENTRY)driver->DriverSection;
    if (entry->InLoadOrderLinks.Flink == &entry->InLoadOrderLinks) return; // already out

    RemoveEntryList(&entry->InLoadOrderLinks);
    entry->InLoadOrderLinks.Flink = &entry->InLoadOrderLinks;
    entry->InLoadOrderLinks.Blink = &entry->InLoadOrderLinks;
}

// ---------------------------------------------------------------------------
// Process protection (ObRegisterCallbacks)
// ---------------------------------------------------------------------------

static BOOLEAN IsProtectedPid(_In_ ULONG pid) {
    KIRQL irql;
    BOOLEAN found = FALSE;
    ULONG i;

    KeAcquireSpinLock(&gProtectedLock, &irql);
    for (i = 0; i < FFPANEL_MAX_PROTECTED; ++i) {
        if (gProtected[i].process != NULL && gProtected[i].pid == pid) { found = TRUE; break; }
    }
    KeReleaseSpinLock(&gProtectedLock, irql);
    return found;
}

// Strips the handle rights an anti-cheat needs to inspect or patch a protected
// process. Only handles opened by someone other than our client are touched,
// and only for PIDs that were explicitly registered.
static OB_PREOP_CALLBACK_STATUS PreProcessHandle(_In_ PVOID context,
                                                 _In_ POB_PRE_OPERATION_INFORMATION info) {
    ULONG targetPid;
    ULONG requesterPid;
    ACCESS_MASK denied;

    UNREFERENCED_PARAMETER(context);

    if (info->KernelOperation) return OB_PREOP_SUCCESS;
    if (info->ObjectType != *PsProcessType) return OB_PREOP_SUCCESS;

    targetPid = (ULONG)(ULONG_PTR)PsGetProcessId((PEPROCESS)info->Object);
    if (!IsProtectedPid(targetPid)) return OB_PREOP_SUCCESS;

    requesterPid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    if (requesterPid == gClientPid || requesterPid == targetPid) return OB_PREOP_SUCCESS;

    denied = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
             PROCESS_CREATE_THREAD | PROCESS_SUSPEND_RESUME | PROCESS_DUP_HANDLE;

    if (info->Operation == OB_OPERATION_HANDLE_CREATE) {
        info->Parameters->CreateHandleInformation.DesiredAccess &= ~denied;
    } else if (info->Operation == OB_OPERATION_HANDLE_DUPLICATE) {
        info->Parameters->DuplicateHandleInformation.DesiredAccess &= ~denied;
    }
    return OB_PREOP_SUCCESS;
}

static NTSTATUS RegisterProtection(VOID) {
    OB_CALLBACK_REGISTRATION registration;

    RtlInitUnicodeString(&gAltitude, FF_ALTITUDE);

    gOperations[0].ObjectType = PsProcessType;
    gOperations[0].Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    gOperations[0].PreOperation = PreProcessHandle;
    gOperations[0].PostOperation = NULL;

    RtlZeroMemory(&registration, sizeof(registration));
    registration.Version = OB_FLT_REGISTRATION_VERSION;
    registration.OperationRegistrationCount = 1;
    registration.Altitude = gAltitude;
    registration.RegistrationContext = NULL;
    registration.OperationRegistration = gOperations;

    return ObRegisterCallbacks(&registration, &gObRegistration);
}

static VOID UnregisterProtection(VOID) {
    if (gObRegistration != NULL) {
        ObUnRegisterCallbacks(gObRegistration);
        gObRegistration = NULL;
    }
}

// ---------------------------------------------------------------------------
// Process hiding / protection requests
// ---------------------------------------------------------------------------

// Candidates seen in the wild, newest first. The probe validates each one, so
// the order only affects how quickly the right one is found.
static const ULONG kLinksOffsets[] = { 0x448, 0x2f0, 0x2e8, 0x2e0, 0x188 };

// A real ActiveProcessLinks entry is a circular list whose owning EPROCESS has
// its own pid 8 bytes in front of the link. Anything else is a false positive.
static BOOLEAN ProbeLinksOffset(_In_ ULONG offset, _In_ ULONG systemPid) {
    BOOLEAN ok = FALSE;

    __try {
        PEPROCESS system = PsInitialSystemProcess;
        PLIST_ENTRY entry;
        PLIST_ENTRY cur;
        ULONG seen = 0;
        ULONG plausible = 0;

        if (system == NULL) return FALSE;

        entry = (PLIST_ENTRY)((PUCHAR)system + offset);
        if ((ULONG_PTR)entry->Flink < 0xFFFF000000000000ull) return FALSE;
        if ((ULONG_PTR)entry->Blink < 0xFFFF000000000000ull) return FALSE;
        if (entry->Flink->Blink != entry || entry->Blink->Flink != entry) return FALSE;
        if (*(ULONG*)((PUCHAR)system + offset - sizeof(PVOID)) != systemPid) return FALSE;

        cur = entry->Flink;
        while (cur != entry && seen < 8) {
            const ULONG otherPid = *(ULONG*)((PUCHAR)cur - sizeof(PVOID));
            if (otherPid >= 4 && otherPid < 0x100000) ++plausible;
            cur = cur->Flink;
            ++seen;
        }

        ok = (plausible >= 2) ? TRUE : FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    return ok;
}

static ULONG DetectLinksOffset(void) {
    ULONG systemPid;
    ULONG i;

    if (PsInitialSystemProcess == NULL) return 0;
    systemPid = (ULONG)(ULONG_PTR)PsGetProcessId(PsInitialSystemProcess);

    for (i = 0; i < RTL_NUMBER_OF(kLinksOffsets); ++i) {
        if (ProbeLinksOffset(kLinksOffsets[i], systemPid)) return kLinksOffsets[i];
    }
    return 0;
}

static NTSTATUS HideProcess(_In_ ULONG pid) {
    PEPROCESS target = NULL;
    PLIST_ENTRY link;
    PLIST_ENTRY prev;
    PLIST_ENTRY next;
    NTSTATUS status;
    ULONG offset;

    if (pid < 8) return STATUS_INVALID_PARAMETER; // Idle / System

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &target);
    if (!NT_SUCCESS(status)) return status;

    offset = (gActiveLinksOffset != 0) ? gActiveLinksOffset : (ULONG)OFF_EPROCESS_ACTIVE_LINKS;
    link = (PLIST_ENTRY)((PUCHAR)target + offset);
    prev = link->Blink;
    next = link->Flink;

    if (prev != NULL && next != NULL && prev != link) {
        prev->Flink = next;
        next->Blink = prev;
        link->Flink = link;
        link->Blink = link;
    }

    ObDereferenceObject(target);
    return STATUS_SUCCESS;
}

static NTSTATUS ProtectProcess(_In_ ULONG pid) {
    PEPROCESS target = NULL;
    NTSTATUS status;
    KIRQL irql;
    ULONG i;
    BOOLEAN stored = FALSE;

    if (pid < 8) return STATUS_INVALID_PARAMETER;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &target);
    if (!NT_SUCCESS(status)) return status;

    KeAcquireSpinLock(&gProtectedLock, &irql);
    for (i = 0; i < FFPANEL_MAX_PROTECTED; ++i) {
        if (gProtected[i].process == target) { stored = TRUE; break; }
    }
    if (!stored) {
        for (i = 0; i < FFPANEL_MAX_PROTECTED; ++i) {
            if (gProtected[i].process == NULL) {
                gProtected[i].pid = pid;
                gProtected[i].process = target; // reference kept until unload
                stored = TRUE;
                break;
            }
        }
    }
    KeReleaseSpinLock(&gProtectedLock, irql);

    if (!stored) {
        ObDereferenceObject(target);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Module hiding / base query
// ---------------------------------------------------------------------------

typedef struct _FF_PEB_LDR_DATA {
    ULONG Length;
    BOOLEAN Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} FF_PEB_LDR_DATA, *PFF_PEB_LDR_DATA;

typedef struct _FF_PEB {
    UCHAR Reserved1[2];
    UCHAR BeingDebugged;
    UCHAR Reserved2[1];
    PVOID Reserved3[2];
    PFF_PEB_LDR_DATA Ldr;
} FF_PEB, *PFF_PEB;

typedef struct _FF_LDR_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} FF_LDR_ENTRY, *PFF_LDR_ENTRY;

static BOOLEAN NameEqualsCI(_In_ PCUNICODE_STRING a, _In_ PCUNICODE_STRING b) {
    USHORT i, count;
    if (a == NULL || b == NULL || a->Buffer == NULL || b->Buffer == NULL) return FALSE;
    if (a->Length != b->Length) return FALSE;
    count = (USHORT)(a->Length / sizeof(WCHAR));
    for (i = 0; i < count; ++i) {
        if (RtlUpcaseUnicodeChar(a->Buffer[i]) != RtlUpcaseUnicodeChar(b->Buffer[i])) return FALSE;
    }
    return TRUE;
}

static NTSTATUS ModuleNameFromRequest(_In_reads_(maxChars) const WCHAR* raw,
                                      ULONG maxChars,
                                      _Out_ PUNICODE_STRING name) {
    ULONG len = 0;
    if (raw == NULL) return STATUS_INVALID_PARAMETER;
    while (len < maxChars && raw[len] != L'\0') ++len;
    if (len == 0 || len >= maxChars) return STATUS_INVALID_PARAMETER;

    name->Buffer = (PWCH)raw; // IRP system buffer: kernel memory, valid for the call
    name->Length = (USHORT)(len * sizeof(WCHAR));
    name->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static VOID AttachToProcess(_In_ PEPROCESS process, _Out_ PKAPC_STATE apcState, _Out_ PBOOLEAN attached) {
    if (process != PsGetCurrentProcess()) {
        KeStackAttachProcess((PKPROCESS)process, apcState);
        *attached = TRUE;
    } else {
        *attached = FALSE;
    }
}

static NTSTATUS FindModuleAttached(_In_ PEPROCESS process,
                                   _In_ PCUNICODE_STRING name,
                                   _Out_ PFF_LDR_ENTRY* entry) {
    PFF_PEB peb;
    PLIST_ENTRY head, cur;
    ULONG visited = 0;

    *entry = NULL;
    peb = (PFF_PEB)PsGetProcessPeb(process);
    if (peb == NULL || peb->Ldr == NULL) return STATUS_NOT_FOUND;

    head = &peb->Ldr->InLoadOrderModuleList;
    if (head->Flink == NULL || head->Flink == head) return STATUS_DLL_NOT_FOUND;

    for (cur = head->Flink; cur != head; cur = cur->Flink) {
        PFF_LDR_ENTRY candidate = CONTAINING_RECORD(cur, FF_LDR_ENTRY, InLoadOrderLinks);
        if (NameEqualsCI(&candidate->BaseDllName, name)) {
            *entry = candidate;
            return STATUS_SUCCESS;
        }
        if (++visited >= FFPANEL_MAX_MODULES) break;
        if (cur->Flink == NULL) break;
    }
    return STATUS_DLL_NOT_FOUND;
}

static VOID UnlinkListEntry(_Inout_ PLIST_ENTRY entry) {
    if (entry->Flink == NULL || entry->Blink == NULL) return;
    if (entry->Flink == entry) return;
    entry->Blink->Flink = entry->Flink;
    entry->Flink->Blink = entry->Blink;
    entry->Flink = entry;
    entry->Blink = entry;
}

static VOID RelinkListEntry(_Inout_ PLIST_ENTRY head, _Inout_ PLIST_ENTRY entry) {
    entry->Flink = head->Flink;
    entry->Blink = head;
    head->Flink->Blink = entry;
    head->Flink = entry;
}

// Remembers which entry was unlinked, so IOCTL_UNHIDE_MODULE can put it back.
// The entry lives in the target's heap and stays valid while the module is
// loaded; if the game frees the module first, unhiding returns NOT_FOUND.
static VOID RememberHiddenModule(_In_ ULONG pid, _In_ PVOID entry) {
    KIRQL irql;
    ULONG i;

    KeAcquireSpinLock(&gProtectedLock, &irql);
    for (i = 0; i < FFPANEL_MAX_HIDDEN; ++i) {
        if (gHidden[i].entry == NULL || gHidden[i].entry == entry) {
            gHidden[i].pid = pid;
            gHidden[i].entry = entry;
            break;
        }
    }
    KeReleaseSpinLock(&gProtectedLock, irql);
}

static PVOID TakeHiddenModule(_In_ ULONG pid) {
    KIRQL irql;
    ULONG i;
    PVOID entry = NULL;

    KeAcquireSpinLock(&gProtectedLock, &irql);
    for (i = 0; i < FFPANEL_MAX_HIDDEN; ++i) {
        if (gHidden[i].entry != NULL && gHidden[i].pid == pid) {
            entry = gHidden[i].entry;
            gHidden[i].entry = NULL;
            gHidden[i].pid = 0;
            break;
        }
    }
    KeReleaseSpinLock(&gProtectedLock, irql);
    return entry;
}

static NTSTATUS HideModule(ULONG pid, PCUNICODE_STRING name) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    KAPC_STATE apcState;
    BOOLEAN attached = FALSE;
    PFF_LDR_ENTRY entry = NULL;

    if (!NT_SUCCESS(status)) return status;

    AttachToProcess(process, &apcState, &attached);
    __try {
        status = FindModuleAttached(process, name, &entry);
        if (NT_SUCCESS(status) && entry != NULL) {
            UnlinkListEntry(&entry->InLoadOrderLinks);
            UnlinkListEntry(&entry->InMemoryOrderLinks);
            UnlinkListEntry(&entry->InInitializationOrderLinks);
            RememberHiddenModule(pid, entry);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (attached) KeUnstackDetachProcess(&apcState);

    ObDereferenceObject(process);
    return status;
}

// Puts the entry back at the front of the three loader lists. The order inside
// the list is not part of any contract, so re-inserting at the head is enough.
static NTSTATUS UnhideModule(ULONG pid) {
    PEPROCESS process = NULL;
    NTSTATUS status;
    KAPC_STATE apcState;
    BOOLEAN attached = FALSE;
    PVOID entry = TakeHiddenModule(pid);

    if (entry == NULL) return STATUS_NOT_FOUND;

    status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    if (!NT_SUCCESS(status)) { RememberHiddenModule(pid, entry); return status; }

    AttachToProcess(process, &apcState, &attached);
    __try {
        PFF_PEB peb = (PFF_PEB)PsGetProcessPeb(process);
        if (peb == NULL || peb->Ldr == NULL) {
            status = STATUS_NOT_FOUND;
        } else {
            PFF_LDR_ENTRY ldr = (PFF_LDR_ENTRY)entry;
            RelinkListEntry(&peb->Ldr->InLoadOrderModuleList, &ldr->InLoadOrderLinks);
            RelinkListEntry(&peb->Ldr->InMemoryOrderModuleList, &ldr->InMemoryOrderLinks);
            RelinkListEntry(&peb->Ldr->InInitializationOrderModuleList, &ldr->InInitializationOrderLinks);
            status = STATUS_SUCCESS;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (attached) KeUnstackDetachProcess(&apcState);

    if (!NT_SUCCESS(status)) RememberHiddenModule(pid, entry);
    ObDereferenceObject(process);
    return status;
}

static NTSTATUS QueryModuleBase(ULONG pid, PCUNICODE_STRING name, _Out_ PULONG64 base) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &process);
    KAPC_STATE apcState;
    BOOLEAN attached = FALSE;
    PFF_LDR_ENTRY entry = NULL;

    if (!NT_SUCCESS(status)) return status;

    *base = 0;
    AttachToProcess(process, &apcState, &attached);
    __try {
        status = FindModuleAttached(process, name, &entry);
        if (NT_SUCCESS(status)) {
            if (entry->DllBase == NULL) status = STATUS_NOT_FOUND;
            else *base = (ULONG64)(ULONG_PTR)entry->DllBase;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (attached) KeUnstackDetachProcess(&apcState);

    ObDereferenceObject(process);
    return status;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

static NTSTATUS DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;

    UNREFERENCED_PARAMETER(dev);

    switch (code) {
    case IOCTL_HIDE_PROCESS: {
        if (buffer == NULL || inLen < sizeof(HIDE_PROCESS_REQ)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = HideProcess(((PHIDE_PROCESS_REQ)buffer)->pid);
        break;
    }
    case IOCTL_PROTECT_PROCESS: {
        if (buffer == NULL || inLen < sizeof(HIDE_PROCESS_REQ)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = ProtectProcess(((PHIDE_PROCESS_REQ)buffer)->pid);
        break;
    }
    case IOCTL_HIDE_MODULE: {
        UNICODE_STRING name;
        if (buffer == NULL || inLen < sizeof(HIDE_MODULE_REQ)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = ModuleNameFromRequest(((PHIDE_MODULE_REQ)buffer)->moduleName,
                                       (ULONG)RTL_NUMBER_OF(((PHIDE_MODULE_REQ)buffer)->moduleName),
                                       &name);
        if (!NT_SUCCESS(status)) break;
        status = HideModule(((PHIDE_MODULE_REQ)buffer)->pid, &name);
        break;
    }
    case IOCTL_UNHIDE_MODULE: {
        if (buffer == NULL || inLen < sizeof(HIDE_PROCESS_REQ)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = UnhideModule(((PHIDE_PROCESS_REQ)buffer)->pid);
        break;
    }
    case IOCTL_GET_MODULE_BASE: {
        UNICODE_STRING name;
        ULONG64 base = 0;
        if (buffer == NULL || inLen < sizeof(MODULE_BASE_REQ)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        if (outLen < sizeof(MODULE_BASE_RESP)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = ModuleNameFromRequest(((PMODULE_BASE_REQ)buffer)->moduleName,
                                       (ULONG)RTL_NUMBER_OF(((PMODULE_BASE_REQ)buffer)->moduleName),
                                       &name);
        if (!NT_SUCCESS(status)) break;
        status = QueryModuleBase(((PMODULE_BASE_REQ)buffer)->pid, &name, &base);
        if (NT_SUCCESS(status)) {
            ((PMODULE_BASE_RESP)buffer)->base = base;
            info = sizeof(MODULE_BASE_RESP);
        }
        break;
    }
    default:
        break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    UNREFERENCED_PARAMETER(dev);

    if (IoGetCurrentIrpStackLocation(irp)->MajorFunction == IRP_MJ_CREATE) {
        // The process that talks to us keeps full access to protected PIDs.
        gClientPid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    }

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static VOID DriverUnload(PDRIVER_OBJECT driver) {
    UNICODE_STRING symlink;
    ULONG i;

    // Must happen before anything else: the callbacks point into this image.
    UnregisterProtection();

    for (i = 0; i < FFPANEL_MAX_PROTECTED; ++i) {
        if (gProtected[i].process != NULL) {
            ObDereferenceObject(gProtected[i].process);
            gProtected[i].process = NULL;
            gProtected[i].pid = 0;
        }
    }

    RtlZeroMemory(gHidden, sizeof(gHidden));

    RtlInitUnicodeString(&symlink, FF_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlink);
    if (driver->DeviceObject != NULL) IoDeleteDevice(driver->DeviceObject);

    DbgPrint("[FFPanel] driver unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING regPath) {
    UNICODE_STRING devName;
    UNICODE_STRING symlink;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(regPath);

    KeInitializeSpinLock(&gProtectedLock);
    RtlZeroMemory(gProtected, sizeof(gProtected));
    RtlZeroMemory(gHidden, sizeof(gHidden));
    gClientPid = 0;
    gObRegistration = NULL;
    gActiveLinksOffset = 0;

    // EPROCESS layout differs per build; work it out from the live system
    // instead of trusting one hardcoded number.
    gActiveLinksOffset = DetectLinksOffset();
    if (gActiveLinksOffset != 0)
        DbgPrint("[FFPanel] ActiveProcessLinks = 0x%X\n", gActiveLinksOffset);
    else
        DbgPrint("[FFPanel] ActiveProcessLinks not detected, using 0x%X\n",
                 (ULONG)OFF_EPROCESS_ACTIVE_LINKS);

    HideDriver(driver);

    RtlInitUnicodeString(&devName, FF_DEVICE_NAME);
    RtlInitUnicodeString(&symlink, FF_SYMLINK_NAME);

    status = IoCreateDevice(driver, 0, &devName, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status)) return status;

    status = IoCreateSymbolicLink(&symlink, &devName);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(device); return status; }

    status = RegisterProtection();
    if (!NT_SUCCESS(status)) {
        // No signature or a blocked altitude: keep running without the handle
        // filter instead of failing to load.
        DbgPrint("[FFPanel] ObRegisterCallbacks failed: 0x%08X\n", status);
        gObRegistration = NULL;
    }

    driver->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoctl;
    driver->DriverUnload = DriverUnload;

    DbgPrint("[FFPanel] driver loaded\n");
    return STATUS_SUCCESS;
}

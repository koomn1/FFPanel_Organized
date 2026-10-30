#pragma once
#include <ntifs.h>
#include <ntddk.h>

#define DRIVER_TAG 'nelP'

// Device names. They are compile-time strings: the client has to use the same
// pair. Hiding them would mean generating them at load time and storing them
// where the client can read them (the service's Parameters key).
#define FF_DEVICE_NAME  L"\\Device\\_rT7y9Xa2Q"
#define FF_SYMLINK_NAME L"\\DosDevices\\_rT7y9Xa2Q"

// Altitude for the process-handle callback. Higher runs later; the number only
// has to be unique per machine.
#define FF_ALTITUDE L"321000"

#define IOCTL_HIDE_PROCESS    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HIDE_MODULE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROTECT_PROCESS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_MODULE_BASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_UNHIDE_MODULE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _HIDE_PROCESS_REQ {
    ULONG pid;
} HIDE_PROCESS_REQ, *PHIDE_PROCESS_REQ;

typedef struct _HIDE_MODULE_REQ {
    ULONG pid;
    WCHAR moduleName[64];
} HIDE_MODULE_REQ, *PHIDE_MODULE_REQ;

typedef struct _MODULE_BASE_REQ {
    ULONG pid;
    WCHAR moduleName[64];
} MODULE_BASE_REQ, *PMODULE_BASE_REQ;

typedef struct _MODULE_BASE_RESP {
    ULONG64 base;
} MODULE_BASE_RESP, *PMODULE_BASE_RESP;

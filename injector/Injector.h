#pragma once
#include <Windows.h>
#include <vector>
#include <cstdint>

// Manual map an image into `hProc`.
//
// `pid` is the target's process id: it is used to find a thread to hijack, so
// the payload can start without creating a thread of our own.
bool ManualMap(HANDLE hProc, DWORD pid, const std::vector<uint8_t>& dllBytes);

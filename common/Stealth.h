#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <random>
#include <chrono>

// Anti-detection helpers shared by the loader and the injected panel.
//
// Each helper closes one concrete detection surface rather than adding noise:
//   randomToken        - no product string in window classes, titles or log paths
//   hideThreadFromDebugger - our threads are not enumerable by a debugger
//   excludeFromCapture - the overlay is left out of screen capture / screenshots
//   antiCheatPresent   - loaded AC modules are visible to the caller
//
// Nothing here hides the cheat from a kernel driver that inspects memory or
// thread creation: that requires the driver side, not user mode.

namespace stealth {

inline std::string randomToken(size_t len = 8) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::mt19937 rng((unsigned)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
                     ((unsigned)GetCurrentProcessId() << 16) ^ (unsigned)GetCurrentThreadId());
    std::uniform_int_distribution<int> pick(0, (int)sizeof(alphabet) - 2);

    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) out += alphabet[pick(rng)];
    return out;
}

inline std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Must be called on the thread to be hidden (or with a handle to it).
inline void hideThreadFromDebugger(HANDLE thread) {
    using NtSetInformationThread_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    static NtSetInformationThread_t setInfo = (NtSetInformationThread_t)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationThread");

    ULONG threadHideFromDebugger = 0x11;
    if (setInfo && thread) setInfo(thread, threadHideFromDebugger, nullptr, 0);
}

// Windows 10 2004+. Older builds reject WDA_EXCLUDEFROMCAPTURE; the caller can
// fall back to WDA_MONITOR, which still blanks the window in captures.
inline bool excludeFromCapture(HWND window) {
    if (!window) return false;
    const DWORD excludeFromCaptureFlag = 0x00000011;
    const DWORD monitorFlag = 0x00000001;
    if (SetWindowDisplayAffinity(window, excludeFromCaptureFlag)) return true;
    return SetWindowDisplayAffinity(window, monitorFlag) != FALSE;
}

// Loaded user-mode anti-cheat components. Best effort: a missing module proves
// nothing, a hit is a strong signal.
inline bool antiCheatPresent() {
    static const wchar_t* kModules[] = {
        L"ace-base.dll", L"ace-game.dll", L"anticheatexpert.dll",
        L"tenprotect.dll", L"tp3helper.dll", L"sguard.dll",
    };
    for (const wchar_t* name : kModules) {
        if (GetModuleHandleW(name)) return true;
    }
    return false;
}

inline bool antiCheatProcess() {
    static const wchar_t* kProcesses[] = {
        L"ace-guard.exe", L"anticheatexpert.exe", L"sguard.exe", L"teniodl.exe",
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            for (const wchar_t* name : kProcesses) {
                if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
            }
            if (found) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace stealth

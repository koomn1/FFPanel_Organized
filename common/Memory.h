#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <string>

class Memory {
public:
    HANDLE hProcess = nullptr;
    DWORD pid = 0;
    uintptr_t base = 0;

    bool attach(const std::wstring& processName, const std::wstring& moduleName) {
        detach();
        pid = findPid(processName);
        if (!pid) return false;
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!hProcess) return false;
        base = findModuleBase(moduleName);
        return base != 0;
    }

    void detach() {
        if (hProcess) { CloseHandle(hProcess); hProcess = nullptr; }
        pid = 0; base = 0;
    }

    template <typename T>
    T read(uintptr_t addr) const {
        T val{};
        if (hProcess && addr) ReadProcessMemory(hProcess, (LPCVOID)addr, &val, sizeof(T), nullptr);
        return val;
    }

    bool readBuf(uintptr_t addr, void* buf, size_t size) const {
        return hProcess && addr && ReadProcessMemory(hProcess, (LPCVOID)addr, buf, size, nullptr);
    }

private:
    static DWORD findPid(const std::wstring& name) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{ sizeof(pe) };
        DWORD out = 0;
        if (Process32FirstW(hSnap, &pe)) {
            do { if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) { out = pe.th32ProcessID; break; } } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return out;
    }

    uintptr_t findModuleBase(const std::wstring& moduleName) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W me{ sizeof(me) };
        uintptr_t out = 0;
        if (Module32FirstW(hSnap, &me)) {
            do { if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) { out = (uintptr_t)me.modBaseAddr; break; } } while (Module32NextW(hSnap, &me));
        }
        CloseHandle(hSnap);
        return out;
    }
};

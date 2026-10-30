#pragma once
#include <Windows.h>
#include <string>

// Client for the driver's control device.
//
// Everything here is best effort: the loader keeps working when the driver is
// not installed or not started. The IOCTL numbers are computed exactly like
// CTL_CODE() in driver/Driver.h, so the two files have to stay in sync.
namespace drv {

#define FF_DEVICE_PATH L"\\\\.\\_rT7y9Xa2Q"

#define FF_CTL_CODE(device, function, method, access) \
    (((device) << 16) | ((access) << 14) | ((function) << 2) | (method))

// FILE_DEVICE_UNKNOWN = 0x22, FILE_ANY_ACCESS = 0, METHOD_BUFFERED = 0
inline constexpr DWORD kIoctlHideProcess    = FF_CTL_CODE(0x22, 0x800, 0, 0); // 0x00222000
inline constexpr DWORD kIoctlProtectProcess = FF_CTL_CODE(0x22, 0x802, 0, 0); // 0x00222008
inline constexpr DWORD kIoctlUnhideModule   = FF_CTL_CODE(0x22, 0x804, 0, 0); // 0x00222010

struct ProcessRequest {
    ULONG pid;
};

class DriverClient {
public:
    ~DriverClient() { close(); }

    // False when the driver is not loaded; `error()` says why.
    bool open() {
        close();
        handle_ = CreateFileW(FF_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = nullptr;
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                error_ = L"driver not loaded";
            } else if (err == ERROR_ACCESS_DENIED) {
                error_ = L"access denied (run as admin)";
            } else {
                wchar_t buffer[64]{};
                wsprintfW(buffer, L"CreateFile failed (%lu)", err);
                error_ = buffer;
            }
            return false;
        }
        error_.clear();
        return true;
    }

    bool protect(DWORD pid) { return ioctl(kIoctlProtectProcess, pid); }
    bool hideProcess(DWORD pid) { return ioctl(kIoctlHideProcess, pid); }
    bool unhideModules(DWORD pid) { return ioctl(kIoctlUnhideModule, pid); }

    void close() {
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
    }

    const std::wstring& error() const { return error_; }
    bool isOpen() const { return handle_ != nullptr; }

private:
    bool ioctl(DWORD code, DWORD pid) {
        if (!handle_) return false;

        ProcessRequest request{};
        request.pid = pid;

        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(handle_, code, &request, sizeof(request),
                                        nullptr, 0, &returned, nullptr);
        if (!ok) {
            wchar_t buffer[64]{};
            wsprintfW(buffer, L"DeviceIoControl 0x%lX failed (%lu)", code, GetLastError());
            error_ = buffer;
        }
        return ok != FALSE;
    }

    HANDLE handle_ = nullptr;
    std::wstring error_;
};

} // namespace drv

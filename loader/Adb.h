#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>

// Thin wrapper around the adb CLI.
//
// Everything here shells out to a real adb.exe over a pipe, so it works with
// the SDK build and with the adb that emulators ship (Gameloop, LDPlayer, Nox,
// MEmu, BlueStacks). No shell32 dependency, no daemon of our own.
//
// Commands run with a timeout so a wedged adb server cannot hang the loader.

namespace adbcli {

struct Result {
    DWORD exitCode = 1;
    bool timedOut = false;
    std::string out;
};

inline void trim(std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

inline std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

inline bool fileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs exe with args, merges stdout+stderr into one pipe, stops after timeoutMs.
inline bool runCapture(const std::wstring& exe, const std::wstring& args, Result& r, DWORD timeoutMs) {
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) { cmd += L' '; cmd += args; }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }

    const ULONGLONG start = GetTickCount64();
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            const DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            DWORD got = 0;
            if (ReadFile(rd, buf, want, &got, nullptr) && got > 0) r.out.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                const DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
                DWORD got = 0;
                if (!ReadFile(rd, buf, want, &got, nullptr) || got == 0) break;
                r.out.append(buf, got);
            }
            break;
        }
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            r.timedOut = true;
            break;
        }
        Sleep(5);
    }

    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return true;
}

// Bounded recursive search for a file name under root. Returns "" when not found.
inline std::wstring findInTree(const std::wstring& root, const wchar_t* fileName, int depthLeft) {
    if (depthLeft < 0) return std::wstring();
    const std::wstring direct = root + L"\\" + fileName;
    if (fileExists(direct)) return direct;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        found = findInTree(root + L"\\" + fd.cFileName, fileName, depthLeft - 1);
        if (!found.empty()) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

inline std::wstring expandEnv(const wchar_t* pattern) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(pattern, buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
    return std::wstring(buf);
}

} // namespace adbcli

class Adb {
public:
    struct Device {
        std::string serial;
        std::string state;
    };

    // explicitPath wins; otherwise PATH, %ADB%, the SDK, then the usual emulators.
    bool init(const std::wstring& explicitPath = L"") {
        if (!explicitPath.empty()) {
            if (!adbcli::fileExists(explicitPath)) return false;
            exe_ = explicitPath;
            return true;
        }
        exe_ = locate();
        return !exe_.empty();
    }

    const std::wstring& exe() const { return exe_; }
    const std::string& serial() const { return serial_; }
    void setSerial(const std::string& s) { serial_ = s; }

    bool startServer() {
        adbcli::Result r;
        return adbcli::runCapture(exe_, L"start-server", r, 20000);
    }

    std::vector<Device> devices() {
        std::vector<Device> list;
        adbcli::Result r;
        if (!adbcli::runCapture(exe_, L"devices", r, 20000)) return list;

        bool inList = false;
        size_t pos = 0;
        while (pos < r.out.size()) {
            const size_t eol = r.out.find('\n', pos);
            std::string line = r.out.substr(pos, (eol == std::string::npos ? r.out.size() : eol) - pos);
            pos = (eol == std::string::npos) ? r.out.size() : eol + 1;
            adbcli::trim(line);
            if (line.rfind("List of devices", 0) == 0) { inList = true; continue; }
            if (!inList || line.empty() || line[0] == '*') continue;

            const size_t sep = line.find_first_of(" \t");
            if (sep == std::string::npos) continue;
            Device d;
            d.serial = line.substr(0, sep);
            d.state = line.substr(sep + 1);
            adbcli::trim(d.state);
            list.push_back(d);
        }
        return list;
    }

    // Picks the first usable device (or the one requested through setSerial).
    bool pickDevice(std::string& chosen) {
        const std::vector<Device> list = devices();
        if (!serial_.empty()) {
            for (const Device& d : list) {
                if (d.serial == serial_ && d.state == "device") { chosen = d.serial; return true; }
            }
        }
        for (const Device& d : list) {
            if (d.state == "device") { serial_ = d.serial; chosen = d.serial; return true; }
        }
        return false;
    }

    // Emulators often do not register with the server until told to. Tries the
    // default ports the common ones listen on.
    bool connectLocalEmulator(std::string& chosen) {
        static const int ports[] = { 5555, 5556, 21503, 62001, 7555, 5585 };
        for (int port : ports) {
            adbcli::Result r;
            adbcli::runCapture(exe_, L"connect 127.0.0.1:" + std::to_wstring(port), r, 5000);
            if (pickDevice(chosen)) return true;
        }
        return false;
    }

    bool shell(const std::string& command, std::string& out, DWORD timeoutMs = 15000) {
        std::wstring args;
        if (!serial_.empty()) args = L"-s " + adbcli::widen(serial_) + L" ";
        args += L"shell \"" + adbcli::widen(command) + L"\"";

        adbcli::Result r;
        if (!adbcli::runCapture(exe_, args, r, timeoutMs)) return false;
        out = r.out;
        return !r.timedOut;
    }

    bool property(const char* key, std::string& value) {
        std::string out;
        if (!shell(std::string("getprop ") + key, out, 5000)) return false;
        adbcli::trim(out);
        value = out;
        return !value.empty();
    }

    // "package:/data/app/.../base.apk" -> apk path on device.
    bool packagePath(const std::string& pkg, std::string& apkPath) {
        std::string out;
        apkPath.clear();
        if (!shell("pm path " + pkg, out, 8000)) return false;
        static const std::string key = "package:";
        const size_t p = out.find(key);
        if (p == std::string::npos) return false;
        const size_t e = out.find_first_of("\r\n", p);
        apkPath = out.substr(p + key.size(), (e == std::string::npos ? out.size() : e) - (p + key.size()));
        adbcli::trim(apkPath);
        return !apkPath.empty();
    }

    // Reads versionName/versionCode out of "dumpsys package <pkg>".
    bool packageVersion(const std::string& pkg, std::string& versionName, std::string& versionCode) {
        versionName.clear();
        versionCode.clear();
        std::string out;
        if (!shell("dumpsys package " + pkg, out, 20000)) return false;
        versionName = tokenAfter(out, "versionName=");
        versionCode = tokenAfter(out, "versionCode=");
        if (versionName == "null") versionName.clear();
        return !versionName.empty() || !versionCode.empty();
    }

    bool pull(const std::string& remote, const std::wstring& local) {
        std::wstring args;
        if (!serial_.empty()) args = L"-s " + adbcli::widen(serial_) + L" ";
        args += L"pull \"" + adbcli::widen(remote) + L"\" \"" + local + L"\"";
        adbcli::Result r;
        return adbcli::runCapture(exe_, args, r, 120000) && r.exitCode == 0;
    }

private:
    static std::string tokenAfter(const std::string& src, const char* key) {
        const size_t p = src.find(key);
        if (p == std::string::npos) return std::string();
        const size_t b = p + strlen(key);
        const size_t e = src.find_first_of(" \t\r\n", b);
        std::string v = src.substr(b, (e == std::string::npos ? src.size() : e) - b);
        adbcli::trim(v);
        return v;
    }

    static std::wstring locate() {
        wchar_t onPath[MAX_PATH]{};
        if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, onPath, nullptr)) return onPath;

        const wchar_t* fromEnv = _wgetenv(L"ADB");
        if (fromEnv && *fromEnv && adbcli::fileExists(fromEnv)) return fromEnv;

        const wchar_t* candidates[] = {
            L"%LOCALAPPDATA%\\Android\\Sdk\\platform-tools\\adb.exe",
            L"%ProgramFiles%\\LDPlayer\\LDPlayer9\\adb.exe",
            L"%ProgramFiles(x86)%\\LDPlayer\\LDPlayer9\\adb.exe",
            L"%ProgramFiles%\\LDPlayer\\LDPlayer4.0\\adb.exe",
            L"%ProgramFiles%\\Nox\\bin\\adb.exe",
            L"%ProgramFiles(x86)%\\Nox\\bin\\adb.exe",
            L"%ProgramFiles%\\BlueStacks_nxt\\HD-Adb.exe",
            L"%ProgramFiles%\\Microvirt\\MEmu\\adb.exe",
            L"%ProgramFiles(x86)%\\Microvirt\\MEmu\\adb.exe",
            L"%ProgramFiles%\\SmartGaga\\adb.exe",
            L"%ProgramFiles(x86)%\\SmartGaga\\adb.exe",
            L"%ProgramFiles%\\TxGameAssistant\\adb.exe",
        };
        for (const wchar_t* c : candidates) {
            const std::wstring path = adbcli::expandEnv(c);
            if (!path.empty() && adbcli::fileExists(path)) return path;
        }

        // Gameloop and friends hide adb a few levels down; search their trees.
        const wchar_t* roots[] = {
            L"%ProgramFiles%\\TxGameAssistant",
            L"%ProgramFiles(x86)%\\TxGameAssistant",
            L"%ProgramFiles%\\LDPlayer",
            L"%ProgramFiles(x86)%\\LDPlayer",
            L"%ProgramFiles%\\SmartGaga",
        };
        for (const wchar_t* r : roots) {
            const std::wstring root = adbcli::expandEnv(r);
            if (root.empty()) continue;
            std::wstring hit = adbcli::findInTree(root, L"adb.exe", 5);
            if (!hit.empty()) return hit;
        }
        return std::wstring();
    }

    std::wstring exe_;
    std::string serial_;
};

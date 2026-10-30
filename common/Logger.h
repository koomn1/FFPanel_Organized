#pragma once
#include <cstdio>
#include <cstdarg>
#include <Windows.h>

// Two sinks, both off by default:
//   initConsole() - visible console, for the loader and for debugging
//   initFile()    - append to a file, for the injected panel
// With neither initialised every call is a no-op, so nothing leaks into the
// game's stdout once the panel is inside the process.
namespace logx {

inline FILE*& fileSink() { static FILE* sink = nullptr; return sink; }
inline bool& consoleOn() { static bool on = false; return on; }

inline void initConsole(const char* title = nullptr) {
    if (!GetConsoleWindow()) AllocConsole();
    if (title && *title) SetConsoleTitleA(title);

    FILE* out = nullptr;
    freopen_s(&out, "CONOUT$", "w", stdout);
    freopen_s(&out, "CONOUT$", "w", stderr);
    consoleOn() = true;
}

inline void initFile(const wchar_t* path) {
    FILE* out = nullptr;
    if (_wfopen_s(&out, path, L"ab") != 0 || !out) return;
    fileSink() = out;
}

inline void shutdown() {
    if (fileSink()) { fclose(fileSink()); fileSink() = nullptr; }
}

inline void vemit(const char* prefix, const char* fmt, va_list ap) {
    FILE* out = fileSink();
    if (!out && !consoleOn()) return;

    char buffer[2048];
    if (vsnprintf(buffer, sizeof(buffer), fmt, ap) < 0) return;

    FILE* target = out ? out : stdout;
    fprintf(target, "%s%s\n", prefix, buffer);
    fflush(target);
}

inline void info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit("", fmt, ap); va_end(ap);
}
inline void warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit("[!] ", fmt, ap); va_end(ap);
}
inline void error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit("[x] ", fmt, ap); va_end(ap);
}

} // namespace logx

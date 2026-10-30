#include <Windows.h>
#include "Menu.h"
#include "Logger.h"
#include "Stealth.h"

static HMODULE gModule = nullptr;

static DWORD WINAPI MainThread(LPVOID) {
    // Keep this thread out of debugger thread lists.
    stealth::hideThreadFromDebugger(GetCurrentThread());

    Menu& m = Menu::get();
    if (!m.init(gModule)) {
        logx::error("menu init failed");
        return 1;
    }

    while (m.isRunning()) {
        m.render();
        Sleep(1);
    }

    logx::info("shutting down");
    logx::shutdown();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = hModule;
        DisableThreadLibraryCalls(hModule);

        HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (thread) {
            stealth::hideThreadFromDebugger(thread);
            CloseHandle(thread);
        }
    }
    return TRUE;
}

#include <Windows.h>
#include <string>
#include <vector>
#include "Logger.h"
#include "Process.h"
#include "Injector.h"
#include "Adb.h"
#include "Config.h"
#include "DriverClient.h"

// Free Fire global and FF Max/India.
static const char* kPackages[] = { "com.dts.freefireth", "com.dts.freefiremax" };

struct Options {
    std::wstring process = L"HD-Player.exe";
    std::wstring payload = L"panel.dll";
    std::wstring adbPath;
    std::string serial;
    std::string package;
    bool useAdb = true;
    bool strictBuild = false;
    bool versionOnly = false;
    bool protectGame = false;   // ask the driver to filter handles to the game
    bool hideProcess = false;   // ask the driver to unlink the game process
    bool unhideModules = false; // ask the driver to re-link a previously hidden module
};

struct GameInfo {
    bool haveDevice = false;
    bool haveVersion = false;
    bool buildMismatch = false;
    std::string serial, model, android, package, versionName, versionCode;
};

static std::string narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

static void usage() {
    logx::info("usage: loader.exe [process] [payload.dll] [options]");
    logx::info("");
    logx::info("  process            target process (default HD-Player.exe)");
    logx::info("  payload.dll        DLL to manual-map (default panel.dll)");
    logx::info("  --adb <path>       explicit adb.exe");
    logx::info("  --serial <serial>  adb device serial");
    logx::info("  --package <pkg>    override the Free Fire package name");
    logx::info("  --no-adb           skip device probing entirely");
    logx::info("  --version          print device/game version and exit");
    logx::info("  --strict-build     refuse to inject when offsets.json is for another build");
    logx::info("  --protect          ask the driver to strip VM access to the game process");
    logx::info("  --hide-process     ask the driver to unlink the game process");
    logx::info("  --unhide           ask the driver to re-link a hidden module");
    logx::info("");
    logx::info("  --protect breaks any other tool that reads the game (the external");
    logx::info("  overlay included); --hide-process hides the game from the process list.");
}

static bool parseArgs(int argc, wchar_t** argv, Options& o) {
    std::vector<std::wstring> positional;
    auto take = [&](int& i, std::wstring& out) -> bool {
        if (i + 1 >= argc) return false;
        out = argv[++i];
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        std::wstring value;
        if (a == L"--no-adb") { o.useAdb = false; }
        else if (a == L"--strict-build") { o.strictBuild = true; }
        else if (a == L"--version") { o.versionOnly = true; }
        else if (a == L"--protect") { o.protectGame = true; }
        else if (a == L"--hide-process") { o.hideProcess = true; }
        else if (a == L"--unhide") { o.unhideModules = true; }
        else if (a == L"--adb") { if (!take(i, o.adbPath)) return false; }
        else if (a == L"--serial") { if (!take(i, value)) return false; o.serial = narrow(value); }
        else if (a == L"--package") { if (!take(i, value)) return false; o.package = narrow(value); }
        else if (a == L"-h" || a == L"--help" || a == L"/?") { return false; }
        else if (!a.empty() && a[0] == L'-') { logx::warn("unknown option %ls", a.c_str()); return false; }
        else positional.push_back(a);
    }

    if (positional.size() > 0) o.process = positional[0];
    if (positional.size() > 1) o.payload = positional[1];
    return true;
}

static std::string offsetsBuild(const char* path) {
    const std::string src = cfgjson::readAll(path);
    if (src.empty()) return std::string();
    std::string v;
    if (!cfgjson::findValue(src, "gameBuild", v)) return std::string();
    adbcli::trim(v);
    return v;
}

// Resolves adb -> device -> Free Fire package -> version, and checks the
// version against the build offsets.json was dumped for.
static bool probeGame(const Options& o, GameInfo& g) {
    Adb adb;
    if (!adb.init(o.adbPath)) {
        logx::warn("adb not found - device/game version unavailable");
        logx::warn("  pass --adb <path\\adb.exe>, set %%ADB%%, or use --no-adb");
        return false;
    }
    logx::info("adb            : %ls", adb.exe().c_str());

    adb.startServer();
    if (!o.serial.empty()) adb.setSerial(o.serial);

    std::string chosen;
    if (!adb.pickDevice(chosen)) {
        logx::warn("no device attached - probing local emulator ports");
        adb.connectLocalEmulator(chosen);
    }
    if (chosen.empty()) {
        for (const Adb::Device& d : adb.devices()) {
            if (d.state != "device") logx::warn("device %s is %s", d.serial.c_str(), d.state.c_str());
        }
        logx::warn("no usable adb device - device/game version unavailable");
        return false;
    }

    g.haveDevice = true;
    g.serial = chosen;
    logx::info("device         : %s", chosen.c_str());
    if (adb.property("ro.product.model", g.model)) logx::info("model          : %s", g.model.c_str());
    if (adb.property("ro.build.version.release", g.android)) logx::info("android        : %s", g.android.c_str());

    std::vector<std::string> packages;
    if (!o.package.empty()) packages.push_back(o.package);
    else { packages.push_back(kPackages[0]); packages.push_back(kPackages[1]); }

    for (const std::string& pkg : packages) {
        std::string apk;
        if (!adb.packagePath(pkg, apk)) continue;
        g.package = pkg;
        logx::info("package        : %s", pkg.c_str());
        logx::info("apk            : %s", apk.c_str());
        break;
    }
    if (g.package.empty()) {
        logx::warn("Free Fire is not installed on this device (%s / %s)", kPackages[0], kPackages[1]);
        return false;
    }

    if (adb.packageVersion(g.package, g.versionName, g.versionCode)) {
        g.haveVersion = true;
        logx::info("ff version     : %s", g.versionName.empty() ? "unknown" : g.versionName.c_str());
        if (!g.versionCode.empty()) logx::info("ff build code  : %s", g.versionCode.c_str());
    } else {
        logx::warn("could not read the package version from dumpsys");
    }

    const std::string expected = offsetsBuild("offsets.json");
    if (expected.empty()) {
        logx::warn("offsets.json has no gameBuild - cannot check the dump matches this build");
    } else if (!g.versionName.empty() && expected != g.versionName) {
        g.buildMismatch = true;
        logx::warn("offsets.json was dumped for %s, device runs %s", expected.c_str(), g.versionName.c_str());
        logx::warn("  re-dump the offsets or update offsets.json - expect wrong reads");
    } else {
        logx::info("offsets build  : %s (matches)", expected.c_str());
    }
    return true;
}

static bool readFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)256 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    const bool ok = ReadFile(file, out.data(), (DWORD)out.size(), &read, nullptr) &&
                    read == (DWORD)out.size();
    CloseHandle(file);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    logx::initConsole("FFPanel loader");

    Options o;
    if (!parseArgs(argc, argv, o)) { usage(); return 0; }

    logx::info("target process : %ls", o.process.c_str());
    logx::info("payload        : %ls", o.payload.c_str());

    std::vector<uint8_t> bytes;
    if (!o.versionOnly && !readFileBytes(o.payload, bytes)) {
        logx::error("cannot read payload file");
        return 1;
    }

    GameInfo game;
    const bool probed = o.useAdb && probeGame(o, game);

    if (o.versionOnly) {
        if (!probed) { logx::error("no device/game information available"); return 2; }
        return game.haveVersion ? 0 : 2;
    }

    if (game.buildMismatch && o.strictBuild) {
        logx::error("offsets.json does not match the installed game build");
        return 3;
    }

    DWORD pid = 0;
    for (int i = 0; i < 40 && !pid; i++) {
        pid = findProcessId(o.process);
        if (!pid) Sleep(500);
    }
    if (!pid) {
        logx::error("process %ls not found, start the game first", o.process.c_str());
        return 1;
    }
    logx::info("pid            : %lu", pid);

    // Talk to the driver first: it exempts this process, so the OpenProcess
    // below still gets full access after the game is protected.
    if (o.protectGame || o.hideProcess || o.unhideModules) {
        drv::DriverClient driver;
        if (!driver.open()) {
            logx::warn("driver not available (%ls) - continuing without it",
                       driver.error().c_str());
        } else {
            if (o.protectGame) {
                logx::info("driver protect : %s",
                           driver.protect(pid) ? "ok" : "failed");
            }
            if (o.hideProcess) {
                logx::info("driver hide    : %s",
                           driver.hideProcess(pid) ? "ok" : "failed");
            }
            if (o.unhideModules) {
                logx::info("driver unhide  : %s",
                           driver.unhideModules(pid) ? "ok" : "failed");
            }
            if (!driver.error().empty())
                logx::warn("driver: %ls", driver.error().c_str());
        }
    }

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        logx::error("OpenProcess failed (%lu)", GetLastError());
        return 1;
    }

    const bool ok = ManualMap(hProc, pid, bytes);
    CloseHandle(hProc);

    if (!ok) {
        logx::error("manual map failed");
        return 1;
    }

    logx::info("payload mapped, press INSERT in game to open the menu");
    return 0;
}

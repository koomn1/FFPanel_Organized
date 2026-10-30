# FFPanel

Project layout for the FFPanel build: a DLL panel (ImGui + DX11 overlay) that is
manual-mapped into the game process, a standalone external overlay, an injector
library, a loader, and an optional kernel driver.

## Layout

| Path | Contents |
| --- | --- |
| `CMakeLists.txt` | root build, MinHook static lib, subprojects |
| `setup.bat` | fetches `third_party/minhook` and `third_party/json.hpp` |
| `build.bat` | configure + build (`cmake -S . -B build`) |
| `build_driver.bat` | WDK/MSBuild driver build |
| `install_driver.bat` | build + test-sign + create + start the driver service |
| `uninstall_driver.bat` | stop and delete the service (`--remove-cert` drops the cert) |
| `config.json` | runtime settings, copied next to each binary |
| `common/` | `Config.h` (settings + JSON load/save), `Logger.h`, `Memory.h`, `Stealth.h` |
| `sdk/` | `Offsets.h`, `offsets.example.json`, `Structs.h`, `dump_placeholder.cs` |
| `panel/` | injected DLL: `dllmain.cpp`, `Menu.*`, `GameData.h`, `ESP.*`, `Aimbot.*`, `Silent.*` |
| `external/` | standalone overlay process: `main.cpp`, `Overlay.*` |
| `injector/` | `ManualMap` PE mapper (static lib) |
| `loader/` | console loader: `main.cpp`, `Adb.h`, `Auth.h`, `HWID.h`, `Process.h` |
| `driver/` | kernel driver: `Driver.c`, `Driver.h`, `Driver.inf`, `Driver.vcxproj`, test-cert helpers |

## Build

```bat
setup.bat
build.bat
build_driver.bat   :: needs Visual Studio 2022 + WDK, x64, Release
```

Outputs: `build/external/Release/external.exe`, `build/panel/Release/panel.dll`,
`build/loader/Release/loader.exe`, `driver/x64/Release/FFPanel.sys`.

## Offsets

`sdk/Offsets.h` holds two kinds of value:

* **Layout offsets** (`OFF_P_*`, `OFF_CAM_*`, `OFF_LIST_*`) - frozen by the IL2CPP
  object layout and compiled in. They only change if the game changes its classes.
* **Build offsets** (`OFF_PLAYERMANAGER_INSTANCE`, `OFF_CAMERA_INSTANCE`,
  `OFF_LOCALPLAYER_INSTANCE`, `OFF_WEAPON_DIRECTION_FN`) - per game build, so they
  are never compiled in. They are read at startup from `offsets.json`.

```bat
copy sdk\offsets.example.json offsets.json
```

`offsets.json` is looked up in the current working directory: next to
`HD-Player.exe` for the panel, next to `external.exe` for the external overlay.
Each value is an RVA inside `libil2cpp.so` - the number you add to the module
base - taken from your own dump (see `sdk/dump_placeholder.cs`). `0` means
"not resolved": the loader skips it, `ff::Ready()` stays false, and the panel
logs which keys are still missing instead of reading at `base + 0`.

Fill `"gameBuild"` too: the loader compares it against the version it reads from
the device and warns when the offsets belong to another build.

`"module"` says which module the RVAs belong to (`libil2cpp.so` by default). The
panel and the external overlay look the module up by that name first, then fall
back to `libil2cpp.so` / `UnityPlayer.dll`.

`OFF_P_NAME` is read as an Il2Cpp string (length at `+0x10`, UTF-16 at `+0x14`)
and converted to UTF-8, so the ESP "Name" toggle has something to draw.

## Loader

```bat
loader.exe HD-Player.exe panel.dll
```

| Flag | Effect |
| --- | --- |
| `--adb <path>` | explicit `adb.exe` instead of auto-detection |
| `--serial <s>` | adb device serial |
| `--package <pkg>` | override `com.dts.freefireth` / `com.dts.freefiremax` |
| `--no-adb` | skip device probing entirely |
| `--version` | print device/game version and exit (2 = nothing found) |
| `--strict-build` | abort (exit 3) when `offsets.json` is for another build |
| `--protect` | ask the driver to strip VM access to the game process |
| `--hide-process` | ask the driver to unlink the game process |
| `--unhide` | ask the driver to re-link a previously hidden module |

On startup it locates adb (PATH, `%ADB%`, the SDK, then Gameloop / LDPlayer /
Nox / MEmu / BlueStacks trees), starts the server, picks a device - probing the
usual localhost ports when nothing is attached - and reads:

```
adb            : C:\...\platform-tools\adb.exe
device         : 127.0.0.1:5555
model          : ...
android        : 11
package        : com.dts.freefireth
apk            : /data/app/.../base.apk
ff version     : 1.104.1
ff build code  : 2019040001
offsets build  : 1.104.1 (matches)
```

Then it maps `panel.dll` manually into the target:

* imports are rebased to the target's module bases (API sets such as
  `api-ms-win-crt-*.dll` are resolved to the real DLL the target knows), and
  modules the target lacks are loaded there through remote `LoadLibraryA`;
* `DllMain` is entered through a stub that supplies `(hModule, DLL_PROCESS_ATTACH,
  NULL)` - `CreateRemoteThread` alone passes one argument and the panel would
  never initialize;
* a 32-bit (WOW64) target is rejected instead of being written with an x64 image.

## Usage

`INSERT` toggles the menu, `END` (panic key) shuts the panel down.

## Driver

`build_driver.bat` finds MSBuild through `vswhere`, picks the newest WDK under
`%ProgramFiles(x86)%\Windows Kits\10`, and builds `driver/Driver.vcxproj` as a
WDM driver (`SignMode=Off`, no inf2cat - sign it yourself).

```bat
bcdedit /set testsigning on
sc create FFPanel type= kernel binPath= C:\path\to\FFPanel.sys
sc start FFPanel
```

IOCTLs on `\\.\FFPanel` (all `METHOD_BUFFERED`, see `driver/Driver.h`):

| Code | Input | Output | Does |
| --- | --- | --- | --- |
| `IOCTL_HIDE_PROCESS` | `HIDE_PROCESS_REQ` | - | unlinks the process from `ActiveProcessLinks` |
| `IOCTL_PROTECT_PROCESS` | `HIDE_PROCESS_REQ` | - | registers the PID for handle filtering and pins its `EPROCESS` |
| `IOCTL_HIDE_MODULE` | `HIDE_MODULE_REQ` | - | unlinks a module from the three PEB loader lists |
| `IOCTL_GET_MODULE_BASE` | `MODULE_BASE_REQ` | `MODULE_BASE_RESP` | returns `DllBase` of a named module |
| `IOCTL_UNHIDE_MODULE` | `HIDE_PROCESS_REQ` (pid) | - | re-links the module hidden for that pid |

The device here is `\\.\_rT7y9Xa2Q`, not `\\.\FFPanel` - see `FF_SYMLINK_NAME`.

On load the driver removes itself from `PsLoadedModuleList`, so
`NtQuerySystemInformation(SystemModuleInformation)` no longer lists it, and
registers an `ObRegisterCallbacks` filter at altitude `321000`:
* only PIDs passed to `IOCTL_PROTECT_PROCESS` are filtered, so it never becomes
  a machine-wide "nobody can read anything" filter;
* the process that opened the device, and the target process opening a handle to
  itself, keep full access;
* `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION`,
  `PROCESS_CREATE_THREAD`, `PROCESS_SUSPEND_RESUME` and `PROCESS_DUP_HANDLE` are
  stripped from both new handles and duplicated ones;
* `ObUnRegisterCallbacks` runs first in `DriverUnload` - unloading a driver
  while its callback is registered is an immediate bugcheck.

`ActiveProcessLinks` is not hardcoded any more: on load the driver probes the
candidate offsets against `PsInitialSystemProcess` (circular list, owning PID 8
bytes in front of the link, sane PIDs one hop away) and uses the first one that
validates, falling back to `0x448`. The chosen value is printed with `DbgPrint`.

Both module IOCTLs walk `PsGetProcessPeb(process)->Ldr->InLoadOrderModuleList`
under `KeStackAttachProcess`, match `BaseDllName` case-insensitively, and are
wrapped in `__try/__except` so a torn loader list returns an error instead of
bugchecking. `HideModule` leaves `LdrpHashTable` alone, so the game can still
`GetModuleHandle`/`GetProcAddress` the module it can no longer enumerate.

`loader/DriverClient.h` is the client side: it opens `\\.\_rT7y9Xa2Q`, sends
`IOCTL_PROTECT_PROCESS` / `IOCTL_HIDE_PROCESS`, and reports a missing driver as
"driver not loaded" instead of failing the run. The loader sends them only with
`--protect` / `--hide-process`, because protecting the game also locks out any
other tool reading it - the external overlay included. Opening the device sets
the driver's client PID, so the loader keeps full access to the process it just
protected.

## Stealth

`config.json` -> `"stealth"` controls the anti-detection pass. All four default
to the safe value:

| Key | Default | Effect |
| --- | --- | --- |
| `randomizeNames` | `true` | window class and title are random per launch |
| `hideFromCapture` | `true` | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` - the overlay is missing from screenshots and desktop capture |
| `suppressConsole` | `true` | no console in the game process; the panel logs to `%TEMP%\ff_<random>.log` |
| `panicOnDetect` | `false` | `END` still panics; this makes the panel do it by itself when an anti-cheat module shows up |

What the injector does beyond plain manual mapping:

| Detection surface | Handling |
| --- | --- |
| `MZ` / `PE\0\0` in unbacked executable memory | both signatures zeroed after the image is written |
| RWX image | reserved RW, written, then per-section protections (`RX` for `.text`, `RW` for `.data`, ...) |
| thread starting in no module | created through `NtCreateThreadEx` with `THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER` |
| inline hook left in game code | panic calls `MH_DisableHook` + `MH_RemoveHook`, restoring the original bytes |
| our own console | none unless `suppressConsole` is false |

`logx` is silent unless a sink is initialised, so nothing of ours writes to the
game's stdout.

## Injection

`ManualMap(hProc, pid, bytes)` starts the payload without creating a thread of
its own wherever it can:

* **thread hijack** - the first threads of the target are suspended in turn and
  their `RIP` is pointed at a stub. The stub saves every register, runs the TLS
  callbacks, calls `DllMain(hModule, DLL_PROCESS_ATTACH, NULL)`, restores the
  registers and `ret`s into the address that was pushed on the stack before the
  redirect, so the hijacked thread continues its own work. A thread parked in a
  wait only picks the new `RIP` up when the wait ends, so each candidate gets
  80 ms to report in before its context is restored and the next one is tried.
* **thread creation** - if no thread runs the stub in time, the same stub is
  started with `NtCreateThreadEx` (`THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER`).
  The stub ends in `ret` either way, which is why one build serves both paths.

The payload is written into a `PAGE_READWRITE` image, gets its `MZ` and `PE\0\0`
signatures zeroed, then each section is protected according to its flags (`.text`
= `RX`, `.data` = `RW`, never a blanket `RWX`). Imports are rebased onto the
target's module bases; API sets such as `api-ms-win-crt-*.dll` are resolved to
the real DLL the target knows, and modules the target does not have are loaded
there through a remote `LoadLibraryA`.

## First run, step by step

### Without the driver (nothing to install, start here)

```bat
setup.bat                 :: fetches MinHook + json.hpp, needs git
build.bat                 :: -> build\panel\Release\panel.dll
```

1. Put `loader.exe`, `panel.dll`, `config.json` and `offsets.json` in **one
   folder** - the game's own folder is the easiest choice, because the panel
   reads `offsets.json` from the game's working directory.
2. `copy sdk\offsets.example.json offsets.json` and paste the four numbers from
   your dump. Fill `"module"` and `"gameBuild"` while you are there.
3. Start the game and wait until you are past the login screen.
4. Run the loader from that same folder:

```bat
loader.exe HD-Player.exe panel.dll
```

Everything is logged. If the panel does not come up, look at
`%TEMP%\ff_<random>.log` - it names the missing offsets, the module it attached
to, and why the hook or the map failed.

### With the driver (optional, adds handle filtering + hiding)

The driver is **not** needed for ESP, aimbot or silent aim. It only adds:

* `--protect`: other processes lose `PROCESS_VM_READ`/`WRITE`/`THREAD` on the game
* `--hide-process`: the game disappears from the process list
* `IOCTL_HIDE_MODULE` / `IOCTL_GET_MODULE_BASE`

Steps:

1. Visual Studio Installer -> **Modify** -> *Individual components* -> install
   **Windows Driver Kit** (and the matching Windows SDK), then reboot.
2. Turn on test signing once, then reboot:
   ```bat
   bcdedit /set testsigning on
   ```
3. Right click `install_driver.bat` -> **Run as administrator**. It builds the
   `.sys`, creates a self-signed `CN=FFPanelTest` certificate, signs the driver,
   creates the `FFPanel` service and starts it.
4. Use the loader with the driver options:
   ```bat
   loader.exe HD-Player.exe panel.dll --protect --hide-process
   ```

Uninstall with `uninstall_driver.bat` (add `--remove-cert` to drop the
certificate too), then `bcdedit /set testsigning off` if you want the watermark
gone.

## Known limits

1. `offsets.json` must be filled from your own dump. Nothing ships with real
   numbers - the placeholder values were removed rather than guessed.
2. Building the driver needs the WDK kernel headers; inf2cat and signing are
   left to you (`build_driver.bat` stops at the `.sys`).
3. `loader/Auth.h` and `loader/HWID.h` are not wired into the loader: no key
   check runs and `HWID` is not sent anywhere.
4. The driver device names are compile-time strings, so they are the same on
   every install. Hiding from `PsLoadedModuleList` does not hide the device or
   the service entry.
5. TLS callbacks are executed, but `AddressOfIndex` is not set up, so a payload
   that uses `__declspec(thread)` variables still breaks.
6. The `VirtualAllocEx` / `WriteProcessMemory` pair is still visible to a kernel
   anti-cheat; the handle filter only covers read access to the protected PID.
7. `ActiveProcessLinks` is detected from the running build, but the probe only
   knows the offsets listed at the top of `Driver.c`; a new Windows layout needs
   one more entry there.
8. `IOCTL_UNHIDE_MODULE` re-links a module at the front of the loader lists. It
   is not a true undo: the entry order changes, and if the game frees the module
   while it is hidden, unhiding returns `STATUS_NOT_FOUND`.

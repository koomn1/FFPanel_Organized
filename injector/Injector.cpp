#include "Injector.h"
#include <winternl.h>
#include <TlHelp32.h>
#include <cstring>
#include <string>
#include <vector>
#include <string.h>

// ---------------------------------------------------------------------------
// Remote helpers
// ---------------------------------------------------------------------------

static bool writeRemote(HANDLE hProc, void* addr, const void* data, size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(hProc, addr, data, size, &written) && written == size;
}

static bool readRemote(HANDLE hProc, uintptr_t addr, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(hProc, (LPCVOID)addr, out, size, &got) && got == size;
}

typedef LONG(NTAPI* NtCreateThreadEx_t)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID,
                                        ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

static HANDLE remoteCreateThread(HANDLE hProc, uintptr_t start, uintptr_t parameter) {
    static NtCreateThreadEx_t createEx = (NtCreateThreadEx_t)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx");

    const ULONG kHideFromDebugger = 0x4;
    if (createEx) {
        HANDLE thread = nullptr;
        if (createEx(&thread, THREAD_ALL_ACCESS, nullptr, hProc,
                     (PVOID)start, (PVOID)parameter, kHideFromDebugger,
                     0, 0, 0, nullptr) == 0 && thread) {
            return thread;
        }
    }
    return CreateRemoteThread(hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)start,
                              (PVOID)parameter, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Target module lookup (PEB walk)
// ---------------------------------------------------------------------------

namespace pebx {

static const uintptr_t kPebLdr = 0x18;
static const uintptr_t kLdrInLoadOrder = 0x10;
static const uintptr_t kEntryDllBase = 0x30;
static const uintptr_t kEntryBaseDllName = 0x58;

} // namespace pebx

typedef LONG(NTAPI* NtQueryInformationProcess_t)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

static NtQueryInformationProcess_t ntQueryInformationProcess() {
    static NtQueryInformationProcess_t fn = (NtQueryInformationProcess_t)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    return fn;
}

static uintptr_t findRemoteModule(HANDLE hProc, const char* name) {
    const NtQueryInformationProcess_t query = ntQueryInformationProcess();
    if (!query) return 0;

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG returned = 0;
    if (query(hProc, ProcessBasicInformation, &pbi, sizeof(pbi), &returned) != 0) return 0;
    if (!pbi.PebBaseAddress) return 0;

    uintptr_t ldr = 0;
    if (!readRemote(hProc, (uintptr_t)pbi.PebBaseAddress + pebx::kPebLdr, &ldr, sizeof(ldr)) || !ldr)
        return 0;

    const uintptr_t head = ldr + pebx::kLdrInLoadOrder;
    uintptr_t cursor = 0;
    if (!readRemote(hProc, head, &cursor, sizeof(cursor)) || !cursor) return 0;

    wchar_t wanted[MAX_PATH * 2]{};
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wanted, MAX_PATH * 2) <= 0) return 0;

    for (int guard = 0; guard < 512 && cursor != head; ++guard) {
        uintptr_t dllBase = 0;
        UNICODE_STRING baseName{};
        if (!readRemote(hProc, cursor + pebx::kEntryDllBase, &dllBase, sizeof(dllBase))) return 0;
        if (!readRemote(hProc, cursor + pebx::kEntryBaseDllName, &baseName, sizeof(baseName))) return 0;

        if (dllBase && baseName.Buffer && baseName.Length > 0 && baseName.Length < 512) {
            wchar_t buffer[256]{};
            if (readRemote(hProc, (uintptr_t)baseName.Buffer, buffer, baseName.Length) &&
                _wcsicmp(buffer, wanted) == 0) {
                return dllBase;
            }
        }

        uintptr_t next = 0;
        if (!readRemote(hProc, cursor, &next, sizeof(next)) || !next) return 0;
        cursor = next;
    }
    return 0;
}

static uintptr_t remoteLoadLibrary(HANDLE hProc, const char* name) {
    const uintptr_t remoteKernel32 = findRemoteModule(hProc, "kernel32.dll");
    HMODULE localKernel32 = GetModuleHandleA("kernel32.dll");
    if (!remoteKernel32 || !localKernel32) return 0;

    const uintptr_t localLoadLibrary = (uintptr_t)GetProcAddress(localKernel32, "LoadLibraryA");
    if (!localLoadLibrary) return 0;
    const uintptr_t remoteLoadLibraryFn = localLoadLibrary - (uintptr_t)localKernel32 + remoteKernel32;

    const size_t len = strlen(name) + 1;
    void* arg = VirtualAllocEx(hProc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!arg) return 0;
    if (!writeRemote(hProc, arg, name, len)) { VirtualFreeEx(hProc, arg, 0, MEM_RELEASE); return 0; }

    HANDLE thread = remoteCreateThread(hProc, remoteLoadLibraryFn, (uintptr_t)arg);
    if (thread) {
        WaitForSingleObject(thread, 15000);
        CloseHandle(thread);
    }
    VirtualFreeEx(hProc, arg, 0, MEM_RELEASE);

    return findRemoteModule(hProc, name);
}

// ---------------------------------------------------------------------------
// Image fixups
// ---------------------------------------------------------------------------

static bool handleRelocations(uint8_t* localImage, uintptr_t remoteBase, uintptr_t preferredBase) {
    auto* dos = (IMAGE_DOS_HEADER*)localImage;
    auto* nt = (IMAGE_NT_HEADERS*)(localImage + dos->e_lfanew);
    auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (relocDir.VirtualAddress == 0 || relocDir.Size == 0) return true;

    const uintptr_t delta = remoteBase - preferredBase;
    if (delta == 0) return true;

    auto* reloc = (IMAGE_BASE_RELOCATION*)(localImage + relocDir.VirtualAddress);
    while (reloc->VirtualAddress && reloc->SizeOfBlock) {
        uint32_t count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
        uint16_t* entries = (uint16_t*)(reloc + 1);
        for (uint32_t i = 0; i < count; i++) {
            uint16_t type = entries[i] >> 12;
            uint16_t off = entries[i] & 0x0FFF;
            uint8_t* patchAddr = localImage + reloc->VirtualAddress + off;
            if (type == IMAGE_REL_BASED_DIR64) {
                *(uint64_t*)patchAddr += delta;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                *(uint32_t*)patchAddr += (uint32_t)delta;
            }
        }
        reloc = (IMAGE_BASE_RELOCATION*)((uint8_t*)reloc + reloc->SizeOfBlock);
    }
    return true;
}

static std::string localModuleName(HMODULE module) {
    char path[MAX_PATH * 2]{};
    if (!GetModuleFileNameA(module, path, (DWORD)sizeof(path))) return std::string();
    const char* slash = strrchr(path, '\\');
    return slash ? std::string(slash + 1) : std::string(path);
}

static bool resolveImports(HANDLE hProc, uint8_t* localImage, IMAGE_NT_HEADERS* nt) {
    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0) return true;

    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(localImage + impDir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dllName = (const char*)(localImage + imp->Name);
        HMODULE local = LoadLibraryA(dllName);
        if (!local) return false;

        // API sets (api-ms-win-crt-*.dll) resolve to a real DLL; the target only
        // knows the real name.
        const std::string realName = localModuleName(local);
        if (realName.empty()) return false;

        uintptr_t remoteBase = findRemoteModule(hProc, realName.c_str());
        if (!remoteBase) {
            remoteLoadLibrary(hProc, dllName);
            remoteBase = findRemoteModule(hProc, realName.c_str());
        }
        if (!remoteBase) return false;

        const uintptr_t delta = remoteBase - (uintptr_t)local;

        auto* byName = (IMAGE_THUNK_DATA*)(localImage +
                        (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* thunk = (IMAGE_THUNK_DATA*)(localImage + imp->FirstThunk);
        for (; byName->u1.AddressOfData; ++byName, ++thunk) {
            FARPROC fn = nullptr;
            if (byName->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                fn = GetProcAddress(local, (LPCSTR)(byName->u1.Ordinal & 0xFFFF));
            } else {
                auto* hint = (IMAGE_IMPORT_BY_NAME*)(localImage + byName->u1.AddressOfData);
                fn = GetProcAddress(local, (LPCSTR)hint->Name);
            }
            if (!fn) return false;
            thunk->u1.Function = (uintptr_t)fn + delta;
        }
    }
    return true;
}

static bool wipeSignatures(HANDLE hProc, uintptr_t imageBase, uintptr_t peOffset) {
    const uint16_t zeroMZ = 0;
    const uint32_t zeroPE = 0;
    return writeRemote(hProc, (void*)imageBase, &zeroMZ, sizeof(zeroMZ)) &&
           writeRemote(hProc, (void*)(imageBase + peOffset), &zeroPE, sizeof(zeroPE));
}

static DWORD protectionFor(uint32_t characteristics) {
    const bool execute = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (execute && write) return PAGE_EXECUTE_READWRITE;
    if (execute) return PAGE_EXECUTE_READ;
    if (write) return PAGE_READWRITE;
    return PAGE_READONLY;
}

static bool protectImage(HANDLE hProc, uintptr_t imageBase, IMAGE_NT_HEADERS* nt) {
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const SIZE_T size = section->Misc.VirtualSize ? section->Misc.VirtualSize
                                                      : section->SizeOfRawData;
        if (!size) continue;
        DWORD previous = 0;
        if (!VirtualProtectEx(hProc, (void*)(imageBase + section->VirtualAddress), size,
                              protectionFor(section->Characteristics), &previous)) {
            return false;
        }
    }
    return true;
}

// Copies the TLS callback table into the target so the stub can run it. The
// entries in the local image are already rebased by handleRelocations(), so no
// second adjustment is applied here.
static uintptr_t buildTlsArray(HANDLE hProc, uint8_t* localImage, IMAGE_NT_HEADERS* nt,
                               uintptr_t remoteBase, uintptr_t preferredBase) {
    auto& tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.VirtualAddress == 0 || tlsDir.Size < sizeof(IMAGE_TLS_DIRECTORY)) return 0;

    auto* tls = (IMAGE_TLS_DIRECTORY*)(localImage + tlsDir.VirtualAddress);
    if (!tls->AddressOfCallBacks) return 0;

    // Normally rebased with the rest of the image; some linkers leave it at the
    // preferred base, so both cases are accepted.
    const uintptr_t callbacksVa = (uintptr_t)tls->AddressOfCallBacks;
    uintptr_t callbacksOffset = 0;
    if (callbacksVa >= remoteBase) callbacksOffset = callbacksVa - remoteBase;
    else if (callbacksVa >= preferredBase) callbacksOffset = callbacksVa - preferredBase;
    else return 0;
    if (callbacksOffset >= nt->OptionalHeader.SizeOfImage) return 0;

    auto* callbacks = (uint64_t*)(localImage + callbacksOffset);
    size_t count = 0;
    while (count < 64 && callbacks[count]) ++count;
    if (count == 0) return 0;

    const size_t bytes = (count + 1) * sizeof(uint64_t);
    void* array = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!array) return 0;
    if (!writeRemote(hProc, array, callbacks, bytes)) {
        VirtualFreeEx(hProc, array, 0, MEM_RELEASE);
        return 0;
    }
    DWORD previous = 0;
    VirtualProtectEx(hProc, array, bytes, PAGE_READONLY, &previous);
    return (uintptr_t)array;
}

// ---------------------------------------------------------------------------
// Startup stub
//
// Runs the TLS callbacks, then DllMain(hModule, DLL_PROCESS_ATTACH, NULL), then
// restores every register and returns. It is position independent and works
// both as a hijacked thread's new RIP and as a plain thread start routine,
// because it ends in `ret`: for the hijack the resume address was pushed on the
// thread's stack before it was redirected.
//
// Page 0 : code (RX)   Page 1 : data (RW)
// ---------------------------------------------------------------------------

namespace stub {

const uintptr_t kDataOffset = 0x1000;
const uintptr_t kSize = 0x2000;

const uintptr_t kSlotTlsArray = 0x00;
const uintptr_t kSlotImageBase = 0x08;
const uintptr_t kSlotEntry = 0x10;
const uintptr_t kSlotResult = 0x18;
const uintptr_t kSlotStarted = 0x20;

const uint64_t kPending = 0xFFFFFFFFFFFFFFFFull;

// Small x64 emitter: only the handful of encodings the stub needs.
class Builder {
public:
    Builder() { code_.reserve(kDataOffset); data_.assign(5, 0); }

    void set(uintptr_t slot, uint64_t value) { data_[slot / 8] = value; }

    void emit(std::initializer_list<uint8_t> b) { code_.insert(code_.end(), b); }

    // mov r64, [rip + slot]
    void movRegSlot(uint8_t modrm, uintptr_t slot) {
        emit({0x48, 0x8B, modrm});
        displacement(slot);
    }
    // mov [rip + slot], rax
    void movSlotRax(uintptr_t slot) {
        emit({0x48, 0x89, 0x05});
        displacement(slot);
    }
    // mov dword [rip + slot], 1
    void movSlotOne(uintptr_t slot) {
        emit({0xC7, 0x05});
        displacement(slot);
        emit({0x01, 0x00, 0x00, 0x00});
    }
    void jump(uint8_t opcode, int label) {
        emit({opcode, 0x00});
        fixups_.push_back({ code_.size() - 1, label });
    }
    void bind(int label) { labels_[label] = code_.size(); }

    std::vector<uint8_t> build() {
        for (const Fixup& f : fixups_)
            code_[f.at] = (uint8_t)((int)labels_[f.label] - (int)(f.at + 1));
        code_.resize(kDataOffset, 0xCC);
        for (uint64_t v : data_)
            for (int i = 0; i < 8; ++i) code_.push_back((uint8_t)(v >> (8 * i)));
        code_.resize(kSize, 0x00);
        return code_;
    }

private:
    struct Fixup { size_t at; int label; };

    void displacement(uintptr_t slot) {
        // The RIP-relative instructions above are 7 bytes: 3 opcode bytes plus a
        // 32-bit displacement. movSlotOne adds its immediate afterwards.
        const int32_t d = (int32_t)(kDataOffset + slot) - (int32_t)(code_.size() + 4);
        for (int i = 0; i < 4; ++i) code_.push_back((uint8_t)((uint32_t)d >> (8 * i)));
    }

    std::vector<uint8_t> code_;
    std::vector<uint64_t> data_;
    std::vector<Fixup> fixups_;
    size_t labels_[8] = {};
};

inline std::vector<uint8_t> emitProgram(uint64_t imageBase, uint64_t entry, uint64_t tlsArray) {
    Builder b;
    b.set(kSlotImageBase, imageBase);
    b.set(kSlotEntry, entry);
    b.set(kSlotTlsArray, tlsArray);
    b.set(kSlotResult, kPending);
    b.set(kSlotStarted, 0);

    b.emit({ 0x9C });                                       // pushfq
    b.emit({ 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57 });   // push rax rcx rdx rbx rbp rsi rdi
    b.emit({ 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,
             0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57 }); // push r8..r15
    b.emit({ 0x48, 0x89, 0xE5 });                           // mov rbp, rsp
    b.emit({ 0x48, 0x83, 0xE4, 0xF0 });                     // and rsp, -16
    b.emit({ 0x48, 0x83, 0xEC, 0x20 });                     // sub rsp, 0x20
    b.movSlotOne(kSlotStarted);                             // mov dword [started], 1

    // for (cb = tlsArray; cb && *cb; ++cb) (*cb)(imageBase, DLL_PROCESS_ATTACH, NULL);
    b.movRegSlot(0x1D, kSlotTlsArray);                      // mov rbx, [tlsArray]
    b.emit({ 0x48, 0x85, 0xDB });                           // test rbx, rbx
    b.jump(0x74, 0);                                        // jz skip_tls

    b.bind(1);
    b.emit({ 0x48, 0x8B, 0x03 });                           // mov rax, [rbx]
    b.emit({ 0x48, 0x85, 0xC0 });                           // test rax, rax
    b.jump(0x74, 0);                                        // jz skip_tls
    b.movRegSlot(0x0D, kSlotImageBase);                     // mov rcx, [imageBase]
    b.emit({ 0xBA, 0x01, 0x00, 0x00, 0x00 });               // mov edx, 1
    b.emit({ 0x45, 0x31, 0xC0 });                           // xor r8d, r8d
    b.emit({ 0xFF, 0xD0 });                                 // call rax
    b.emit({ 0x48, 0x83, 0xC3, 0x08 });                     // add rbx, 8
    b.jump(0xEB, 1);                                        // jmp loop

    b.bind(0);
    b.movRegSlot(0x0D, kSlotImageBase);                     // mov rcx, [imageBase]
    b.emit({ 0xBA, 0x01, 0x00, 0x00, 0x00 });               // mov edx, 1
    b.emit({ 0x45, 0x31, 0xC0 });                           // xor r8d, r8d
    b.movRegSlot(0x05, kSlotEntry);                         // mov rax, [entry]
    b.emit({ 0xFF, 0xD0 });                                 // call rax
    b.movSlotRax(kSlotResult);                              // mov [result], rax

    b.emit({ 0x48, 0x89, 0xEC });                           // mov rsp, rbp
    b.emit({ 0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,
             0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58 }); // pop r15..r8
    b.emit({ 0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58 });   // pop rdi rsi rbp rbx rdx rcx rax
    b.emit({ 0x9D });                                       // popfq
    b.emit({ 0xC3 });                                       // ret

    return b.build();
}

} // namespace stub

static bool waitForStarted(HANDLE hProc, uintptr_t startedAddress, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        uint64_t started = 0;
        if (readRemote(hProc, startedAddress, &started, sizeof(started)) && started != 0) return true;
        if (GetTickCount64() - start > timeoutMs) return false;
        Sleep(2);
    }
}

// Redirects a thread that is about to wake up onto the stub. The resume address
// is pushed on its stack first, so the stub's final `ret` continues exactly
// where the thread was. Returns false without side effects when no thread runs
// the stub in time.
static bool startViaHijack(HANDLE hProc, DWORD pid, uintptr_t stubBase, uintptr_t startedAddress) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    std::vector<DWORD> ids;
    THREADENTRY32 te{ sizeof(te) };
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ids.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    const int kMaxProbes = 8;
    const DWORD kProbeMs = 80;
    int probes = 0;

    for (DWORD id : ids) {
        if (probes++ >= kMaxProbes) break;

        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }

        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread, &context)) { ResumeThread(thread); CloseHandle(thread); continue; }

        // `ret` at the end of the stub pops this and continues the thread.
        const uint64_t resume = context.Rip;
        const uint64_t newStack = context.Rsp - sizeof(uint64_t);
        if (!writeRemote(hProc, (void*)(uintptr_t)newStack, &resume, sizeof(resume))) {
            ResumeThread(thread);
            CloseHandle(thread);
            continue;
        }

        context.Rip = stubBase;
        context.Rsp = newStack;
        if (!SetThreadContext(thread, &context)) {
            ResumeThread(thread);
            CloseHandle(thread);
            continue;
        }
        ResumeThread(thread);

        if (waitForStarted(hProc, startedAddress, kProbeMs)) {
            CloseHandle(thread);
            return true;
        }

        // A thread parked in a wait only takes the new RIP when the wait
        // completes, so put it back and try the next one.
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); return false; }
        uint64_t started = 0;
        if (readRemote(hProc, startedAddress, &started, sizeof(started)) && started != 0) {
            ResumeThread(thread);
            CloseHandle(thread);
            return true;
        }
        context.Rip = resume;
        context.Rsp = newStack + sizeof(uint64_t);
        SetThreadContext(thread, &context);
        ResumeThread(thread);
        CloseHandle(thread);
    }
    return false;
}

static bool waitForResult(HANDLE hProc, uintptr_t resultAddress, DWORD timeoutMs, uint64_t& value) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (readRemote(hProc, resultAddress, &value, sizeof(value)) && value != stub::kPending)
            return true;
        if (GetTickCount64() - start > timeoutMs) return false;
        Sleep(10);
    }
}

bool ManualMap(HANDLE hProc, DWORD pid, const std::vector<uint8_t>& dllBytes) {
    if (dllBytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;

    BOOL wow64 = FALSE;
    if (IsWow64Process(hProc, &wow64) && wow64) return false; // x64 layout only

    auto* dos = (IMAGE_DOS_HEADER*)dllBytes.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dllBytes.size() < (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS)) return false;
    auto* nt = (IMAGE_NT_HEADERS*)(dllBytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize == 0) return false;
    std::vector<uint8_t> local(imageSize, 0);

    memcpy(local.data(), dllBytes.data(), nt->OptionalHeader.SizeOfHeaders);

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (section->SizeOfRawData == 0) continue;
        if ((size_t)section->PointerToRawData + section->SizeOfRawData > dllBytes.size()) return false;
        memcpy(local.data() + section->VirtualAddress,
               dllBytes.data() + section->PointerToRawData,
               section->SizeOfRawData);
    }

    void* remoteBase = VirtualAllocEx(hProc, nullptr, imageSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBase) return false;

    const uintptr_t preferred = (uintptr_t)nt->OptionalHeader.ImageBase;
    if (!handleRelocations(local.data(), (uintptr_t)remoteBase, preferred) ||
        !resolveImports(hProc, local.data(), nt) ||
        !writeRemote(hProc, remoteBase, local.data(), imageSize) ||
        !wipeSignatures(hProc, (uintptr_t)remoteBase, (uintptr_t)dos->e_lfanew) ||
        !protectImage(hProc, (uintptr_t)remoteBase, nt)) {
        VirtualFreeEx(hProc, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    const uintptr_t tlsArray = buildTlsArray(hProc, local.data(), nt, (uintptr_t)remoteBase, preferred);
    const uintptr_t entry = (uintptr_t)remoteBase + nt->OptionalHeader.AddressOfEntryPoint;

    const std::vector<uint8_t> program = stub::emitProgram((uintptr_t)remoteBase, entry, tlsArray);

    void* stubMemory = VirtualAllocEx(hProc, nullptr, stub::kSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stubMemory) {
        VirtualFreeEx(hProc, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    bool ok = writeRemote(hProc, stubMemory, program.data(), program.size());
    if (ok) {
        DWORD previous = 0;
        ok = VirtualProtectEx(hProc, stubMemory, stub::kDataOffset, PAGE_EXECUTE_READ, &previous) != FALSE;
    }

    const uintptr_t dataPage = (uintptr_t)stubMemory + stub::kDataOffset;
    bool launched = false;

    if (ok) launched = startViaHijack(hProc, pid, (uintptr_t)stubMemory, dataPage + stub::kSlotStarted);

    if (ok && !launched) {
        // No thread woke up in time: use one of our own. The same stub supports
        // that because it ends in `ret`.
        HANDLE thread = remoteCreateThread(hProc, (uintptr_t)stubMemory, 0);
        if (thread) launched = true, CloseHandle(thread);
        else ok = false;
    }

    if (!ok || !launched) {
        if (ok) VirtualFreeEx(hProc, stubMemory, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    uint64_t result = stub::kPending;
    const bool finished = waitForResult(hProc, dataPage + stub::kSlotResult, 20000, result);
    if (!finished || result == 0) {
        // The image and the stub stay mapped: a hijacked thread may still be
        // parked inside one of them.
        return false;
    }
    return true;
}

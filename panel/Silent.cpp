#include "Silent.h"
#include "Offsets.h"
#include "MinHook.h"
#include "Logger.h"
#include <atomic>

using GetShootDir_t = Vec3* (__fastcall*)(uintptr_t weapon, Vec3* out);
static GetShootDir_t gOriginal = nullptr;
static void* gHookTarget = nullptr;
static bool gMhInitialized = false;

// The panel thread writes the aim point, the game thread reads it inside the
// hook. A seqlock keeps the read tear-free without a lock in the hot path:
// the writer bumps the counter to odd while writing, the reader retries if the
// counter changed or was odd.
static std::atomic<uint32_t> gSeq{ 0 };
static Vec3 gTargetPos{};
static Vec3 gOriginPos{};
static bool gHasTarget = false;

void setSilentTarget(const Vec3& targetWorld, const Vec3& originWorld, bool valid) {
    const uint32_t s = gSeq.load(std::memory_order_relaxed);
    gSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    gTargetPos = targetWorld;
    gOriginPos = originWorld;
    gHasTarget = valid;

    std::atomic_thread_fence(std::memory_order_release);
    gSeq.store(s + 2, std::memory_order_relaxed);
}

static bool readSilentTarget(Vec3& targetWorld, Vec3& originWorld) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = gSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;

        targetWorld = gTargetPos;
        originWorld = gOriginPos;
        const bool valid = gHasTarget;

        std::atomic_thread_fence(std::memory_order_acquire);
        if (gSeq.load(std::memory_order_relaxed) == s1) return valid;
    }
    return false;
}

static Vec3* __fastcall HookedGetShootDir(uintptr_t weapon, Vec3* out) {
    Vec3* result = gOriginal(weapon, out);

    Vec3 target, origin;
    if (!result || !readSilentTarget(target, origin)) return result;

    Vec3 dir = target - origin;
    const float len = dir.length();
    if (len < 1e-3f) return result; // degenerate, keep the game's direction

    result->x = dir.x / len;
    result->y = dir.y / len;
    result->z = dir.z / len;
    return result;
}

bool installSilentHook(uintptr_t moduleBase, uintptr_t shootDirectionRva) {
    if (!moduleBase || !ff::IsResolved(shootDirectionRva)) {
        logx::warn("Silent hook: OFF_WEAPON_DIRECTION_FN unresolved - fill offsets.json (see sdk/offsets.example.json)");
        return false;
    }

    if (!gMhInitialized) {
        if (MH_Initialize() != MH_OK) {
            logx::warn("MinHook init failed");
            return false;
        }
        gMhInitialized = true;
    }

    void* target = (void*)(moduleBase + shootDirectionRva);
    if (MH_CreateHook(target, (LPVOID)&HookedGetShootDir, (LPVOID*)&gOriginal) != MH_OK) {
        logx::warn("MinHook create failed at %p", target);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        logx::warn("MinHook enable failed at %p", target);
        return false;
    }

    gHookTarget = target;
    logx::info("Silent hook installed at %p", target);
    return true;
}

void uninstallSilentHook() {
    if (!gHookTarget) return;
    MH_DisableHook(gHookTarget);
    MH_RemoveHook(gHookTarget);
    gHookTarget = nullptr;
    gOriginal = nullptr;
    logx::info("Silent hook removed");
}

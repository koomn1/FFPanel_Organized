#pragma once
// FFPanel offset table.
//
// Two different kinds of value live in this file and they have different
// lifetimes, so they are handled differently:
//
//   * Layout offsets - fixed by the IL2CPP object layout that Structs.h
//     models (List<T> header, PlayerData field order, camera matrix). These
//     only change when the game changes its classes, so they stay constexpr.
//
//   * Build offsets - the four values that point into one *specific* build of
//     libil2cpp.so: three static-field slots holding instance pointers and one
//     function body. They change with every game update, so they are NOT
//     compiled in. They are loaded at runtime from offsets.json (schema in
//     sdk/offsets.example.json) which you fill from your own Il2CppDumper run.
//
// Any build offset left at 0 is unresolved. Consumers check Ready() /
// SilentReady() before touching the game, so an unfilled offsets.json shows up
// as a log line instead of a read at base+0.

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Config.h" // cfgjson: readAll / findSection / findValue

namespace ff {
inline constexpr uintptr_t kUnresolved = 0;
}

// --------------------------------------------------------------- layout ----
// Stable across builds. Offsets are into the managed objects themselves.

constexpr uintptr_t OFF_PM_PLAYERS_LIST = 0x10;   // PlayerManager -> List<Player>
constexpr uintptr_t OFF_P_POSITION      = 0x50;
constexpr uintptr_t OFF_P_HEADPOSITION  = 0x60;
constexpr uintptr_t OFF_P_HEALTH        = 0x68;
constexpr uintptr_t OFF_P_TEAMID        = 0x70;
constexpr uintptr_t OFF_P_NAME          = 0x88;
constexpr uintptr_t OFF_P_ISDEAD        = 0x90;
constexpr uintptr_t OFF_P_ISLOCAL       = 0x94;
constexpr uintptr_t OFF_P_WEAPON        = 0x98;

constexpr uintptr_t OFF_CAM_WORLD2CAM   = 0x2E4;
constexpr uintptr_t OFF_CAM_FOV         = 0x324;

// IL2CPP List<T> header: items pointer and size (see Il2CppList in Structs.h).
constexpr uintptr_t OFF_LIST_ITEMS      = 0x10;
constexpr uintptr_t OFF_LIST_SIZE       = 0x18;
constexpr uintptr_t OFF_ARRAY_FIRST     = 0x20;   // inline array data in the object

// ---------------------------------------------------------- build slots ----
// RVAs inside libil2cpp.so. Resolved from offsets.json at startup.

inline uintptr_t OFF_PLAYERMANAGER_INSTANCE = ff::kUnresolved;
inline uintptr_t OFF_CAMERA_INSTANCE        = ff::kUnresolved;
inline uintptr_t OFF_LOCALPLAYER_INSTANCE   = ff::kUnresolved; // optional, reserved
inline uintptr_t OFF_WEAPON_DIRECTION_FN    = ff::kUnresolved;

namespace ff {

// Module the RVAs above are relative to. Read from offsets.json ("module").
inline std::string gModuleName = "libil2cpp.so";

inline const std::string& ModuleName() { return gModuleName; }

inline std::wstring ModuleNameWide() {
    return std::wstring(gModuleName.begin(), gModuleName.end());
}

struct BuildOffset {
    const char* key;     // JSON key in offsets.json
    uintptr_t*  value;   // slot it fills
    const char* doc;     // what the number has to be
};

inline BuildOffset* BuildOffsetTable(std::size_t& count) {
    static BuildOffset table[] = {
        { "OFF_PLAYERMANAGER_INSTANCE", &OFF_PLAYERMANAGER_INSTANCE,
          "RVA of the static slot holding PlayerManager.Instance" },
        { "OFF_CAMERA_INSTANCE", &OFF_CAMERA_INSTANCE,
          "RVA of the static slot holding the main Camera instance" },
        { "OFF_LOCALPLAYER_INSTANCE", &OFF_LOCALPLAYER_INSTANCE,
          "RVA of the static slot holding LocalPlayer.Instance (optional)" },
        { "OFF_WEAPON_DIRECTION_FN", &OFF_WEAPON_DIRECTION_FN,
          "RVA of the function body that writes the shot direction" },
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

inline bool IsResolved(uintptr_t v) { return v != kUnresolved; }

// True when the offset set used for ESP / player iteration is usable.
inline bool Ready() {
    return IsResolved(OFF_PLAYERMANAGER_INSTANCE) && IsResolved(OFF_CAMERA_INSTANCE);
}

// True when the silent-aim hook has a real target address.
inline bool SilentReady() { return IsResolved(OFF_WEAPON_DIRECTION_FN); }

inline bool AllResolved() {
    std::size_t n = 0;
    BuildOffset* t = BuildOffsetTable(n);
    for (std::size_t i = 0; i < n; ++i)
        if (!IsResolved(*t[i].value)) return false;
    return true;
}

inline std::string UnresolvedList() {
    std::size_t n = 0;
    BuildOffset* t = BuildOffsetTable(n);
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        if (IsResolved(*t[i].value)) continue;
        if (!out.empty()) out += ", ";
        out += t[i].key;
    }
    if (out.empty()) out = "none";
    return out;
}

// Accepts 0x1A2B3C, 12345 and "0x1A2B3C" (quoted or bare).
inline bool ParseUptr(const std::string& raw, uintptr_t& out) {
    const char* s = raw.c_str();
    while (*s == ' ' || *s == '\t' || *s == '"') ++s;
    if (*s == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 0);
    if (end == s || errno != 0) return false;
    while (*end == ' ' || *end == '\t' || *end == '"') ++end;
    if (*end != '\0') return false;
    if (v > (unsigned long long)(~(uintptr_t)0)) return false;
    out = (uintptr_t)v;
    return true;
}

// Reads the "offsets" object of `path` and fills the build slots.
// Returns the number of non-zero values applied, or -1 if the file is missing.
// Every slot is reset first, so calling this again re-reads from scratch.
inline int Load(const char* path) {
    std::size_t n = 0;
    BuildOffset* table = BuildOffsetTable(n);

    const std::string src = cfgjson::readAll(path);
    if (src.empty()) { for (std::size_t i = 0; i < n; ++i) *table[i].value = kUnresolved; return -1; }

    std::string body;
    if (!cfgjson::findSection(src, "offsets", body)) body = src; // flat file also works

    int applied = 0;
    for (std::size_t i = 0; i < n; ++i) {
        *table[i].value = kUnresolved;
        std::string tok;
        if (!cfgjson::findValue(body, table[i].key, tok)) continue;
        uintptr_t v = kUnresolved;
        if (!ParseUptr(tok, v)) continue;
        *table[i].value = v;
        if (v != kUnresolved) ++applied;
    }

    // Optional: which module the RVAs belong to (top-level "module").
    std::string module;
    if (cfgjson::findValue(src, "module", module) && !module.empty()) {
        gModuleName = module;
    }
    return applied;
}

inline int LoadDefault() { return Load("offsets.json"); }

} // namespace ff

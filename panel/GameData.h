#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include "Structs.h"
#include "Offsets.h"

namespace gamedata {

// Reads an Il2Cpp string object: int32 length at +0x10, UTF-16 characters at
// +0x14. Returns UTF-8, empty on anything that does not look like a string.
inline std::string ReadManagedString(uintptr_t address) {
    if (address < 0x10000) return {};

    const int32_t length = *(const int32_t*)(address + 0x10);
    if (length <= 0 || length > 64) return {};

    const uint16_t* chars = (const uint16_t*)(address + 0x14);
    std::string out;
    out.reserve((size_t)length);
    for (int32_t i = 0; i < length; ++i) {
        const uint32_t c = chars[i];
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace gamedata

struct PlayerData {
    uintptr_t addr = 0; Vec3 position; Vec3 headPosition; int health = 0; int teamId = 0;
    bool isDead = false; bool isLocal = false; std::string name; Vec2 screenPos; Vec2 headScreenPos;
    bool onScreen = false; float distance3D = 0;
};

class GameData {
public:
    void bindInjected(HMODULE module) {
        (void)module;
        // The module the offsets belong to, from offsets.json.
        base_ = (uintptr_t)GetModuleHandleA(ff::ModuleName().c_str());
        if (!base_) base_ = (uintptr_t)GetModuleHandleA("libil2cpp.so");
        if (!base_) base_ = (uintptr_t)GetModuleHandleA("UnityPlayer.dll");
        if (!base_) base_ = (uintptr_t)GetModuleHandleA(nullptr);
        attached_ = base_ != 0;
    }
    bool isAttached() const { return attached_; }
    const std::vector<PlayerData>& players() const { return players_; }
    const Vec3& localPosition() const { return localPos_; }
    int localTeam() const { return localTeam_; }

    void update() {
        players_.clear(); if (!attached_ || !ff::Ready()) return;
        uintptr_t pmInst = *(uintptr_t*)(base_ + OFF_PLAYERMANAGER_INSTANCE);
        uintptr_t camInst = *(uintptr_t*)(base_ + OFF_CAMERA_INSTANCE);
        if (!pmInst) return;
        if (camInst) view_ = *(Matrix4x4*)(camInst + OFF_CAM_WORLD2CAM);
        uintptr_t list = *(uintptr_t*)(pmInst + OFF_PM_PLAYERS_LIST); if (!list) return;
        uintptr_t items = *(uintptr_t*)(list + OFF_LIST_ITEMS); int size = *(int*)(list + OFF_LIST_SIZE);
        if (!items || size <= 0 || size > 200) return;
        for (int i = 0; i < size; i++) {
            uintptr_t p = *(uintptr_t*)(items + OFF_ARRAY_FIRST + i * 8); if (!p) continue;
            PlayerData d; d.addr = p; d.position = *(Vec3*)(p + OFF_P_POSITION); d.headPosition = *(Vec3*)(p + OFF_P_HEADPOSITION);
            d.health = *(int*)(p + OFF_P_HEALTH); d.teamId = *(int*)(p + OFF_P_TEAMID);
            d.isDead = *(uint8_t*)(p + OFF_P_ISDEAD) != 0; d.isLocal = *(uint8_t*)(p + OFF_P_ISLOCAL) != 0;
            d.name = gamedata::ReadManagedString(*(uintptr_t*)(p + OFF_P_NAME));
            if (d.isDead) continue;
            if (d.isLocal) { localPos_ = d.position; localTeam_ = d.teamId; continue; }
            if (d.teamId == localTeam_) continue;
            d.onScreen = worldToScreen(d.position, d.screenPos);
            if (d.onScreen) { worldToScreen(d.headPosition, d.headScreenPos); Vec3 diff = d.position - localPos_; d.distance3D = diff.length(); }
            players_.push_back(d);
        }
    }
    bool worldToScreen(const Vec3& w, Vec2& out) const {
        Vec4 clip;
        clip.x = w.x * view_.m[0][0] + w.y * view_.m[1][0] + w.z * view_.m[2][0] + view_.m[3][0];
        clip.y = w.x * view_.m[0][1] + w.y * view_.m[1][1] + w.z * view_.m[2][1] + view_.m[3][1];
        clip.w = w.x * view_.m[0][3] + w.y * view_.m[1][3] + w.z * view_.m[2][3] + view_.m[3][3];
        if (clip.w < 0.1f) return false; float iw = 1.0f / clip.w;
        int sw = GetSystemMetrics(SM_CXSCREEN); int sh = GetSystemMetrics(SM_CYSCREEN);
        out.x = (clip.x * iw + 1.0f) * 0.5f * sw; out.y = (1.0f - clip.y * iw) * 0.5f * sh; return true;
    }
private:
    uintptr_t base_ = 0; bool attached_ = false; std::vector<PlayerData> players_; Matrix4x4 view_{}; Vec3 localPos_{}; int localTeam_ = 0;
};

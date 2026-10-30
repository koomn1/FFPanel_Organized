#include <Windows.h>
#include <gdiplus.h>
#include <thread>
#include <atomic>
#include <vector>
#include "Memory.h"
#include "Config.h"
#include "Logger.h"
#include "Structs.h"
#include "Offsets.h"
#include "Overlay.h"
#pragma comment(lib, "gdiplus.lib")

static std::atomic<bool> gRunning{ true };
struct ExternalPlayer { Vec3 position; Vec3 head; int health; int teamId; bool dead; bool local; };

int main() {
    logx::initConsole("FFPanel external");
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR tok; Gdiplus::GdiplusStartup(&tok, &gi, nullptr);
    Config cfg; cfg.load("config.json");

    // Offsets first: the module name in offsets.json says what to attach to.
    if (ff::LoadDefault() < 0) {
        logx::error("offsets.json not found - copy sdk/offsets.example.json next to external.exe");
        return 1;
    }
    if (!ff::Ready()) {
        logx::error("Offsets unresolved: %s", ff::UnresolvedList().c_str());
        return 1;
    }

    Memory mem;
    std::wstring proc = L"HD-Player.exe";
    const std::wstring mod = ff::ModuleNameWide();
    if (!mem.attach(proc, mod)) { proc = L"LdVBoxHeadless.exe"; if (!mem.attach(proc, mod)) return 1; }
    Overlay ov(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)); ov.show();
    Matrix4x4 view{}; std::vector<ExternalPlayer> players;
    while (gRunning) {
        if (GetAsyncKeyState(VK_F9) & 0x8000) break;
        uintptr_t pmInst = mem.read<uintptr_t>(mem.base + OFF_PLAYERMANAGER_INSTANCE);
        uintptr_t camInst = mem.read<uintptr_t>(mem.base + OFF_CAMERA_INSTANCE);
        players.clear();
        if (pmInst && camInst) {
            mem.readBuf(camInst + OFF_CAM_WORLD2CAM, &view, sizeof(view));
            uintptr_t list = mem.read<uintptr_t>(pmInst + OFF_PM_PLAYERS_LIST);
            if (list) {
                uintptr_t items = mem.read<uintptr_t>(list + OFF_LIST_ITEMS);
                int size = mem.read<int>(list + OFF_LIST_SIZE);
                if (items && size > 0 && size < 200) {
                    for (int i = 0; i < size; i++) {
                        uintptr_t p = mem.read<uintptr_t>(items + OFF_ARRAY_FIRST + i * 8);
                        if (!p) continue;
                        ExternalPlayer ep; ep.position = mem.read<Vec3>(p + OFF_P_POSITION); ep.head = mem.read<Vec3>(p + OFF_P_HEADPOSITION);
                        ep.health = mem.read<int>(p + OFF_P_HEALTH); ep.teamId = mem.read<int>(p + OFF_P_TEAMID);
                        ep.dead = mem.read<uint8_t>(p + OFF_P_ISDEAD) != 0; ep.local = mem.read<uint8_t>(p + OFF_P_ISLOCAL) != 0;
                        if (!ep.dead) players.push_back(ep);
                    }
                }
            }
        }
        ov.beginFrame(); int W = ov.width(), H = ov.height();
        auto w2s = [&](const Vec3& w, Vec2& out) {
            Vec4 clip;
            clip.x = w.x * view.m[0][0] + w.y * view.m[1][0] + w.z * view.m[2][0] + view.m[3][0];
            clip.y = w.x * view.m[0][1] + w.y * view.m[1][1] + w.z * view.m[2][1] + view.m[3][1];
            clip.w = w.x * view.m[0][3] + w.y * view.m[1][3] + w.z * view.m[2][3] + view.m[3][3];
            if (clip.w < 0.1f) return false;
            float iw = 1.0f / clip.w; out.x = (clip.x * iw + 1.0f) * 0.5f * W; out.y = (1.0f - clip.y * iw) * 0.5f * H; return true;
        };
        for (auto& p : players) {
            Vec2 sFeet, sHead;
            if (!w2s(p.position, sFeet) || !w2s(p.head, sHead)) continue;
            float h = sFeet.y - sHead.y; float w = h * 0.45f;
            ov.drawBox(sHead.x - w / 2, sHead.y, w, h, cfg.esp.boxColor);
            ov.drawLine(W / 2, H / 2, sHead.x, sHead.y, cfg.esp.snaplineColor);
        }
        ov.endFrame(); Sleep(10);
    }
    ov.close(); Gdiplus::GdiplusShutdown(tok); return 0;
}

#include "Aimbot.h"
#include "Silent.h"
#include "Logger.h"
#include <cmath>

static void mouseMove(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static void mouseDown() {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &in, sizeof(in));
}

static void mouseUp() {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

void Aimbot::tryInstallSilent() {
    if (silentInstalled_ || silentAttempted_ || !cfg_) return;
    silentAttempted_ = true;
    if (installSilentHook((uintptr_t)GetModuleHandleA(nullptr), OFF_WEAPON_DIRECTION_FN)) {
        silentInstalled_ = true;
        logx::info("Silent aim hook installed");
    } else {
        logx::warn("Silent aim hook not installed - silent mode stays passive");
    }
}

void Aimbot::snap(float tx, float ty) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int dx = (int)std::round(tx - sw / 2.0f);
    int dy = (int)std::round(ty - sh / 2.0f);
    mouseMove(dx, dy);
}

void Aimbot::smooth(float tx, float ty, float factor) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float dx = (tx - sw / 2.0f) * factor;
    float dy = (ty - sh / 2.0f) * factor;

    if (cfg_->aim.humanize) {
        std::uniform_real_distribution<float> noise(-0.6f, 0.6f);
        dx += noise(rng_);
        dy += noise(rng_);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        if (u(rng_) < 0.18f) { dx *= 0.55f; dy *= 0.55f; }
    }

    if (std::abs(dx) < cfg_->aim.deadZonePx && std::abs(dy) < cfg_->aim.deadZonePx) return;
    mouseMove((int)std::round(dx), (int)std::round(dy));
}

void Aimbot::fire() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFire_).count();
    if (ms < cfg_->trigger.fireRateMs) return;
    mouseDown();
    Sleep(cfg_->trigger.holdMs);
    mouseUp();
    lastFire_ = now;
}

void Aimbot::update(const GameData& data, Config& cfg) {
    cfg_ = &cfg;
    hasTarget_ = false;
    setSilentTarget(Vec3{}, Vec3{}, false);

    if (cfg.silent.enabled) tryInstallSilent();

    if (!cfg.aim.enabled && !cfg.trigger.enabled && !cfg.silent.enabled) return;
    if (data.players().empty()) return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float cx = sw / 2.0f;
    float cy = sh / 2.0f;
    float fov = (float)cfg.aim.fovRadius;

    const PlayerData* target = nullptr;
    float bestDist = fov;

    for (const auto& p : data.players()) {
        if (!p.onScreen) continue;
        float tx = p.headScreenPos.x;
        float ty = p.headScreenPos.y + (p.screenPos.y - p.headScreenPos.y) * cfg.aim.headOffset;
        float dx = tx - cx, dy = ty - cy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > fov) continue;

        bool better = false;
        if (!target) better = true;
        else if (cfg.aim.priority == "lowest_health" && p.health < target->health) better = true;
        else if (cfg.aim.priority == "highest_threat" && p.distance3D < target->distance3D) better = true;
        else if (cfg.aim.priority == "nearest" && dist < bestDist) better = true;

        if (better) {
            target = &p;
            bestDist = dist;
        }
    }

    if (!target) {
        trackInitialized_ = false;
        return;
    }
    hasTarget_ = true;

    float tx = target->headScreenPos.x;
    float ty = target->headScreenPos.y + (target->screenPos.y - target->headScreenPos.y) * cfg.aim.headOffset;

    if (cfg.aim.mode == "prediction" || cfg.aim.mode == "smooth") {
        auto now = std::chrono::steady_clock::now();
        if (!trackInitialized_) {
            kx_.pos = tx; kx_.vel = 0;
            ky_.pos = ty; ky_.vel = 0;
            trackInitialized_ = true;
        } else {
            float dt = std::chrono::duration<float>(now - lastTrackUpdate_).count();
            if (dt > 0.001f) {
                kx_.predict(dt); ky_.predict(dt);
                kx_.update(tx); ky_.update(ty);
                float ahead = cfg.aim.predictionMs / 1000.0f;
                tx = kx_.ahead(ahead);
                ty = ky_.ahead(ahead);
            }
        }
        lastTrackUpdate_ = now;
    }

    bool firePressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const std::string& mode = cfg.aim.mode;

    if (mode == "silent") {
        if (cfg.silent.enabled && silentInstalled_)
            setSilentTarget(target->headPosition,
                            data.localPosition() + Vec3{ 0.0f, kSilentEyeHeight, 0.0f }, true);
        return;
    }
    if (mode == "trigger_snap") {
        static bool prevFire = false;
        if (firePressed && !prevFire && hasTarget_) {
            snap(tx, ty);
            fire();
        }
        prevFire = firePressed;
        return;
    }
    if (mode == "instant") {
        snap(tx, ty);
        fire();
        return;
    }
    if (!cfg.aim.enabled) return;

    if (mode == "snap") snap(tx, ty);
    else smooth(tx, ty, mode == "prediction" ? cfg.aim.smooth * 0.7f : cfg.aim.smooth);

    if (cfg.trigger.enabled && hasTarget_) {
        float dx = target->headScreenPos.x - cx;
        float dy = target->headScreenPos.y - cy;
        if (std::sqrt(dx * dx + dy * dy) < cfg.trigger.radiusPx) fire();
    }

    if (cfg.aim.recoilCompensation && firePressed) mouseMove(0, 2);
}
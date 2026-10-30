#pragma once
#include <Windows.h>
#include <random>
#include <chrono>
#include "Config.h"
#include "GameData.h"

class Aimbot {
public:
    void bindConfig(Config* cfg) { cfg_ = cfg; }
    void update(const GameData& data, Config& cfg);

    bool hasTarget() const { return hasTarget_; }
    bool silentInstalled() const { return silentInstalled_; }

private:
    struct Kalman1D {
        float pos = 0, vel = 0;
        float P00 = 50, P01 = 0, P10 = 0, P11 = 50;
        static constexpr float Q = 0.5f, R = 4.0f;
        void predict(float dt) {
            pos += vel * dt;
            float p00 = P00 + dt * P10 + dt * (P01 + dt * P11) + Q;
            float p01 = P01 + dt * P11;
            float p10 = P10 + dt * P11;
            float p11 = P11 + Q;
            P00 = p00; P01 = p01; P10 = p10; P11 = p11;
        }
        void update(float z) {
            float S = P00 + R;
            float K0 = P00 / S, K1 = P10 / S;
            float y = z - pos;
            pos += K0 * y; vel += K1 * y;
            float p00 = (1 - K0) * P00, p01 = (1 - K0) * P01;
            float p10 = P10 - K1 * P00, p11 = P11 - K1 * P01;
            P00 = p00; P01 = p01; P10 = p10; P11 = p11;
        }
        float ahead(float t) const { return pos + vel * t; }
    };

    void snap(float tx, float ty);
    void smooth(float tx, float ty, float factor);
    void fire();
    void tryInstallSilent();

    Config* cfg_ = nullptr;
    bool hasTarget_ = false;
    bool silentInstalled_ = false;
    bool silentAttempted_ = false;
    std::chrono::steady_clock::time_point lastFire_{};

    Kalman1D kx_, ky_;
    bool trackInitialized_ = false;
    std::chrono::steady_clock::time_point lastTrackUpdate_{};

    std::mt19937 rng_{(unsigned)std::chrono::steady_clock::now().time_since_epoch().count()};
};
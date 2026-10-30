#include "ESP.h"
#include <Windows.h>
#include <cmath>
#include <algorithm>

static ImU32 parseHexU32(const std::string& hex, int alpha = 255) {
    if (hex.size() != 7 || hex[0] != '#') return IM_COL32(255, 255, 255, alpha);
    int r = std::stoi(hex.substr(1, 2), nullptr, 16);
    int g = std::stoi(hex.substr(3, 2), nullptr, 16);
    int b = std::stoi(hex.substr(5, 2), nullptr, 16);
    return IM_COL32(r, g, b, alpha);
}

void drawESP(ImDrawList* draw, const GameData& data, const Config& cfg) {
    if (!cfg.esp.enabled) return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float cx = sw / 2.0f;
    float cy = sh / 2.0f;

    ImU32 boxCol = parseHexU32(cfg.esp.boxColor);
    ImU32 lineCol = parseHexU32(cfg.esp.snaplineColor);
    ImU32 fovCol = parseHexU32(cfg.esp.fovColor, 120);

    if (cfg.esp.fovCircle) {
        draw->AddCircle(ImVec2(cx, cy), (float)cfg.aim.fovRadius, fovCol, 64, 1.0f);
    }

    for (const auto& p : data.players()) {
        if (!p.onScreen) continue;

        float feetX = p.screenPos.x;
        float feetY = p.screenPos.y;
        float headX = p.headScreenPos.x;
        float headY = p.headScreenPos.y;
        (void)feetX;
        float boxH = feetY - headY;
        if (boxH < 4) continue;
        float boxW = boxH * 0.45f;

        if (cfg.esp.box) {
            draw->AddRect(
                ImVec2(headX - boxW / 2, headY),
                ImVec2(headX + boxW / 2, feetY),
                boxCol, 0, 0, 2.0f);
        }

        if (cfg.esp.snapline) {
            draw->AddLine(ImVec2(cx, cy), ImVec2(headX, headY), lineCol, 1.0f);
        }

        if (cfg.esp.health) {
            float hp = std::clamp(p.health / 100.0f, 0.0f, 1.0f);
            ImU32 col = IM_COL32((int)(255 * (1 - hp)), (int)(255 * hp), 0, 255);
            float bh = boxH * hp;
            draw->AddRectFilled(
                ImVec2(headX + boxW / 2 + 4, feetY - bh),
                ImVec2(headX + boxW / 2 + 9, feetY),
                col);
        }

        if (cfg.esp.distance) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%dm", (int)p.distance3D);
            draw->AddText(ImVec2(headX - boxW / 2, feetY + 2), boxCol, buf);
        }

        if (cfg.esp.name && !p.name.empty()) {
            draw->AddText(ImVec2(headX - boxW / 2, headY - 16), boxCol, p.name.c_str());
        }
    }

    if (cfg.esp.radar) {
        int rs = cfg.esp.radarSize;
        float rx = 20 + rs / 2.0f;
        float ry = 20 + rs / 2.0f;
        draw->AddCircle(ImVec2(rx, ry), rs / 2.0f, IM_COL32(0, 255, 255, 255), 48, 1.0f);
        draw->AddLine(ImVec2(rx - rs / 2, ry), ImVec2(rx + rs / 2, ry), IM_COL32(0, 255, 255, 100), 1.0f);
        draw->AddLine(ImVec2(rx, ry - rs / 2), ImVec2(rx, ry + rs / 2), IM_COL32(0, 255, 255, 100), 1.0f);
        for (const auto& p : data.players()) {
            float dx = (p.position.x - data.localPosition().x) / 100.0f;
            float dz = (p.position.z - data.localPosition().z) / 100.0f;
            float px = rx + std::clamp(dx, -1.0f, 1.0f) * (rs / 2.0f);
            float py = ry + std::clamp(dz, -1.0f, 1.0f) * (rs / 2.0f);
            draw->AddCircleFilled(ImVec2(px, py), 3.0f, IM_COL32(255, 0, 0, 255));
        }
    }
}
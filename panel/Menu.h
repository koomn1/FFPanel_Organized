#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "Config.h"
#include "GameData.h"
#include "Aimbot.h"

class Menu {
public:
    static Menu& get() { static Menu m; return m; }
    bool init(HMODULE module);
    void render();
    void panic();
    bool isRunning() const { return running_; }
    GameData& data() { return data_; }
    Aimbot& aimbot() { return aimbot_; }
    Config& cfg() { return cfg_; }
private:
    Menu() = default;
    bool createDevice();
    bool initImGui();
    void drawMenu();
    void tickHotkeys();
    void hookWndProc();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HMODULE module_ = nullptr;
    HWND hwnd_ = nullptr;
    WNDPROC origWndProc_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    Config cfg_;
    GameData data_;
    Aimbot aimbot_;
    std::atomic<bool> running_{ false };
    bool menuVisible_ = true;
    bool prevHotkeyState_[16] = {};
    unsigned tick_ = 0;
};

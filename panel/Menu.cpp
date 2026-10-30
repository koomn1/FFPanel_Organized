#include "Menu.h"
#include "ESP.h"
#include "Offsets.h"
#include "Silent.h"
#include "Logger.h"
#include "Stealth.h"
#include <dwmapi.h>
#include <string>
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static Menu* gMenu = nullptr;

bool Menu::init(HMODULE module) {
    module_ = module; gMenu = this;
    cfg_.load("config.json");

    // Inside the game process a console is a giveaway, so the default is a log
    // file with a random name in %TEMP%; nothing is written when that fails.
    if (cfg_.stealth.suppressConsole) {
        wchar_t temp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temp)) {
            const std::wstring logPath = std::wstring(temp) + L"ff_" +
                                         stealth::widen(stealth::randomToken(10)) + L".log";
            logx::initFile(logPath.c_str());
        }
    } else {
        logx::initConsole("FFPanel");
    }
    logx::info("panel attached at %p", (void*)module_);
    if (stealth::antiCheatPresent() || stealth::antiCheatProcess())
        logx::warn("anti-cheat component detected at attach");

    const int applied = ff::LoadDefault();
    if (applied < 0) logx::warn("offsets.json not found - copy sdk/offsets.example.json next to the game exe");
    else logx::info("offsets.json: %d build offset(s) applied", applied);
    if (!ff::Ready()) logx::warn("Offsets unresolved: %s", ff::UnresolvedList().c_str());
    if (!createDevice()) return false;
    if (!initImGui()) return false;
    data_.bindInjected(module_);
    aimbot_.bindConfig(&cfg_);
    hookWndProc();
    running_ = true;
    logx::info("Menu initialized. Press INSERT to toggle.");
    return true;
}

bool Menu::createDevice() {
    const std::string className = cfg_.stealth.randomizeNames
        ? "C_" + stealth::randomToken(12)
        : "FFPanelOverlayWnd";

    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = className.c_str(); RegisterClassA(&wc);

    hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                            className.c_str(), cfg_.stealth.randomizeNames ? "" : "FFPanel",
                            WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd_, SW_SHOW);
    if (cfg_.stealth.hideFromCapture && !stealth::excludeFromCapture(hwnd_))
        logx::warn("window capture exclusion unavailable on this build");
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 144; sd.BufferDesc.RefreshRate.Denominator = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL lvl; D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &lvl, &ctx_))) return false;
    ID3D11Texture2D* back = nullptr; swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false; device_->CreateRenderTargetView(back, nullptr, &rtv_); back->Release(); return true;
}

bool Menu::initImGui() {
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd_); ImGui_ImplDX11_Init(device_, ctx_); return true;
}

void Menu::hookWndProc() { origWndProc_ = (WNDPROC)SetWindowLongPtrA(hwnd_, GWLP_WNDPROC, (LONG_PTR)WndProc); }

LRESULT CALLBACK Menu::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_KEYDOWN) {
        if (w == VK_INSERT && gMenu) {
            gMenu->menuVisible_ = !gMenu->menuVisible_;
            if (gMenu->menuVisible_) SetWindowLongPtrA(gMenu->hwnd_, GWL_EXSTYLE, GetWindowLongPtrA(gMenu->hwnd_, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
            else SetWindowLongPtrA(gMenu->hwnd_, GWL_EXSTYLE, GetWindowLongPtrA(gMenu->hwnd_, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
            return 0;
        }
        if (w == VK_END) { if (gMenu) gMenu->panic(); PostQuitMessage(0); return 0; }
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (gMenu && gMenu->origWndProc_) return CallWindowProcA(gMenu->origWndProc_, h, m, w, l);
    return DefWindowProcA(h, m, w, l);
}

void Menu::panic() {
    running_ = false;
    // Leave no inline patch behind in the game's code section.
    uninstallSilentHook();
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
    }
    logx::info("panic");
}

void Menu::render() {
    MSG msg; while (PeekMessageA(&msg, hwnd_, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (!running_) return;

    // Optional watchdog: bail out as soon as an anti-cheat component shows up.
    if (cfg_.stealth.panicOnDetect && (++tick_ % 120u) == 0) {
        if (stealth::antiCheatPresent() || stealth::antiCheatProcess()) {
            logx::warn("anti-cheat present - panicking");
            panic();
            return;
        }
    }

    data_.update(); aimbot_.update(data_, cfg_); tickHotkeys();
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if (menuVisible_) drawMenu();
    drawESP(ImGui::GetBackgroundDrawList(), data_, cfg_);
    ImGui::Render(); const float clear[4] = { 0, 0, 0, 0 }; ctx_->OMSetRenderTargets(1, &rtv_, nullptr); ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); swap_->Present(1, 0);
}

void Menu::tickHotkeys() {
    auto pressed = [&](int idx, int vk) { bool down = (GetAsyncKeyState(vk) & 0x8000) != 0; bool result = down && !prevHotkeyState_[idx]; prevHotkeyState_[idx] = down; return result; };
    if (pressed(0, cfg_.hotkeys.toggleAim)) cfg_.aim.enabled = !cfg_.aim.enabled;
    if (pressed(1, cfg_.hotkeys.toggleTrigger)) cfg_.trigger.enabled = !cfg_.trigger.enabled;
    if (pressed(2, cfg_.hotkeys.toggleEsp)) cfg_.esp.enabled = !cfg_.esp.enabled;
    if (pressed(3, cfg_.hotkeys.toggleSilent)) cfg_.silent.enabled = !cfg_.silent.enabled;
    if (pressed(4, cfg_.hotkeys.panic)) running_ = false;
}

void Menu::drawMenu() {
    ImGui::SetNextWindowSize(ImVec2(380, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("FFPanel", &menuVisible_);
    if (ImGui::CollapsingHeader("Aimbot", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable##aim", &cfg_.aim.enabled);
        const char* modes[] = { "smooth", "snap", "prediction", "trigger_snap", "instant", "silent" };
        int cur = 0; for (int i = 0; i < 6; i++) if (cfg_.aim.mode == modes[i]) cur = i;
        if (ImGui::Combo("Mode", &cur, modes, 6)) cfg_.aim.mode = modes[cur];
        ImGui::SliderInt("FOV", &cfg_.aim.fovRadius, 50, 800); ImGui::SliderFloat("Smooth", &cfg_.aim.smooth, 0.05f, 1.5f, "%.2f");
        ImGui::SliderFloat("Head offset", &cfg_.aim.headOffset, 0.0f, 0.5f, "%.2f"); ImGui::SliderInt("Prediction ms", &cfg_.aim.predictionMs, 0, 300);
        ImGui::Checkbox("Humanize", &cfg_.aim.humanize); ImGui::Checkbox("Recoil compensation", &cfg_.aim.recoilCompensation);
    }
    if (ImGui::CollapsingHeader("Trigger", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable##trig", &cfg_.trigger.enabled); ImGui::SliderInt("Radius", &cfg_.trigger.radiusPx, 10, 200);
        ImGui::SliderInt("Hold ms", &cfg_.trigger.holdMs, 10, 200); ImGui::SliderInt("Fire rate ms", &cfg_.trigger.fireRateMs, 30, 500);
    }
    if (ImGui::CollapsingHeader("ESP", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable##esp", &cfg_.esp.enabled); ImGui::Checkbox("Box", &cfg_.esp.box); ImGui::Checkbox("Snapline", &cfg_.esp.snapline);
        ImGui::Checkbox("Health", &cfg_.esp.health); ImGui::Checkbox("Distance", &cfg_.esp.distance); ImGui::Checkbox("Name", &cfg_.esp.name);
        ImGui::Checkbox("FOV circle", &cfg_.esp.fovCircle); ImGui::Checkbox("Radar", &cfg_.esp.radar); ImGui::Checkbox("Target highlight", &cfg_.esp.targetHighlight);
    }
    if (ImGui::CollapsingHeader("Silent aim")) {
        ImGui::Checkbox("Enable##silent", &cfg_.silent.enabled); ImGui::TextDisabled("Requires driver + hook");
        ImGui::Text("Hook status: %s", aimbot_.silentInstalled() ? "OK" : "NOT INSTALLED");
    }
    if (ImGui::CollapsingHeader("Config")) {
        if (ImGui::Button("Save config")) cfg_.save("config.json"); if (ImGui::Button("Reload config")) cfg_.load("config.json");
    }
    if (ImGui::CollapsingHeader("Stealth")) {
        ImGui::Checkbox("Random window names", &cfg_.stealth.randomizeNames);
        ImGui::Checkbox("Hide from capture", &cfg_.stealth.hideFromCapture);
        ImGui::Checkbox("Suppress console", &cfg_.stealth.suppressConsole);
        ImGui::Checkbox("Panic on anti-cheat", &cfg_.stealth.panicOnDetect);
        if (ImGui::Button("Panic now")) panic();
        ImGui::Text("AC modules: %s", stealth::antiCheatPresent() ? "detected" : "none");
    }
    if (ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::Text("Attached: %s", data_.isAttached() ? "yes" : "no"); ImGui::Text("Players: %d", (int)data_.players().size());
        ImGui::Text("Offsets: %s", ff::Ready() ? "ready" : "unresolved");
        ImGui::Text("Target valid: %s", aimbot_.hasTarget() ? "yes" : "no");
    }
    ImGui::End();
}

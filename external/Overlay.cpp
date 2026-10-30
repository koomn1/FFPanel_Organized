#include "Overlay.h"
using namespace Gdiplus;

Overlay::Overlay(int width, int height) : w_(width), h_(height) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "FFPanelExternalOverlay"; wc.hbrBackground = CreateSolidBrush(RGB(1, 1, 1));
    RegisterClassA(&wc);
    hwnd_ = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "FFPanelExternalOverlay", "", WS_POPUP, 0, 0, w_, h_, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd_, RGB(1, 1, 1), 0, LWA_COLORKEY);
    ShowWindow(hwnd_, SW_HIDE);
    HDC screenDC = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screenDC);
    bmp_ = CreateCompatibleBitmap(screenDC, w_, h_);
    oldBmp_ = (HBITMAP)SelectObject(memDC_, bmp_);
    ReleaseDC(nullptr, screenDC);
    gfx_ = new Graphics(memDC_);
    gfx_->SetSmoothingMode(SmoothingModeAntiAlias);
}
Overlay::~Overlay() {
    delete gfx_; SelectObject(memDC_, oldBmp_); DeleteObject(bmp_); DeleteDC(memDC_); if (hwnd_) DestroyWindow(hwnd_);
}
void Overlay::show() { ShowWindow(hwnd_, SW_SHOW); }
void Overlay::close() { ShowWindow(hwnd_, SW_HIDE); }
void Overlay::beginFrame() { HBRUSH bg = CreateSolidBrush(RGB(1, 1, 1)); RECT rc = { 0, 0, w_, h_ }; FillRect(memDC_, &rc, bg); DeleteObject(bg); }
void Overlay::endFrame() { HDC screenDC = GetDC(nullptr); BitBlt(screenDC, 0, 0, w_, h_, memDC_, 0, 0, SRCCOPY); ReleaseDC(nullptr, screenDC); }
LRESULT CALLBACK Overlay::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
Gdiplus::Color Overlay::parseHex(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return Color(255, 255, 255, 255);
    return Color(255, std::stoi(hex.substr(1, 2), nullptr, 16), std::stoi(hex.substr(3, 2), nullptr, 16), std::stoi(hex.substr(5, 2), nullptr, 16));
}
void Overlay::drawBox(float x, float y, float w, float h, const std::string& color) { Pen pen(parseHex(color), 2.0f); gfx_->DrawRectangle(&pen, (int)x, (int)y, (int)w, (int)h); }
void Overlay::drawLine(float x1, float y1, float x2, float y2, const std::string& color) { Pen pen(parseHex(color), 1.0f); gfx_->DrawLine(&pen, x1, y1, x2, y2); }

#pragma once
#include <Windows.h>
#include <gdiplus.h>
#include <string>

class Overlay {
public:
    Overlay(int width, int height);
    ~Overlay();
    void show();
    void close();
    void beginFrame();
    void endFrame();
    void drawBox(float x, float y, float w, float h, const std::string& color);
    void drawLine(float x1, float y1, float x2, float y2, const std::string& color);
    int width() const { return w_; }
    int height() const { return h_; }
private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static Gdiplus::Color parseHex(const std::string& hex);
    HWND hwnd_ = nullptr;
    int w_, h_;
    HDC memDC_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP oldBmp_ = nullptr;
    Gdiplus::Graphics* gfx_ = nullptr;
};

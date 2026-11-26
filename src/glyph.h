// glyph.h — the ghost glyph, shared by GhostWin.exe and tools/make_icon.cpp
// so the app icon, the tray icon and the title-bar buttons are one drawing.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <vector>

// Coverage mask (0..255) of the ghost glyph, rendered with GDI at 4x and
// averaged down, so it's anti-aliased at any size.
inline std::vector<BYTE> GhostCoverage(int w, int h, int glyph) {
    const int k = 4, W = w * k, H = h * k;
    std::vector<BYTE> out((size_t)w * h, 0);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return out;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, white);

    const int gs = glyph * k;
    const int bw = gs * 4 / 5;
    const int cx = W / 2, top = (H - gs) / 2, left = cx - bw / 2, right = left + bw;
    const int r = std::max(1, bw / 6);
    const int yEnd = top + gs - r;
    Ellipse(dc, left, top, right + 1, top + bw + 1);                    // head
    Rectangle(dc, left, top + bw / 2, right + 1, yEnd + 1);             // body
    for (int i = 0; i < 3; ++i)                                         // wavy hem
        Ellipse(dc, left + i * bw / 3, yEnd - r, left + (i + 1) * bw / 3 + 1, yEnd + r + 1);
    SelectObject(dc, GetStockObject(BLACK_BRUSH));                     // eyes
    const int er = std::max(k, bw / 10), ey = top + bw * 2 / 5, dx = bw / 5;
    Ellipse(dc, cx - dx - er, ey - er, cx - dx + er + 1, ey + er * 13 / 10 + 1);
    Ellipse(dc, cx + dx - er, ey - er, cx + dx + er + 1, ey + er * 13 / 10 + 1);
    GdiFlush();

    const BYTE* px = static_cast<const BYTE*>(bits);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int sy = 0; sy < k; ++sy)
                for (int sx = 0; sx < k; ++sx) sum += px[((size_t)(y * k + sy) * W + (x * k + sx)) * 4 + 2];
            out[(size_t)y * w + x] = (BYTE)(sum / (k * k));
        }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBmp);
    DeleteObject(white);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}


// The icon's colour: lavender, with the eyes cut out (transparent).
constexpr BYTE kIconR = 0xB4, kIconG = 0xA6, kIconB = 0xFF;
inline int IconGlyphSize(int size) { return size * 9 / 10; }

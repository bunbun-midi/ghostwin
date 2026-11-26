// make_icon.cpp — renders the ghost glyph (src/glyph.h) at every icon size as
// raw RGBA files; tools/make_icon.py packs them into res/ghostwin.ico.
//   make_icon.exe <output folder>
#include <stdio.h>
#include <string>
#include "../src/glyph.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    for (int s : sizes) {
        std::vector<BYTE> cov = GhostCoverage(s, s, IconGlyphSize(s));
        std::wstring path = std::wstring(argv[1]) + L"\\icon-" + std::to_wstring(s) + L".rgba";
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return 1;
        for (BYTE a : cov) {
            BYTE px[4] = {kIconR, kIconG, kIconB, a};
            fwrite(px, 1, 4, f);
        }
        fclose(f);
    }
    return 0;
}

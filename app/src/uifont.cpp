#include "uifont.hpp"

namespace gdapp {
namespace {

Font g_font{};
bool g_loaded = false;

// Loaded well above anything drawn on screen (the app's largest text is the
// ~20px menu title) so every DrawTextEx call downscales rather than upscales —
// upscaling a bitmap font atlas is what makes text look soft/blurry, the exact
// problem this replaces.
constexpr int kBaseSize = 64;

// Consolas first: this app is a data-dense debug tool (Physics Lab's aligned
// value columns, the transport bar's frame counters, trigger dumps) where a
// monospace face keeps numbers lined up and tells 0/O and 1/l/I apart at a
// glance — both of which raylib's old default font and a proportional face
// would lose. Segoe UI is the fallback for a non-Consolas Windows install.
const char* kCandidates[] = {
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
};

} // namespace

void LoadUIFont() {
    if (g_loaded) return;
    for (const char* path : kCandidates) {
        Font f = LoadFontEx(path, kBaseSize, nullptr, 0);
        // LoadFontEx never returns a null texture on failure — it silently hands
        // back GetFontDefault() instead. Detect that by comparing the texture id
        // against the real default font's, so a missing file falls through to
        // the NEXT candidate instead of "succeeding" with the very font this
        // exists to replace.
        if (f.texture.id != GetFontDefault().texture.id && f.glyphCount > 0) {
            SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
            g_font = f;
            g_loaded = true;
            return;
        }
    }
    // Every candidate failed (non-Windows machine, fonts moved, etc.) — keep
    // raylib's default font rather than leaving g_font zero-initialized.
    g_font = GetFontDefault();
    g_loaded = true;
}

void UIText(const char* text, int posX, int posY, int fontSize, Color color) {
    if (!g_loaded) LoadUIFont();
    // raylib's own convention (used internally by DrawText/GuiXxx): spacing
    // scales with size so letterforms don't visually collide at large sizes or
    // look sparse at small ones.
    DrawTextEx(g_font, text, Vector2{(float)posX, (float)posY}, (float)fontSize,
               (float)fontSize / 10.f, color);
}

int UITextWidth(const char* text, int fontSize) {
    if (!g_loaded) LoadUIFont();
    return (int)MeasureTextEx(g_font, text, (float)fontSize, (float)fontSize / 10.f).x;
}

} // namespace gdapp

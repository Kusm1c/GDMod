#pragma once
#include "raylib.h"

// ─────────────────────────────────────────────────────────────────────────────
// UI FONT — replaces raylib's built-in default font (a tiny, blocky 10x10
// bitmap face that reads poorly at the small sizes this app's data-dense
// panels use — Physics Lab rows, the transport bar, trajectory-fit tables)
// with a real TTF loaded from the system, rendered at a high base size and
// scaled down for clean, anti-aliased text at any UI size.
//
// UIText / UITextWidth are drop-in replacements for raylib's DrawText /
// MeasureText with the IDENTICAL signature, so every call site in this app was
// mechanically renamed rather than restructured. Using them as a PAIR matters:
// mixing UITextWidth (this font's real glyph metrics) with the old MeasureText
// (the default font's metrics) would silently misalign anything laid out with
// the width, e.g. right-aligned numbers or centred labels.
// ─────────────────────────────────────────────────────────────────────────────

namespace gdapp {

// Loads the UI font. Call once, after InitWindow. Safe to call again (a second
// call is a no-op) and safe if no suitable font is found on the system — falls
// back to raylib's default font, so the app degrades rather than failing.
void LoadUIFont();

// Same signature as raylib's DrawText(text, posX, posY, fontSize, color).
void UIText(const char* text, int posX, int posY, int fontSize, Color color);

// Same signature/contract as raylib's MeasureText(text, fontSize) -> width in
// pixels. Always pair with UIText, never with plain MeasureText — see above.
int UITextWidth(const char* text, int fontSize);

} // namespace gdapp

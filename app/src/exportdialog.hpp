#pragma once
#include <string>
#include <optional>

// Native "Save As" dialog for exporting a .gdr2 — kept in its own translation
// unit (no raylib.h here) because windows.h/commdlg.h's macros (Rectangle,
// CloseWindow, ...) collide with raylib's own — the same reason the app keeps
// network.cpp/leveldata.cpp separate from anything raylib-facing.
namespace gdapp {

// Shows the dialog pre-filled with `defaultFileName`, starting in `initialDir`
// (if non-empty and it exists). Returns the chosen full path, or nullopt if
// the dialog was cancelled.
std::optional<std::string> pickGdr2SavePath(const std::string& defaultFileName,
                                             const std::string& initialDir);

// Native "Open" dialog for importing a .gdr2 to replay/compare against the
// physics trail. Returns the chosen full path, or nullopt if cancelled.
std::optional<std::string> pickGdr2OpenPath(const std::string& initialDir);

// Last-used export folder, persisted as a plain text file in cache/ (mirrors
// leveldata.cpp's own on-disk cache convention — this app has no Geode
// Mod::get() saved-value store to reuse).
std::string loadLastExportFolder();
void saveLastExportFolder(const std::string& folder);

} // namespace gdapp

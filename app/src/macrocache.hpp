#pragma once
#include "macroentry.hpp"
#include <vector>

// On-disk persistence for cleared macros (cache/macros/*.macro), so they
// survive across app launches instead of only living in memory for the
// session. The level string (by far the largest part of an entry — a big
// level's decoded string can be 300KB+) is gzip-compressed on disk; clicks
// and metadata are tiny and stored raw.
namespace gdapp {

// Appends one entry as a new file under cache/macros/. Returns false on any
// I/O or compression failure (caller can ignore — the in-memory copy still
// works for the current session either way).
bool saveMacroToDisk(const MacroEntry& m);

// Loads every *.macro file under cache/macros/, decompressing each level
// string back to plain text. Corrupt/unreadable files are skipped rather
// than aborting the whole load.
std::vector<MacroEntry> loadMacrosFromDisk();

} // namespace gdapp

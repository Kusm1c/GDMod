#include "exportdialog.hpp"
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>

namespace gdapp {

static std::filesystem::path lastExportFolderFile() {
    return std::filesystem::path("cache") / "last_export_folder.txt";
}

std::string loadLastExportFolder() {
    std::ifstream f(lastExportFolderFile());
    std::string line;
    if (f.is_open()) std::getline(f, line);
    return line;
}

void saveLastExportFolder(const std::string& folder) {
    std::error_code ec;
    std::filesystem::create_directories("cache", ec);
    std::ofstream f(lastExportFolderFile(), std::ios::trunc);
    if (f.is_open()) f << folder;
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    w.resize(len - 1); // drop the trailing NUL MultiByteToWideChar counted
    return w;
}

static std::string wideToUtf8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    s.resize(len - 1);
    return s;
}

std::optional<std::string> pickGdr2SavePath(const std::string& defaultFileName,
                                             const std::string& initialDir) {
    wchar_t szFile[MAX_PATH] = {0};
    std::wstring wDefault = utf8ToWide(defaultFileName);
    wcsncpy_s(szFile, wDefault.c_str(), _TRUNCATE);

    std::wstring wInitDir;
    std::error_code ec;
    if (!initialDir.empty() && std::filesystem::exists(initialDir, ec))
        wInitDir = utf8ToWide(initialDir);

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = L"GDR2 Replay (*.gdr2)\0*.gdr2\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt  = L"gdr2";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!wInitDir.empty()) ofn.lpstrInitialDir = wInitDir.c_str();

    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return wideToUtf8(szFile);
}

} // namespace gdapp

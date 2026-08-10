#pragma once
#include <string>

namespace gdapp {

// Minimal blocking HTTP POST (WinHTTP-backed, no external deps). Returns the
// full response body, or an empty string with `ok` set false on any failure.
struct HttpResult {
    bool ok = false;
    std::string body;
    std::string error;
};

HttpResult httpPost(const std::wstring& host, unsigned short port,
                     const std::wstring& path, const std::string& body,
                     const std::wstring& contentType, bool useHttps);

} // namespace gdapp

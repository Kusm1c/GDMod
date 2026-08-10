#include "network.hpp"
#include <windows.h>
#include <winhttp.h>

namespace gdapp {

HttpResult httpPost(const std::wstring& host, unsigned short port,
                     const std::wstring& path, const std::string& body,
                     const std::wstring& contentType, bool useHttps) {
    HttpResult result;

    // Empty user-agent, matching RobTop's own client requirement (a browser-style
    // UA gets rejected by the endpoint) — verified against the real server.
    HINTERNET hSession = WinHttpOpen(L"",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { result.error = "WinHttpOpen failed"; return result; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { result.error = "WinHttpConnect failed"; WinHttpCloseHandle(hSession); return result; }

    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { result.error = "WinHttpOpenRequest failed"; WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    std::wstring headers = L"Content-Type: " + contentType;
    BOOL sent = WinHttpSendRequest(hRequest,
        headers.c_str(), (DWORD)headers.size(),
        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);

    if (sent) sent = WinHttpReceiveResponse(hRequest, nullptr);

    if (sent) {
        std::string out;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            out += chunk;
        }
        result.ok = true;
        result.body = std::move(out);
    } else {
        result.error = "WinHttp request failed (code " + std::to_string(GetLastError()) + ")";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

} // namespace gdapp

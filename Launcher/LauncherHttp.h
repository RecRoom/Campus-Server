#pragma once
// LauncherHttp.h — WinHTTP-based HTTP client for the launcher.
//
// Deliberately tiny: only what we need to talk to the Campus API from a native EXE.
//   - GET with optional Bearer token
//   - POST JSON body with optional Bearer token
//   - Returns { status, body } — caller parses JSON however it likes (we use a tiny JSON
//     scraper in LauncherAuth.h for the two/three fields we need).
//   - Blocking (called from worker threads only; UI thread never blocks on the network).
//
// WinHTTP over WinINet: WinINet is a per-user proxy stack that pops UI on failure. WinHTTP
// is service-safe and doesn't touch IE settings. Since the launcher may launch under weird
// contexts (fresh install, no browser configured) WinHTTP is the safer default.
//
// TLS: uses the OS trust store. Our Campus cert is Let's Encrypt via Caddy, so this Just Works
// on Windows 10+ — no cert-pinning needed. If the cert is invalid we surface the WinHTTP error.

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdint>
#pragma comment(lib, "winhttp.lib")

namespace CampusHttp {

    struct Response {
        int         status = 0;      // HTTP status code, 0 on transport failure
        std::string body;            // response body as UTF-8 bytes (may be empty)
        std::string transport_error; // populated only when status == 0
    };

    // Split "https://gameauth.ddns.net/api/auth/me" into (host, path, https). The launcher only
    // ever talks to one host (gameauth.ddns.net) but keeping this generic means we can point at
    // a staging server just by swapping the base URL constant.
    inline bool ParseUrl(const std::wstring& url,
                         std::wstring& host, std::wstring& path, bool& https, INTERNET_PORT& port)
    {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t h[256] = {}, p[1024] = {};
        uc.lpszHostName = h; uc.dwHostNameLength = _countof(h);
        uc.lpszUrlPath  = p; uc.dwUrlPathLength  = _countof(p);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
        host = h;
        path = p;
        https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        port = uc.nPort ? uc.nPort : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
        return true;
    }

    // Core request. `method` is L"GET" / L"POST"; `body` may be empty for GET; `bearer` is a
    // bare JWT (we prepend "Bearer " automatically) or empty for anonymous.
    inline Response Request(const std::wstring& url, const std::wstring& method,
                            const std::string& body, const std::string& bearer,
                            const std::wstring& contentType = L"application/json")
    {
        Response out;
        std::wstring host, path; bool https = true; INTERNET_PORT port = 443;
        if (!ParseUrl(url, host, path, https, port)) { out.transport_error = "bad url"; return out; }

        HINTERNET hSession = WinHttpOpen(L"CampusLauncher/1.0",
                                         WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { out.transport_error = "WinHttpOpen failed"; return out; }
        HINTERNET hConn = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConn) { out.transport_error = "WinHttpConnect failed"; WinHttpCloseHandle(hSession); return out; }

        HINTERNET hReq = WinHttpOpenRequest(hConn, method.c_str(), path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            https ? WINHTTP_FLAG_SECURE : 0);
        if (!hReq) { out.transport_error = "WinHttpOpenRequest failed";
                     WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return out; }

        // Headers
        std::wstring headers = L"Content-Type: " + contentType + L"\r\n";
        if (!bearer.empty()) {
            std::wstring wbearer(bearer.begin(), bearer.end());
            headers += L"Authorization: Bearer " + wbearer + L"\r\n";
        }

        BOOL ok = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)headers.size(),
                                     (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (!ok) { out.transport_error = "WinHttpSendRequest failed"; goto cleanup; }
        if (!WinHttpReceiveResponse(hReq, nullptr)) { out.transport_error = "WinHttpReceiveResponse failed"; goto cleanup; }

        {
            DWORD statusCode = 0, statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &statusCode, &statusSize, nullptr);
            out.status = (int)statusCode;
        }
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
            if (avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hReq, buf.data(), avail, &read) || read == 0) break;
            out.body.append(buf.data(), read);
        }

    cleanup:
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return out;
    }

    inline Response Get (const std::wstring& url, const std::string& bearer = {}) { return Request(url, L"GET",  {},  bearer); }
    inline Response Post(const std::wstring& url, const std::string& body, const std::string& bearer = {}) { return Request(url, L"POST", body, bearer); }
}

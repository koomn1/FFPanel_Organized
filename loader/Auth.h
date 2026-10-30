#pragma once
#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include "json.hpp"

#pragma comment(lib, "winhttp.lib")

class Auth {
public:
    bool login(const std::string& serverUrl, const std::string& user,
               const std::string& pass, const std::string& hwid,
               std::string& outToken) {
        std::string body = "{\"user\":\"" + user + "\",\"pass\":\"" + pass +
                           "\",\"hwid\":\"" + hwid + "\"}";
        std::string resp;
        if (!post(serverUrl + "/api/auth", body, resp)) return false;
        try {
            auto j = nlohmann::json::parse(resp);
            if (!j.value("ok", false)) return false;
            outToken = j.value("token", "");
            return !outToken.empty();
        } catch (...) { return false; }
    }

    bool download(const std::string& url, const std::string& token,
                  std::vector<uint8_t>& out) {
        std::string full = url + (url.find('?') == std::string::npos ? "?" : "&") +
                           "token=" + token;
        return get(full, out);
    }

private:
    bool post(const std::string& url, const std::string& body, std::string& out) {
        std::vector<uint8_t> buf;
        if (!request(url, L"POST", (const uint8_t*)body.data(), body.size(), buf)) return false;
        out.assign(buf.begin(), buf.end());
        return true;
    }

    bool get(const std::string& url, std::vector<uint8_t>& out) {
        return request(url, L"GET", nullptr, 0, out);
    }

    bool request(const std::string& url, const wchar_t* method,
                 const uint8_t* body, size_t bodyLen, std::vector<uint8_t>& out) {
        std::wstring wurl(url.begin(), url.end());
        URL_COMPONENTS uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[2048] = {};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

        HINTERNET session = WinHttpOpen(L"FFPanel/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
        if (!conn) { WinHttpCloseHandle(session); return false; }

        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, method, path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }

        if (body && bodyLen > 0) {
            const wchar_t* hdrs = L"Content-Type: application/json\r\n";
            WinHttpSendRequest(req, hdrs, (DWORD)-1L, (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0);
        } else {
            WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }

        if (!WinHttpReceiveResponse(req, nullptr)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }

        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }

        DWORD avail;
        do {
            avail = 0;
            WinHttpQueryDataAvailable(req, &avail);
            if (avail == 0) break;
            size_t old = out.size();
            out.resize(old + avail);
            DWORD read = 0;
            WinHttpReadData(req, out.data() + old, avail, &read);
            out.resize(old + read);
        } while (avail > 0);

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return true;
    }
};
#pragma once
// LauncherAuth.h — session state, JWT persistence, and thin wrappers over the /api/auth/*
// endpoints served by AuthLauncherController.
//
// Storage: %APPDATA%\Campus\auth.json — a tiny JSON file with just { "token": "<jwt>" }. If
// the file's missing or the token has expired (401 on /api/auth/me), we wipe it and drop back
// to the login screen. No cross-machine sync, no encryption at rest — this is desktop-launcher
// convenience storage, not a secret vault.
//
// JSON parsing: we don't pull in a real JSON library for the launcher (would balloon the
// binary and complicate the build). Instead a tiny key-picker regex grabs the top-level
// fields we care about (token, account_id, etc). Good enough for the two dozen fields on
// /api/auth/me responses; the launcher never has to serialize complex JSON, only two literals
// (register + login POST bodies).

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "LauncherHttp.h"
#pragma comment(lib, "shell32.lib")

namespace CampusAuth {

    static const wchar_t* API_BASE = L"https://gameauth.ddns.net";

    // What the launcher UI needs to know about the current session. Populated by Refresh().
    struct Session {
        bool         logged_in     = false;
        std::string  token;                 // JWT
        uint64_t     account_id    = 0;
        std::string  username;
        std::string  display_name;
        std::string  profile_image;         // pfp filename, resolved via /img/<name>
        std::string  bio;
        uint64_t     discord_id    = 0;     // 0 if not linked
        std::string  discord_username;
        std::string  steam_id;              // string form; may be empty
        std::string  meta_id;
        bool         banned        = false;
        int64_t      ban_ends_at   = 0;     // unix seconds, 0 if not banned
        std::string  ban_reason;
        bool         is_dev        = false;
        bool         is_mod        = false;

        // Status of the last refresh; UI reads this to decide login screen vs "expired" toast.
        // "ok"       — /api/auth/me returned 200, fields above are current
        // "expired"  — 401 from /api/auth/me, token was wiped, show "Login time expired..."
        // "invalid"  — 401 from login/register, show "Logged out due to Invalid Credentials"
        // "network"  — transport failure (no net, server down); UI should show a retry
        // "unauth"   — no token on disk, just show login screen (not an error)
        std::string  status         = "unauth";
    };

    // ------- Local storage --------------------------------------------------
    inline std::wstring ConfigDir() {
        wchar_t appdata[MAX_PATH] = {};
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata) != S_OK) return L"";
        std::wstring dir = std::wstring(appdata) + L"\\Campus";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    inline std::wstring TokenPath() { return ConfigDir() + L"\\auth.json"; }

    inline std::string LoadToken() {
        std::ifstream f(TokenPath());
        if (!f) return "";
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        // Extremely narrow scraper: look for  "token"  :  "  <value>  "
        auto k = s.find("\"token\"");
        if (k == std::string::npos) return "";
        auto q = s.find('"', k + 7);
        if (q == std::string::npos) return "";
        auto q2 = s.find('"', q + 1);
        if (q2 == std::string::npos) return "";
        return s.substr(q + 1, q2 - q - 1);
    }
    inline void SaveToken(const std::string& tok) {
        std::wstring path = TokenPath();
        std::ofstream f(path);
        f << "{\n  \"token\": \"" << tok << "\"\n}\n";
    }
    inline void WipeToken() {
        DeleteFileW(TokenPath().c_str());
    }

    // ------- Micro JSON pickers ---------------------------------------------
    // Grab the value of a top-level string field. Returns "" if not found or if the field is
    // null. NOT a real parser — doesn't handle escaped quotes inside strings, nested objects,
    // etc. Good enough for /api/auth/me which is a flat object with plain-ASCII values.
    inline std::string PickString(const std::string& s, const char* key) {
        std::string needle = std::string("\"") + key + "\"";
        auto k = s.find(needle);
        if (k == std::string::npos) return "";
        auto colon = s.find(':', k + needle.size());
        if (colon == std::string::npos) return "";
        // Look at the next non-space char. If it's " we have a string; if it's n (null) return "".
        size_t i = colon + 1;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= s.size()) return "";
        if (s[i] == 'n') return "";   // null
        if (s[i] != '"') return "";
        auto q2 = s.find('"', i + 1);
        if (q2 == std::string::npos) return "";
        return s.substr(i + 1, q2 - i - 1);
    }
    inline int64_t PickInt(const std::string& s, const char* key, int64_t defv = 0) {
        std::string needle = std::string("\"") + key + "\"";
        auto k = s.find(needle);
        if (k == std::string::npos) return defv;
        auto colon = s.find(':', k + needle.size());
        if (colon == std::string::npos) return defv;
        size_t i = colon + 1;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= s.size()) return defv;
        if (s[i] == 'n') return defv;
        return _atoi64(s.c_str() + i);
    }
    inline bool PickBool(const std::string& s, const char* key, bool defv = false) {
        std::string needle = std::string("\"") + key + "\"";
        auto k = s.find(needle);
        if (k == std::string::npos) return defv;
        auto colon = s.find(':', k + needle.size());
        if (colon == std::string::npos) return defv;
        size_t i = colon + 1;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i + 3 < s.size() && s.compare(i, 4, "true") == 0) return true;
        return false;
    }

    // ------- Trivial JSON body writer ---------------------------------------
    inline std::string EscapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out.push_back(c);
            }
        }
        return out;
    }

    // ------- Endpoint wrappers ----------------------------------------------
    // Fills Session from /api/auth/me. Called from the auth worker thread.
    inline void Refresh(Session& out) {
        std::string token = LoadToken();
        if (token.empty()) {
            out.logged_in = false;
            out.status = "unauth";
            return;
        }
        auto r = CampusHttp::Get(std::wstring(API_BASE) + L"/api/auth/me", token);
        if (r.status == 401) {
            WipeToken();
            out.logged_in = false;
            out.status    = "expired";   // trigger "Login time expired..." toast
            return;
        }
        if (r.status != 200) {
            out.logged_in = false;
            out.status    = (r.status == 0) ? "network" : "unauth";
            return;
        }
        // Parse the flat response.
        out.token            = token;
        out.account_id       = (uint64_t)PickInt (r.body, "id");
        out.username         =           PickString(r.body, "username");
        out.display_name     =           PickString(r.body, "display_name");
        out.profile_image    =           PickString(r.body, "profile_image");
        out.bio              =           PickString(r.body, "bio");
        out.discord_id       = (uint64_t)PickInt (r.body, "discord_id");
        out.discord_username =           PickString(r.body, "discord_username");
        out.steam_id         =           PickString(r.body, "steam_id");
        out.meta_id          =           PickString(r.body, "meta_id");
        out.banned           =           PickBool (r.body, "banned");
        out.ban_ends_at      =           PickInt  (r.body, "ban_ends_at");
        out.ban_reason       =           PickString(r.body, "ban_reason");
        out.is_dev           =           PickBool (r.body, "is_dev");
        out.is_mod           =           PickBool (r.body, "is_mod");
        out.logged_in        = true;
        out.status           = "ok";
    }

    // Register + persist token on success. Sets `err` to the server-provided error text on
    // failure. On success `s` is repopulated via Refresh() so all fields are current.
    inline bool Register(const std::string& username, const std::string& password,
                         Session& s, std::string& err)
    {
        std::string body = "{\"username\":\"" + EscapeJson(username)
                         + "\",\"password\":\"" + EscapeJson(password) + "\"}";
        auto r = CampusHttp::Post(std::wstring(API_BASE) + L"/api/auth/register", body);
        if (r.status != 200) {
            err = PickString(r.body, "error");
            if (err.empty()) err = "register failed (status " + std::to_string(r.status) + ")";
            return false;
        }
        std::string token = PickString(r.body, "token");
        if (token.empty()) { err = "no token in register response"; return false; }
        SaveToken(token);
        Refresh(s);
        return true;
    }

    inline bool Login(const std::string& username, const std::string& password,
                      Session& s, std::string& err)
    {
        std::string body = "{\"username\":\"" + EscapeJson(username)
                         + "\",\"password\":\"" + EscapeJson(password) + "\"}";
        auto r = CampusHttp::Post(std::wstring(API_BASE) + L"/api/auth/login", body);
        if (r.status != 200) {
            err = PickString(r.body, "error");
            if (err.empty()) err = "login failed (status " + std::to_string(r.status) + ")";
            return false;
        }
        std::string token = PickString(r.body, "token");
        if (token.empty()) { err = "no token in login response"; return false; }
        SaveToken(token);
        Refresh(s);
        return true;
    }

    inline void Logout(Session& s) {
        if (!s.token.empty())
            CampusHttp::Post(std::wstring(API_BASE) + L"/api/auth/logout", "", s.token);
        WipeToken();
        s = Session{};
        s.status = "unauth";
    }

    // Link the local Steam ID (from CampusSteam::CurrentSteamId64) to the current account.
    // Returns true on 200. 409 means the steam id is claimed by another Campus account —
    // caller decides what to do.
    inline bool LinkSteam(uint64_t steamId64, const Session& s, std::string& err)
    {
        char body[128];
        _snprintf_s(body, sizeof(body), _TRUNCATE, "{\"steam_id\":%llu}", (unsigned long long)steamId64);
        auto r = CampusHttp::Post(std::wstring(API_BASE) + L"/api/auth/link/steam", body, s.token);
        if (r.status == 200) return true;
        err = PickString(r.body, "error");
        if (err.empty()) err = "link/steam failed (status " + std::to_string(r.status) + ")";
        return false;
    }
}

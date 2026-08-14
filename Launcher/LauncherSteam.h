#pragma once
// LauncherSteam.h — "is Steam running?" check.
//
// Simplest reliable strategy on Windows: enumerate processes via tlhelp32 and look for a
// process named "steam.exe". Alternatives considered:
//   - Reading HKCU\Software\Valve\Steam\ActiveProcess\pid: reads the registry, but the value
//     stays populated after Steam crashes, so it can lie. Not used.
//   - Steam SDK's SteamAPI_IsSteamRunning: pulls in the whole steam_api dylib and requires an
//     appid, overkill for a "is it up" check that runs before we care about any Steam feature.
//
// We ALSO grab the current user's SteamID64 opportunistically via the RegOpenKeyEx path
// HKCU\Software\Valve\Steam\ActiveUser (returns a 32-bit "steamID3"). Combined with the base
// Valve OSId 0x0110000100000000 that gives us the full SteamID64 we hand to /api/auth/link/steam.

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <cstdint>

namespace CampusSteam {

    inline bool IsRunning() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        bool found = false;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) { found = true; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
    }

    // Returns 0 if Steam is not signed in, otherwise the full SteamID64. Reads only HKCU so
    // no admin required. Safe to call every frame if we want but ideally called once during
    // account-link.
    inline uint64_t CurrentSteamId64() {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess",
                          0, KEY_READ, &hk) != ERROR_SUCCESS) return 0;
        DWORD activeUser = 0, sz = sizeof(activeUser);
        LONG rc = RegQueryValueExW(hk, L"ActiveUser", nullptr, nullptr,
                                    (LPBYTE)&activeUser, &sz);
        RegCloseKey(hk);
        if (rc != ERROR_SUCCESS || activeUser == 0) return 0;
        // ActiveUser is the 32-bit account id. Full SteamID64 = 0x0110000100000000 + account id.
        return 0x0110000100000000ULL + (uint64_t)activeUser;
    }
}

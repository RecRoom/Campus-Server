#pragma once
// CampusPatch console tee: mirrors every std::cout write into a per-session log file next to
// the DLL. Existing call sites (`std::cout << ...`) don't need to change - the redirect is
// transparent once Init() runs.
//
// File path: <dll folder>\Logs\logsession<YYYYMMDD_HHMMSS>.txt   (BST timestamp)
// Header:    date/time in BST + full log path, so opening the file cold identifies the session.
//
// Implementation note: an earlier revision used std::ofstream + std::cout.rdbuf() and swapped
// buffers. That interacted badly with the game's CRT (crash on first write). This version uses
// only raw Win32 HANDLEs and WriteFile, so nothing about std::cout's original rdbuf, the game's
// C++ runtime state, or std::sync_with_stdio matters. If Init fails for any reason we simply
// leave logging in console-only mode - the game keeps running.

#include <windows.h>
#include <streambuf>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>

namespace CampusLogging {

// A streambuf whose overflow/xsputn write bytes to two Win32 handles: the process's console
// (CONOUT$) and a session log file. We deliberately don't chain to a "parent" streambuf - by
// writing to CONOUT$ ourselves we avoid ever touching the original std::cout rdbuf and its
// unclear ownership/lifetime relationship with the CRT.
//
// Concurrency: multiple hook threads write concurrently (heartbeat + SendRequest + Steam
// callbacks + SteamHookThread's install log all race). A CRITICAL_SECTION guards each write so
// individual `std::cout << ...` chains land as atomic byte groups instead of interleaving.
// Any log line (delimited by '\n') that contains one of these substrings is DROPPED before
// hitting the console or the log file. This is where Unity's OpenVR spam gets swallowed
// ("Unable to read VR Path Registry ...") — it doesn't originate from CampusPatch code but
// flows through the same std::cout our tee owns, so a substring-match filter here catches
// it once and for all. Add more strings here to squash other noise.
static const char* const kDroppedLineSubstrings[] = {
    "Unable to read VR Path Registry",   // OpenVR loader on non-VR PCs
    nullptr,
};

class TeeBuf : public std::streambuf {
    HANDLE hConsole_ = INVALID_HANDLE_VALUE;
    HANDLE hFile_    = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION cs_{};
    // Per-thread line accumulator. Writes come in fragments (single chars via overflow,
    // batches via xsputn); we buffer until '\n' then decide keep/drop for the whole line.
    // thread_local so overlapping writes from different threads don't corrupt each other.
    static thread_local std::string lineBuf_;
public:
    TeeBuf() { InitializeCriticalSection(&cs_); }

    // Open both destinations. Returns true iff the FILE handle is valid; console failure is
    // tolerated (we'll silently drop the console mirror if CONOUT$ can't be opened, which
    // shouldn't happen since CreateConsole() already AllocConsole'd it).
    bool Open(const std::string& path) {
        hConsole_ = CreateFileA("CONOUT$",
            GENERIC_WRITE,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        hFile_ = CreateFileA(path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return hFile_ != INVALID_HANDLE_VALUE;
    }

    HANDLE FileHandle() const { return hFile_; }

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) { sync(); return 0; }
        appendAndMaybeEmit(static_cast<char>(ch));
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; i++) appendAndMaybeEmit(s[i]);
        return n;
    }
    int sync() override {
        if (hFile_ != INVALID_HANDLE_VALUE) FlushFileBuffers(hFile_);
        return 0;
    }

private:
    // Accumulate into the thread-local line buffer; on '\n' decide whether to keep or drop.
    // Kept lines are written atomically (line + newline) so multi-thread interleaving is at
    // whole-line granularity rather than mid-word.
    void appendAndMaybeEmit(char c) {
        lineBuf_.push_back(c);
        if (c != '\n') return;

        bool drop = false;
        for (const char* const* p = kDroppedLineSubstrings; *p; ++p) {
            if (lineBuf_.find(*p) != std::string::npos) { drop = true; break; }
        }
        if (!drop) {
            writeBoth(lineBuf_.data(), static_cast<DWORD>(lineBuf_.size()));
            if (hFile_ != INVALID_HANDLE_VALUE) FlushFileBuffers(hFile_);
        }
        lineBuf_.clear();
    }
    // Serialise writes across threads so a `std::cout << "a"` chain from one thread doesn't
    // interleave mid-word with another thread's chain. Console gets the raw bytes (ANSI escape
    // sequences included, so tags render coloured). The FILE gets a stripped copy — the same
    // bytes with any `\x1b[...m` runs removed — so tailing the log in Notepad shows readable
    // plain text instead of `[38;5;51m[NET]     [0m...`.
    void writeBoth(const char* p, DWORD n) {
        EnterCriticalSection(&cs_);
        DWORD w = 0;
        if (hConsole_ != INVALID_HANDLE_VALUE) WriteFile(hConsole_, p, n, &w, nullptr);

        if (hFile_ != INVALID_HANDLE_VALUE) {
            // Strip ANSI escape sequences on the fly. Sequences look like ESC [ <params> <letter>
            // where letter is any byte in 0x40..0x7E. Copy non-escape bytes verbatim; when we
            // hit ESC, skip until the terminator letter. Falls back to writing the tail as-is
            // if the sequence is malformed / cut off mid-fragment.
            char stripped[1024];
            DWORD si = 0;
            for (DWORD i = 0; i < n; i++) {
                unsigned char c = (unsigned char)p[i];
                if (c == 0x1B && i + 1 < n && p[i + 1] == '[') {
                    // Skip until we find a byte in 0x40..0x7E (final byte of CSI sequence).
                    i += 2;
                    while (i < n && !(p[i] >= 0x40 && p[i] <= 0x7E)) i++;
                    // Loop's `i++` handles the terminator itself.
                    continue;
                }
                if (si < sizeof(stripped)) stripped[si++] = (char)c;
                else {
                    WriteFile(hFile_, stripped, si, &w, nullptr);
                    si = 0;
                    stripped[si++] = (char)c;
                }
            }
            if (si > 0) WriteFile(hFile_, stripped, si, &w, nullptr);
        }
        LeaveCriticalSection(&cs_);
    }
};

// Storage for the thread_local line accumulator declared inside the class.
thread_local std::string TeeBuf::lineBuf_;

// Kept as a raw pointer for the process lifetime - never deleted, so no destruction race with
// hook threads still logging during shutdown.
static TeeBuf* g_tee = nullptr;

// UTC + 1 hour = BST. Hard-coded rather than reading the OS timezone so the label is stable
// across machines. In UK winter (GMT) this would be an hour ahead - accepted trade-off, port
// work is happening in summer.
static void UtcNowAsBst(SYSTEMTIME& out) {
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += 3600ULL * 10000000ULL;   // +1h in 100-ns units
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &out);
}

// Call once from InitThread. selfModule is the DLL's own HMODULE (from DllMain) - used to
// resolve <dll folder>\Logs. All status/failure messages use printf so they're visible in the
// console even if the tee itself never comes up.
static void Init(HMODULE selfModule) {
    // Resolve <dll folder>\Logs. If selfModule is null (unexpected), fall back to the process
    // exe's folder rather than crashing.
    char dllPath[MAX_PATH] = {};
    if (GetModuleFileNameA(selfModule, dllPath, MAX_PATH) == 0) {
        printf("[Campus/Log] GetModuleFileNameA failed err=%lu - console only\n", GetLastError());
        return;
    }
    char* slash = strrchr(dllPath, '\\');
    if (slash) *slash = 0;
    std::string dir = std::string(dllPath) + "\\Logs";
    // CreateDirectoryA on an existing dir returns FALSE with ERROR_ALREADY_EXISTS - fine.
    CreateDirectoryA(dir.c_str(), nullptr);

    SYSTEMTIME bst;
    UtcNowAsBst(bst);
    char id[32];
    _snprintf_s(id, sizeof(id), _TRUNCATE, "%04d%02d%02d_%02d%02d%02d",
                bst.wYear, bst.wMonth, bst.wDay,
                bst.wHour, bst.wMinute, bst.wSecond);
    std::string path = dir + "\\logsession" + id + ".txt";

    // Print the target path FIRST via printf so even if the tee itself fails, the user can see
    // where we tried to write.
    printf("[Campus/Log] session log -> %s\n", path.c_str());

    TeeBuf* tee = new TeeBuf();
    if (!tee->Open(path)) {
        printf("[Campus/Log] failed to open '%s' err=%lu - console only\n",
               path.c_str(), GetLastError());
        delete tee;
        return;
    }

    // Write the session header directly to both destinations via the TeeBuf, WITHOUT swapping
    // std::cout's rdbuf first. Reason: writing via std::cout hits the game's shared C++
    // streambuf machinery, which is what crashed the earlier version. sputn() below only touches
    // our own HANDLEs.
    char header[512];
    int hn = _snprintf_s(header, sizeof(header), _TRUNCATE,
        "=== CampusPatch session log ===\n"
        "Started (BST): %04d-%02d-%02d  %02d:%02d:%02d BST\n"
        "Log path:      %s\n"
        "===============================\n",
        bst.wYear, bst.wMonth, bst.wDay,
        bst.wHour, bst.wMinute, bst.wSecond,
        path.c_str());
    if (hn > 0) tee->sputn(header, hn);

    // Publish the tee AFTER a known-good write proved it works. Any std::cout << ... call after
    // this point routes through our overflow/xsputn -> WriteFile to both handles.
    g_tee = tee;
    std::cout.rdbuf(g_tee);
}

}   // namespace CampusLogging

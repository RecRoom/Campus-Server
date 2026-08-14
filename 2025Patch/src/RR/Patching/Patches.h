#pragma once
#include "../Methods.h"
#include "../../Utils/scan.h"
#include "../../Utils/deps/spoofcall/RetSpoof.hpp"
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

// Console cosmetics — ANSI colored tags so the log at a glance shows which subsystem said
// what. Windows 10+ consoles enable VT sequences by default; on the rare console where they
// don't, the escapes just appear inline as text — no crash, just uglier. Kept as macros so
// use sites read cleanly: `LOG_NET << "Uri: " << url << "\n";`.
//
// Foreground colors used, so the tag POPS but the following text stays plain-white.
#define CLR_RESET   "\x1b[0m"
#define CLR_DIM     "\x1b[90m"       // grey — for less-important lines
#define CLR_CYAN    "\x1b[38;5;51m"  // NET     (URIs, HTTP status)
#define CLR_GREEN   "\x1b[38;5;120m" // PATCH   (hook installs, "OK")
#define CLR_YELLOW  "\x1b[38;5;227m" // SIG     (signature checks / anti-cheat)
#define CLR_RED     "\x1b[38;5;203m" // HILE    (dropped reports)
#define CLR_MAGENTA "\x1b[38;5;213m" // HTTP-DBG (response body dumps)
#define CLR_BLUE    "\x1b[38;5;111m" // PHOTON  (redirects / voice)
#define CLR_ORANGE  "\x1b[38;5;215m" // WARN
#define CLR_BOLD    "\x1b[1m"

// Tag builders — a single-macro form like TAG_NET() expands to a fully-formed prefix so a
// call site is just `std::cout << TAG_NET() << "..." << CLR_RESET << "\n";`.
#define TAG_NET()     CLR_CYAN    "[NET]     " CLR_RESET
#define TAG_HTTP()    CLR_CYAN    "[HTTP]    " CLR_RESET
#define TAG_PATCH()   CLR_GREEN   "[PATCH]   " CLR_RESET
#define TAG_SIG()     CLR_YELLOW  "[SIG]     " CLR_RESET
#define TAG_HILE()    CLR_RED     "[HILE]    " CLR_RESET
#define TAG_DEBUG()   CLR_MAGENTA "[HTTP-DBG]" CLR_RESET " "
#define TAG_PHOTON()  CLR_BLUE    "[PHOTON]  " CLR_RESET
#define TAG_WARN()    CLR_ORANGE  "[WARN]    " CLR_RESET
#define TAG_INIT()    CLR_BOLD CLR_GREEN "[INIT]    " CLR_RESET

// HTTP debug: read a filter file at startup and dump matching request/response bodies to disk.
//
// Filter file:  <dll folder>\Patch\http-debug-filter.txt
//   - One URL substring per line. A request URL is "matched" if it contains any line as a
//     substring. Comments start with #. Empty lines ignored.
//   - Ship an empty file (or delete it) to disable dumps entirely — logging stays quiet then.
//   - Example lines:
//         /Matchmaking/player/connection-info
//         /voice/config
//         /econ/customAvatarItems/v1/owned
//
// What we capture:
//   - SendRequest_H (already fires per URL) writes an entry into a per-thread "last URL"
//     slot. Not perfect correlation (async in flight can misattribute), but good enough for
//     bursty debugging on single-flight endpoints like connection-info.
//   - RecNetResponse_H (already fires per completed response) reads the response body as
//     text (DPEENGNNEEB dataAsText field @ +40, an Il2CppString*) and, if the thread's last
//     URL matched a filter, writes  Logs\http-dumps\<ts>_<sanitized>.txt  with status + body.
//
// Cost when the filter file is absent or empty: one map lookup per response, zero disk writes.
namespace HttpDebug {

    static std::vector<std::string>  g_filters;
    static bool                      g_ready = false;
    static std::string               g_dumpDir;    // <dll folder>\Logs\http-dumps
    static CRITICAL_SECTION          g_lastUrlCs;

    // Per-thread "last outbound URL" so a response callback can correlate back to its request
    // without needing a real request->response map (which would need to bridge BestHTTP's
    // HTTPRequest to RecNet's CDHPGPJPLII, and we don't have the layout for that bridge).
    static thread_local std::string  t_lastUrl;

    static void Init(HMODULE selfModule) {
        InitializeCriticalSection(&g_lastUrlCs);

        char dllPath[MAX_PATH] = {};
        if (GetModuleFileNameA(selfModule, dllPath, MAX_PATH) == 0) {
            std::cout << "[Campus/HttpDebug] GetModuleFileNameA failed - HTTP debug disabled\n";
            return;
        }
        char* slash = strrchr(dllPath, '\\');
        if (slash) *slash = 0;
        std::string dllDir = dllPath;

        // Filter file, sitting NEXT TO the DLL (dllDir is already the ...\CampusPatch\Patch
        // folder — earlier version appended \Patch\ again and looked for Patch\Patch\...).
        std::string filterPath = dllDir + "\\http-debug-filter.txt";
        std::ifstream f(filterPath);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                // Strip CR (Windows line endings) and trim.
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                if (line[start] == '#') continue;
                g_filters.push_back(line.substr(start));
            }
        }

        g_dumpDir = dllDir + "\\Logs\\http-dumps";
        CreateDirectoryA((dllDir + "\\Logs").c_str(), nullptr);
        CreateDirectoryA(g_dumpDir.c_str(), nullptr);

        g_ready = true;
        std::cout << TAG_DEBUG() << CLR_BOLD << g_filters.size() << CLR_RESET
                  << " filter(s) loaded from " << CLR_DIM << filterPath << CLR_RESET << "\n";
        if (!g_filters.empty()) {
            std::cout << TAG_DEBUG() << "response bodies -> "
                      << CLR_DIM << g_dumpDir << CLR_RESET << "\n";
            for (const auto& s : g_filters)
                std::cout << TAG_DEBUG() << "  match on: " << CLR_YELLOW << s << CLR_RESET << "\n";
        }
    }

    static bool Matches(const std::string& url) {
        if (!g_ready || g_filters.empty()) return false;
        for (const auto& sub : g_filters)
            if (url.find(sub) != std::string::npos) return true;
        return false;
    }

    // Called by SendRequest_H after URL is stringified (and possibly rewritten).
    static void RecordLastUrl(const std::string& url) {
        t_lastUrl = url;
    }

    // Sanitize URL for use as a filename: keep alnum + [-._], replace others with '_'.
    static std::string SanitizeUrl(const std::string& url) {
        std::string out;
        out.reserve(url.size());
        for (char c : url) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
                out.push_back(c);
            else
                out.push_back('_');
        }
        if (out.size() > 120) out.resize(120);   // filesystem safety
        return out;
    }

    // Write a dump file with URL, status, and body. `body` may be empty if we couldn't read it.
    static void DumpResponse(const std::string& url, int status, const std::string& body) {
        if (!g_ready) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[64];
        _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%04d%02d%02d_%02d%02d%02d_%03d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::string path = g_dumpDir + "\\" + ts + "_" + SanitizeUrl(url) + ".txt";
        std::ofstream out(path, std::ios::binary);
        if (!out) return;
        out << "URL:     " << url    << "\n"
            << "STATUS:  " << status << "\n"
            << "BYTES:   " << body.size() << "\n"
            << "-----BODY-----\n"
            << body;
        out.close();
        std::cout << TAG_DEBUG() << "dumped " << CLR_BOLD << body.size() << "B" << CLR_RESET
                  << " -> " << CLR_DIM << path << CLR_RESET << "\n";
    }

    // MSVC forbids __try in functions that also construct C++ objects with destructors (like
    // std::string), so all SEH-guarded raw pointer reads live in these no-object helpers.
    // Any bad-pointer access from a wrong-offset guess is caught and turned into a null return.
    static void* SafeReadPtr(void* base, size_t off) {
        __try {
            return *(void**)((char*)base + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static int32_t SafeReadInt32(void* base, size_t off) {
        __try {
            return *(int32_t*)((char*)base + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    // Best-effort read of an Il2CppString* at a given offset on `obj`. Uses the SEH-guarded
    // helpers above so the containing function is free of __try and can hold std::string.
    static std::string TryReadStringAt(void* obj, size_t off) {
        void* p = SafeReadPtr(obj, off);
        if (!p) return "";
        // Il2CppString layout: 16-byte header (klass + monitor), then int32 length at +16,
        // then wchar_t data at +20. Sanity-check the length so we don't dump 2GB of noise.
        int32_t len = SafeReadInt32(p, 16);
        if (len <= 0 || len > (1 << 22)) return "";   // reject > 4M chars
        return ReadIl2CppStringOwned((Il2cppString*)p);
    }

    // Probe several candidate offsets on the RecNet response wrapper and write a diagnostic
    // dump showing which slot held what. Once we identify the body slot from a real capture,
    // DumpResponse (above) becomes the surgical version.
    static void DumpResponseProbe(const std::string& url, int status, void* response) {
        if (!g_ready) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[64];
        _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%04d%02d%02d_%02d%02d%02d_%03d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::string s24 = TryReadStringAt(response, 24);
        std::string s40 = TryReadStringAt(response, 40);
        std::string s48 = TryReadStringAt(response, 48);
        std::string s72 = TryReadStringAt(response, 72);

        // NKCJNDJFNDM<byte> at +32 is a variant/span wrapper. First 8 bytes usually point at
        // an internal helper; treat as opaque and log the pointer for grep purposes.
        void* span32 = SafeReadPtr(response, 32);

        std::string path = g_dumpDir + "\\" + ts + "_" + SanitizeUrl(url) + ".txt";
        std::ofstream out(path, std::ios::binary);
        if (!out) return;
        out << "URL:      " << url    << "\n"
            << "STATUS:   " << status << "\n"
            << "-- PROBE (candidate body/text slots on DPEENGNNEEB) --\n"
            << "+24 str (" << s24.size() << " chars): " << s24 << "\n"
            << "+32 span ptr: 0x" << std::hex << (uintptr_t)span32 << std::dec << "\n"
            << "+40 str (" << s40.size() << " chars): " << s40 << "\n"
            << "+48 str (" << s48.size() << " chars): " << s48 << "\n"
            << "+72 str (" << s72.size() << " chars): " << s72 << "\n";
        out.close();
        std::cout << "[Campus/HttpDebug] probed response -> " << path
                  << "  (+24=" << s24.size()
                  << " +40=" << s40.size()
                  << " +48=" << s48.size()
                  << " +72=" << s72.size() << ")\n";
    }

}   // namespace HttpDebug

// Campus server target
// Change these constants if you move to a different server / Photon apps.
static const char* CAMPUS_NAMESERVER_HOST = "gameauth.ddns.net";
static const char* CAMPUS_APP_ID_REALTIME = "3034d59c-49d5-4441-b627-a90683970217";
static const char* CAMPUS_APP_ID_VOICE    = "2f0fac1b-e91a-4271-8e95-8aee7fd67360";
static const char* CAMPUS_APP_ID_CHAT     = "e75ba377-2f03-46a4-a87d-f4fc151913b8";
static const char* CAMPUS_APP_VERSION     = "20250719_prod";

// Referee bypass: NOP the 4 anti-tamper checks so they return immediately. Same technique the
// reference 2025 patch uses. Also reused for TLS NotifyServerCertificate (a bool method - "return"
// with an empty body coerces to false but the caller path doesn't care about the value; what
// matters is that the real check body never runs, so self-signed certs are accepted).
void (*nop_O)();
void nop_H() { return; }

// il2cpp API resolution
// On protected (Referee) builds GameAssembly.dll's export table is stripped, so the reference
// patch's plain GetProcAddress path returns null and every downstream il2cpp_* call blows up.
// We fall back to scanning UnityPlayer.dll's decrypted code for the standard
// `lea reg, "il2cpp_name"; ... mov [rip+slot], rax` pattern - same technique the Puck Dumper
// uses to resolve il2cpp APIs on this exact build. Runs off already-decrypted memory; no exports
// required.
namespace UnityPlayerScan {
    static void* find_il2cpp_api(const char* name) {
        HMODULE up = GetModuleHandleA("UnityPlayer.dll");
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!up || !ga) return nullptr;
        auto upBase = (uint8_t*)up;
        auto dos = (PIMAGE_DOS_HEADER)upBase;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto nt = (PIMAGE_NT_HEADERS64)(upBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const uint8_t* modEnd = upBase + nt->OptionalHeader.SizeOfImage;

        auto gaDos = (PIMAGE_DOS_HEADER)ga;
        auto gaNt  = (PIMAGE_NT_HEADERS64)((uint8_t*)ga + gaDos->e_lfanew);
        const uint8_t* gaBase = (uint8_t*)ga;
        const uint8_t* gaEnd  = gaBase + gaNt->OptionalHeader.SizeOfImage;

        size_t nameLen = strlen(name);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++) {
            if (!(sec[s].Characteristics & IMAGE_SCN_CNT_CODE)) continue;
            uint8_t* code = upBase + sec[s].VirtualAddress;
            size_t codeSize = sec[s].Misc.VirtualSize;
            if (code + codeSize > modEnd) codeSize = (size_t)(modEnd - code);

            for (size_t off = 0; off + 7 < codeSize; off++) {
                uint8_t* p = code + off;
                // lea r64, [rip+disp32]  (REX.W/REX.WR + 8D + modrm mod=00 rm=101)
                if (!((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05)) continue;
                int32_t disp = *(int32_t*)(p + 3);
                uint8_t* target = p + 7 + disp;
                if (target < upBase || target + nameLen + 1 >= modEnd) continue;
                if (memcmp(target, name, nameLen) != 0 || target[nameLen] != 0) continue;

                // Next store-to-global (mov [rip+d], rax) within a short window is the slot.
                for (size_t k = 7; k < 128 && off + k + 7 < codeSize; k++) {
                    uint8_t* q = p + k;
                    if (q[0] == 0x48 && q[1] == 0x89 && (q[2] & 0xC7) == 0x05) {
                        int32_t d2 = *(int32_t*)(q + 3);
                        uint8_t* slot = q + 7 + d2;
                        if (slot >= upBase && slot + 8 <= modEnd) {
                            void* fn = *(void**)slot;
                            // Sanity: resolved pointer must land INSIDE GameAssembly.dll.
                            if (fn && (uint8_t*)fn >= gaBase && (uint8_t*)fn < gaEnd)
                                return fn;
                        }
                        break;
                    }
                }
            }
        }
        return nullptr;
    }
}

// SendRequest hook: rewrite ns.rec.net -> CAMPUS_NAMESERVER_HOST 
// Direct port of the reference 2025 patch's URL redirect, just retargeted at our host instead of
// the reference's ns.cgb.net. HTTPRequest carries the URI on the object at +0x10; we read it,
// stringify it, and if it references ns.rec.net we build a new Uri object with the swapped host
// and swap it into the request before the real SendRequest runs.
void (*SendRequest_O)(void*);
void SendRequest_H(void* request) {
    void* uri = read<void*>(request, 16);
    if (uri) {
        using ToStringFn = Il2cppString * (*)(void*);
        auto toStringFn = reinterpret_cast<ToStringFn>(GA + RR::Methods::System::Uri::ToString);

        // ONE toString per request. The previous version called this twice (once to inspect,
        // once for the debug print), doubling IL2CPP string allocations on every HTTP request.
        Il2cppString* Uri = spoof_call(RetAddr, toStringFn, uri);
        std::string url = ReadIl2CppStringOwned(Uri);

        const std::string find = "ns.rec.net";
        bool rewritten = false;
        auto findPos = url.find(find);
        if (findPos != std::string::npos) {
            url.replace(findPos, find.length(), CAMPUS_NAMESERVER_HOST);

            using Il2cppStringNewFn = Il2cppString * (*)(const char*);
            auto strNew = reinterpret_cast<Il2cppStringNewFn>(GA + RR::Methods::Il2cpp::il2cpp_string_new);
            Il2cppString* NewStr = spoof_call(RetAddr, strNew, url.c_str());

            using ObjectGetClassFn = void * (*)(void*);
            auto getClass = reinterpret_cast<ObjectGetClassFn>(GA + RR::Methods::Il2cpp::il2cpp_object_get_class);
            void* UriClass = spoof_call(RetAddr, getClass, uri);

            using ObjectNewFn = void* (*)(void*);
            auto objNew = reinterpret_cast<ObjectNewFn>(GA + RR::Methods::Il2cpp::il2cpp_object_new);
            void* NewUri = spoof_call(RetAddr, objNew, UriClass);

            if (NewUri && NewStr) {
                using UriCtor = void (*)(void*, Il2cppString*);
                auto uriCtor = reinterpret_cast<UriCtor>(GA + RR::Methods::System::Uri::ctor);
                spoof_call(RetAddr, uriCtor, NewUri, NewStr);
                set<void*>(request, 16, NewUri);
                rewritten = true;
            }
        }
        // HTTP-layer hile suppressor - checked BEFORE the `Uri: <url>` print so the console
        // shows a red DROPPED tag instead of a misleading Uri line for a request that never
        // actually leaves the process.
        if (url.find("/api/PlayerReporting/v1/hile")   != std::string::npos ||
            url.find("/api/PlayerReporting/v1/referee")!= std::string::npos)
        {
            std::cout << TAG_HILE() << "DROPPED " << CLR_DIM << url << CLR_RESET << "\n";
            return;
        }

        // Log the URL. Status is logged separately by RecNetResponse_H — the console shows
        //   [NET]  Uri: <url>
        //   [HTTP] <- HTTP <status>
        // for the same request. Pairing them into one line would need a request-pointer -> URL
        // side map, which is more surface than it's worth.
        std::cout << TAG_NET() << url;
        if (rewritten) std::cout << CLR_YELLOW " (rewritten)" CLR_RESET;
        std::cout << "\n";

        // Record the URL for RecNetResponse_H's dump correlation (see HttpDebug namespace).
        HttpDebug::RecordLastUrl(url);
    }
    return SendRequest_O(request);
}

//  RecNet HTTP response hook: log HTTP status per completed request
// Signature confirmed against DUMPER-7 (CBACIMLIBPF.h line 1361):
//   static void CLCAHKCJDLC(CDHPGPJPLII* request, DPEENGNNEEB* response, JFNOOGODOOA* promise)
// It IS a static method (no `this` arg) — the 3-arg raw signature matches perfectly.
//
// IMPORTANT: `CDHPGPJPLII` is RecNet's own request wrapper, NOT BestHTTP's `HTTPRequest`.
// Its offset 16 is a `bool`, NOT a `Uri*`. The previous version of this hook tried to read
// `request+16` as a Uri pointer, got garbage, called Uri.ToString on it, and black-screened
// the client on the first RecNet response. We now touch ONLY the response object, whose
// layout IS confirmed (DPEENGNNEEB.h: `int32_t backingField__IKBENAMGBFE` at offset 16 =
// the HTTP status code the ctor stores).
void (*RecNetResponse_O)(void*, void*, void*);
void RecNetResponse_H(void* request, void* response, void* promise) {
    if (response) {
        int status = read<int>(response, 16);
        // Colour the status code by class: 2xx green, 3xx cyan, 4xx orange, 5xx red.
        const char* col = CLR_GREEN;
        if      (status >= 500) col = CLR_RED;
        else if (status >= 400) col = CLR_ORANGE;
        else if (status >= 300) col = CLR_CYAN;
        std::cout << TAG_HTTP() << col << std::dec << status << CLR_RESET << "\n";
    }
    return RecNetResponse_O(request, response, promise);
}

// Gradient recolor: Rec Room orange -> Campus purple/magenta/orange/gold ramp
// Direct native port of CampusPatch (2023)'s RRUIGradientPatch: intercept UIGradient.set_Gradient,
// walk the incoming Unity Gradient's color keys, and if ANY key looks like Rec Room orange,
// replace ALL keys with the Campus 4-stop ramp (times preserved so the gradient's shape stays).
//
// UnityEngine.Gradient stores its actual key data on the C++ side behind m_Ptr; managed
// get_colorKeys hands back a fresh copy each call, so mutating that copy is invisible until we
// push it back via SetKeys.
namespace CampusGradient {
    struct Color { float r, g, b, a; };

    // 4 stops sampled from the admin panel's Background.png so recolored UI reads as the same
    // visual palette Puck / the dashboard uses.
    static constexpr Color Purple  { 0.475f, 0.106f, 0.553f, 1.0f }; // #791B8D
    static constexpr Color Magenta { 0.749f, 0.161f, 0.400f, 1.0f }; // #BF2966
    static constexpr Color Orange  { 0.976f, 0.365f, 0.141f, 1.0f }; // #F95D24
    static constexpr Color Gold    { 0.980f, 0.600f, 0.071f, 1.0f }; // #FA9912

    static Color rampAt(float t) {
        const Color stops[4] = { Purple, Magenta, Orange, Gold };
        if (t <= 0.0f) return stops[0];
        if (t >= 1.0f) return stops[3];
        float f = t * 3.0f;
        int   i = (int)f;
        float u = f - (float)i;
        return Color{
            stops[i].r * (1.0f - u) + stops[i+1].r * u,
            stops[i].g * (1.0f - u) + stops[i+1].g * u,
            stops[i].b * (1.0f - u) + stops[i+1].b * u,
            1.0f,
        };
    }

    static void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
        float mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
        float mn = r; if (g < mn) mn = g; if (b < mn) mn = b;
        v = mx;
        float d = mx - mn;
        s = (mx == 0.0f) ? 0.0f : d / mx;
        if (d == 0.0f) { h = 0.0f; return; }
        if      (mx == r) h = fmodf((g - b) / d, 6.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }

    // Two-branch HSV window matching bright Rec Room orange and the lighter yellow-orange used
    // in secondary UI sweeps.
    static bool isRecRoomOrange(float r, float g, float b) {
        float h, s, v;
        rgbToHsv(r, g, b, h, s, v);
        return
            (h >= 0.02f && h <= 0.07f && s >= 0.70f && v >= 0.60f) ||
            (h >= 0.05f && h <= 0.10f && s >= 0.45f && s <= 0.80f && v >= 0.85f);
    }
}

void (*UIGradient_set_Gradient_O)(void*, void*);
void UIGradient_set_Gradient_H(void* self, void* gradient) {
    if (gradient) {
        using GetKeysFn = void*(*)(void*);
        using SetKeysFn = void (*)(void*, void*, void*);
        auto getColorKeys = (GetKeysFn)(GA + RR::Methods::UnityGradient::get_colorKeys);
        auto getAlphaKeys = (GetKeysFn)(GA + RR::Methods::UnityGradient::get_alphaKeys);
        auto setKeys      = (SetKeysFn)(GA + RR::Methods::UnityGradient::SetKeys);

        void* colorKeys = spoof_call(RetAddr, getColorKeys, gradient);
        if (colorKeys) {
            // Il2Cpp single-dim value-type array: length at +24, packed data at +32.
            // GradientColorKey = { Color color(16B), float time(4B) } = 20 bytes.
            uint64_t len = read<uint64_t>(colorKeys, 24);
            if (len > 0 && len <= 32) {
                uint8_t* keyData = (uint8_t*)colorKeys + 32;
                constexpr size_t STRIDE = 20;

                bool looksOrange = false;
                for (uint64_t i = 0; i < len; i++) {
                    float r = *(float*)(keyData + i * STRIDE + 0);
                    float g = *(float*)(keyData + i * STRIDE + 4);
                    float b = *(float*)(keyData + i * STRIDE + 8);
                    if (CampusGradient::isRecRoomOrange(r, g, b)) { looksOrange = true; break; }
                }

                if (looksOrange) {
                    void* alphaKeys = spoof_call(RetAddr, getAlphaKeys, gradient);
                    for (uint64_t i = 0; i < len; i++) {
                        float t = (len > 1) ? (float)i / (float)(len - 1) : 0.5f;
                        auto c = CampusGradient::rampAt(t);
                        *(float*)(keyData + i * STRIDE + 0)  = c.r;
                        *(float*)(keyData + i * STRIDE + 4)  = c.g;
                        *(float*)(keyData + i * STRIDE + 8)  = c.b;
                        *(float*)(keyData + i * STRIDE + 12) = c.a;
                    }
                    spoof_call(RetAddr, setKeys, gradient, colorKeys, alphaKeys);
                }
            }
        }
    }
    UIGradient_set_Gradient_O(self, gradient);
}

// Photon connect hooks: rewrite AppSettings identity to Campus values on the way through
// Each connect entry point takes (this, AppSettings*, [maybe extra args]). We mutate AppSettings'
// string fields BEFORE the real method runs (Prefix-style behaviour), so the game connects to
// OUR Photon apps and version. This is the native equivalent of CampusPatch (2023)'s
// PhotonAppSettingsPatch Postfix that overwrote AppIdRealtime / AppIdVoice / AppVersion.
// The previous implementation allocated a fresh IL2CPP string for realtime/voice/version on
// EVERY Photon connect (matchmaking, room join, voice channel switch, reconnects). The GC does
// clean these up eventually, but under a busy session they piled up and showed as a memory
// climb. Allocate once, cache forever, and short-circuit if AppSettings already points at our
// cached strings.
static Il2cppString* g_campusAppIdRealtime = nullptr;
static Il2cppString* g_campusAppIdVoice    = nullptr;
static Il2cppString* g_campusAppIdChat     = nullptr;
static Il2cppString* g_campusAppVersion    = nullptr;

// The four Photon connect hooks (Realtime / PUN / Voice / VoicePUN) can fire on independent
// threads simultaneously. Without a guard, two threads could each see all four globals as null,
// each call il2cpp_string_new four times, and orphan the four losers of the race.
static std::once_flag g_campusPhotonStringsOnce;

static void EnsureCampusPhotonStrings()
{
    std::call_once(g_campusPhotonStringsOnce, []{
        if (!RR::Methods::Il2cpp::il2cpp_string_new) return;

        using Il2cppStringNewFn = Il2cppString * (*)(const char*);
        auto strNew = reinterpret_cast<Il2cppStringNewFn>(GA + RR::Methods::Il2cpp::il2cpp_string_new);

        g_campusAppIdRealtime = spoof_call(RetAddr, strNew, CAMPUS_APP_ID_REALTIME);
        g_campusAppIdVoice    = spoof_call(RetAddr, strNew, CAMPUS_APP_ID_VOICE);
        g_campusAppIdChat     = spoof_call(RetAddr, strNew, CAMPUS_APP_ID_CHAT);
        g_campusAppVersion    = spoof_call(RetAddr, strNew, CAMPUS_APP_VERSION);
    });
}

static void RewriteAppSettings(void* appSettings)
{
    if (!appSettings) return;
    EnsureCampusPhotonStrings();
    if (!g_campusAppIdRealtime) {
        std::cout << "[Campus/Photon] AppSettings NOT rewritten - il2cpp_string_new not resolved\n";
        return;
    }

    // Skip the write when AppSettings already points at our cached strings — cuts out a bunch
    // of __TypeStore / write-barrier work on hot Photon reconnect paths.
    void* curRt   = read<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdRealtime);
    void* curVc   = read<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdVoice);
    void* curChat = read<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdChat);
    void* curVer  = read<void*>(appSettings, RR::Methods::AppSettingsFields::AppVersion);
    if (curRt   == g_campusAppIdRealtime &&
        curVc   == g_campusAppIdVoice    &&
        curChat == g_campusAppIdChat     &&
        curVer  == g_campusAppVersion) {
        return; // already redirected — nothing to do
    }

    if (g_campusAppIdRealtime) set<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdRealtime, g_campusAppIdRealtime);
    if (g_campusAppIdVoice)    set<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdVoice,    g_campusAppIdVoice);
    if (g_campusAppIdChat)     set<void*>(appSettings, RR::Methods::AppSettingsFields::AppIdChat,     g_campusAppIdChat);
    if (g_campusAppVersion)    set<void*>(appSettings, RR::Methods::AppSettingsFields::AppVersion,    g_campusAppVersion);

    std::cout << TAG_PHOTON() << "AppSettings redirected -> "
              << CLR_YELLOW << CAMPUS_APP_ID_REALTIME << CLR_RESET
              << CLR_DIM " ver " CLR_RESET << CAMPUS_APP_VERSION << "\n";
}

// Universal HTTP response body capture (fires for EVERY response that any caller reads
// as text, unlike RecNetResponse_H which only fires for RecNet's own callback path).
//
// Hooks BestHTTP.HTTPResponse.get_DataAsText. Layout of `self` (HTTPResponse):
//   +24  int32              StatusCode
//   +72  Array<byte>*       Data          (raw response bytes)
//   +80  int32              DataLength
//   +96  Il2CppString*      dataAsText    (cached decoded text)
//   +120 HTTPRequest*       baseRequest   (has Uri* at +16)
//
// We call the original first so the returned string is real, then log it against the URL
// we extract from baseRequest->Uri via System::Uri::ToString (already resolved earlier for
// SendRequest_H's URL rewriter).
//
// Two guards keep this cheap:
//   1) Re-entry guard: we call the same il2cpp method inside the hook body, and it might
//      internally trigger another get_DataAsText somewhere — thread-local flag stops loops.
//   2) URL-filter check: if the URL doesn't match the filter list, we just call original
//      and return. Zero I/O in that path.
// Small helper: walk HTTPResponse* -> baseRequest -> Uri -> ToString(). Used by both the
// get_DataAsText and Receive hooks below. Returns "(unknown url)" on any read failure so we
// still write a dump file (body + status) rather than skipping entirely.
static std::string HttpResponseUrl(void* httpResponse) {
    if (!httpResponse) return "(null response)";
    void* baseRequest = HttpDebug::SafeReadPtr(httpResponse, 120);
    if (!baseRequest) return "(unknown url)";
    void* uri = HttpDebug::SafeReadPtr(baseRequest, 16);
    if (!uri) return "(unknown url)";
    using ToStringFn = Il2cppString * (*)(void*);
    auto toStringFn = reinterpret_cast<ToStringFn>(GA + RR::Methods::System::Uri::ToString);
    Il2cppString* uriStr = spoof_call(RetAddr, toStringFn, uri);
    return uriStr ? ReadIl2CppStringOwned(uriStr) : "(unknown url)";
}

// BestHTTP.HTTPResponse.Receive — the low-level body reader that BestHTTP calls exactly
// once per response once the payload is on the wire. Fires UNCONDITIONALLY, unlike
// get_DataAsText which the caller might skip if they only want raw bytes. After the
// original returns true, self->Data (byte[]* at +72) and self->DataLength (int32 at +80)
// are populated with the response body.
//
// Il2Cpp Array<T> layout: [16-byte header (klass, monitor)][8-byte bounds ptr][8-byte
// length int64][data T[]]. For a byte[], data starts at +32 from the array base.
thread_local bool g_inReceive = false;
bool (*Receive_O)(void*, int32_t, bool);
bool Receive_H(void* self, int32_t forceReadRawContentLength, bool readPayloadData) {
    bool ok = Receive_O(self, forceReadRawContentLength, readPayloadData);
    if (!ok || !readPayloadData || g_inReceive) return ok;
    g_inReceive = true;

    std::string url = HttpResponseUrl(self);
    if (HttpDebug::Matches(url)) {
        int32_t status = HttpDebug::SafeReadInt32(self, 24);
        int32_t len    = HttpDebug::SafeReadInt32(self, 80);
        void*   data   = HttpDebug::SafeReadPtr  (self, 72);

        std::string body;
        if (data && len > 0 && len < (32 << 20)) {   // cap at 32MB safety
            // byte[] payload starts at +32 in the Il2Cpp array header layout.
            const char* p = reinterpret_cast<const char*>(data) + 32;
            body.assign(p, (size_t)len);
        }
        HttpDebug::DumpResponse(url, status, body);
    }

    g_inReceive = false;
    return ok;
}

thread_local bool g_inGetDataAsText = false;
Il2cppString* (*GetDataAsText_O)(void*);
Il2cppString* GetDataAsText_H(void* self) {
    // Always call the original first — its return value is what the caller expects.
    Il2cppString* result = GetDataAsText_O(self);

    // Re-entry / null / non-ready guards.
    if (g_inGetDataAsText || !self || !result) return result;
    g_inGetDataAsText = true;

    std::string url = HttpResponseUrl(self);
    if (HttpDebug::Matches(url)) {
        int status = HttpDebug::SafeReadInt32(self, 24);
        std::string body = ReadIl2CppStringOwned(result);
        HttpDebug::DumpResponse(url, status, body);
    }

    g_inGetDataAsText = false;
    return result;
}

// Hile-suppression trial hooks — ALL REMOVED. The five candidates in the previous
// iteration crashed the client on launch. Two proven culprits are documented in
// Methods.h (do-not-hook table); the other three might be safe individually but we
// don't know which broke it, so all are pulled until we can re-introduce them one at
// a time with call-original + log-only semantics (not nop).
//
// The HTTP-layer suppressor in SendRequest_H remains the only hile bypass and is
// guaranteed to work — no /api/PlayerReporting/v1/{hile,referee} POST leaves the
// process regardless of which in-process path enqueued it.

// Rec Room -> Campus logo swap (SCAFFOLDING, not yet installed)
// -----------------------------------------------------------------------------------------
// StreamingAssets/aa/StandaloneWindows64/*.bundle are Unity Addressables asset bundles - the
// Rec Room brand logo textures live inside those bundles, not on disk as loose PNGs, so a
// file-swap approach requires UABE/AssetStudio to repack the bundle with the matching Unity
// version. That's out of scope for a runtime DLL.
//
// The RUNTIME-safe approach is: hook UnityEngine.UI.Image.set_sprite (and/or
// UnityEngine.UI.RawImage.set_texture / SpriteRenderer.set_sprite), read the incoming
// Texture2D's name via UnityEngine.Object.get_name, and when the name matches a Rec Room
// logo (see LOGO_NAME_PREFIXES below), swap in a Texture2D loaded from a PNG next to the
// DLL (CampusPatch\\Assets\\CampusLogo.png -> Texture2D.LoadImage byte[]).
//
// This build's runtime dump does NOT include UnityEngine.UI (only Assembly-CSharp + a
// handful of RecRoom/Photon modules), so the RVAs for set_sprite / set_texture / LoadImage
// can't be pinned from static analysis alone. To wire this up you'll need to either:
//   (a) run an il2cpp_dumper pass that includes UnityEngine.UI + UnityEngine.CoreModule, OR
//   (b) pattern-scan GameAssembly.dll for the set_sprite prologue using a known-good byte
//       signature (grab one from a public il2cpp dump of the same Unity version).
//
// When those RVAs are available, add them to Methods.h under a new namespace
// RR::Methods::UnityUI and enable the hooks in Patch(). Leaving the plumbing here as a
// TODO marker so the next pass has one obvious place to land.
namespace CampusLogoSwap {
    static const char* LOGO_NAME_PREFIXES[] = {
        "RecRoomLogo", "RRLogo", "recroom_logo", "brand_logo", "Logo_RecRoom",
        "logo_rec_room", "MainLogo", "Splash_Logo",
        nullptr
    };
    // Cached campus logo Texture2D once loaded from disk; nullptr until first successful load.
    // static void* g_campusLogoTex = nullptr;
    // static void* LoadCampusLogoTexture() { /* Texture2D.LoadImage(File.ReadAllBytes(...)) */ }
}

bool (*RealtimeConnect_O)(void*, void*);
bool RealtimeConnect_H(void* self, void* appSettings) {
    RewriteAppSettings(appSettings);
    return RealtimeConnect_O(self, appSettings);
}

bool (*PunConnect_O)(void*, void*, bool);
bool PunConnect_H(void* self, void* appSettings, bool arg) {
    RewriteAppSettings(appSettings);
    return PunConnect_O(self, appSettings, arg);
}

bool (*VoiceConnect_O)(void*, void*);
bool VoiceConnect_H(void* self, void* appSettings) {
    RewriteAppSettings(appSettings);
    return VoiceConnect_O(self, appSettings);
}

bool (*VoicePUNConnect_O)(void*, void*);
bool VoicePUNConnect_H(void* self, void* appSettings) {
    RewriteAppSettings(appSettings);
    return VoicePUNConnect_O(self, appSettings);
}

//Patch installer
namespace RR::Patches {
    void Resolve() {
        RetAddr = PatternScan<uint64_t*>("FF 23", GA, NtHeaders->OptionalHeader.SizeOfImage);
        std::cout << "[Campus] RetSpoof gadget (FF 23): 0x" << std::hex << (uintptr_t)RetAddr << "\n";
        auto assm = GetModuleHandle("GameAssembly.dll");

        // 1) Try GetProcAddress first (works on unpacked builds).
        auto tryExport = [&](const char* name) -> uintptr_t {
            void* p = spoof_call(RetAddr, GetProcAddress, assm, name);
            return p ? (uintptr_t)p - GA : 0;
        };
        RR::Methods::Il2cpp::il2cpp_string_new       = tryExport("il2cpp_string_new");
        RR::Methods::Il2cpp::il2cpp_object_get_class = tryExport("il2cpp_object_get_class");
        RR::Methods::Il2cpp::il2cpp_object_new       = tryExport("il2cpp_object_new");

        // 2) Fall back to UnityPlayer scan when exports are stripped (Referee'd builds).
        auto fallback = [&](const char* name, uintptr_t& slot) {
            if (slot) return;
            void* p = UnityPlayerScan::find_il2cpp_api(name);
            if (p) slot = (uintptr_t)p - GA;
        };
        fallback("il2cpp_string_new",       RR::Methods::Il2cpp::il2cpp_string_new);
        fallback("il2cpp_object_get_class", RR::Methods::Il2cpp::il2cpp_object_get_class);
        fallback("il2cpp_object_new",       RR::Methods::Il2cpp::il2cpp_object_new);

        auto label = [](uintptr_t v) { return v ? "OK  " : "FAIL"; };
        std::cout << "[Campus] " << label(RR::Methods::Il2cpp::il2cpp_string_new)
                  << " il2cpp_string_new       -> 0x" << std::hex << RR::Methods::Il2cpp::il2cpp_string_new << "\n";
        std::cout << "[Campus] " << label(RR::Methods::Il2cpp::il2cpp_object_get_class)
                  << " il2cpp_object_get_class -> 0x" << std::hex << RR::Methods::Il2cpp::il2cpp_object_get_class << "\n";
        std::cout << "[Campus] " << label(RR::Methods::Il2cpp::il2cpp_object_new)
                  << " il2cpp_object_new       -> 0x" << std::hex << RR::Methods::Il2cpp::il2cpp_object_new << "\n";
    }

    static void hook(uintptr_t abs, void* detour, void** orig, const char* label) {
        MH_STATUS s = MH_CreateHook((void*)abs, detour, (LPVOID*)orig);
        if (s == MH_OK) s = MH_EnableHook((void*)abs);
        std::cout << TAG_PATCH() << label
                  << CLR_DIM  " @ 0x" << std::hex << abs << CLR_RESET
                  << "  -> " << (s == MH_OK ? CLR_GREEN "OK" CLR_RESET
                                            : CLR_RED   "FAIL" CLR_RESET) << "\n";
    }

    void Patch() {
        if (MH_Initialize() != MH_OK) {
            std::cout << "[Campus] MH_Initialize FAILED\n";
            return;
        }

        // Referee bypass (must run before anything else calls into libil2cpp under Referee's watch).
        hook(Referee + RR::Methods::Referee::Check1, (void*)nop_H, (void**)&nop_O, "Referee Check1");
        hook(Referee + RR::Methods::Referee::Check2, (void*)nop_H, (void**)&nop_O, "Referee Check2");
        hook(Referee + RR::Methods::Referee::Check3, (void*)nop_H, (void**)&nop_O, "Referee Check3");
        hook(Referee + RR::Methods::Referee::Check4, (void*)nop_H, (void**)&nop_O, "Referee Check4");

        // Accept any TLS cert (private server uses self-signed).
        hook(GA + RR::Methods::LegacyTlsAuthentication::NotifyServerCertificate,
             (void*)nop_H, (void**)&nop_O, "TLS NotifyServerCertificate");

        // URI rewrite (ns.rec.net -> CAMPUS_NAMESERVER_HOST).
        hook(GA + RR::Methods::HTTPManager::SendRequest,
             (void*)SendRequest_H, (void**)&SendRequest_O, "HTTPManager.SendRequest");

        // RecNet response callback — logs "  <- HTTP <status>" per completed request. Pairs
        // visually with the SendRequest URL log line above.
        hook(GA + RR::Methods::RecNetHttp::ResponseCallback,
             (void*)RecNetResponse_H, (void**)&RecNetResponse_O, "RecNet response callback (status)");

        // HTTPResponse hooks disabled. Enabling get_DataAsText + Receive appeared to break
        // watch-menu room loading (client uses byte-array response consumers that our hook
        // sat in the middle of). And since the reverted string reader made body dumps 0B
        // anyway, we lose zero debug utility by not installing them. Keep the code around
        // for a future re-attempt once we have a safer response-capture path (probably
        // reading raw byte[] via a direct SEH-guarded copy, not via Il2CppString).

        // Gradient recolor: Rec Room orange -> Campus purple/magenta/orange/gold ramp.
        hook(GA + RR::Methods::UIGradient::set_Gradient,
             (void*)UIGradient_set_Gradient_H, (void**)&UIGradient_set_Gradient_O,
             "UIGradient.set_Gradient (gradient recolor)");

        // In-process hile hooks: NONE (see comment block above the removed hook bodies).
        // Suppression is at the HTTP layer only, in SendRequest_H, and known-good.

        // Photon redirect (all four connect-with-AppSettings entry points).
        hook(GA + RR::Methods::Photon::OLPEILEPEAD_JBNCMFDFDLM,
             (void*)RealtimeConnect_H, (void**)&RealtimeConnect_O, "Photon Realtime connect");
        hook(GA + RR::Methods::Photon::OEOMKFKDCCG_JBNCMFDFDLM,
             (void*)PunConnect_H, (void**)&PunConnect_O, "Photon PUN connect");
        hook(GA + RR::Methods::Photon::VoiceConnection_ConnectUsingSettings,
             (void*)VoiceConnect_H, (void**)&VoiceConnect_O, "Photon Voice connect");
        hook(GA + RR::Methods::Photon::PhotonVoiceNetwork_ConnectUsingSettings,
             (void*)VoicePUNConnect_H, (void**)&VoicePUNConnect_O, "Photon Voice PUN connect");

        std::cout << "[Campus] all hooks installed. target = " << CAMPUS_NAMESERVER_HOST << "\n";
    }
}

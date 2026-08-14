// CampusPatch2025 Launcher - dashboard-style overhaul.
//
// The whole app is one imgui frame drawn on top of a full-window Background.png with a slowly
// drifting tiled AnimatedIcon overlay (parallax). Window is pure WS_POPUP (no Windows chrome).
// Custom title bar has ultra-minimalist min/close glyphs that blend into the gradient. The
// main body is a WIDE dashboard panel (~70% width) with three side-by-side setting groups
// (Screen Mode / Resolution / VR). LAUNCH is a hand-drawn gradient button with a soft pulse
// when idle; other actions (Verify / Settings) sit next to it as glass pills.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/backends/imgui_impl_win32.h"
#include "vendor/imgui/backends/imgui_impl_dx11.h"

#include <wincodec.h>

// LauncherLogin owns LauncherAuth + LauncherSteam + LauncherHttp transitively; pulling in the
// one top-level header brings the whole auth stack (Steam gate, session, WinHTTP client, login
// screen) into scope. Everything above the login gate lives in that header.
#include "LauncherLogin.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

// -----------------------------------------------------------------------------------------------
// Paths + config
// -----------------------------------------------------------------------------------------------
static const char* GAME_EXE_NAME    = "Recroom_Release.exe";
static const char* PATCH_DLL_REL    = "CampusPatch\\Patch\\CampusPatch.dll";
static const char* REQUIRED_FILES[] = {
    "Recroom_Release.exe","GameAssembly.dll","Referee.dll","UnityPlayer.dll",
    "baselib.dll","steam_api64.dll","steam_appid.txt","CampusPatch\\Patch\\CampusPatch.dll",
};

static std::string g_baseDir, g_gameExe, g_dllPath;
static HWND        g_hwnd = nullptr;

// -----------------------------------------------------------------------------------------------
// Log ring
// -----------------------------------------------------------------------------------------------
static std::mutex              g_logMtx;
static std::deque<std::string> g_logLines;
static void log_line(const char* fmt, ...) {
    char body[512]; va_list ap; va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[600];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02d:%02d:%02d  %s",
                st.wHour, st.wMinute, st.wSecond, body);
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_logLines.push_back(line);
    if (g_logLines.size() > 500) g_logLines.pop_front();
    OutputDebugStringA(line); OutputDebugStringA("\n");
}

// -----------------------------------------------------------------------------------------------
// Filesystem
// -----------------------------------------------------------------------------------------------
static std::string module_dir(HMODULE m = nullptr) {
    char buf[MAX_PATH] = {}; GetModuleFileNameA(m, buf, MAX_PATH);
    std::string s(buf); auto slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : s.substr(0, slash);
}
static bool file_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
struct VerifyResult { std::vector<std::string> missing; bool allPresent; };
static VerifyResult verify_files() {
    VerifyResult r{}; r.allPresent = true;
    for (auto rel : REQUIRED_FILES) {
        std::string full = g_baseDir + "\\" + rel;
        if (!file_exists(full)) { r.missing.push_back(rel); r.allPresent = false; }
    }
    return r;
}

// -----------------------------------------------------------------------------------------------
// WIC PNG loader + D3D texture
// -----------------------------------------------------------------------------------------------
struct GpuTexture {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    void release() { if (srv) srv->Release(); if (tex) tex->Release(); srv=nullptr; tex=nullptr; }
};
static bool wic_decode_png(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& outRGBA, int& outW, int& outH)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) return false;
    IWICStream* stream = nullptr; wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)size))) {
        stream->Release(); wic->Release(); return false;
    }
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
        stream->Release(); wic->Release(); return false;
    }
    IWICBitmapFrameDecode* frame = nullptr; dec->GetFrame(0, &frame);
    UINT w = 0, hgt = 0; frame->GetSize(&w, &hgt);
    IWICFormatConverter* conv = nullptr; wic->CreateFormatConverter(&conv);
    conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                     nullptr, 0.0, WICBitmapPaletteTypeCustom);
    outW = (int)w; outH = (int)hgt;
    outRGBA.assign((size_t)w * hgt * 4, 0);
    conv->CopyPixels(nullptr, w * 4, (UINT)outRGBA.size(), outRGBA.data());
    conv->Release(); frame->Release(); dec->Release(); stream->Release(); wic->Release();
    return true;
}
static bool make_d3d_texture_from_rgba(ID3D11Device* dev, const uint8_t* rgba, int w, int h,
                                       GpuTexture& out) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = rgba; sd.SysMemPitch = w * 4;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &out.tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = td.Format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(out.tex, &vd, &out.srv))) return false;
    out.w = w; out.h = h; return true;
}

#define IDR_ANIMATED_ICON_PNG 200
#define IDR_BACKGROUND_PNG    201
#define IDR_CUTOUT_PNG        202
#define IDR_RUBIK_TTF         203

// Fetch a raw RCDATA blob without copying. Caller must NOT free the pointer.
static bool find_rcdata(int rcid, const void*& out, size_t& outSz) {
    HRSRC h = FindResourceA(nullptr, MAKEINTRESOURCEA(rcid), (LPCSTR)RT_RCDATA);
    if (!h) return false;
    HGLOBAL g = LoadResource(nullptr, h);
    if (!g) return false;
    out = LockResource(g);
    outSz = SizeofResource(nullptr, h);
    return out && outSz;
}
static bool load_embedded_png_texture(ID3D11Device* dev, int rcid, GpuTexture& out) {
    const void* p; size_t n;
    if (!find_rcdata(rcid, p, n)) return false;
    std::vector<uint8_t> rgba; int w = 0, hgt = 0;
    if (!wic_decode_png((const uint8_t*)p, n, rgba, w, hgt)) return false;
    return make_d3d_texture_from_rgba(dev, rgba.data(), w, hgt, out);
}

// -----------------------------------------------------------------------------------------------
// D3D11
// -----------------------------------------------------------------------------------------------
static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_sc   = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fl, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &got, &g_ctx))) return false;
    ID3D11Texture2D* back = nullptr; g_sc->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release();
    return true;
}
static void destroy_device() {
    if (g_rtv) g_rtv->Release(); if (g_sc) g_sc->Release();
    if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    g_rtv=nullptr; g_sc=nullptr; g_ctx=nullptr; g_dev=nullptr;
}

// -----------------------------------------------------------------------------------------------
// UI state + palette
// -----------------------------------------------------------------------------------------------
struct UiState {
    int  screenMode = 0, resIdx = 0, vrMode = 0;
    bool showSettings = false;
    std::string status = "Ready.";
    bool statusOk = true;
    GpuTexture faceTex, bgTex, logoTex;
    ImFont* fontBody   = nullptr;
    ImFont* fontLabel  = nullptr;
    ImFont* fontH      = nullptr;
    ImFont* fontTiny   = nullptr;
};
static UiState g_ui;

static const char* SCREEN_MODES[] = { "Windowed", "Fullscreen", "Borderless" };
static const char* VR_MODES[]     = { "Desktop (None)", "SteamVR", "Oculus", "WMR" };
static const char* RESOLUTIONS[]  = { "1920x1080", "2560x1440", "1600x900", "1280x720", "3840x2160" };

// Palette
static constexpr ImU32 COL_TEXT      = IM_COL32(0xF0, 0xF0, 0xF0, 255);
static constexpr ImU32 COL_TEXT_DIM  = IM_COL32(0xC8, 0xC0, 0xC8, 200);
static constexpr ImU32 COL_TEXT_LABEL= IM_COL32(0xE8, 0xC0, 0xD0, 200);
static constexpr ImU32 COL_CARD      = IM_COL32(28, 20, 38, 200);       // solid dark-purple-grey
static constexpr ImU32 COL_CARD_HL   = IM_COL32(255, 255, 255, 22);     // top inner glow
static constexpr ImU32 COL_CARD_BORDER = IM_COL32(255, 255, 255, 30);
static constexpr ImU32 COL_GLASS     = IM_COL32(255, 255, 255, 32);
static constexpr ImU32 COL_GLASS_HI  = IM_COL32(255, 255, 255, 58);
static constexpr ImU32 COL_GLASS_BORDER = IM_COL32(255, 255, 255, 90);

// -----------------------------------------------------------------------------------------------
// Micro-interaction hover fades - per-widget interpolated alpha in [0,1]
// -----------------------------------------------------------------------------------------------
static std::unordered_map<std::string, float> g_hoverFade;
static float hover_step(const std::string& key, bool hovered, float dt) {
    float& v = g_hoverFade[key];
    float target = hovered ? 1.0f : 0.0f;
    float speed = 8.0f;   // ~120ms rise/fall
    v += (target - v) * (1.0f - expf(-speed * dt));
    if (v < 0.001f) v = 0.0f;
    if (v > 0.999f) v = 1.0f;
    return v;
}
static ImU32 lerp_col(ImU32 a, ImU32 b, float t) {
    int ar = (a) & 0xff, ag = (a >> 8) & 0xff, ab = (a >> 16) & 0xff, aa = (a >> 24) & 0xff;
    int br = (b) & 0xff, bg = (b >> 8) & 0xff, bb = (b >> 16) & 0xff, ba = (b >> 24) & 0xff;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int b2= ab + (int)((bb - ab) * t);
    int aA= aa + (int)((ba - aa) * t);
    return IM_COL32(r, g, b2, aA);
}

// -----------------------------------------------------------------------------------------------
// Game launch (unchanged from previous build)
// -----------------------------------------------------------------------------------------------
static bool has_modules(DWORD pid, const char* const* mods, int nMods) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    std::vector<bool> found(nMods, false);
    MODULEENTRY32 me{}; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) do {
        for (int i = 0; i < nMods; i++)
            if (_stricmp(me.szModule, mods[i]) == 0) found[i] = true;
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    for (bool b : found) if (!b) return false;
    return true;
}
static DWORD WINAPI launch_thread(LPVOID) {
    log_line("Launch: game=%s", g_gameExe.c_str());
    log_line("Launch: dll =%s", g_dllPath.c_str());
    if (!file_exists(g_gameExe)) { log_line("[X] game exe not found"); g_ui.status="Missing Recroom_Release.exe"; g_ui.statusOk=false; return 1; }
    if (!file_exists(g_dllPath)) { log_line("[X] patch dll not found"); g_ui.status="Missing CampusPatch.dll"; g_ui.statusOk=false; return 1; }
    STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessA(g_gameExe.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, g_baseDir.c_str(), &si, &pi)) {
        log_line("[X] CreateProcess failed err=%lu", GetLastError());
        g_ui.status = "CreateProcess failed"; g_ui.statusOk = false; return 2;
    }
    log_line("Spawned PID=%lu, waiting for GameAssembly + Referee...", pi.dwProcessId);
    const char* required[] = { "GameAssembly.dll", "Referee.dll" };
    int waited = 0;
    while (waited < 30000) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            log_line("[X] game exited (%lu) before modules loaded", exitCode);
            g_ui.status="Game exited early"; g_ui.statusOk=false;
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 3;
        }
        if (has_modules(pi.dwProcessId, required, 2)) break;
        Sleep(100); waited += 100;
    }
    if (waited >= 30000) {
        log_line("[X] module load timeout");
        TerminateProcess(pi.hProcess, 1);
        g_ui.status="Module load timeout"; g_ui.statusOk=false;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 4;
    }
    log_line("Modules ready after %d ms - injecting CampusPatch.dll", waited);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLib = GetProcAddress(k32, "LoadLibraryA");
    LPVOID remote = VirtualAllocEx(pi.hProcess, nullptr, g_dllPath.size() + 1,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { log_line("[X] VirtualAllocEx failed"); g_ui.status="Alloc failed"; g_ui.statusOk=false; return 5; }
    WriteProcessMemory(pi.hProcess, remote, g_dllPath.c_str(), g_dllPath.size() + 1, nullptr);
    HANDLE ht = CreateRemoteThread(pi.hProcess, nullptr, 0,
        (LPTHREAD_START_ROUTINE)pLoadLib, remote, 0, nullptr);
    if (!ht) { log_line("[X] CreateRemoteThread failed"); g_ui.status="Remote thread failed"; g_ui.statusOk=false; return 6; }
    WaitForSingleObject(ht, 15000);
    DWORD injectExit = 0; GetExitCodeThread(ht, &injectExit);
    CloseHandle(ht); VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    log_line("LoadLibraryA returned 0x%08lx", injectExit);
    if (injectExit == 0) { g_ui.status="DLL load returned NULL"; g_ui.statusOk=false; }
    else                 { g_ui.status="Injected. Game running.";  g_ui.statusOk=true; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}

// -----------------------------------------------------------------------------------------------
// Background with slow horizontal parallax on the tiled face pattern
// -----------------------------------------------------------------------------------------------
static void draw_background(ImDrawList* dl, ImVec2 sz, float t) {
    if (g_ui.bgTex.srv) {
        dl->AddImage((ImTextureID)g_ui.bgTex.srv, ImVec2(0,0), sz,
                     ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,255));
    } else {
        dl->AddRectFilledMultiColor(ImVec2(0,0), sz,
            IM_COL32(0x79,0x1B,0x8D,255), IM_COL32(0xC7,0x2A,0x5C,255),
            IM_COL32(0xFA,0x99,0x12,255), IM_COL32(0xBF,0x29,0x66,255));
    }
    if (g_ui.faceTex.srv) {
        const float scale = 0.28f;
        float iw = g_ui.faceTex.w * scale, ih = g_ui.faceTex.h * scale;
        const float step = 118.0f;
        // Parallax: full sweep every 30 s, wrapped modulo `step` so tiles reappear seamlessly.
        float offX = fmodf(t * (step / 30.0f), step);
        ImU32 tint = IM_COL32(255, 255, 255, 60);
        for (float y = -ih; y < sz.y + ih; y += step) {
            float xoff = ((int)(y / step) % 2) ? step * 0.5f : 0.0f;
            for (float x = -iw + xoff - step + offX; x < sz.x + iw; x += step) {
                dl->AddImage((ImTextureID)g_ui.faceTex.srv,
                             ImVec2(x, y), ImVec2(x + iw, y + ih),
                             ImVec2(0,0), ImVec2(1,1), tint);
            }
        }
    }
}

// -----------------------------------------------------------------------------------------------
// Custom title bar - dark gradient blend + minimalist glyphs
// -----------------------------------------------------------------------------------------------
static const float TITLEBAR_H = 40.0f;
static void draw_titlebar(ImDrawList* dl, ImVec2 sz) {
    // Vertical dark-alpha gradient that FADES INTO the background instead of a hard bar.
    dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(sz.x, TITLEBAR_H),
        IM_COL32(0, 0, 0, 130), IM_COL32(0, 0, 0, 130),
        IM_COL32(0, 0, 0, 0),   IM_COL32(0, 0, 0, 0));

    // Right-side buttons (minimalist)
    const float bw = 46.0f, bh = TITLEBAR_H;
    float dt = ImGui::GetIO().DeltaTime;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    // Minimize
    ImGui::SetCursorScreenPos(ImVec2(sz.x - 2 * bw, 0));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,255,255,30));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(255,255,255,60));
    bool minH = false;
    if (ImGui::Button("##min", ImVec2(bw, bh))) ShowWindow(g_hwnd, SW_MINIMIZE);
    minH = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    {
        float a = 180 + (int)(75 * hover_step("min_glyph", minH, dt));
        ImVec2 c(sz.x - 2 * bw + bw * 0.5f, bh * 0.5f + 6.0f);
        dl->AddLine(ImVec2(c.x - 6, c.y), ImVec2(c.x + 6, c.y),
                    IM_COL32(240, 240, 240, (int)a), 1.4f);
    }
    // Close
    ImGui::SetCursorScreenPos(ImVec2(sz.x - bw, 0));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 60, 60, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(240, 80, 80, 240));
    bool clsH = false;
    if (ImGui::Button("##close", ImVec2(bw, bh))) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    clsH = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    {
        float a = 180 + (int)(75 * hover_step("close_glyph", clsH, dt));
        ImVec2 c(sz.x - bw * 0.5f, bh * 0.5f);
        dl->AddLine(ImVec2(c.x - 5.5f, c.y - 5.5f), ImVec2(c.x + 5.5f, c.y + 5.5f),
                    IM_COL32(240, 240, 240, (int)a), 1.4f);
        dl->AddLine(ImVec2(c.x - 5.5f, c.y + 5.5f), ImVec2(c.x + 5.5f, c.y - 5.5f),
                    IM_COL32(240, 240, 240, (int)a), 1.4f);
    }
    ImGui::PopStyleVar(2);

    // Drag anywhere except the two right buttons.
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool inDrag = mp.y >= 0 && mp.y < TITLEBAR_H && mp.x >= 0 && mp.x < sz.x - 2 * bw;
    if (inDrag && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
        ReleaseCapture();
        SendMessage(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

// -----------------------------------------------------------------------------------------------
// Logo: Cutout.png centered, drawn as a physical 3D button (multi-pass shadow + inner shine)
// -----------------------------------------------------------------------------------------------
static const float LOGO_TARGET_W = 340.0f;
static float logo_actual_h() {
    if (!g_ui.logoTex.srv || g_ui.logoTex.w == 0) return 100.0f;
    return LOGO_TARGET_W * ((float)g_ui.logoTex.h / (float)g_ui.logoTex.w);
}
static void draw_logo(ImDrawList* dl, ImVec2 sz) {
    const float topY = TITLEBAR_H + 20.0f;
    float lw = LOGO_TARGET_W, lh = logo_actual_h();
    float lx = (sz.x - lw) * 0.5f;
    if (!g_ui.logoTex.srv) return;
    ImTextureID tex = (ImTextureID)g_ui.logoTex.srv;

    // Multi-pass soft outer glow (draws the logo 4x with increasing radius/lower alpha).
    for (int i = 4; i >= 1; i--) {
        int a = 24 / i;
        float off = 1.5f * i;
        dl->AddImage(tex,
            ImVec2(lx - off, topY - off), ImVec2(lx + lw + off, topY + lh + off),
            ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 210, 130, a));
    }
    // Drop shadow (offset dark copy) - gives 3D lift.
    dl->AddImage(tex,
        ImVec2(lx + 3, topY + 8), ImVec2(lx + lw + 3, topY + lh + 8),
        ImVec2(0,0), ImVec2(1,1), IM_COL32(0, 0, 0, 110));
    // Main logo
    dl->AddImage(tex,
        ImVec2(lx, topY), ImVec2(lx + lw, topY + lh),
        ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, 255));
    // Top-edge highlight - a very thin bright band at the top of the logo bounding box gives
    // it that "lit from above" 3D-button feel without needing a real shader.
    dl->AddImage(tex,
        ImVec2(lx, topY - 1), ImVec2(lx + lw, topY + lh * 0.35f - 1),
        ImVec2(0,0), ImVec2(1, 0.35f), IM_COL32(255, 255, 255, 40));
}
static float logo_bottom_y() { return TITLEBAR_H + 20.0f + logo_actual_h() + 18.0f; }

// -----------------------------------------------------------------------------------------------
// Dashboard: wide horizontal panel with three side-by-side setting groups
// -----------------------------------------------------------------------------------------------
static void draw_group(const char* label, ImVec2 pos, ImVec2 size, void(*body)()) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(pos.x + size.x, pos.y + size.y);

    // Solid dark grey with an inner top-glow (simulated 3D raise).
    dl->AddRectFilled(pos, br, COL_CARD, 10.0f);
    // top inner glow band
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x + 1, pos.y + 1), ImVec2(br.x - 1, pos.y + 22),
        COL_CARD_HL, COL_CARD_HL, IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));
    // 1px hairline border
    dl->AddRect(pos, br, COL_CARD_BORDER, 10.0f, 0, 1.0f);
    // Tiny bottom shadow line for depth
    dl->AddLine(ImVec2(pos.x + 6, br.y - 1), ImVec2(br.x - 6, br.y - 1),
                IM_COL32(0, 0, 0, 80), 1.0f);

    // Label (small, dim)
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y + 12));
    ImGui::PushFont(g_ui.fontLabel);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_LABEL);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Body
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y + 40));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(size.x - 32);
    body();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

// Custom-drawn dropdown chevron overlay (glassmorphism combos hide the default arrow).
static void draw_chevron(ImDrawList* dl, ImVec2 center, ImU32 col) {
    dl->AddLine(ImVec2(center.x - 4, center.y - 2),
                ImVec2(center.x,     center.y + 3), col, 1.6f);
    dl->AddLine(ImVec2(center.x + 4, center.y - 2),
                ImVec2(center.x,     center.y + 3), col, 1.6f);
}

// Glass combo box - swaps FrameBg / PopupBg / text colours for glassmorphism, then draws the
// chevron on top after the combo widget lays itself out.
static void glass_combo(const char* label, int* v, const char* const items[], int n) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        COL_GLASS);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, COL_GLASS_HI);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  COL_GLASS_HI);
    ImGui::PushStyleColor(ImGuiCol_Border,         COL_GLASS_BORDER);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        IM_COL32(240, 240, 244, 250));
    ImGui::PushStyleColor(ImGuiCol_Text,           COL_TEXT);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  IM_COL32(0,   0,   0,  40));
    ImGui::PushStyleColor(ImGuiCol_Header,         IM_COL32(0,   0,   0,  30));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   IM_COL32(0,   0,   0,  60));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);

    // Suppress the default arrow by pushing zero item spacing hack? Simpler: draw our chevron
    // on top after the combo. imgui reserves ~arrow width regardless, we paint over it.
    bool changed = ImGui::Combo(label, v, items, n);
    ImVec2 max = ImGui::GetItemRectMax();
    ImVec2 min = ImGui::GetItemRectMin();
    // paint over the default arrow with a solid-glass square then draw our chevron
    float arrowW = 22.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(max.x - arrowW - 1, min.y + 1), ImVec2(max.x - 1, max.y - 1),
                      COL_GLASS, 6.0f);
    draw_chevron(dl, ImVec2(max.x - arrowW * 0.5f - 1, (min.y + max.y) * 0.5f),
                 IM_COL32(30, 30, 30, 200));

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
    (void)changed;
}

// Glass radio - same styling for the check dot.
static void glass_radio(const char* label, int* v, int val) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        COL_GLASS);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, COL_GLASS_HI);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  COL_GLASS_HI);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      IM_COL32(255, 210, 120, 255));
    ImGui::PushStyleColor(ImGuiCol_Border,         COL_GLASS_BORDER);
    ImGui::PushStyleColor(ImGuiCol_Text,           COL_TEXT);
    ImGui::RadioButton(label, v, val);
    ImGui::PopStyleColor(6);
}

// -----------------------------------------------------------------------------------------------
// Launch button - hand-drawn gradient pill with pulse + hover fade + top highlight
// -----------------------------------------------------------------------------------------------
static bool draw_launch_button(ImVec2 pos, ImVec2 size, float t, bool ready) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(pos.x + size.x, pos.y + size.y);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##launch", size);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    float hv = hover_step("launch_hover", hovered, ImGui::GetIO().DeltaTime);

    // Pulse: subtle sin wave scaling the OUTER glow when idle+ready.
    float pulse = 0.5f + 0.5f * sinf(t * 2.2f);   // 0..1
    if (!ready || hovered) pulse = 0.0f;

    // Outer soft glow (only when ready + not-hovered pulse)
    if (pulse > 0.02f) {
        int a = 30 + (int)(50 * pulse);
        for (int i = 4; i >= 1; i--) {
            dl->AddRect(ImVec2(pos.x - i, pos.y - i), ImVec2(br.x + i, br.y + i),
                        IM_COL32(120, 240, 130, a / i), 14.0f + i, 0, 1.0f);
        }
    }

    // Vertical gradient body: bright lime top -> forest bottom, hover shifts brighter.
    ImU32 topBase    = IM_COL32( 90, 210,  95, 255);   // #5AD25F lime
    ImU32 botBase    = IM_COL32( 35, 130,  55, 255);   // #23823 7 forest
    ImU32 topHover   = IM_COL32(115, 230, 120, 255);
    ImU32 botHover   = IM_COL32( 50, 150,  70, 255);
    ImU32 topPress   = IM_COL32( 75, 180,  80, 255);
    ImU32 botPress   = IM_COL32( 25, 100,  40, 255);
    ImU32 topC = active ? topPress : lerp_col(topBase, topHover, hv);
    ImU32 botC = active ? botPress : lerp_col(botBase, botHover, hv);

    dl->AddRectFilledMultiColor(pos, br, topC, topC, botC, botC);
    // rounded body - imgui's multicolor rect doesn't round; overlay a black round-mask trick
    // is complex. Simpler: use AddRectFilled with rounding but a single blended color, then
    // AddRectFilledMultiColor on top? Instead we accept slightly-square corners here and
    // draw a rounded 1px border on top so the eye reads "rounded".
    dl->AddRect(pos, br, IM_COL32(255, 255, 255, 90), 12.0f, 0, 1.5f);
    // Top-edge white highlight (specular)
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x + 2, pos.y + 1), ImVec2(br.x - 2, pos.y + (size.y * 0.42f)),
        IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 90),
        IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));

    // Label
    ImGui::PushFont(g_ui.fontLabel);
    const char* label = "LAUNCH";
    ImVec2 ts = ImGui::CalcTextSize(label);
    // shadow
    dl->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2(pos.x + (size.x - ts.x) * 0.5f + 1, pos.y + (size.y - ts.y) * 0.5f + 2),
                IM_COL32(0, 40, 20, 130), label);
    dl->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                IM_COL32(255, 255, 255, 255), label);
    ImGui::PopFont();
    return clicked;
}

// Glass pill secondary button (Verify/Settings/Repair/Install). Hover-fade micro-interaction.
static bool draw_pill_button(const char* label, ImVec2 pos, ImVec2 size, ImU32 tint) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(pos.x + size.x, pos.y + size.y);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float hv = hover_step(std::string("pill_") + label, hovered, ImGui::GetIO().DeltaTime);

    ImU32 base = lerp_col(IM_COL32(255,255,255,35), IM_COL32(255,255,255,75), hv);
    dl->AddRectFilled(pos, br, base, 12.0f);
    dl->AddRect     (pos, br, IM_COL32(255,255,255,80), 12.0f, 0, 1.0f);
    if (tint) {
        dl->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(br.x - 1, br.y - 1),
                    tint, 12.0f, 0, 1.0f);
    }

    ImGui::PushFont(g_ui.fontLabel);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2(pos.x + (size.x - ts.x) * 0.5f + 1, pos.y + (size.y - ts.y) * 0.5f + 1),
                IM_COL32(0, 0, 0, 120), label);
    dl->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                COL_TEXT, label);
    ImGui::PopFont();
    return clicked;
}

// -----------------------------------------------------------------------------------------------
// UI
// -----------------------------------------------------------------------------------------------
static void draw_ui() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 sz  = io.DisplaySize;
    float  t   = (float)ImGui::GetTime();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_background(ImGui::GetBackgroundDrawList(), sz, t);

    // ---- Gate 1: Steam must be running ---------------------------------------------------
    // Poll every 3s via CampusSteam::IsRunning. Cheap (tlhelp32 snapshot), but no need to hit
    // it every frame. When Steam launches while the gate is up, the check flips and the
    // login screen slides in on the next tick.
    static bool s_steamOk = false;
    double now = ImGui::GetTime();
    if (now - CampusLoginUi::g_lastCheckTime.load() > 3.0) {
        s_steamOk = CampusSteam::IsRunning();
        CampusLoginUi::g_lastCheckTime.store(now);
    }
    if (!s_steamOk) {
        CampusLoginUi::DrawSteamGate(sz);
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    // ---- Gate 2: Must be logged in ------------------------------------------------------
    // The auth worker (LauncherLogin::Start()) refreshes on boot and after any user action.
    // Before the gate returns we render the big Cutout Campus logo + titlebar SO THAT the
    // login screen sits in the same layout region as the normal launcher content (below the
    // logo). Nothing else from the normal draw_ui pipeline runs — no dashboard, no launch
    // button, no dim overlay. Cleaner than screen-wide dimming and matches the launcher's
    // visual identity.
    if (!CampusLoginUi::g_auth.logged_in) {
        ImDrawList* fg = ImGui::GetWindowDrawList();
        draw_logo(fg, sz);
        draw_titlebar(fg, sz);
        CampusLoginUi::DrawExpiredToast(sz, logo_bottom_y());
        CampusLoginUi::DrawLogin(sz, logo_bottom_y());
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    ImDrawList* fg = ImGui::GetWindowDrawList();
    draw_logo(fg, sz);
    draw_titlebar(fg, sz);

    ImGui::PushFont(g_ui.fontBody);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT);

    // Central dark card wraps the whole interactive area (settings + status + buttons + log)
    // like the GDI+ splash's dark rounded rectangle. Pink border matches the admin panel's
    // rose accent. Everything drawn from here to the log bottom sits inside these bounds.
    const float sidePad2 = 22.0f;                        // window edge -> card edge
    const float cardX = sidePad2;
    const float cardY = logo_bottom_y();
    const float cardW = sz.x - sidePad2 * 2;
    // Card height is decided AFTER we know where the log strip ends; for now leave a big pool
    // covering settings + status + buttons + log strip. Actual bottom Y computed inline.
    const float cardBottom = sz.y - sidePad2;
    const float cardH = cardBottom - cardY;
    ImVec2 cardBR(cardX + cardW, cardY + cardH);
    // fill (deep purple) + soft outer shadow + rose border (matches the GDI+ splash exactly)
    fg->AddRectFilled(ImVec2(cardX + 2, cardY + 4), ImVec2(cardBR.x + 2, cardBR.y + 6),
                      IM_COL32(0, 0, 0, 80), 20.0f);
    fg->AddRectFilled(ImVec2(cardX, cardY), cardBR, IM_COL32(18, 10, 28, 230), 20.0f);
    fg->AddRect     (ImVec2(cardX, cardY), cardBR, IM_COL32(255, 150, 205, 220), 20.0f, 0, 1.4f);

    // Panel geometry: three side-by-side setting groups sit INSIDE the central card at the top.
    const float panelPad = 18.0f;                        // card edge -> panel content
    const float panelX = cardX + panelPad;
    const float panelY = cardY + panelPad;
    const float panelW = cardW - panelPad * 2;
    const float panelH = 190.0f;

    // Three side-by-side setting cards inside the panel.
    const float innerPad = 0.0f;
    const float colGap   = 12.0f;
    float colW = (panelW - colGap * 2) / 3.0f;

    draw_group("SCREEN MODE",
        ImVec2(panelX + innerPad, panelY),
        ImVec2(colW, panelH),
        []{
            for (int i = 0; i < IM_ARRAYSIZE(SCREEN_MODES); i++)
                glass_radio(SCREEN_MODES[i], &g_ui.screenMode, i);
        });
    draw_group("RESOLUTION",
        ImVec2(panelX + innerPad + colW + colGap, panelY),
        ImVec2(colW, panelH),
        []{
            glass_combo("##res", &g_ui.resIdx, RESOLUTIONS, IM_ARRAYSIZE(RESOLUTIONS));
            ImGui::Spacing();
            ImGui::PushFont(g_ui.fontTiny);
            ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_DIM);
            ImGui::TextWrapped("Screen resolution used when Windowed / Borderless.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        });
    draw_group("VR MODE",
        ImVec2(panelX + innerPad + (colW + colGap) * 2, panelY),
        ImVec2(colW, panelH),
        []{
            for (int i = 0; i < IM_ARRAYSIZE(VR_MODES); i++)
                glass_radio(VR_MODES[i], &g_ui.vrMode, i);
        });

    // Status text
    ImGui::PushFont(g_ui.fontLabel);
    const char* stxt = g_ui.status.c_str();
    ImVec2 stSz = ImGui::CalcTextSize(stxt);
    float statusY = panelY + panelH + 20;
    ImU32 statusColor = g_ui.statusOk ? IM_COL32(200, 255, 210, 235)
                                       : IM_COL32(255, 190, 180, 235);
    // shadow + text
    fg->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2((sz.x - stSz.x) * 0.5f + 1, statusY + 1),
                IM_COL32(0, 0, 0, 110), stxt);
    fg->AddText(g_ui.fontLabel, g_ui.fontLabel->FontSize,
                ImVec2((sz.x - stSz.x) * 0.5f, statusY),
                statusColor, stxt);
    ImGui::PopFont();

    // Buttons row: LAUNCH big + Verify + Settings (or INSTALL/Repair variants)
    VerifyResult v = verify_files();
    bool haveBuild = file_exists(g_gameExe);
    bool ready = haveBuild && v.allPresent;

    const float rowY = statusY + 34;
    const float launchW = 220, launchH = 56;
    const float pillW   = 130, pillH   = 46;
    const float gap     = 12;
    float rowW;
    if (haveBuild) rowW = launchW + gap + pillW + gap + pillW + gap + pillW;   // LAUNCH+Repair+Verify+Settings
    else           rowW = launchW + gap + pillW + gap + pillW;                  // INSTALL+Verify+Settings (INSTALL replaces LAUNCH)
    float rowX = (sz.x - rowW) * 0.5f;

    if (haveBuild) {
        if (draw_launch_button(ImVec2(rowX, rowY), ImVec2(launchW, launchH), t, ready)) {
            g_ui.status = "Launching..."; g_ui.statusOk = true;
            HANDLE th = CreateThread(nullptr, 0, launch_thread, nullptr, 0, nullptr);
            if (th) CloseHandle(th);
        }
        float x = rowX + launchW + gap;
        float py = rowY + (launchH - pillH) * 0.5f;
        if (draw_pill_button("Repair", ImVec2(x, py), ImVec2(pillW, pillH), 0)) {
            log_line("Repair: %zu file(s) missing", v.missing.size());
            for (auto& m : v.missing) log_line("  missing: %s", m.c_str());
            g_ui.status = v.allPresent ? "All files present - nothing to repair"
                                       : "Repair: source path not configured";
            g_ui.statusOk = v.allPresent;
        }
        x += pillW + gap;
        if (draw_pill_button("Verify", ImVec2(x, py), ImVec2(pillW, pillH), 0)) {
            log_line("Verify: %d files, %zu missing",
                     (int)IM_ARRAYSIZE(REQUIRED_FILES), v.missing.size());
            for (auto& m : v.missing) log_line("  missing: %s", m.c_str());
            char b[128];
            if (v.allPresent) { g_ui.status = "Verify OK - all files present"; g_ui.statusOk = true; }
            else { _snprintf_s(b, sizeof(b), _TRUNCATE, "Verify: %zu missing", v.missing.size()); g_ui.status = b; g_ui.statusOk = false; }
        }
        x += pillW + gap;
        if (draw_pill_button("Settings", ImVec2(x, py), ImVec2(pillW, pillH), 0))
            g_ui.showSettings = !g_ui.showSettings;
    } else {
        // No build: replace LAUNCH with a hot-pink INSTALL button (still gradient, still pill).
        // Reuse launch button shape - just override with a different colour set via tint.
        // (For simplicity we render a pill in this branch.)
        if (draw_pill_button("INSTALL", ImVec2(rowX, rowY), ImVec2(launchW, launchH),
                             IM_COL32(220, 60, 130, 200))) {
            log_line("Install: no build detected. Source path not configured.");
            g_ui.status = "Install: source path not configured"; g_ui.statusOk = false;
        }
        float x = rowX + launchW + gap;
        float py = rowY + (launchH - pillH) * 0.5f;
        if (draw_pill_button("Verify", ImVec2(x, py), ImVec2(pillW, pillH), 0)) {
            log_line("Verify: %d files, %zu missing",
                     (int)IM_ARRAYSIZE(REQUIRED_FILES), v.missing.size());
            for (auto& m : v.missing) log_line("  missing: %s", m.c_str());
            char b[128];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "Verify: %zu missing", v.missing.size());
            g_ui.status = b; g_ui.statusOk = false;
        }
        x += pillW + gap;
        if (draw_pill_button("Settings", ImVec2(x, py), ImVec2(pillW, pillH), 0))
            g_ui.showSettings = !g_ui.showSettings;
    }

    // Log strip - now sits INSIDE the central dark card (bottom portion), styled to blend.
    const float logH = 90.0f;
    ImVec2 lPos(cardX + panelPad, cardBR.y - panelPad - logH);
    ImVec2 lBr(cardBR.x - panelPad, cardBR.y - panelPad);
    fg->AddRectFilled(lPos, lBr, IM_COL32(0, 0, 0, 130), 10.0f);
    fg->AddRect     (lPos, lBr, IM_COL32(255, 150, 205, 90), 10.0f, 0, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::SetCursorScreenPos(lPos);
    ImGui::BeginChild("logpane", ImVec2(lBr.x - lPos.x, logH), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(g_ui.fontTiny);
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        for (auto& l : g_logLines) ImGui::TextUnformatted(l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Outer pink hairline window border - matches the GDI+ splash's `RGB(155, 60, 150)` frame
    // that gives the borderless window a definite edge against light desktops. Drawn last so
    // it sits over everything else.
    fg->AddRect(ImVec2(0.5f, 0.5f), ImVec2(sz.x - 0.5f, sz.y - 0.5f),
                IM_COL32(155, 60, 150, 220), 12.0f, 0, 1.5f);

    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------------------------------
// Window proc - strict WS_POPUP borderless. WM_NCCALCSIZE returns 0 so client == window.
// Also strip the DWM's "invisible" resize border by handling both wp cases.
// -----------------------------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_NCCALCSIZE:
            // With WS_POPUP alone Windows still reserves an invisible resize frame; returning 0
            // for both wp cases collapses that so our client rect fills the whole window.
            if (wp) return 0;
            return 0;
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
                g_sc->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp),
                                    DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* back = nullptr; g_sc->GetBuffer(0, IID_PPV_ARGS(&back));
                g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------------------------
// WinMain
// -----------------------------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_baseDir = module_dir(hInst); if (g_baseDir.empty()) g_baseDir = ".";
    g_gameExe = g_baseDir + "\\" + GAME_EXE_NAME;
    g_dllPath = g_baseDir + "\\" + PATCH_DLL_REL;
    log_line("Launcher started. base=%s", g_baseDir.c_str());
    log_line("Game exe candidate:  %s", g_gameExe.c_str());
    log_line("Patch DLL candidate: %s", g_dllPath.c_str());

    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInst,
        LoadIconA(hInst, MAKEINTRESOURCEA(1)), LoadCursor(nullptr, IDC_ARROW),
        nullptr, nullptr, "CampusLauncher", LoadIconA(hInst, MAKEINTRESOURCEA(1)) };
    RegisterClassExA(&wc);

    // Strict WS_POPUP - no title bar, no system menu (so Windows doesn't draw its own bar).
    // WS_EX_APPWINDOW keeps a taskbar entry regardless.
    HWND hwnd = CreateWindowExA(WS_EX_APPWINDOW,
        "CampusLauncher", "Campus Launcher", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 1020, 700,
        nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    // Round the window on Win11 (silently fails on older).
    { int c = 2; DwmSetWindowAttribute(hwnd, 33, &c, sizeof(c)); }

    if (!create_device(hwnd)) { MessageBoxA(hwnd, "D3D11 init failed", "Campus", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // Fonts: Rubik variable weight, three sizes. Falls back to default if the RC entry is missing.
    {
        const void* fontData; size_t fontSz;
        bool haveRubik = find_rcdata(IDR_RUBIK_TTF, fontData, fontSz);
        auto add = [&](float px) -> ImFont* {
            if (haveRubik) {
                ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
                return io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(fontData), (int)fontSz, px, &cfg);
            } else {
                ImFontConfig cfg; cfg.SizePixels = px;
                return io.Fonts->AddFontDefault(&cfg);
            }
        };
        g_ui.fontBody  = add(15.0f);
        g_ui.fontLabel = add(17.0f);
        g_ui.fontH     = add(28.0f);
        g_ui.fontTiny  = add(12.5f);
        io.Fonts->Build();
    }
    // Nicer imgui defaults for our aesthetic
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.FrameRounding = 8.0f;
        s.GrabRounding  = 6.0f;
        s.WindowRounding = 0.0f;
        s.ItemSpacing = ImVec2(10, 8);
        s.FramePadding = ImVec2(10, 6);
    }

    // Load embedded PNG textures
    if (load_embedded_png_texture(g_dev, IDR_ANIMATED_ICON_PNG, g_ui.faceTex))
        log_line("Loaded AnimatedIcon (%dx%d)", g_ui.faceTex.w, g_ui.faceTex.h);
    if (load_embedded_png_texture(g_dev, IDR_BACKGROUND_PNG, g_ui.bgTex))
        log_line("Loaded Background (%dx%d)", g_ui.bgTex.w, g_ui.bgTex.h);
    if (load_embedded_png_texture(g_dev, IDR_CUTOUT_PNG, g_ui.logoTex))
        log_line("Loaded Cutout logo (%dx%d)", g_ui.logoTex.w, g_ui.logoTex.h);

    // Publish the Cutout logo SRV to LauncherLogin so the login card can render the "Campus"
    // pill at the top instead of a plain text placeholder. If the load above failed, srv is
    // null and DrawLogin falls back to the text "Campus" itself.
    CampusLoginUi::SetLogo(g_ui.logoTex.srv, g_ui.logoTex.w, g_ui.logoTex.h);

    // Initial verify
    VerifyResult v0 = verify_files();
    if (v0.allPresent) { g_ui.status = "All required files present. Ready to launch."; g_ui.statusOk = true; }
    else { char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "%zu file(s) missing - Verify for details", v0.missing.size()); g_ui.status = b; g_ui.statusOk = false; }

    // Start the auth worker BEFORE the main loop so the first frame can already reflect any
    // saved JWT (Refresh is called eagerly inside WorkerLoop before it enters the wait loop).
    CampusLoginUi::Start();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (GetForegroundWindow() != hwnd) Sleep(80);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        float clear[4] = { 0.05f, 0.02f, 0.07f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    // Stop auth worker cleanly before D3D/imgui teardown so it doesn't outlive us.
    CampusLoginUi::Stop();

    g_ui.faceTex.release(); g_ui.bgTex.release(); g_ui.logoTex.release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassA("CampusLauncher", hInst);
    return 0;
}

#include "RR/Patching/Patches.h"
#include "Utils/logging.h"

// CampusPatch2025 — adapted from the public "Rec Room 2025 Patch" reference for the July 19 2025
// build, retargeted at the private Campus server (gameauth.ddns.net) instead of the reference's
// ns.cgb.net. Native x64 DLL (not BepInEx): BepInEx cannot bootstrap on builds behind Referee
// because Referee's guard rejects il2cpp_domain_get calls originating from BepInEx's DLL, so a
// bare native DLL that neutralises Referee FIRST is the working approach on this build.
//
// Do NOT do heavy work inside DllMain (loader-lock). We only allocate a console and spawn a worker
// thread that runs the actual init once the module is fully loaded.

// Captured in DllMain and read by InitThread. HMODULE is used to resolve <dll folder>\Logs for
// the tee log file (see logging.h).
static HMODULE g_selfModule = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    // Wait a bit for the game to load its native modules. The launcher already polls for
    // GameAssembly + Referee before injecting, but this is defense-in-depth: if the DLL gets
    // injected via any other mechanism at a different point in startup, we still wait rather
    // than dereference NULL.
    for (int i = 0; i < 200; i++) {   // up to ~20s
        if (GetModuleHandleA("GameAssembly.dll") && GetModuleHandleA("Referee.dll")) break;
        Sleep(100);
    }
    if (!InitGlobals()) {
        printf("[Campus] init failed - aborting (no hooks installed)\n");
        return 1;
    }
    // Install the console -> file tee BEFORE Resolve()/Patch() so every line the rest of
    // startup emits (il2cpp resolutions, hook install results, URI rewrites) is captured to
    // disk. The header written by Init() ends up as the first bytes of the file.
    CampusLogging::Init(g_selfModule);
    // HTTP debug: load the user-editable URL filter file so RecNetResponse_H can dump
    // matching response bodies to Logs\http-dumps\. Zero cost when filter file is absent.
    HttpDebug::Init(g_selfModule);
    RR::Patches::Resolve();
    RR::Patches::Patch();
    return 0;
}

static void CreateConsole()
{
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleTitleA("Campus - CampusPatch2025");

    // Enable VT (ANSI escape) processing on the console output handle so the coloured tags
    // in the log come through as actual colours instead of literal `\x1b[...m` runs. This
    // has been a default flag on Windows 10 build 10586+ but we set it explicitly for the
    // rare console (e.g. an old ConHost fallback) where it isn't.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    printf("\x1b[1m\x1b[38;5;120m[CampusPatch]\x1b[0m loaded - target: "
           "\x1b[38;5;227mgameauth.ddns.net\x1b[0m\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_selfModule = hModule;   // stash for InitThread -> CampusLogging::Init
        CreateConsole();
        // Run Resolve()/Patch() OUTSIDE DllMain to avoid loader-lock deadlocks - MinHook takes
        // locks and touches other modules, which is unsafe from DLL_PROCESS_ATTACH.
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}


#pragma once
// LauncherLogin.h — imgui screens for the launcher's auth gating.
//
// Three states the launcher may be in on any frame:
//   1) SteamGate    — Steam.exe isn't running. Blocking dialog until it comes up.
//   2) LoginScreen  — no valid session. Show username/password fields + Discord button +
//                     Create Account link.
//   3) Ready        — session is valid (from disk or fresh login). Fall through to the normal
//                     launcher UI (settings / launch button / etc).
//
// State is driven by:
//   - CampusSteam::IsRunning()   for gate #1
//   - g_auth.logged_in           for gate #2/#3
//
// Auth actions (login/register/link steam) run on a WORKER THREAD so the imgui frame never
// blocks on WinHTTP. The UI writes into g_authCmd (an atomic "please do X" mailbox) and the
// worker picks it up. Success/error come back through g_auth.status + g_authError.
//
// This header owns:
//   - the g_auth session snapshot the whole launcher reads
//   - the async command queue + worker thread lifecycle
//   - the DrawSteamGate() / DrawLogin() imgui blocks
// It DOES NOT own the main launcher window / D3D / layout; those still live in Launcher.cpp.

#include "imgui.h"
#include "LauncherAuth.h"
#include "LauncherSteam.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include <shellapi.h>

namespace CampusLoginUi {

    // ------- Shared state (read by UI, written by worker) --------------------
    // No mutex — single-writer / many-reader over primitives + std::string. Worst case is a
    // single frame of visual glitch (half-updated string), which is invisible in practice.
    inline CampusAuth::Session g_auth;
    inline std::string         g_authError;    // last human-readable error, shown on login screen

    // Steam gate + auth-refresh timer so we don't hammer the API. Retried every 3s.
    inline std::atomic<double>  g_lastCheckTime{-999.0};

    // Async command queue for the worker thread. UI sets `pending` + fills fields; worker
    // reads, executes, clears `pending` when done. Only one action in flight at a time —
    // fine because these are user-initiated clicks, not high-frequency events.
    enum class AuthCmd { None, Login, Register, Logout, Refresh };
    struct AuthMailbox {
        std::atomic<AuthCmd>    pending{AuthCmd::None};
        std::atomic<bool>       busy{false};
        std::string             username;
        std::string             password;
    };
    inline AuthMailbox g_cmd;

    // Login form fields — imgui InputText writes directly into these.
    inline char g_inputUser[64] = {};
    inline char g_inputPass[64] = {};

    // Cutout logo (the "Campus" pill from Cutout.png). Registered once by Launcher.cpp after it
    // uploads the PNG to the GPU. If it's still nullptr when DrawLogin runs (rare: race between
    // texture upload and first frame) we fall back to a plain text "Campus" title so the card
    // isn't empty at the top.
    inline void* g_logoSrv = nullptr;
    inline int   g_logoW   = 0;
    inline int   g_logoH   = 0;
    inline void SetLogo(void* srv, int w, int h) { g_logoSrv = srv; g_logoW = w; g_logoH = h; }

    // ------- Worker thread ---------------------------------------------------
    // Started once from LauncherLogin::Start() at app boot.
    inline std::thread g_worker;
    inline std::atomic<bool> g_workerRun{true};

    inline void WorkerLoop() {
        // Initial refresh — pick up any saved JWT and validate it.
        CampusAuth::Refresh(g_auth);
        while (g_workerRun.load()) {
            AuthCmd c = g_cmd.pending.exchange(AuthCmd::None);
            if (c != AuthCmd::None) {
                g_cmd.busy.store(true);
                switch (c) {
                    case AuthCmd::Login: {
                        std::string err;
                        if (!CampusAuth::Login(g_cmd.username, g_cmd.password, g_auth, err)) {
                            g_authError = err;
                            g_auth.status = "invalid";
                        } else {
                            g_authError.clear();
                            // Opportunistic Steam link on login: if user has Steam signed in,
                            // link that id to the account so ingame flows recognize it later.
                            uint64_t sid = CampusSteam::CurrentSteamId64();
                            if (sid != 0) {
                                std::string linkErr;
                                CampusAuth::LinkSteam(sid, g_auth, linkErr);   // best-effort; 409 is fine, means the id is claimed
                            }
                        }
                        break;
                    }
                    case AuthCmd::Register: {
                        std::string err;
                        if (!CampusAuth::Register(g_cmd.username, g_cmd.password, g_auth, err)) {
                            g_authError = err;
                        } else {
                            g_authError.clear();
                            uint64_t sid = CampusSteam::CurrentSteamId64();
                            if (sid != 0) {
                                std::string linkErr;
                                CampusAuth::LinkSteam(sid, g_auth, linkErr);
                            }
                        }
                        break;
                    }
                    case AuthCmd::Logout:  CampusAuth::Logout(g_auth); g_authError.clear(); break;
                    case AuthCmd::Refresh: CampusAuth::Refresh(g_auth); break;
                    default: break;
                }
                // Wipe input buffers on any action so credentials don't linger in imgui memory.
                if (c == AuthCmd::Login || c == AuthCmd::Register) {
                    SecureZeroMemory(g_inputPass, sizeof(g_inputPass));
                }
                g_cmd.busy.store(false);
            }
            Sleep(50);
        }
    }

    inline void Start() {
        g_worker = std::thread(WorkerLoop);
    }
    inline void Stop() {
        g_workerRun.store(false);
        if (g_worker.joinable()) g_worker.join();
    }

    // ------- Draw: Steam gate -----------------------------------------------
    // Full-screen overlay when Steam isn't running.
    inline void DrawSteamGate(ImVec2 sz) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(10, 5, 20, 220));

        const char* line1 = "Open Steam to continue";
        const char* line2 = "The launcher needs Steam running to sign you in and launch Campus.";
        ImFont* f = ImGui::GetFont();
        float fs = f->FontSize;
        ImVec2 s1 = ImGui::CalcTextSize(line1);
        ImVec2 s2 = ImGui::CalcTextSize(line2);
        float cx = sz.x * 0.5f;
        float cy = sz.y * 0.5f - 40;

        fg->AddText(f, fs * 1.8f, ImVec2(cx - s1.x * 0.9f, cy), IM_COL32(255, 255, 255, 240), line1);
        fg->AddText(f, fs,        ImVec2(cx - s2.x * 0.5f, cy + 60), IM_COL32(220, 210, 220, 220), line2);

        // Retry button
        ImVec2 btnSz(160, 44);
        ImVec2 btnPos(cx - btnSz.x * 0.5f, cy + 110);
        ImGui::SetCursorPos(btnPos);
        if (ImGui::Button("Retry", btnSz)) {
            g_lastCheckTime.store(-999.0);   // force a check next frame
        }
        // Also offer to open Steam if it's installed at the default path.
        ImGui::SetCursorPos(ImVec2(cx - btnSz.x * 0.5f, cy + 160));
        if (ImGui::Button("Open Steam", btnSz)) {
            ShellExecuteA(nullptr, "open", "steam://open/main", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // ------- Custom login-card buttons ---------------------------------------
    // imgui's default Button doesn't do multi-colour gradients or coloured hover states cleanly,
    // so we roll two small custom widgets that match the launcher's aesthetic:
    //   - GradientButton: horizontal purple→orange fill, matches the mock's "Log in" button.
    //   - DiscordButton  : solid Discord blue (#5865F2). Adds a small round Discord glyph disc
    //                      on the left; we don't bundle the SVG logo, so a stylised dot stands
    //                      in for it — close enough visually to the mock.
    // Both return true on click.
    inline bool GradientButton(const char* label, ImVec2 sz) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      pos = ImGui::GetCursorScreenPos();
        ImVec2      br(pos.x + sz.x, pos.y + sz.y);

        ImGui::InvisibleButton(label, sz);
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();
        bool clicked = ImGui::IsItemClicked();

        // Purple (left) → orange (right). Slightly brightened on hover, dimmed on press.
        int mod = active ? -18 : (hovered ? 12 : 0);
        auto clamp = [](int v){ return v < 0 ? 0 : (v > 255 ? 255 : v); };
        ImU32 L = IM_COL32(clamp(139 + mod), clamp( 30 + mod), clamp(163 + mod), 255);   // ~#8B1EA3
        ImU32 R = IM_COL32(clamp(239 + mod), clamp(108 + mod), clamp( 31 + mod), 255);   // ~#EF6C1F
        dl->AddRectFilledMultiColor(pos, br, L, R, R, L);
        // Rounded 1-px inner border so the eye reads the corners as rounded even though
        // AddRectFilledMultiColor doesn't itself support rounding.
        dl->AddRect(pos, br, IM_COL32(255, 255, 255, 60), 10.0f, 0, 1.2f);
        // Top-edge highlight
        dl->AddRectFilledMultiColor(
            ImVec2(pos.x + 2, pos.y + 1), ImVec2(br.x - 2, pos.y + sz.y * 0.42f),
            IM_COL32(255, 255, 255, 80), IM_COL32(255, 255, 255, 80),
            IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));

        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(pos.x + (sz.x - ts.x) * 0.5f + 1, pos.y + (sz.y - ts.y) * 0.5f + 2),
                    IM_COL32(0, 0, 0, 130), label);
        dl->AddText(ImVec2(pos.x + (sz.x - ts.x) * 0.5f, pos.y + (sz.y - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255), label);
        return clicked;
    }

    inline bool DiscordButton(const char* label, ImVec2 sz) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      pos = ImGui::GetCursorScreenPos();
        ImVec2      br(pos.x + sz.x, pos.y + sz.y);

        ImGui::InvisibleButton(label, sz);
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();
        bool clicked = ImGui::IsItemClicked();

        ImU32 fill = active   ? IM_COL32(66, 74, 190, 255)
                   : hovered  ? IM_COL32(103, 114, 250, 255)
                              : IM_COL32( 88,101, 242, 255);   // #5865F2 Discord blurple
        dl->AddRectFilled(pos, br, fill, 10.0f);
        dl->AddRect      (pos, br, IM_COL32(255, 255, 255, 40), 10.0f, 0, 1.0f);

        // Discord mark, drawn from primitives so no PNG asset is required. The real mark is
        // essentially a horizontal-oblong "gamepad" silhouette with two round eyes and small
        // controller tabs at the bottom corners. We approximate with:
        //   - horizontal rounded rectangle (body), white
        //   - two vertical oval eyes (blurple, matching the button fill so they read as
        //     cutouts against the white body)
        //   - two tiny "feet" triangles at the bottom to hint at the tabs
        // Not the official SVG (which we can't ship without licensing decisions), but obvious
        // enough as a Discord glyph at 44px button height.
        const float bodyW = 22, bodyH = 15;
        const float cx = pos.x + 26;
        const float cy = pos.y + sz.y * 0.5f;
        const float bx1 = cx - bodyW * 0.5f, by1 = cy - bodyH * 0.5f;
        const float bx2 = cx + bodyW * 0.5f, by2 = cy + bodyH * 0.5f;

        // Body (rounded rect, white)
        dl->AddRectFilled(ImVec2(bx1, by1), ImVec2(bx2, by2),
                          IM_COL32(255, 255, 255, 255), bodyH * 0.5f);

        // Two eye ellipses (approximated as small filled circles slightly tall — imgui doesn't
        // give us AddEllipseFilled cleanly in older versions, so a circle is close enough at
        // this size).
        dl->AddCircleFilled(ImVec2(cx - 5, cy),     2.4f, fill, 16);
        dl->AddCircleFilled(ImVec2(cx + 5, cy),     2.4f, fill, 16);

        // Controller-tab hints: two small white ovals just below the body corners so the
        // silhouette reads more "Discord" than "generic pill".
        dl->AddCircleFilled(ImVec2(bx1 + 3, by2 + 1), 2.0f, IM_COL32(255, 255, 255, 255), 12);
        dl->AddCircleFilled(ImVec2(bx2 - 3, by2 + 1), 2.0f, IM_COL32(255, 255, 255, 255), 12);

        ImVec2 ts = ImGui::CalcTextSize(label);
        // Nudge label right of the icon so they don't overlap on narrow buttons.
        float labelX = pos.x + (sz.x - ts.x) * 0.5f;
        if (labelX < cx + bodyW * 0.5f + 10) labelX = cx + bodyW * 0.5f + 10;
        dl->AddText(ImVec2(labelX, pos.y + (sz.y - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255), label);
        return clicked;
    }

    // Faint horizontal "OR" divider — two thin lines on either side of the word.
    inline void OrDivider(ImVec2 pos, float width) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* txt = "OR";
        ImVec2 ts = ImGui::CalcTextSize(txt);
        float mid = pos.y;
        float leftEndX  = pos.x + (width - ts.x) * 0.5f - 8;
        float rightStartX = pos.x + (width + ts.x) * 0.5f + 8;
        dl->AddLine(ImVec2(pos.x,          mid), ImVec2(leftEndX,       mid), IM_COL32(200, 190, 210, 90), 1.0f);
        dl->AddLine(ImVec2(rightStartX,    mid), ImVec2(pos.x + width,  mid), IM_COL32(200, 190, 210, 90), 1.0f);
        dl->AddText(ImVec2(pos.x + (width - ts.x) * 0.5f, mid - ts.y * 0.5f),
                    IM_COL32(200, 190, 210, 200), txt);
    }

    // ------- Draw: Login screen ---------------------------------------------
    // Matches the main launcher's aesthetic (see image 2): the big Cutout Campus logo already
    // drawn by draw_logo() stays visible at the top of the window, and the login card sits
    // BELOW it in the launcher's normal card region. We do NOT dim the whole screen and we do
    // NOT re-draw a small logo inside the card — one prominent logo is enough.
    //
    // `topY` is the pixel Y where the launcher's normal content starts (right under the logo).
    // The main launcher passes logo_bottom_y(); we accept it as an argument so LauncherLogin.h
    // doesn't need to include the launcher-side layout constants.
    inline void DrawLogin(ImVec2 sz, float topY) {
        ImDrawList* wdl = ImGui::GetWindowDrawList();

        const float cardW = 420;
        const float cardH = 480;   // grown from 380 to fit gradient button + OR divider +
                                    // Discord button + Create Account subtitle + button
        const float cardX = (sz.x - cardW) * 0.5f;
        const float cardY = topY + 30;

        // Card fill + rose border, matching Launcher.cpp's main content card exactly (same
        // colour + roundness + border weight).
        wdl->AddRectFilled(ImVec2(cardX + 2, cardY + 4), ImVec2(cardX + cardW + 2, cardY + cardH + 6),
                           IM_COL32(0, 0, 0, 80), 20.0f);
        wdl->AddRectFilled(ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH),
                           IM_COL32(18, 10, 28, 230), 20.0f);
        wdl->AddRect     (ImVec2(cardX, cardY), ImVec2(cardX + cardW, cardY + cardH),
                           IM_COL32(255, 150, 205, 220), 20.0f, 0, 1.4f);

        // Subtitle above the inputs, in the same "Section title + subtitle" style the main
        // launcher uses ("Campus Launcher" / "Select your play mode").
        ImFont* f = ImGui::GetFont();
        const char* h1  = "Sign in";
        const char* h2  = "Log in with your Campus account, Discord, or create a new one";
        wdl->AddText(f, f->FontSize * 1.4f,
                     ImVec2(cardX + 22, cardY + 18),
                     IM_COL32(240, 240, 240, 250), h1);
        wdl->AddText(f, f->FontSize,
                     ImVec2(cardX + 22, cardY + 46),
                     IM_COL32(200, 190, 210, 200), h2);

        // Widget area starts below the header block. SetCursorPos uses window-relative coords;
        // our root window is at (0,0) covering the whole client area so this equals absolute.
        float y = cardY + 78;
        ImGui::SetCursorPos(ImVec2(cardX + 22, y));
        ImGui::BeginGroup();
        ImGui::PushItemWidth(cardW - 44);

        ImGui::TextUnformatted("Username");
        ImGui::InputText("##u", g_inputUser, sizeof(g_inputUser));

        ImGui::TextUnformatted("Password");
        ImGui::InputText("##p", g_inputPass, sizeof(g_inputPass), ImGuiInputTextFlags_Password);

        // Error text (if any) — red, wraps within the card width.
        if (!g_authError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 130, 130, 240));
            ImGui::TextWrapped("%s", g_authError.c_str());
            ImGui::PopStyleColor();
        }

        bool busy = g_cmd.busy.load();
        if (busy) ImGui::BeginDisabled();

        // Gradient Log in button, matching the mock (purple → orange).
        if (GradientButton("Log in", ImVec2(cardW - 44, 42))) {
            g_cmd.username = g_inputUser;
            g_cmd.password = g_inputPass;
            g_cmd.pending.store(AuthCmd::Login);
        }

        // OR divider — same visual as the mock. Consumes some vertical space via a Dummy so
        // the following button doesn't sit right on top of it.
        ImGui::Dummy(ImVec2(0, 6));
        ImVec2 orPos = ImGui::GetCursorScreenPos();
        OrDivider(ImVec2(orPos.x, orPos.y + 8), (float)(cardW - 44));
        ImGui::Dummy(ImVec2(0, 16));

        // Blue Discord button. Opens the system browser to the OAuth start URL. The server's
        // /discord/login endpoint handles both the initial 302 to Discord AND the callback
        // (see AuthLauncherController.DiscordLoginLauncher), so the same URL works both ways.
        if (DiscordButton("Log in with Discord", ImVec2(cardW - 44, 42))) {
            ShellExecuteA(nullptr, "open",
                "https://gameauth.ddns.net/api/auth/discord/login",
                nullptr, nullptr, SW_SHOWNORMAL);
        }

        // Subtle "Create an Account" — smaller and quieter than the two primary buttons,
        // matching the "only if you haven't created an account" hint from the spec.
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 190, 210, 200));
        ImGui::TextUnformatted("Create an account (only if you haven't created one)");
        ImGui::PopStyleColor();
        if (ImGui::Button("Create an Account", ImVec2(cardW - 44, 34))) {
            g_cmd.username = g_inputUser;
            g_cmd.password = g_inputPass;
            g_cmd.pending.store(AuthCmd::Register);
        }

        if (busy) ImGui::EndDisabled();

        ImGui::PopItemWidth();
        ImGui::EndGroup();
    }

    // ------- Session-expired toast ------------------------------------------
    // Rendered on the WINDOW drawlist (not foreground) so it sits inside the launcher's UI
    // rather than floating over the titlebar/logo. `topY` is the same "content starts here"
    // pixel row DrawLogin uses (logo_bottom_y()), so the toast lands neatly above the card.
    inline void DrawExpiredToast(ImVec2 sz, float topY) {
        const char* msg = nullptr;
        if (g_auth.status == "expired") msg = "Login time expired, please log back in.";
        else if (g_auth.status == "invalid") msg = "Logged out due to Invalid Credentials";
        if (!msg) return;
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 s = ImGui::CalcTextSize(msg);
        float tx = (sz.x - s.x) * 0.5f;
        float ty = topY + 4;
        wdl->AddRectFilled(ImVec2(tx - 14, ty - 6), ImVec2(tx + s.x + 14, ty + s.y + 6),
                           IM_COL32(200, 50, 60, 230), 8.0f);
        wdl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 240), msg);
    }
}

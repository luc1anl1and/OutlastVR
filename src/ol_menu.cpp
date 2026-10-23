// ol_menu.cpp - in-headset ImGui menu (INSERT), same tech as the Mass Effect / Dishonored menus.
// =============================================================================
// This is a REAL ImGui window: small, movable, resizable. The earlier hand-rolled GDI panel could
// never look or behave like the ME one because it was the wrong tool; ME1 and the Dishonored D3D9
// probe both use ImGui, so this does too.
//
// Outlast packs BOTH eyes into one backbuffer (the SBS split), so - unlike Dishonored, which drew
// ImGui straight onto the backbuffer + a sidecar - ImGui renders into an OFFSCREEN target here
// and that target is drawn into each half. The window floats over the game with a transparent
// surround.
//
// Input: a low-level mouse hook (WH_MOUSE_LL) drives a virtual cursor and reads the buttons below
// DirectInput, so it works even though Outlast owns the mouse; the game camera is frozen in the
// engine while the menu is open (ol_ue3.cpp), so nothing leaks. Gamepad + keyboard nav are fed to
// ImGui too.
// =============================================================================

#include <Windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <cstdio>
#include <math.h>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "ol_menu.h"
#include "ol_xr.h"
#include "ol_motion.h"
#include "ol_interact.h"

extern "C" void OLProxyLog(const char* line);
extern "C" int  OLUE3_StereoMode();
extern "C" int  OLUE3_SplitActive();
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy);

// ---- knobs owned elsewhere -------------------------------------------------------------------
extern "C" float OLUE3_GetHalfEye();      extern "C" void OLUE3_SetHalfEye(float v);
extern "C" float OLUE3_GetWorldToMeters();extern "C" void OLUE3_SetWorldToMeters(float v);
extern "C" int   OLUE3_GetHeadTrack();    extern "C" void OLUE3_SetHeadTrack(int v);
extern "C" int   OLUE3_GetHeadPos();      extern "C" void OLUE3_SetHeadPos(int v);
extern "C" int   OLXR_PosArmed();         extern "C" float OLXR_GetRtEyeSep();
extern "C" float OLUE3_GetLeanGain();     extern "C" void OLUE3_SetLeanGain(float v);
extern "C" int   OLXR_GetHandCam();       extern "C" void OLXR_SetHandCam(int v);
extern "C" float OLXR_GetHandCamStrength(); extern "C" void OLXR_SetHandCamStrength(float v);
extern "C" int   OLUE3_GetHeadRoll();     extern "C" void OLUE3_SetHeadRoll(int v);
extern "C" int   OLUE3_GetEyeSwap();      extern "C" void OLUE3_SetEyeSwap(int v);
extern "C" int   OLUE3_GetCineFov();      extern "C" void OLUE3_SetCineFov(int v);
extern "C" int   OLUE3_CineFovState();
extern "C" int   OLUE3_GetNvLight();      extern "C" void OLUE3_SetNvLight(int v);
extern "C" float OLXR_GetConvergence();   extern "C" void OLXR_SetConvergence(float v);
extern "C" int   OLXR_GetStereoModel();   extern "C" void OLXR_SetStereoModel(int v);
extern "C" float OLXR_GetSmoothing();     extern "C" void OLXR_SetSmoothing(float v);
extern "C" float OLXR_GetScreenBelow();   extern "C" void OLXR_SetScreenBelow(float v);
extern "C" float OLXR_GetGamma();         extern "C" void OLXR_SetGamma(float v);
extern "C" void  OLXR_ResetGamma();
extern "C" int   OLXR_GammaRampApplied();
extern "C" void  OLXR_GetEyeInfo(int* w, int* h, float* fovX, float* fovY);
extern "C" int   OLProxy_GetUiMirror();   extern "C" void OLProxy_SetUiMirror(int v);
extern "C" void  OLProxy_GetHudFit(float* scale, float* offX, float* offY);
extern "C" void  OLProxy_SetHudFit(float scale, float offX, float offY);
extern "C" int   OLProxy_SaveSettings();
extern "C" int   OLUE3_HiResActive();
extern "C" float OLUE3_GetRenderScale();
extern "C" int   OLUE3_SetRenderScale(float s);
extern "C" void  OLUE3_GetHiResTarget(long* w, long* h);
extern "C" int   OLProxy_GetRecenterKey();
extern "C" int   OLProxy_GetRecenterPad();
extern "C" int   OLProxy_GetMenuKey();
extern "C" int   OLProxy_GetBindCapture();
extern "C" void  OLProxy_StartBindCapture(int which);
extern "C" const char* OLProxy_KeyName(int vk);
extern "C" const char* OLProxy_PadName(int mask);
extern "C" int   OLUE3_GetArmImproved();  extern "C" void OLUE3_SetArmImproved(int on);
extern "C" float OLUE3_GetWorldSize(); extern "C" void OLUE3_SetWorldSize(float s); extern "C" float OLUE3_GetEyeSpacingCm();   // (Beta 1.1)
extern "C" int OLUE3_GetBodyTurn(); extern "C" void OLUE3_SetBodyTurn(int on);   // (Beta 1.1) [BODYTURN]
extern "C" float OLUE3_GetBobReduce();    extern "C" void OLUE3_SetBobReduce(float v);
extern "C" int   OLUE3_GetLevelHorizon(); extern "C" void OLUE3_SetLevelHorizon(int v);
extern "C" int   OLUE3_GetLookUse();      extern "C" void OLUE3_SetLookUse(int v);
extern "C" int   OLUE3_LookUseReady();
extern "C" int   OLUE3_GetWideFov();      extern "C" void OLUE3_SetWideFov(int v);
extern "C" int   OLUE3_GetFilmGrain();    extern "C" void OLUE3_SetFilmGrain(int on);   // (Test47) [GRAIN]
extern "C" int   OLXR_GetUpscaler();      extern "C" void OLXR_SetUpscaler(int v);
extern "C" float OLXR_GetSharpness();     extern "C" void OLXR_SetSharpness(float v);

// ---- the offscreen target ImGui renders into --------------------------------------------------
static const int kRTW = 1400, kRTH = 1050;   // 4:3; the window lives (and moves) inside this

static IDirect3DDevice9*  g_dev  = nullptr;
static IDirect3DTexture9*  g_rtTex  = nullptr;
static IDirect3DSurface9*  g_rtSurf = nullptr;
static bool g_inited  = false;
static bool g_visible = false;
static bool g_firstLayout = true;
static int  g_savedFrames = 0;    // "saved" confirmation lingers a few seconds
static int  g_savedOk = 0;
// [MENUTHEME] fonts, colours and the open time for the camcorder timecode (see ApplyTheme).
static ImFont* g_fontMain = nullptr;
static ImFont* g_fontHead = nullptr;
static double  g_openedAt = 0.0;
static const ImU32 kNvGreen = IM_COL32(150, 240, 140, 255);
static const ImU32 kPumpkin = IM_COL32(255, 140,  30, 255);
static const ImU32 kRecRed  = IM_COL32(235,  45,  30, 255);
static const float kFontPx = 30.0f;
static void ApplyTheme();
static ImFont* AddWinFont(const wchar_t* file, float px);
// [MENUTHEME] In a headset nothing hovers: the explanation of whatever the controller has
// selected goes in a fixed help box at the bottom instead of a floating tooltip. The mouse only
// counts once it has actually moved since the menu opened.
static const char* g_helpText = nullptr;
static volatile long g_mouseMoved = 0;

// ---- virtual cursor from the low-level mouse hook ---------------------------------------------
static volatile float g_curX = kRTW * 0.5f, g_curY = kRTH * 0.5f;
static volatile long  g_lBtn = 0, g_rBtn = 0, g_wheel = 0;
static HHOOK g_llHook = nullptr;
static HANDLE g_inputThread = nullptr;
static HINSTANCE g_self = nullptr;
static POINT g_center = { 0, 0 };

static LRESULT CALLBACK LLMouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_visible) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)l;
        const bool injected = (m->flags & LLMHF_INJECTED) != 0;
        switch (w) {
            case WM_MOUSEMOVE:
                if (!injected) {
                    InterlockedExchange(&g_mouseMoved, 1);
                    const float s = 1.1f;
                    float nx = g_curX + (m->pt.x - g_center.x) * s;
                    float ny = g_curY + (m->pt.y - g_center.y) * s;
                    if (nx < 0) nx = 0; if (nx > kRTW) nx = kRTW;
                    if (ny < 0) ny = 0; if (ny > kRTH) ny = kRTH;
                    g_curX = nx; g_curY = ny;
                    SetCursorPos(g_center.x, g_center.y);
                }
                return 1;
            case WM_LBUTTONDOWN: InterlockedExchange(&g_lBtn, 1); return 1;
            case WM_LBUTTONUP:   InterlockedExchange(&g_lBtn, 0); return 1;
            case WM_RBUTTONDOWN: InterlockedExchange(&g_rBtn, 1); return 1;
            case WM_RBUTTONUP:   InterlockedExchange(&g_rBtn, 0); return 1;
            case WM_MOUSEWHEEL:  InterlockedExchangeAdd(&g_wheel, (short)HIWORD(m->mouseData)); return 1;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 1;
            default: break;
        }
    }
    return CallNextHookEx(g_llHook, code, w, l);
}

static DWORD WINAPI InputThread(LPVOID) {
    g_center.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    g_center.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    g_llHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc, g_self, 0);
    OLProxyLog(g_llHook ? "[OLVR][MENU] mouse hook installed - the cursor works inside the menu"
                        : "[OLVR][MENU] LL mouse hook FAILED - gamepad/keyboard only");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

void OLMenu_StartInput(void* hinst) {
    if (g_inputThread) return;
    g_self = (HINSTANCE)hinst;
    g_inputThread = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
}

// ---- gamepad ----------------------------------------------------------------------------------
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE*);
static XInputGetState_t g_xinput = nullptr;
static void LoadXInput() {
    const wchar_t* d[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (auto n : d) { HMODULE m = GetModuleHandleW(n); if (!m) m = LoadLibraryW(n);
        if (m) { g_xinput = (XInputGetState_t)GetProcAddress(m, "XInputGetState"); if (g_xinput) return; } }
}

// ---- init / lifecycle -------------------------------------------------------------------------
int  OLMenu_Visible() { return g_visible ? 1 : 0; }

static bool EnsureRT() {
    if (g_rtTex) return true;
    if (FAILED(g_dev->CreateTexture(kRTW, kRTH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &g_rtTex, nullptr))) { g_rtTex = nullptr; return false; }
    if (FAILED(g_rtTex->GetSurfaceLevel(0, &g_rtSurf))) { g_rtTex->Release(); g_rtTex = nullptr; return false; }
    return true;
}

static bool EnsureInit(IDirect3DDevice9* dev) {
    if (g_inited) return true;
    g_dev = dev;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = true;
    ApplyTheme();   // [MENUTHEME]
    // Typewriter-style faces that every Windows install has; ImGui's built-in font if not.
    g_fontMain = AddWinFont(L"consola.ttf", kFontPx);
    g_fontHead = AddWinFont(L"courbd.ttf", kFontPx);
    if (!g_fontMain) { io.FontGlobalScale = 2.0f; OLProxyLog("[OLVR][MENU] Consolas not found - default font"); }
    if (!ImGui_ImplDX9_Init(dev)) { OLProxyLog("[OLVR][MENU] ImGui_ImplDX9_Init FAILED"); ImGui::DestroyContext(); return false; }
    LoadXInput();
    if (!EnsureRT()) { OLProxyLog("[OLVR][MENU] offscreen RT create FAILED"); }
    g_inited = true;
    OLProxyLog("[OLVR][MENU] ImGui menu ready - move/resize the window, drag the sliders");
    return true;
}

void OLMenu_OnDeviceLost() {   // called from Hook_Reset BEFORE the game resets the device
    if (g_rtSurf) { g_rtSurf->Release(); g_rtSurf = nullptr; }
    if (g_rtTex)  { g_rtTex->Release();  g_rtTex  = nullptr; }
    if (g_inited) ImGui_ImplDX9_InvalidateDeviceObjects();
}
void OLMenu_OnDeviceReset() {   // called from Hook_Reset AFTER a successful reset
    if (g_inited) { ImGui_ImplDX9_CreateDeviceObjects(); EnsureRT(); }
}

void OLMenu_Toggle() {
    g_visible = !g_visible;
    if (g_visible && g_inited) g_openedAt = ImGui::GetTime();   // [MENUTHEME] the timecode
    if (g_visible) { g_curX = 2.0f; g_curY = 2.0f; InterlockedExchange(&g_mouseMoved, 0); }   // cursor parked
    if (g_visible) { InterlockedExchange(&g_lBtn, 0); InterlockedExchange(&g_rBtn, 0); ClipCursor(nullptr); }
    OLProxyLog(g_visible ? "[OLVR][MENU] open - mouse, keyboard or VR controllers (left grip + Menu closes)"
                         : "[OLVR][MENU] closed");
}
// Kept so the proxy's key thread still links; ImGui does its own nav now.
void OLMenu_Nav(int) {}
void OLMenu_Adjust(int) {}

// ---- input feed -------------------------------------------------------------------------------
static void PumpInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(g_curX, g_curY);
    io.AddMouseButtonEvent(0, g_lBtn != 0);
    io.AddMouseButtonEvent(1, g_rBtn != 0);
    long wh = InterlockedExchange(&g_wheel, 0);
    if (wh) io.AddMouseWheelEvent(0.0f, (float)wh / WHEEL_DELTA);

    // keyboard nav (the camera is frozen, so these are safe to read directly)
    io.AddKeyEvent(ImGuiKey_UpArrow,    (GetAsyncKeyState(VK_UP)     & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_DownArrow,  (GetAsyncKeyState(VK_DOWN)   & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_LeftArrow,  (GetAsyncKeyState(VK_LEFT)   & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_RightArrow, (GetAsyncKeyState(VK_RIGHT)  & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_Enter,      (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiKey_Space,      (GetAsyncKeyState(VK_SPACE)  & 0x8000) != 0);

    // Gamepad nav from a physical pad and/or the VR controllers ([TOUCHMENU]). ImGui ignores
    // every gamepad key unless the backend declares a gamepad - that flag was never set, which
    // is why pad navigation in this menu never did anything before Test33.
    bool up = false, dn = false, lf = false, rt = false, act = false, back = false, havePad = false;
    if (g_xinput) {
        XINPUT_STATE st; ZeroMemory(&st, sizeof(st));
        if (g_xinput(0, &st) == ERROR_SUCCESS) {
            const WORD b = st.Gamepad.wButtons;
            havePad = true;
            up   |= (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
            dn   |= (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            lf   |= (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            rt   |= (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            act  |= (b & XINPUT_GAMEPAD_A) != 0;
            back |= (b & XINPUT_GAMEPAD_B) != 0;
        }
    }
    // Either stick moves (whichever is pushed further), A or X selects / grabs a slider, B or Y
    // backs out. The game gets a neutral pad meanwhile (d3d9_proxy, Hook_XiGetState).
    OLXrPad vp;
    if (OLXR_MotionControlsReady() && OLXR_GetPad(&vp)) {
        havePad = true;
        const float sx = fabsf(vp.lookX) > fabsf(vp.moveX) ? vp.lookX : vp.moveX;
        const float sy = fabsf(vp.lookY) > fabsf(vp.moveY) ? vp.lookY : vp.moveY;
        up   |= sy >  0.55f;
        dn   |= sy < -0.55f;
        lf   |= sx < -0.55f;
        rt   |= sx >  0.55f;
        act  |= (vp.buttons & (OLXR_BTN_A | OLXR_BTN_X)) != 0;
        back |= (vp.buttons & (OLXR_BTN_B | OLXR_BTN_Y)) != 0;
    }
    if (havePad) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else         io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  dn);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  lf);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, rt);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  act);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, back);
}

// ---- [MENUTHEME] the camcorder's night-vision viewfinder, with a pumpkin-orange accent ----------
// Test35: Outlast's own look instead of stock ImGui grey. Pale night-vision green on near-black,
// typewriter headings (the asylum's paperwork), a blinking REC light, battery and timecode like the
// camcorder HUD, viewfinder corner marks and faint scanlines - and October orange for section
// titles and for whatever the controller is pointing at.

static void ApplyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowPadding = ImVec2(28, 20); st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(12, 10); st.ItemInnerSpacing = ImVec2(10, 6);
    st.GrabMinSize = 18; st.ScrollbarSize = 16; st.TabBarBorderSize = 2;
    st.WindowRounding = 3; st.ChildRounding = 2; st.FrameRounding = 2; st.GrabRounding = 2;
    st.TabRounding = 2; st.PopupRounding = 2; st.ScrollbarRounding = 2;
    st.WindowBorderSize = 2; st.FrameBorderSize = 1; st.TabBorderSize = 1;
    ImVec4* c = st.Colors;
    const ImVec4 green(0.55f, 1.00f, 0.50f, 1.00f), pumpkin(1.00f, 0.55f, 0.12f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.80f, 0.94f, 0.76f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.47f, 0.64f, 0.47f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.022f, 0.040f, 0.026f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.03f, 0.06f, 0.04f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.36f, 0.64f, 0.34f, 0.65f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.06f, 0.11f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.11f, 0.20f, 0.11f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.16f, 0.28f, 0.14f, 1.00f);
    c[ImGuiCol_CheckMark]            = green;
    c[ImGuiCol_SliderGrab]           = ImVec4(0.45f, 0.85f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = pumpkin;
    c[ImGuiCol_Button]               = ImVec4(0.09f, 0.17f, 0.09f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.18f, 0.32f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.62f, 0.32f, 0.06f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.11f, 0.20f, 0.11f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.18f, 0.32f, 0.16f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.62f, 0.32f, 0.06f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.30f, 0.52f, 0.28f, 0.55f);
    c[ImGuiCol_Tab]                  = ImVec4(0.06f, 0.11f, 0.07f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.22f, 0.38f, 0.19f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.15f, 0.29f, 0.14f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.05f, 0.09f, 0.06f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.12f, 0.22f, 0.11f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.02f, 0.04f, 0.03f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.20f, 0.36f, 0.18f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.48f, 0.25f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = pumpkin;
    c[ImGuiCol_NavCursor]            = pumpkin;    // the controller's selection is orange
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.62f, 0.32f, 0.06f, 0.55f);
}

static ImFont* AddWinFont(const wchar_t* file, float px) {
    wchar_t dir[MAX_PATH]; if (!GetWindowsDirectoryW(dir, MAX_PATH)) return nullptr;
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\Fonts\\%s", dir, file);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    char utf8[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8, px);
}

static void Help(const char* text) {
    if (ImGui::IsItemFocused() || (g_mouseMoved && ImGui::IsItemHovered())) g_helpText = text;
}

static void Section(const char* title) {
    ImGui::Spacing();
    if (g_fontHead) ImGui::PushFont(g_fontHead, kFontPx * 0.92f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kPumpkin));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (g_fontHead) ImGui::PopFont();
}

static void CamcorderHeader() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const double t = ImGui::GetTime();
    const float lineH = kFontPx * 1.25f;
    // REC light, blinking like the camcorder's
    const bool lit = fmod(t, 1.4) < 0.9;
    dl->AddCircleFilled(ImVec2(p.x + 13, p.y + lineH * 0.5f), 10.0f, lit ? kRecRed : IM_COL32(70, 18, 12, 255));
    ImFont* hf = g_fontHead ? g_fontHead : ImGui::GetFont();
    dl->AddText(hf, kFontPx, ImVec2(p.x + 32, p.y + 2), kRecRed, "REC");
    dl->AddText(hf, kFontPx * 1.08f, ImVec2(p.x + 112, p.y), kNvGreen, "OUTLAST VR");
    // battery (right): outline, nub, three bars
    const float bw = 58, bh = 26, bx = p.x + w - 210, by = p.y + (lineH - bh) * 0.5f;
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), kNvGreen, 2.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(bx + bw, by + 8), ImVec2(bx + bw + 5, by + bh - 8), kNvGreen);
    for (int i = 0; i < 3; ++i)
        dl->AddRectFilled(ImVec2(bx + 5 + i * 17.0f, by + 5), ImVec2(bx + 18 + i * 17.0f, by + bh - 5), kNvGreen);
    // timecode since the menu opened
    const int secs = (int)(t - g_openedAt);
    char tc[16]; sprintf_s(tc, "%02d:%02d:%02d", (secs / 3600) % 100, (secs / 60) % 60, secs % 60);
    dl->AddText(g_fontMain ? g_fontMain : ImGui::GetFont(), kFontPx, ImVec2(p.x + w - 136, p.y + 2), kNvGreen, tc);
    ImGui::Dummy(ImVec2(w, lineH));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.42f, 0.25f, 1.0f));
    ImGui::TextUnformatted("MOUNT MASSIVE ASYLUM  //  VR SETTINGS");
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// Drawn last, over the whole panel: viewfinder corner marks and faint scanlines.
static void ViewfinderOverlay() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    const ImVec2 b(a.x + sz.x, a.y + sz.y);
    dl->PushClipRect(a, b, false);
    for (float y = a.y + 2; y < b.y; y += 4.0f)
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(140, 255, 140, 8));
    const float L = 38, m = 8, th = 3;
    const ImU32 c = IM_COL32(150, 240, 140, 200);
    dl->AddLine(ImVec2(a.x + m, a.y + m), ImVec2(a.x + m + L, a.y + m), c, th);
    dl->AddLine(ImVec2(a.x + m, a.y + m), ImVec2(a.x + m, a.y + m + L), c, th);
    dl->AddLine(ImVec2(b.x - m, a.y + m), ImVec2(b.x - m - L, a.y + m), c, th);
    dl->AddLine(ImVec2(b.x - m, a.y + m), ImVec2(b.x - m, a.y + m + L), c, th);
    dl->AddLine(ImVec2(a.x + m, b.y - m), ImVec2(a.x + m + L, b.y - m), c, th);
    dl->AddLine(ImVec2(a.x + m, b.y - m), ImVec2(a.x + m, b.y - m - L), c, th);
    dl->AddLine(ImVec2(b.x - m, b.y - m), ImVec2(b.x - m - L, b.y - m), c, th);
    dl->AddLine(ImVec2(b.x - m, b.y - m), ImVec2(b.x - m, b.y - m - L), c, th);
    dl->PopClipRect();
}

// ---- the widget tree --------------------------------------------------------------------------
static void BuildUI() {
    ImGuiIO& io = ImGui::GetIO();
    // [MENUTHEME] One fixed, centred panel. It is shown on the theater screen now (see
    // SceneWantsTheater in ol_ue3.cpp), so it no longer has to fit a corner of a head-locked eye.
    g_firstLayout = false;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.66f, io.DisplaySize.y * 0.95f), ImGuiCond_Always);
    ImGui::Begin("Outlast VR", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.44f);   // room for the labels

    // WHAT IS HIDDEN AND WHY. Every setting below still exists, still loads and still saves;
    // it just is not shown, because a player opening this in a headset should see the handful
    // of things worth touching and nothing else. Hidden: positional tracking, head roll and the
    // camcorder light toggle (all correct at their defaults, all confusing to explain), the
    // UI-in-both-eyes mode (wrong on any setting but "on"), world scale (only affects
    // positional, which is off) and the cinema screen (its whole point is to be off).
    CamcorderHeader();
    // Pages are a fixed height so the Save button never moves; long pages scroll and
    // controller navigation scrolls them for you.
    g_helpText = nullptr;
    const float pageH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() -
                        ImGui::GetTextLineHeightWithSpacing() * 4.4f;
    if (ImGui::BeginTabBar("##pages")) {
        if (ImGui::BeginTabItem("VIEW")) {
            ImGui::BeginChild("##page_VIEW", ImVec2(0, pageH), ImGuiChildFlags_None);
    Section("3D DEPTH");
    // The stereo model picker is NOT shown. The Mass Effect model won the live comparison and is
    // the default; the old Outlast model survives only as an ini escape hatch ([Render]
    // StereoModel=0) because it is the history of this lane. It is not a choice worth putting in
    // front of a player: it costs field of view whenever focus distance moves, and its Swap
    // inverts the stereo outright.
    // Separation is shown against a real eye spacing, because "3.5 uu" means nothing in a
    // headset while "0.9x your eyes" is something you can picture. The scale is the MEASURED
    // 100 uu/m ([LEAN100] - eye height 168 uu, walk speed, and the tuned depth landing at the
    // measured real IPD all agree), which is why a separation of 3.30 reads as ~1x: the stereo
    // was tuned to true scale by eye, without the number being derived on purpose.
    { float v = OLUE3_GetWorldSize() * 100.0f;   // (Beta 1.1) [WORLDSIZE] replaces the raw "Depth" slider
      char lbl[64]; sprintf_s(lbl, "%.0f%%  (eyes %.1f cm apart in the game)", v, OLUE3_GetEyeSpacingCm());
      if (ImGui::SliderFloat("World size", &v, 60.0f, 150.0f, lbl)) OLUE3_SetWorldSize(v * 0.01f);
      Help(
          "100% = true size: the eyes are as far apart as yours (from the headset).\n"
          "Things look too big or 'zoomed in'? Lower it. Too small? Raise it.\n"
          "Head movement scales with it."); }
    { float v = OLXR_GetConvergence();
      if (ImGui::SliderFloat("Focus distance", &v, -3.0f, 3.0f, "%.2f")) OLXR_SetConvergence(v);
      Help(
          "Sets how far away things sit before they look flat.\n"
          "If close objects look doubled or hard to look at, raise this a little."); }
    { bool b = OLUE3_GetEyeSwap() != 0;
      if (ImGui::Checkbox("Swap left and right eye", &b)) OLUE3_SetEyeSwap(b);
      Help(
          "Only needed if the 3D looks inside out, with near things\n"
          "appearing far away."); }
    // Lean distance (world scale) stays out of the menu: it is derived from the depth slider now,
    // so there is nothing to tune. Positional is BACK (2026-08-17) as one checkbox, because the
    // thing that made it wrong - measuring from wherever the headset lay when the game started -
    // is fixed underneath ([HEADPOS] in ol_xr.cpp), not papered over with a slider.

    Section("HEAD TRACKING");
    { bool b = OLUE3_GetHeadTrack() != 0;
      if (ImGui::Checkbox("Look around with your head", &b)) OLUE3_SetHeadTrack(b); }
    // [LEAN100] Camera lean at the measured world scale (100 uu/m, from the CamProbe run).
    // Gain 1.0 keeps declared == rendered exactly - the setting that finally made it stable -
    // so the slider is a taste control around truth, not a sensitivity hack.
    { bool b = OLUE3_GetHeadPos() != 0;
      if (ImGui::Checkbox("Lean and duck with your body", &b)) OLUE3_SetHeadPos(b);
      Help(
          "Moving your head moves the camera: lean round a corner, duck under a table.\n"
          "Recenter once you are sat comfortably, and again any time it feels off.");
      if (b) {
          ImGui::SameLine();
          if (OLXR_PosArmed()) ImGui::TextDisabled("ready");
          else ImGui::TextDisabled("recenter to start");
          float g = OLUE3_GetLeanGain();
          if (ImGui::SliderFloat("Lean amount", &g, 0.5f, 2.0f, "%.2f")) OLUE3_SetLeanGain(g);
          Help(
              "1.00 moves the camera exactly as far as you move, which is also the\n"
              "most stable setting. Change it only if you want less or more.");
      } }
    { float v = OLXR_GetSmoothing();
      if (ImGui::SliderFloat("Steadying", &v, 0.0f, 0.9f, v <= 0.001f ? "off" : "%.2f")) OLXR_SetSmoothing(v);
      Help(
          "Settles small shakes while you hold still, and gets out of the way\n"
          "the moment you actually turn your head. Turn it down if looking\n"
          "around ever feels slow to respond."); }

    Section("COMFORT");
    { bool b = OLMotion_GetPhysCrouch() != 0;
      if (ImGui::Checkbox("Crouch when you duck", &b)) OLMotion_SetPhysCrouch(b ? 1 : 0);
      Help(
          "Duck for real and Miles crouches, stand up and he stands.\n"
          "Needs \"Lean and duck with your body\" and a recenter while standing\n"
          "or sitting upright. B still crouches as before.");
      if (b) {
          const int cs = OLMotion_CrouchStatus();
          ImGui::SameLine();
          ImGui::TextDisabled(cs == -2 ? "unavailable (see log)" : cs < 0 ? "active in gameplay" : (cs ? "crouched" : "standing"));
          float d = OLMotion_GetCrouchDepth();
          if (ImGui::SliderFloat("How far to duck", &d, 0.15f, 0.60f, "%.2f m")) OLMotion_SetCrouchDepth(d);
      } }
    { float v = OLUE3_GetBobReduce();
      if (ImGui::SliderFloat("Reduce head bob", &v, 0.0f, 1.0f, v <= 0.001f ? "off" : "%.2f")) OLUE3_SetBobReduce(v);
      Help(
          "1.00 = no head bob walking or running: no bounce, rocking or sway.\n"
          "off = the game's own bob. Stairs, jumps and falls still move you normally."); }
    { bool b = OLUE3_GetLevelHorizon() != 0;
      if (ImGui::Checkbox("Keep the horizon level", &b)) OLUE3_SetLevelHorizon(b ? 1 : 0);
      Help(
          "Removes the game's own camera tilt during play. Your head still tilts freely."); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MOVE")) {
            ImGui::BeginChild("##page_MOVE", ImVec2(0, pageH), ImGuiChildFlags_None);
    // [PHASE1] Turning and walking (ol_motion.cpp). All of it rewrites the virtual pad only, so the
    // game's own movement rules still apply, and none of it acts outside ordinary gameplay.
    Section("TURNING AND WALKING");
    { static const char* kTurn[] = { "Smooth", "Snap" };
      int t = OLMotion_GetTurnMode();
      if (ImGui::Combo("Turning", &t, kTurn, 2)) OLMotion_SetTurnMode(t);
      Help(
          "Snap: flick the right stick to turn a fixed angle at once.\n"
          "Smooth: hold the stick to turn continuously.");
      if (t == 1) {
          int d = OLMotion_GetSnapDeg();
          if (ImGui::SliderInt("Snap angle", &d, 15, 90, "%d degrees")) OLMotion_SetSnapDeg((d + 7) / 15 * 15);
          bool b = OLMotion_GetSnapBlink() != 0;
          if (ImGui::Checkbox("Blink while snapping", &b)) OLMotion_SetSnapBlink(b ? 1 : 0);
          Help(
              "The view goes dark for the moment the turn takes, instead of\n"
              "showing the room sweep past.");
      } else {
          int s = OLMotion_GetTurnDegPerSec();
          if (ImGui::SliderInt("Turn speed", &s, 30, 260, "%d degrees a second")) OLMotion_SetTurnDegPerSec(s >= 255 ? 260 : (s + 7) / 15 * 15);
      } }
    { static const char* kWalk[] = { "Body (classic)", "Where you look", "Where your left hand points" };
      int w = OLMotion_GetWalkDir();
      if (ImGui::Combo("Walk toward", &w, kWalk, 3)) OLMotion_SetWalkDir(w);
      Help(
          "Which way pushing the left stick forward walks.\n"
          "Body lets you run one way while looking back - Outlast's chases.\n"
          "Left hand lets you look anywhere while you steer with your hand."); }
    { bool b = OLMotion_GetStickPitch() != 0;
      if (ImGui::Checkbox("Right stick tilts the view up and down", &b)) OLMotion_SetStickPitch(b ? 1 : 0);
      Help(
          "Off: only your head looks up and down, the stick just turns.\n"
          "Camcorder zoom on the stick is unaffected."); }
    { bool b = OLUE3_GetBodyTurn() != 0;   // (Beta 1.1) [BODYTURN]
      if (ImGui::Checkbox("Body turns with you", &b)) OLUE3_SetBodyTurn(b);
      Help(
          "Turn your own body (or your chair) and after a moment Miles turns to face\n"
          "the same way - the view does not move, his arms and the hip holster line up\n"
          "with yours again. No more recentering for that. Off: only a recenter does it."); }
    // (Test44) [ROOMWALK]
    { bool b = OLInteract_Get(11) != 0;
      if (ImGui::Checkbox("Walk around for real", &b)) OLInteract_Set(11, b);
      Help(
          "Step around your room and Miles's body walks with you. Walls and shut\n"
          "doors stop him the way they stop the stick, and he steps up low steps.\n"
          "Small leans stay leans. Off: the body stays put and only your head leans."); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("HANDS")) {
            ImGui::BeginChild("##page_HANDS", ImVec2(0, pageH), ImGuiChildFlags_None);
    // [ARMTUNE] / [HAPTICS] Test33. One switch for the three arm corrections so a player can A/B
    // them against Test16 in one press; the ini keeps them individually for diagnosis.
    Section("ARMS AND CONTROLLERS");
    { const int state = OLUE3_GetArmImproved();
      bool b = state != 0;
      if (ImGui::Checkbox(state == 2 ? "Improved arm tracking (partly on)" : "Improved arm tracking", &b))
          OLUE3_SetArmImproved(b ? 1 : 0);
      Help(
          "Keeps your hands where they really are when you lean, and steadies them\n"
          "the same at any frame rate. Untick for the original Test16 arms, to compare."); }
    { static const char* kUse[] = { "Where Miles faces (classic)", "Where you look", "Where your right hand points" };
      int u = OLUE3_GetLookUse();
      if (ImGui::Combo("Interact with", &u, kUse, 3)) OLUE3_SetLookUse(u);
      Help(
          "Which way the game checks for things to pick up, open or hide in when you press X.");
      if (!OLUE3_LookUseReady()) { ImGui::SameLine(); ImGui::TextDisabled("(unavailable on this build of the game)"); } }
    { bool b = OLXR_GetHaptics() != 0;
      if (ImGui::Checkbox("Controller vibration", &b)) OLXR_SetHaptics(b ? 1 : 0);
      Help("The game's own gamepad rumble, felt in both hands.");
      if (b) {
          float v = OLXR_GetHapticStrength();
          if (ImGui::SliderFloat("Vibration strength", &v, 0.0f, 1.5f, "%.2f")) OLXR_SetHapticStrength(v);
      } }

    // [HANDCAM] The camcorder IS the game's camera, so aiming the camera from the controller is
    // aiming the camcorder. Hold the right grip and you are holding it.
    Section("CAMCORDER");
    { int m = OLInteract_CamButton() ? 1 : 0;   // (Test49) [CAMBUTTON]
      static const char* kCtl[] = { "Hands-on - raise it to your eye", "Grip button - rest your hand" };
      if (ImGui::Combo("Camcorder controls", &m, kCtl, 2)) OLInteract_SetCamButton(m);
      Help(
          "Hands-on: take the camcorder from your right hip and bring it to your eye.\n"
          "Grip button: squeeze the right grip once to raise it, again to lower it. You\n"
          "look through it with your head, so your hand can rest - for seated play or\n"
          "arms that tire. The right grip then never grabs doors or edges."); }
    if (!OLInteract_CamButton()) {   // the hands-on camcorder settings
    { bool b = OLXR_GetHandCam() != 0;
      if (ImGui::Checkbox("Hold it in your hand", &b)) OLXR_SetHandCam(b);
      Help(
          "Squeeze the right grip to raise the camcorder. While you hold it, the\n"
          "view looks through it, so pointing the controller aims the camera.");
      if (b) {
          float s = OLXR_GetHandCamStrength();
          if (ImGui::SliderFloat("How much it takes over", &s, 0.0f, 1.0f, "%.2f"))
              OLXR_SetHandCamStrength(s);
          Help(
              "1.00 aims entirely with your hand. Lower it to keep some of your\n"
              "head aim mixed in, which is steadier while walking.");
      } }
    { bool b = OLInteract_Get(6) != 0;
      if (ImGui::Checkbox("Camcorder on your hip", &b)) OLInteract_Set(6, b);
      Help(
          "Reach to your right hip and squeeze the grip: the camcorder glides into\n"
          "your hand. Bring it to your eye to look through it. Let go and it goes back.\n"
          "Off: lift your right hand to your eye, or squeeze the right grip."); }
    { bool b = OLInteract_GetBatteryGauge() != 0;   // (Beta 1.1) [BATGAUGE]
      if (ImGui::Checkbox("Large battery gauge while filming", &b)) OLInteract_SetBatteryGauge(b);
      Help(
          "While the camcorder is up, a big battery gauge sits low in your view,\n"
          "with the number of spare batteries beside it."); }
    // (Test43) [CAMHOLD] free hold, [FLIPSCREEN]
    { int hold = OLInteract_CamHold() == 3 ? 1 : 0;
      static const char* kHold[] = { "Free - moves with your controller", "The game's pose" };
      if (ImGui::Combo("Camcorder in your hand", &hold, kHold, 2)) OLInteract_SetCamHold(hold ? 3 : 4);
      Help(
          "Free: it moves and turns exactly with your controller, like holding a real\n"
          "camcorder, its flip screen open toward you. The game's pose: held the way\n"
          "Miles holds it as he raises it; turning the controller does not turn it."); }
    // (Test45) [CAMADJUST] the player places it
    { float r[3], o[3]; OLInteract_GetCamAdjust(r, o);
      bool ch = false;
      ch |= ImGui::SliderFloat("Camcorder tilt", &r[0], -90.0f, 90.0f, "%.0f deg");
      ch |= ImGui::SliderFloat("Camcorder turn", &r[1], -90.0f, 90.0f, "%.0f deg");
      ch |= ImGui::SliderFloat("Camcorder roll", &r[2], -90.0f, 90.0f, "%.0f deg");
      ch |= ImGui::SliderFloat("Camcorder forward", &o[0], -25.0f, 25.0f, "%.0f cm");
      ch |= ImGui::SliderFloat("Camcorder up", &o[1], -25.0f, 25.0f, "%.0f cm");
      ch |= ImGui::SliderFloat("Camcorder right", &o[2], -25.0f, 25.0f, "%.0f cm");
      if (ch) OLInteract_SetCamAdjust(r, o);
      if (ImGui::Button("Reset the camcorder's position")) { const float z[3] = { 0.0f, 0.0f, 0.0f }; OLInteract_SetCamAdjust(z, z); }
      Help(
          "Where the camcorder sits in your hand. Tilt raises or lowers the lens, turn\n"
          "swings it left or right, roll twists it; forward / up / right move your hand\n"
          "and the camcorder together. Close the menu to see it, adjust again, and\n"
          "save. Zero everywhere: the lens points where your controller points."); }
    if (false) { bool b = OLInteract_Get(9) != 0;   // (Test46c) the live screen is out for now ([Motion] CamcorderLiveScreen)
      if (ImGui::Checkbox("Live camcorder screen", &b)) OLInteract_Set(9, b);
      Help(
          "The flip-out screen shows what the lens sees, like a real camcorder: point\n"
          "it behind you, round a corner, at yourself. Click the right stick while you\n"
          "hold the camcorder for night vision on the screen. It is a second camera,\n"
          "drawn only while the screen faces you; off saves a little speed."); }
    if (!OLInteract_Get(6)) {
      bool b = OLInteract_Get(1) != 0;
      if (ImGui::Checkbox("Raise it by lifting your hand to your eye", &b)) OLInteract_Set(1, b);
      Help(
          "Lift your right hand up to your eye and the camcorder comes up; drop it\n"
          "and it goes down. The right grip still toggles it too."); }
    }   // (Test49) end of the hands-on camcorder settings
    { bool b = OLInteract_Get(12) != 0;   // (Test46)
      if (ImGui::Checkbox("Zoom in a framed viewfinder (black around it)", &b)) OLInteract_Set(12, b);
      Help(
          "Off (default): when you zoom, the zoomed picture fills your view.\n"
          "On: it sits in a camcorder-shaped window with black around it."); }
    { bool b = OLInteract_Get(3) != 0;
      if (ImGui::Checkbox("Zoom in a viewfinder", &b)) OLInteract_Set(3, b);
      Help(
          "Right stick up/down zooms. Zoomed, the picture sits in a camcorder\n"
          "viewfinder in front of you, dark around it, like holding it to your eye."); }
    { bool b = OLInteract_Get(2) != 0;
      if (ImGui::Checkbox("Reload with your hands (off: Y reloads)", &b)) OLInteract_Set(2, b);
      Help(
          "Y reloads the camcorder. On: with the camcorder in your right hand, pull\n"
          "the old battery out of its back with your left hand, take a new one from\n"
          "your left hip and slide it in."); }
    { bool b = OLInteract_Get(7) != 0;
      if (ImGui::Checkbox("How-to labels", &b)) OLInteract_Set(7, b);
      Help(
          "Labels in front of you show what to do next (take the camcorder, pull out\n"
          "the battery...). Each one stops after you have done it 3 times.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Show them again")) OLInteract_ResetHints();
      Help("Every how-to label shows again until you have done that action 3 more times."); }

    // [INTERACT] (Test37)
    Section("DOORS");
    { bool b = OLInteract_Get(0) != 0;
      if (ImGui::Checkbox("Hands-on doors", &b)) OLInteract_Set(0, b);
      Help(
          "Doors are heavy: push one with your open hand and it swings as far as you\n"
          "push (a shove swings it on a little), or squeeze the grip on its handle and\n"
          "swing it. X still opens doors too."); }
    // [CLIMB] (Test40)
    Section("CLIMBING");
    { int cm = OLInteract_ClimbMode();
      static const char* kClimb[] = { "A button (the game's climb)", "Guided (the game's climb, your pace)", "Physical (your hands carry Miles)" };
      if (ImGui::Combo("Climbing", &cm, kClimb, 3)) OLInteract_SetClimbMode(cm);
      Help(
          "Physical: squeeze a grip on an edge or a ladder rung and that hand holds on -\n"
          "pull down and Miles rises, move it sideways and he shimmies, hand over hand.\n"
          "Get the edge to your chest and he hauls himself over; let go of everything\n"
          "and he drops. Guided: the game's own climb, playing only as fast as you pull.\n"
          "A button: the old way. A climbs in every mode."); }
    { bool b = OLInteract_Get(13) != 0;   // (Beta 1.1) [AUTOLEDGE]
      if (ImGui::Checkbox("Catch ledges automatically when jumping", &b)) OLInteract_Set(13, b);
      Help(
          "Jump toward a ledge and Miles grabs it by himself, like the original game.\n"
          "Then pull down with both grips (or press A) to climb up, B to drop.\n"
          "Off: in the air your hands take the edge only when you squeeze a grip."); }
    // (Test45) cover-your-mouth removed from the menu ([Motion] CoverMouth=1 still turns it on)
    Section("DEVELOPER");
    { bool b = OLInteract_Get(4) != 0;
      if (ImGui::Checkbox("Show interaction markers (developer)", &b)) OLInteract_Set(4, b);
      Help(
          "Rings for every grab zone: knobs (green open, red locked, yellow in reach),\n"
          "the hip pouch (orange), the raise-to-eye spot (cyan)."); }
    { bool b = OLInteract_Get(5) != 0;
      if (ImGui::Checkbox("Hints name your controller buttons", &b)) OLInteract_Set(5, b);
      Help(
          "The game's hints name the active controller buttons (including Valve Index) instead of showing\n"
          "Xbox buttons: \"Grab the handle to open, or press X\", \"hold down the left grip\"."); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("PICTURE")) {
            ImGui::BeginChild("##page_PICTURE", ImVec2(0, pageH), ImGuiChildFlags_None);
    // [HIRES] How many pixels the game draws for each eye. Changing it resizes the game's
    // render (a brief hitch), so the slider applies once it has been left alone for a moment.
    Section("RESOLUTION");
    if (OLUE3_HiResActive()) {
        static float s_pending = -1.0f; static double s_changedAt = 0.0;
        float s = s_pending > 0.0f ? s_pending : OLUE3_GetRenderScale();
        long tw = 0, th = 0; OLUE3_GetHiResTarget(&tw, &th);
        char lbl[64];
        if (s_pending > 0.0f) sprintf_s(lbl, "%.0f%%%% (applying...)", s * 100.0f);
        else sprintf_s(lbl, "%.0f%%%% (%ld x %ld per eye)", s * 100.0f, tw / 2, th);
        if (ImGui::SliderFloat("Render resolution", &s, 0.50f, 1.20f, lbl)) {
            s_pending = floorf(s * 20.0f + 0.5f) / 20.0f;   // 5% steps
            s_changedAt = ImGui::GetTime();
        }
        if (s_pending > 0.0f && ImGui::GetTime() - s_changedAt > 0.8 && !ImGui::IsItemActive()) {
            OLUE3_SetRenderScale(s_pending);
            s_pending = -1.0f;
        }
        Help(
            "How many pixels the game draws for each eye. 100% is what the headset\n"
            "asks for. Lower it if the game stutters; raise it if your PC has headroom.");
    } else {
        ImGui::TextDisabled("High-resolution rendering is off ([Render] HighResRender=0).");
    }

    // [UPSCALE] How the game's picture is enlarged to the headset's resolution.
    Section("PICTURE");
    { static const char* kUp[] = { "Classic (Test 28)", "Sharp" };
      int v = OLXR_GetUpscaler();
      if (ImGui::Combo("Upscaling", &v, kUp, 2)) OLXR_SetUpscaler(v);
      Help(
          "Sharp: bicubic enlargement with adaptive sharpening.\n"
          "Classic: the earlier bilinear enlargement, for comparison.\n"
          "Matters most when Render resolution is below 100%."); }
    { float s = OLXR_GetSharpness();
      if (ImGui::SliderFloat("Sharpening", &s, 0.0f, 1.0f, s <= 0.001f ? "off" : "%.2f")) OLXR_SetSharpness(s);
      Help("Raise for crisper edges; lower if edges start to sparkle."); }
    { bool b = OLUE3_GetWideFov() != 0;
      if (ImGui::Checkbox("Wide view", &b)) OLUE3_SetWideFov(b ? 1 : 0);
      Help(
          "On: the game fills the headset to its edges, slightly softer.\n"
          "Off: a sharper centre, with dark edges at the far sides."); }
    { bool b = OLUE3_GetFilmGrain() != 0;   // (Test47) [GRAIN]
      if (ImGui::Checkbox("Film grain", &b)) OLUE3_SetFilmGrain(b ? 1 : 0);
      Help(
          "Outlast's film grain. Off: a clean picture - in a headset the grain\n"
          "reads as blur. On: the game's own found-footage look."); }

    // The camcorder readouts (battery, record dot, timecode) are drawn at the very edges of the
    // frame, which a headset lens cannot show. These pull them into the readable middle. Uniform
    // scale, so the display keeps its own proportions: stretching UI to fit is what makes it look
    // wrong. Only the mirrored UI viewport is touched, so nothing here reaches the 3D path.
    Section("CAMCORDER DISPLAY");
    { float s = 1.0f, ox = 0.0f, oy = 0.0f; OLProxy_GetHudFit(&s, &ox, &oy);
      bool ch = false;
      if (ImGui::SliderFloat("Size", &s, 0.40f, 1.00f, "%.2f")) ch = true;
      Help(
          "Cannot see the battery meter or the record light? Lower this.\n"
          "It pulls everything in from the corners, where the lens cuts it off.");
      if (ImGui::SliderFloat("Move left or right", &ox, -0.40f, 0.40f, "%.2f")) ch = true;
      if (ImGui::SliderFloat("Move up or down",    &oy, -0.40f, 0.40f, "%.2f")) ch = true;
      Help("Slide right to move it down.");
      if (ch) OLProxy_SetHudFit(s, ox, oy); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SETUP")) {
            ImGui::BeginChild("##page_SETUP", ImVec2(0, pageH), ImGuiChildFlags_None);
    // RECENTER. The one thing a player needs mid-game without opening anything: sit differently,
    // drift off centre, put the world back where you are facing.
    Section("RECENTER");
    if (ImGui::Button("Recenter now", ImVec2(-1, 0))) OLXR_Recenter();
    { const int cap = OLProxy_GetBindCapture();
      char lbl[96];
      sprintf_s(lbl, "Keyboard: %s", OLProxy_KeyName(OLProxy_GetRecenterKey()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 1) ImGui::TextDisabled("press any key...");
      else if (ImGui::SmallButton("Change key")) OLProxy_StartBindCapture(1);

      sprintf_s(lbl, "Controller: double tap %s", OLProxy_PadName(OLProxy_GetRecenterPad()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 2) ImGui::TextDisabled("press any button...");
      else if (ImGui::SmallButton("Change button")) OLProxy_StartBindCapture(2);
      Help(
          "Two quick taps, so it cannot happen by accident while you are running."); }

    // The key that opens THIS menu. Worth rebinding for anyone on a keyboard without an INSERT
    // key, which is most compact and laptop boards.
    Section("THIS MENU");
    { const int cap = OLProxy_GetBindCapture();
      char lbl[96];
      sprintf_s(lbl, "Opens with: %s", OLProxy_KeyName(OLProxy_GetMenuKey()));
      ImGui::Text("%s", lbl);
      ImGui::SameLine();
      if (cap == 3) ImGui::TextDisabled("press any key...");
      else if (ImGui::SmallButton("Change##menukey")) OLProxy_StartBindCapture(3);
      Help(
          "Change this if your keyboard has no INSERT key.");
      ImGui::TextDisabled("VR controllers: hold the left grip and press Menu"); }

    // THE PICTURE SECTION IS GONE ON PURPOSE (2026-08-03). The brightness control blacked out
    // the headset in one small drag, and a setting that can stop you seeing the menu that would
    // undo it has no business in front of a player. It is not merely hidden here: the value can
    // no longer be reached from ANYWHERE. The menu row is gone, it is no longer read from the
    // ini, it is no longer written to the ini, and the reset hotkey is gone with it, so the
    // brightness is permanently the untouched picture the game rendered and the correction
    // shader is bypassed entirely. Cutscenes in full VR lived here too and is simply always on,
    // which is what it should have been.
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    // One button, so nothing tuned while wearing the headset is ever lost or has to be
    // written down and typed back in afterwards.
    if (ImGui::Button("Save these settings", ImVec2(-1, 0))) {
        g_savedOk = OLProxy_SaveSettings();
        g_savedFrames = 180;   // the confirmation stays up for a few seconds
    }
    if (g_savedFrames > 0) {
        --g_savedFrames;
        ImGui::TextDisabled(g_savedOk ? "Saved. The game will start with these from now on."
                                      : "Could not save. The settings file is not writable.");
    }

    // Help box: the selected setting's explanation, smaller type, always the same place.
    ImGui::Separator();
    if (g_fontMain) ImGui::PushFont(g_fontMain, kFontPx * 0.78f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.80f, 0.62f, 1.0f));
    ImGui::TextUnformatted(g_helpText ? g_helpText : "Select a setting to see what it does.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (g_fontMain) ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                         ImGui::GetTextLineHeight());
    ImGui::TextDisabled("Close: left grip + Menu   A: select   B: back");
    ImGui::PopItemWidth();
    ViewfinderOverlay();
    ImGui::End();
}

// ---- draw the offscreen target into both eyes -------------------------------------------------
struct MenuVtx { float x, y, z, rhw; DWORD col; float u, v; };
static void DrawQuad(IDirect3DDevice9* dev, float x, float y, float w, float h) {
    MenuVtx v[4] = {
        { x,     y,     0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { x + w, y,     0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { x,     y + h, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { x + w, y + h, 0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVtx));
}

void OLMenu_Render(IDirect3DDevice9* dev, unsigned bbW, unsigned bbH) {
    if (!g_visible || !dev || bbW < 64 || bbH < 64) return;
    if (!EnsureInit(dev)) return;
    if (!g_rtTex && !EnsureRT()) return;
    ClipCursor(nullptr);

    // 1) render the ImGui frame into the offscreen target
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_mouseMoved != 0;   // no stray arrow in the headset
    io.DisplaySize = ImVec2((float)kRTW, (float)kRTH);
    io.DeltaTime = 1.0f / 60.0f;
    PumpInput();
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::EndFrame();
    ImGui::Render();

    IDirect3DSurface9* oldRT = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->SetRenderTarget(0, g_rtSurf))) {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);   // transparent surround
        if (SUCCEEDED(dev->BeginScene())) { ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); dev->EndScene(); }
    }
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }

    // 2) draw that target into the backbuffer, once per eye, blended (transparent surround shows game)
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, g_rtTex);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const float rtAspect = (float)kRTW / (float)kRTH;
    const bool split = (OLUE3_StereoMode() == 1) && OLUE3_SplitActive() != 0;
    if (split) {
        const float halfW = (float)bbW * 0.5f;
        // Size the panel so it reads 4:3 ANGULARLY in the headset, and FITS inside the eye that
        // is actually shown. The submitted eye is g_eyeW x g_eyeH px spanning fovX x fovY - and
        // g_eyeW is the CONVERGENCE-CROPPED width (1728, not the 1920 half). The old formula sized
        // against the full half and then clamped, so the panel overflowed the crop, its sides got
        // cut, and the content looked squashed horizontally. A backbuffer quad of w x h px maps
        // 1:1 through the crop, so its angular aspect is
        //   (w/g_eyeW * tan(fovX/2)) / (h/bbH * tan(fovY/2));  solve that == rtAspect.
        int ew = 0, eh = 0; float fdx = 0.0f, fdy = 0.0f;
        OLXR_GetEyeInfo(&ew, &eh, &fdx, &fdy);                 // ew px, fov in DEGREES
        if (ew < 64) ew = (int)halfW;
        float fx = fdx * 0.01745329f, fy = fdy * 0.01745329f;
        if (fx < 0.5f || fx > 2.6f) fx = 1.5708f;
        if (fy < 0.5f || fy > 2.6f) fy = 1.65f;
        const float tfx = tanf(fx * 0.5f), tfy = tanf(fy * 0.5f);
        const float maxH = (float)bbH * 0.50f;                 // [MENUTHEME] small and low: it is head-locked here
        const float maxW = (float)ew * 0.92f;                  // stay inside the shown crop
        float h = maxH;
        float w = rtAspect * (h / (float)bbH) * (tfy / tfx) * (float)ew;
        if (w > maxW) {                                        // too wide to be 4:3 at this height:
            w = maxW;                                          // fit to width, drop height to match
            h = w * (float)bbH * tfx / (rtAspect * tfy * (float)ew);
        }
        const float y = ((float)bbH - h) * 0.5f + (float)bbH * 0.05f;   // a touch below eye level
        DrawQuad(dev, (halfW - w) * 0.5f,         y, w, h);
        DrawQuad(dev, halfW + (halfW - w) * 0.5f, y, w, h);
    } else {
        // [MENUTHEME] The flat frame IS the theater screen: use most of it, so the text is big
        // enough to read across the room.
        float h = (float)bbH * 0.96f;
        float w = h * rtAspect;
        if (w > (float)bbW * 0.80f) { w = (float)bbW * 0.80f; h = w / rtAspect; }
        DrawQuad(dev, ((float)bbW - w) * 0.5f, ((float)bbH - h) * 0.5f, w, h);
    }

    sb->Apply();
    sb->Release();
}

// ol_ue3.cpp - Outlast Stage 3: CalcSceneView hook + live offset discovery.
// =============================================================================
// The stereo mechanism (ported from ME2/ME3, see Docs/MELE_SAMEFRAME_STEREO_METHOD.md):
// hook ULocalPlayer::CalcSceneView, call it TWICE per frame with the player's normalized
// viewport rect rewritten to the left then right half of the backbuffer. The engine renders
// both eyes side-by-side into one frame, each with its own culling/post.
//
// To do that in Outlast, two things come from Ghidra and two things only come from the running
// game:
//   (Ghidra, static)  CalcSceneView RVA, AllocateViewState RVA
//   (live, this file) the ULocalPlayer viewport-rect field offsets, and the FSceneView layout
//
// So this file is BOTH the discovery tool and, once armed, the hook. It is read-only until
// explicitly armed, and every game-memory read is SEH-guarded.
//
// Config: outlastvr.ini next to OLGame.exe
//   [Stage3]
//   CalcSceneViewRva=0x0        ; RVA in the RUNNING exe; 0 = disabled
//   Mode=probe                  ; probe = log only (safe) | split = double-call stereo
// =============================================================================

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <math.h>
#include <share.h>    // _SH_DENYWR for the [CAMPROBE] csv (readable while the game runs)
#include <tlhelp32.h> // [BONEWATCH] thread enumeration, to set debug registers on every thread
#include <intrin.h>   // _ReturnAddress for the call-site gate
#include "ol_ue3.h"
#include "ol_xr.h"
#include "ol_menu.h"
#include "ol_interact.h"
#include "MinHook.h"

static uintptr_t g_base = 0;
static uintptr_t g_cvRva = 0;
static uintptr_t g_vpRva = 0;
static uintptr_t g_avsRva = 0;
static uintptr_t g_sharedRva = 0;
static uintptr_t g_creatorRva = 0;
static volatile long  g_eyePhase = 0;   // 0 = off, 1 = LEFT eye (-half), 2 = RIGHT eye (+half)
// AER state (declared up here because OLUE3_StereoMode(), defined further down, reports it)
static volatile long g_aerMode = 0;
static volatile long g_aerFrame = 0;
static volatile long g_aerEye = 0;      // eye rendered THIS frame: 0 = left, 1 = right
static bool      g_splitMode = false;
static long      g_calls = 0;
static long      g_logged = 0;

// UE3 x64 CalcSceneView, 5-arg form:
//   FSceneView* __fastcall(ULocalPlayer* this, FSceneViewFamily* Family,
//                          FVector& OutViewLoc, FRotator& OutViewRot,
//                          FViewport* Viewport, FViewElementDrawer* Drawer)
typedef void* (__fastcall *CalcSceneView_t)(void*, void*, void*, void*, void*, void*);
static CalcSceneView_t o_CalcSceneView = nullptr;

// ---- SEH-guarded reads. Every one of these can hit a freed/garbage pointer. ----
static bool SafeReadBytes(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeReadFloat(const void* p, float* out) { return SafeReadBytes(p, out, sizeof(float)); }

static bool PlausibleFrac(float f) { return f >= -0.001f && f <= 1.001f; }

// ---- viewport-rect finder -------------------------------------------------------------
// UE3's ULocalPlayer stores the normalized viewport rect as four consecutive floats:
//   Origin.X, Origin.Y, Size.X, Size.Y
// For a single fullscreen player that is EXACTLY {0,0,1,1} - a very distinctive quad. Scan
// the object for it. Dishonored (x86, 2012 UE3) had it at +0x64; Outlast is x64 so the
// offset WILL differ. Do not assume - this is why it is scanned rather than assumed.
static const size_t kScanBytes = 0x900;

static void ScanForRect(void* obj) {
    if (!obj) return;
    unsigned char buf[kScanBytes];
    if (!SafeReadBytes(obj, buf, sizeof(buf))) { OLProxyLog("[OLVR][S3] rect scan: object unreadable"); return; }
    char line[512];
    int hits = 0;
    for (size_t off = 0; off + 16 <= sizeof(buf); off += 4) {
        const float* f = reinterpret_cast<const float*>(buf + off);
        if (f[0] == 0.0f && f[1] == 0.0f && f[2] == 1.0f && f[3] == 1.0f) {
            sprintf_s(line, "[OLVR][S3] rect CANDIDATE at this+0x%zx = {0,0,1,1}", off);
            OLProxyLog(line);
            if (++hits >= 12) break;
        }
    }
    if (!hits) {
        // Not fullscreen (letterboxed/cutscene?) - dump every plausible 0..1 quad instead.
        for (size_t off = 0; off + 16 <= sizeof(buf); off += 4) {
            const float* f = reinterpret_cast<const float*>(buf + off);
            if (PlausibleFrac(f[0]) && PlausibleFrac(f[1]) && f[2] > 0.01f && f[2] <= 1.001f && f[3] > 0.01f && f[3] <= 1.001f) {
                sprintf_s(line, "[OLVR][S3] rect maybe at this+0x%zx = {%.3f,%.3f,%.3f,%.3f}", off, f[0], f[1], f[2], f[3]);
                OLProxyLog(line);
                if (++hits >= 12) break;
            }
        }
    }
    if (!hits) OLProxyLog("[OLVR][S3] rect scan: no 0..1 quad found in the first 0x900 bytes");
}

// ---- FSceneView layout finder ----------------------------------------------------------
// Needed: ViewMatrix (camera right vector = the eye-shift axis), ProjectionMatrix (the FOV that
// must be declared to the compositor), PreViewTranslation and ViewOrigin (where the eye shift is
// applied). Find them by shape: a view matrix's last row is {x,y,z,1}; a UE3 perspective
// projection has proj[0]=1/tan(halfH), proj[5]=1/tan(halfV), proj[11]=1, proj[15]=0.
static void ScanSceneView(void* view) {
    if (!view) { OLProxyLog("[OLVR][S3] CalcSceneView returned NULL"); return; }
    unsigned char buf[0x400];
    if (!SafeReadBytes(view, buf, sizeof(buf))) { OLProxyLog("[OLVR][S3] view scan: unreadable"); return; }
    char line[512];
    int projHits = 0, mtxHits = 0;
    for (size_t off = 0; off + 64 <= sizeof(buf); off += 4) {
        const float* m = reinterpret_cast<const float*>(buf + off);
        // projection: [0] and [5] positive finite, [11]==1, [15]==0, [1]==[2]==[4]==0
        if (m[11] == 1.0f && m[15] == 0.0f && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f &&
            m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f) {
            float fovH = 2.0f * atanf(1.0f / m[0]) * 57.2957795f;
            float fovV = 2.0f * atanf(1.0f / m[5]) * 57.2957795f;
            sprintf_s(line, "[OLVR][S3] PROJECTION at view+0x%zx  fovH=%.1fdeg fovV=%.1fdeg", off, fovH, fovV);
            OLProxyLog(line);
            if (++projHits >= 4) break;
        }
        // rigid transform: last row {x,y,z,1} and the 3x3 rows unit-length
        else if (m[15] == 1.0f && m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f) {
            float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
            if (l0 > 0.98f && l0 < 1.02f && mtxHits < 6) {
                sprintf_s(line, "[OLVR][S3] view-like MATRIX at view+0x%zx right=(%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f)",
                          off, m[0], m[4], m[8], m[12], m[13], m[14]);
                OLProxyLog(line);
                ++mtxHits;
            }
        }
    }
    if (!projHits) OLProxyLog("[OLVR][S3] view scan: no projection-shaped matrix in the first 0x400 bytes");
}

// ---- the split ------------------------------------------------------------------------
// Confirmed live 2026-07-22: ULocalPlayer's normalized viewport rect is four floats at
//   this+0xA0 Origin.X, +0xA4 Origin.Y, +0xA8 Size.X, +0xAC Size.Y
// Rewrite it to the left half, let the engine build a view; rewrite to the right half, build
// a second view; restore. The engine renders both into one backbuffer, side by side, in one
// frame - each with its own culling and post.
static const size_t kRectOff = 0xA0;

// CalcSceneView has FOUR call sites and only one is the render path. The others build views
// for Project/Deproject-style queries at different FOVs (86/96/60 deg were all seen in one
// session). Splitting those would corrupt whatever they feed. Gate on the return address
// landing inside UGameViewportClient::Draw.
static uintptr_t g_drawLo = 0, g_drawHi = 0;
static volatile long g_splitOn = 0;      // runtime toggle (NUM4), seeded from the ini
static volatile long g_inSplit = 0;      // re-entry guard
static long g_splitFrames = 0, g_splitFail = 0;

// The FOV declared to the compositor must be EXACTLY what the engine rendered, or the image
// warps and swims. Do not compute it from the ini's DefaultFOV: the halves are half-width, the
// game changes FOV when running, and the camcorder zooms. Read it off the projection matrix of
// the view the engine actually built (FSceneView+0xC0, confirmed live), every frame.
static const size_t kProjOff = 0xC0;
static const size_t kViewMatOff = 0x80;   // FSceneView::ViewMatrix, ProjectionMatrix follows
// [INTERACT] (Test37) see Hook_GetViewPoint
static volatile float g_fvHead[3] = { 0, 0, 0 }, g_fvBaseYaw = 0.0f;
static volatile ULONGLONG g_fvTick = 0;
extern "C" float OLInteract_ViewfinderZoom();
extern "C" void OLInteract_Tick();
extern "C" int OLInteract_RightArmNative();
extern "C" int OLInteract_HandNV();                    // (Test43) [HANDNV] the camcorder's night vision
extern "C" void OLPrompts_Install(uintptr_t base);   // [PROMPTS] ol_prompts.cpp
extern "C" void OLSound_Install();                     // [SOUND] (Test43) ol_sound.cpp: Wwise - breath, audio report
// [FRAMETAG] (Test36) per-frame head pose: latched on the game thread, identified on the render
// thread - see ol_xr.cpp.
extern "C" void OLXR_FrameLatch();
extern "C" void OLInteract_RoomWalkStep();   // (Test44) [ROOMWALK]
extern "C" void OLXR_FrameCommit(float fovX, float fovY, int lensFit, int viewfinder, const float* key32);
extern "C" void OLXR_RenderViewSeen(const void* key128);
static volatile long  g_splitFresh = 0;      // frames since the last successful split
static volatile float g_fovX = 0.0f, g_fovY = 0.0f;

static void g_cineMeasureK(float eyeTan);   // fwd: [CINEFOV] state lives further down

static void CaptureRenderedFov(void* view) {
    float m[16];
    if (!view || !SafeReadBytes((char*)view + kProjOff, m, sizeof(m))) return;
    if (!(m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f)) return;
    g_fovX = 2.0f * atanf(1.0f / m[0]);
    g_fovY = 2.0f * atanf(1.0f / m[5]);
    // [LENSFIT] an off-centre projection says nothing about the camera-to-eye mapping: its m[0]
    // is the lens's own span. Measuring k from it would drift the widening for normal frames.
    if (fabsf(m[8]) < 1e-4f) g_cineMeasureK(1.0f / m[0]);   // [CINEFOV] self-correct the mapping
}

// Consumed by the XR submit. The freshness counter gives hysteresis: a loading screen stops
// splitting for a moment and the headset must not flip to mono and back (that flashes).
extern "C" int OLUE3_SplitActive() { return (g_splitFresh > 0) ? 1 : 0; }

// HEAD-INJECTION WEIGHT. 1 in gameplay, 0 while the cinema screen is up (menus/cutscenes):
// injecting head rotation there pans the picture inside a screen that itself follows the head -
// the "warpy, off" cutscene look. ME1's cine shape is a tracking-off 3D picture. The weight is
// RAMPED (exponential, ~15 frames), never stepped: stepping a camera rotation in one frame is
// the FP-storm lesson from ME1. Ticked once per Present so both eyes of a frame share one value.
extern "C" int OLXR_ScreenActive();
static volatile float g_injW = 1.0f;
// Number of recent frames whose ORIGINAL (pre-widening) camera was narrower than normal
// gameplay. Menus, authored cinematics and deep camcorder zoom all enter this state even when
// CINEFOV later widens their rendered image. Gameplay-only arm IK uses it to yield to the
// game's authored upper-body animation.
static volatile long g_nativeNarrowFrames = 0;
static long          g_armDrive = 0;
static volatile long g_poseNativeHold = 0;
static volatile long g_poseHookOk = 0;
// AOLPlayerController -> AOLHero layout (RE'd from OLGame.exe, verified live by look-to-use):
// hero = ctrl+0xA4C, hero+0x2BBC points back at ctrl, hero+0x2BD8 = EyeForward, hero+0x7BC =
// locomotion mode (0 = ordinary walking).
static const size_t kCtrlHeroOff = 0xA4C, kHeroCtrlOff = 0x2BBC, kHeroEyeFwdOff = 0x2BD8,
                    kHeroMoveModeOff = 0x7BC;
// [SCENESTATE] What the game itself says it is doing, read through reflection each frame (see the
// [SCENESTATE] block). -1 = not known yet.
static volatile long g_sPaused = -1, g_sCinematic = -1, g_sIgnoreMove = -1, g_sIgnoreLook = -1,
                     g_sMoveMode = -1, g_sHasHero = -1, g_sMenu = -1;
// (Test40) OLPawn.SpecialMove (ESpecialMoveType, the byte after LocomotionMode) - which jump, climb,
// door or ledge move is playing while LocomotionMode is LM_SpecialMove (2).
static volatile long g_sSpecialMove = -1;
static const size_t kHeroSpecialMoveOff = 0x7BD;
// A scripted sequence owns Miles: a cutscene camera, script-locked movement, or any locomotion
// mode other than ordinary walking (locker, bed, ledge, squeeze, the opening car...).
static bool SceneScripted() {
    return g_sCinematic > 0 || g_sIgnoreMove > 0 || g_sMoveMode > 0;
}
// [FREEMOVE] (Test40) What a locomotion mode does to the arms. 0: they stay the player's - jumps,
// falls, landings, vaults, crouches (Test39 handed them to the game for every one of these, so a
// jump snapped your hands away). 1: a climb - with hands climbing on, each hand that holds on plays
// the game's own climbing arm (on the edge, on the rung) and a free hand stays yours; with it off
// the game has both. 2: the game has both arms (doors, lockers, beds, squeezes, cutscenes...).
// LocomotionMode: 0 Walk 1 Fall 2 SpecialMove 3 Ladder 4 LedgeHang 5 LedgeWalk 6 Squeeze 7 Door
// 8 Locker 9 Cinematic ... SpecialMove (ESpecialMoveType): see the enum dump in the Test40 notes.
static int ArmMoveClass() {
    if (g_sCinematic > 0 || g_sIgnoreMove > 0) return 2;
    const long lm = g_sMoveMode, sm = g_sSpecialMove;
    int c = 2;
    if (lm <= 0 || lm == 1) c = 0;                       // walking, falling
    else if (lm == 3 || lm == 4) c = 1;                  // on a ladder, hanging from a ledge
    else if (lm == 2) {
        switch (sm) {
        case 1: case 2: case 3: case 4: case 5: case 7: case 11: case 12: case 13: c = 0; break;   // crouch, jump, land, vault
        case 6: case 8: case 9: case 10: case 14: case 15: case 16: case 17: case 18: case 19:
        case 43: case 44: case 45: case 46: case 47: case 48: c = 1; break;                      // climbs, ledges, ladders
        default: c = 2;
        }
    }
    if (c == 1 && !OLInteract_Get(8)) c = 2;              // [Motion] ClimbWithHands=0: the old way
    return c;
}
extern "C" int OLUE3_HeroMoveState(int* lm, int* sm) {
    if (lm) *lm = (int)g_sMoveMode;
    if (sm) *sm = (int)g_sSpecialMove;
    return g_sHasHero > 0 ? 1 : 0;
}
// Any of the game's own menu screens (main, pause, options, notes, recordings, gamma, splash),
// the world paused, no player body, or the settings menu open: the frame is rendered flat (one
// view) and shown on the theater screen.
static bool SceneWantsTheater() {
    return g_sMenu > 0 || g_sPaused > 0 || g_sHasHero == 0 || OLMenu_Visible();
}
extern "C" int OLUE3_GamePaused() { return g_sPaused > 0 ? 1 : 0; }
// [MENUPRESS] (Test39) the game's own menus are up (pause, notebook, notes, documents, front end)
extern "C" int OLUE3_InGameMenu() { return (g_sMenu > 0 || g_sPaused > 0 || g_sHasHero == 0) ? 1 : 0; }
extern "C" int OLUE3_SceneScripted() { return SceneScripted() ? 1 : 0; }
// [PHYSCROUCH] Miles's visible skeletal component, refreshed by every arm solve and aged here.
// Only trusted while fresh: across a level load the old pointer can dangle.
static void* volatile g_milesComp = nullptr;
static volatile long  g_milesCompFresh = 0;

// [NVLIGHT] the gamepad/body aim, captured pre-injection in Hook_GetViewPoint. Declared here
// because OLUE3_FrameTick (just below) ages its freshness counter.
static volatile int  g_baseViewRot[3] = { 0, 0, 0 };
static volatile long g_baseViewFresh = 0;

extern "C" void OLUE3_NvLightReport();

extern "C" void OLUE3_FrameTick() {
    if (g_splitFresh > 0) InterlockedDecrement(&g_splitFresh);
    if (g_baseViewFresh > 0) InterlockedDecrement(&g_baseViewFresh);
    if (g_nativeNarrowFrames > 0) InterlockedDecrement(&g_nativeNarrowFrames);
    if (g_milesCompFresh > 0) InterlockedDecrement(&g_milesCompFresh);
    // [NVCENSUS] every ~10 s, so one run with the camcorder raised answers which light is which.
    static long s_tick = 0;
    if ((++s_tick % 600) == 0) OLUE3_NvLightReport();
    const float target = OLXR_ScreenActive() ? 0.0f : 1.0f;
    float w = g_injW + (target - g_injW) * 0.15f;
    if (w < 0.001f) w = 0.0f; if (w > 0.999f) w = 1.0f;
    g_injW = w;
    // TEST 13 custom-upper-body ownership: ordinary gameplay arms belong to tracked IK.  The
    // previous broad button hold made every jump/crouch/interact input re-enable the native arm
    // pose for up to 1.5 seconds, which both delayed tracking and pulled the shoulders back into
    // the offset flat-game stance.  Menus and authored/narrow-camera scenes still yield through
    // PoseApplyVisible's explicit screen/camera gates below.
    if (g_armDrive) {
        InterlockedExchange(&g_poseNativeHold, 0);
    }
}
extern "C" void OLUE3_GetEyeFov(float* fx, float* fy) {
    if (fx) *fx = g_fovX;
    if (fy) *fy = g_fovY;
}
// [PHASE1] Ordinary first-person play: no cinema screen, no narrow authored camera, the head
// fully injected, and the settings menu closed. Snap turn, walk direction, physical crouch and
// the comfort filters only ever act inside this state.
extern "C" int OLUE3_GameplayActive() {
    return (!OLXR_ScreenActive() && g_nativeNarrowFrames == 0 && g_injW >= 0.95f &&
            !OLMenu_Visible() && g_sPaused <= 0 && g_sMenu <= 0) ? 1 : 0;
}
// [SNAPTURN] Miles's body yaw as the engine reported it before any head injection (UE3 units,
// 65536 = 360 deg). 0 = not fresh.
// [STICKPITCH] (Test37) The controller's own pitch (Actor.Rotation.Pitch of the PlayerController,
// signed UE3 units) - what the right stick changes, without the camera bone's animation. Read
// by [SCENESTATE] every 15 ms. 0 = not known.
static volatile long g_sCtrlPitch = 0, g_sCtrlPitchOk = 0;
extern "C" int OLUE3_GetBasePitch(int* pitchUnits) {
    if (!pitchUnits || !g_sCtrlPitchOk) return 0;
    *pitchUnits = (int)g_sCtrlPitch;
    return 1;
}
extern "C" int OLUE3_GetBodyYaw(int* yawUnits) {
    if (!yawUnits || g_baseViewFresh <= 0) return 0;
    *yawUnits = g_baseViewRot[1];
    return 1;
}

static bool SafeWriteRect(void* self, const float* r) {
    __try { memcpy((char*)self + kRectOff, r, 4 * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- per-eye view state ------------------------------------------------------------------
// UE3 keeps each view's TEMPORAL history in an FSceneViewState hanging off the LocalPlayer at
// +0xB0 (confirmed: CalcSceneView reads it, and AllocateViewState's caller tests that field).
// History means eye adaptation/auto-exposure, occlusion-query results, and any temporal post.
//
// Handing BOTH eyes the same state makes them ping-pong: the left eye renders and updates the
// history, the right eye then renders against the left's, and vice versa next frame. With two
// identical cameras that was invisible; with real parallax it shows up as flicker and as one
// eye being a different brightness/colour from the other. Exactly what was observed.
//
// Fix: allocate a SECOND state with the engine's own AllocateViewState() and swap it in for the
// right eye, so each eye accumulates its own history. Allocated once, lazily, then reused.
typedef void* (__fastcall *AllocViewState_t)();
static AllocViewState_t o_AllocViewState = nullptr;
static const size_t kViewStateOff = 0xB0;
static void* g_stateR = nullptr;
static bool  g_stateTried = false;
static void* g_localPlayer = nullptr;   // captured in CalcSceneView; SFR needs it to swap states

static bool SafeWritePtr(void* obj, size_t off, void* val) {
    __try { *(void**)((char*)obj + off) = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void* SafeReadPtr(void* obj, size_t off) {
    __try { return *(void**)((char*)obj + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---- the OTHER view-state source ---------------------------------------------------------
// CalcSceneView does NOT always take the view state from the LocalPlayer:
//     if ((this[0xC8] & 1) == 0 || globalFlag != 0)  state = this->ViewState;   // +0xB0
//     else                                           state = GetSharedViewState(g);
// and GetSharedViewState hands back ONE lazily-created object cached at g+0x528. If that is the
// live branch, both eyes share a single state no matter what is written to +0xB0 -- which is
// exactly what happened when swapping +0xB0 changed nothing.
//
// Symptom fit: discrete objects (chairs, windows, doors) flicker while ground and walls do not.
// Those are the primitives UE3 runs hardware OCCLUSION QUERIES on, and the per-primitive
// occlusion history lives in the view state. Two views sharing one history means each frame's
// visibility answers get applied to the wrong camera, so objects wink in and out.
//
// So: hook the getter and hand the right eye its own instance, built by the engine's own
// creator so it is a real, correctly-constructed object rather than a hand-crafted stand-in.
typedef void* (__fastcall *GetSharedState_t)(void*);
typedef void* (__fastcall *StateCreator_t)(void*, void*, void*, void*, void*);
static GetSharedState_t o_GetSharedState = nullptr;
static StateCreator_t   o_StateCreator = nullptr;
static void* g_sharedStateR = nullptr;
static volatile long g_sharedHits = 0, g_sharedGaveOurs = 0;

static void* __fastcall Hook_GetSharedState(void* g) {
    void* orig = o_GetSharedState(g);
    InterlockedIncrement(&g_sharedHits);
    if (g_eyePhase != 2) return orig;                 // left eye / not splitting: untouched

    if (!g_sharedStateR && o_StateCreator && g) {
        __try {
            void* arg = *(void**)((char*)g + 0x530);  // the same argument the engine passes
            g_sharedStateR = o_StateCreator(nullptr, arg, nullptr, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_sharedStateR = nullptr; }
        char l[190];
        sprintf_s(l, "[OLVR][S3] right-eye SHARED view state %s (%p) - occlusion history is now "
                     "per-eye", g_sharedStateR ? "created" : "creation FAILED", g_sharedStateR);
        OLProxyLog(l);
    }
    if (g_sharedStateR) { InterlockedIncrement(&g_sharedGaveOurs); return g_sharedStateR; }
    return orig;
}

// ---- parallax --------------------------------------------------------------------------
// The eye offset is applied AT THE SOURCE, not by patching the built FSceneView afterwards.
// CalcSceneView gets its camera from GetPlayerViewPoint(Actor*, FVector& OutLoc, FRotator& OutRot);
// hooking that and shifting OutLoc during the right-eye pass makes the engine build the ENTIRE
// second view from a genuinely moved camera - view matrix, translated view, projection,
// culling, all of it consistent. Patching PreViewTranslation/ViewOrigin on the finished view
// (the ME2/ME3 approach) means also recomputing every derived matrix by hand and knowing four
// more field offsets. Same result, far less that can silently go stale.
//
// This is a true camera move, so it produces true parallax. It is NOT projection shear /
// convergence, which is a known dead end on this project (it shears the image and breaks fusion).
typedef void (__fastcall *GetViewPoint_t)(void*, float*, int*);
static GetViewPoint_t o_GetViewPoint = nullptr;

static volatile float g_halfEyeUU = 1.6f;  // half-IPD in Unreal units; tune live with NUM1/NUM3
static volatile long  g_eyeSwap = 0;       // flips which side gets the + shift

// ---- 6DOF head tracking --------------------------------------------------------------------
// Until now NOTHING consumed the head pose (ol_xr.h said so in a comment): the pose was located,
// logged, and handed to the compositor as the layer pose - while the game kept rendering from the
// gamepad camera. Declaring "rendered from the head pose" over an image that was NOT is exactly
// what makes the world feel nailed to your face: turn your head and nothing moves relative to you.
//
// Fix: inject the head delta INTO the camera at the source, the same place the eye offset goes.
// The engine then builds the whole view - matrices, culling, projection - from a genuinely
// head-tracked camera, and the declared pose finally describes what was rendered.
//
// Scope: ONLY during the mod's own view-building passes (g_eyePhase != 0). GetPlayerViewPoint is the
// engine's general "where is this actor looking" call; AI, audio and aim must keep the gamepad
// answer. Head moves the CAMERA, not the character.
//
// Rotation conventions, derived not guessed:
//   OpenXR is right-handed, +X right, +Y up, -Z forward. Turning LEFT is a positive rotation
//   about +Y, and g_olYawRad = atan2(fwd.x, -fwd.z) evaluates to -theta for that turn, so a left
//   turn reads NEGATIVE. UE3 yaw is measured CCW from +X toward +Y (the existing right-axis code
//   below assumes exactly that), so turning left INCREASES yaw. Hence UE3 yaw -= g_olYawRad.
//   Pitch: g_olPitchRad = asin(fwd.y) is positive looking up; UE3 pitch is positive up. Same sign.
//   Roll: both systems make "head tilted left" positive (XR: +Z is backwards, so +roll carries
//   +X toward +Y; UE3: roll about forward carries right toward up). Same sign - this is the one
//   axis whose convention is not pinned by existing code, so it is the first suspect if tilting
//   your head tilts the world the wrong way.
//
// Scale: 1 metre of real head movement = g_worldToMeters Unreal units. 50 uu/m is the UE3-era
// human-scale convention AND it is what the existing half-IPD implies: 1.6 uu * 2 = 3.2 uu =
// 6.4 cm at 50 uu/m, a textbook IPD. The two numbers must always agree - if the world ever feels
// the wrong SIZE, these move together.
static volatile float g_worldToMeters = 50.0f;
static volatile long  g_headTrack = 1;

// [LEAN100] THE SCALE WAS THE BUG. Measured 2026-08-17 by the [CAMPROBE] run, three independent
// confirmations: camera sits 168 uu above the pawn root standing (eye height 1.68 m), walking
// logs 210-300 uu/s (2.1-3.0 m/s, human walking pace), and the hand-tuned HalfEyeUU 3.295 makes
// the eyes 6.6 uu apart = 6.6 cm at 100 uu/m, matching the measured 6.97 cm IPD. Outlast is
// ~100 uu/m. Every earlier lean build (50-95 uu/m) rendered HALF the movement the declared pose
// promised, so the compositor had nothing to correct and the world dragged with the head by
// half of every motion, reading as unstable and not moving forward correctly.
// The same run killed the animation theory: standing still the camera moves 0.35 uu per 10 s
// (3 mm) and breathes +/-0.6 deg of pitch. There is no idle bob to fight. No filters, no
// steadying, no forward emphasis, no trigger-driving - position raw at the true scale,
// declared == rendered, and the compositor closes the seam.
static const float kMetersToUU = 100.0f;  // measured, not assumed - see [CAMPROBE] note above
static const float kPosSanityM = 2.0f;    // per-axis sanity clamp, nothing tighter
// (Beta 1.1) [WORLDSIZE] Players: "it feels a lot zoomed in, is there any way to dezoom", "adjustable world scale
// would be sweet". The eyes now sit as far apart as YOUR eyes (the headset's own IPD, read from the runtime's
// two eye poses; the old fixed 3.155 uu = 6.3 cm stays as the fallback), divided by World size: 100% is true
// size, lower makes everything look smaller (eyes wider apart, head movement bigger), higher bigger.
extern "C" float OLXR_GetRtEyeSep();
static volatile float g_worldSize = 1.0f;
extern "C" float OLUE3_GetWorldSize() { return g_worldSize; }
extern "C" void  OLUE3_SetWorldSize(float s) {
    if (!(s >= 0.60f)) s = 0.60f; if (s > 1.50f) s = 1.50f;
    g_worldSize = s;
}
static float EyeHalfUU();   // below, after kMetersToUU
static volatile float g_leanGain = 1.0f;  // 1.0 = declared==rendered exactly; other values trade
                                          // that honesty for taste, keep the default
extern "C" float OLUE3_GetLeanGain() { return g_leanGain; }
static float EyeHalfUU() {
    const float ipd = OLXR_GetRtEyeSep();
    const float base = (ipd >= 0.045f && ipd <= 0.085f) ? ipd * 0.5f * kMetersToUU : g_halfEyeUU;
    return base / g_worldSize;
}
extern "C" float OLUE3_GetEyeSpacingCm() { return EyeHalfUU() * 2.0f / kMetersToUU * 100.0f; }
extern "C" void  OLUE3_SetLeanGain(float v) {
    if (v < 0.5f) v = 0.5f; if (v > 2.0f) v = 2.0f;
    g_leanGain = v;
}

// HEAD ROLL - OFF (2026-07-22: a head tilt must produce no change on screen).
//
// This flag governs BOTH the camera injection here AND the roll component of the pose declared
// to the compositor (ol_xr.cpp asks via OLUE3_HeadRollEnabled). It has to be one flag: declared
// must equal rendered. Cancel roll on only one side and the compositor rotates the flat eye
// image to reconcile the difference - the picture visibly spins as you tilt, with empty wedges
// swinging in at the corners. With roll off on BOTH sides there is nothing to reconcile and a
// head tilt changes nothing on screen.
static volatile long g_headRoll = 0;
extern "C" int OLUE3_HeadRollEnabled() { return g_headRoll ? 1 : 0; }

// [LEAN100] enable flag for the camera lean. Applies only once the position baseline is armed
// ([HEADPOS], ol_xr.cpp); off = the camera is byte-for-byte where Outlast puts it.
static volatile long g_headPos = 1;
extern "C" int OLXR_PosArmed();

void OLUE3_ToggleHeadPos() {
    g_headPos = g_headPos ? 0 : 1;
    char line[190];
    sprintf_s(line, "[OLVR][HEAD] positional tracking %s", g_headPos
        ? "ON - lean/duck moves the camera at the measured 100 uu/m once the baseline is armed"
        : "OFF - camera stays exactly where Outlast puts it; rotation still tracks");
    OLProxyLog(line);
}

void OLUE3_ToggleHeadTrack() {
    g_headTrack = g_headTrack ? 0 : 1;
    char line[160];
    sprintf_s(line, "[OLVR][HEAD] head tracking %s (halfEye=%.2f uu)",
              g_headTrack ? "ON" : "OFF - camera is gamepad-only", g_halfEyeUU);
    OLProxyLog(line);
}
void OLUE3_AdjustWorldScale(float mul) {
    float v = g_worldToMeters * mul;
    if (v < 5.0f) v = 5.0f;
    if (v > 500.0f) v = 500.0f;
    g_worldToMeters = v;
    char line[160];
    sprintf_s(line, "[OLVR][HEAD] worldToMeters = %.1f uu/m (1 m of head motion = %.1f uu; "
                    "matching IPD would be halfEye %.2f uu)", v, v, v * 0.032f);
    OLProxyLog(line);
}

void OLUE3_AdjustEye(float delta) {
    float v = g_halfEyeUU + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 30.0f) v = 30.0f;
    g_halfEyeUU = v;
    char line[128];
    sprintf_s(line, "[OLVR][S3] halfEye = %.2f uu (IPD = %.2f uu)", v, v * 2.0f);
    OLProxyLog(line);
}
// ---- accessors for the in-headset menu (ol_menu.cpp) ------------------------------------------
// Deliberately clamped HERE rather than in the menu: these are the only writers, so a bad value
// cannot reach the camera hook no matter who calls them.
extern "C" float OLUE3_GetHalfEye() { return g_halfEyeUU; }
extern "C" void  OLUE3_SetHalfEye(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 30.0f) v = 30.0f;
    g_halfEyeUU = v;
}
extern "C" float OLUE3_GetWorldToMeters() { return g_worldToMeters; }
extern "C" void  OLUE3_SetWorldToMeters(float v) {
    if (v < 5.0f) v = 5.0f; if (v > 500.0f) v = 500.0f;
    g_worldToMeters = v;
}
extern "C" int  OLUE3_GetHeadTrack() { return g_headTrack ? 1 : 0; }
extern "C" void OLUE3_SetHeadTrack(int v) { g_headTrack = v ? 1 : 0; }
extern "C" int  OLUE3_GetHeadPos() { return g_headPos ? 1 : 0; }
extern "C" void OLUE3_SetHeadPos(int v) { g_headPos = v ? 1 : 0; }
extern "C" int  OLUE3_GetHeadRoll() { return g_headRoll ? 1 : 0; }
extern "C" void OLUE3_SetHeadRoll(int v) { g_headRoll = v ? 1 : 0; }
extern "C" int  OLUE3_GetEyeSwap() { return g_eyeSwap ? 1 : 0; }
extern "C" void OLUE3_SetEyeSwap(int v) { g_eyeSwap = v ? 1 : 0; }

void OLUE3_SwapEyes() {
    g_eyeSwap = g_eyeSwap ? 0 : 1;
    OLProxyLog(g_eyeSwap ? "[OLVR][S3] eyes SWAPPED" : "[OLVR][S3] eyes normal");
}

// ---- [CINEFOV] true VR cutscenes -----------------------------------------------------------
// "I am not in the world, it's like looking through a flat screen" (2026-07-23). Cutscenes and
// the menus are LIVE renders (the split runs during them) but at ~58 and ~33 deg/eye against
// gameplay's 88, and a narrow render can only ever be PRESENTED as a box. So widen the render.
//
// THREE ROUNDS OF GUESSING AT MEMORY, ALL FAILED, ALL LOGGED IN THE HANDOFF:
//   r1 scan for "a float that tracks the FOV" -> never converged, silently did nothing.
//   r2 same, self-verifying -> wedged after one probe (restore keyed off the locked offset,
//      which is 0 while still searching, so the patched flag never cleared).
//   r3 search for the camera POV struct {Location, Rotation, FOV} by matching the Location
//      GetPlayerViewPoint just returned -> "POV signature not found", every time. Outlast
//      drives the view from a camera BONE (OLGame.ini CameraBoneName=Hero-Camera), so the
//      view point is computed from animation each frame and never sits in memory to match.
//
// Then I stopped guessing and read the disassembly, which is what this project's own playbook
// says to do. ULocalPlayer::CalcSceneView (0x62A020) builds the projection as
// tanf(FOV * PI/360) where FOV is the RETURN VALUE of a getter at 0x60F4F0 - which is not a
// field read at all, it is an UnrealScript event dispatch (ProcessEvent -> GetFOVAngle). That
// is why no memory scan could ever have found it.
//
// So hook the getter. It returns a float in XMM0 (verified in the disassembly:
// `MOVSS XMM0,[RSP+0x30]` then RET, and the caller does `MOVAPS XMM13,XMM0`). During these view
// passes only, hand back a widened angle; every other caller of that getter - and there are 4
// others - sees the game's real value. Nothing is written to game memory, so there is nothing
// to restore and nothing that can be left inconsistent if this is interrupted.
//
// Target = kCineTargetDeg per eye, i.e. literally what gameplay renders ("like gameplay" is
// the ask). Applied ONLY when the frame would otherwise come out below kCineNarrowDeg, so
// gameplay is untouched. The camera angle needed for a given per-eye angle goes through the
// engine's own measured mapping k = tan(eye/2)/tan(cam/2), which self-corrects every frame it
// widens from what the projection matrix actually came out as.
static const float kCineTargetDeg = 88.0f;   // per-eye goal = the confirmed-good gameplay width
static const float kCineNarrowDeg = 80.0f;   // only frames below this get widened
static const uintptr_t kFovGetterRva = 0x60F4F0;
static const unsigned char kFovGetterPrologue[13] = {
    0x48,0x89,0x5C,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x20, 0x0F,0x57,0xC0
};

static volatile long g_cineFovOn = 1;
// [WIDEFOV] Render at the HEADSET's width instead of stopping at gameplay's 88. The 88 target
// leaves a shortfall against a ~94-99 deg eye, which is the black wedge on Quest's fitted
// frustum and the crop on SteamVR - and filling it is how the world gets BIGGER without
// touching eye separation (scale rides IPD alone; width rides this). Costs sharpness: the same
// 1920px half now spans more degrees. [Render] WideFov=1, default OFF - with it off every path
// below is byte-identical to the shipped behaviour.
static volatile long g_wideFov = 0;
extern "C" int  OLUE3_GetWideFov() { return g_wideFov ? 1 : 0; }
extern "C" void OLUE3_SetWideFov(int v) { g_wideFov = v ? 1 : 0; }
extern "C" void OLXR_GetHmdFovDeg(float* fx, float* fy);
static float g_fovK = 0.560f;              // tan(eye/2)/tan(cam/2), measured live
static float g_fovWrittenTan = 0.0f;       // tan(written/2); consumed by CaptureRenderedFov
static long  g_fovHookOk = 0;
static long  g_fovApplied = 0;

typedef float (__fastcall *GetFovAngle_t)(void*);
static GetFovAngle_t o_GetFovAngle = nullptr;

extern "C" int  OLUE3_GetCineFov() { return g_cineFovOn ? 1 : 0; }
extern "C" void OLUE3_SetCineFov(int v) { g_cineFovOn = v ? 1 : 0; }
extern "C" int  OLUE3_CineFovState() { return g_fovHookOk ? (g_cineFovOn ? 2 : 0) : 1; }

// The per-eye angle everything gets widened toward. Default = gameplay's confirmed-good 88;
// [WIDEFOV] moves it to the headset's own measured width so the render fills the eye.
static float EyeTargetDeg() {
    if (g_wideFov) {
        float hx = 0.0f, hy = 0.0f;
        OLXR_GetHmdFovDeg(&hx, &hy);
        // Sane band only: 0 means the runtime has not answered yet, and anything huge would
        // ask the engine for a camera past the tan blow-up.
        if (hx > 60.0f) return (hx > 130.0f) ? 130.0f : hx;
    }
    return kCineTargetDeg;
}

// The camera angle to ask for so the ENGINE renders targetDeg per eye.
static float CineFovWriteDeg(float targetDeg) {
    float k = g_fovK;
    if (k < 0.3f || k > 1.0f) k = 0.560f;
    float w = 2.0f * atanf(tanf(targetDeg * 0.00872665f) / k) * 57.2957795f;
    if (w > 165.0f) w = 165.0f;
    return w;
}

static float __fastcall Hook_GetFovAngle(void* self) {
    const float real = o_GetFovAngle(self);
    // ONLY inside the mod's own view-building passes. Aim, HUD and script logic keep the real value.
    if (!g_cineFovOn || !g_eyePhase) return real;
    // [VIEWFINDER] (Test37) Camcorder zoom. Widening flattened it (zoom "did nothing"): while zoomed,
    // the eyes render a NARROW picture for the head-locked viewfinder, from 60 deg (1x in the 60 deg
    // frame) down to 8 deg (7.5x), following the game's own zoom factor.
    {
        const float z = OLInteract_ViewfinderZoom();
        if (z > 0.0f) {
            const float t = (1.0f - z) * tanf(30.0f * 0.0174532925f) + z * tanf(4.0f * 0.0174532925f);
            return CineFovWriteDeg(2.0f * atanf(t) * 57.2957795f);
        }
    }
    if (!(real > 5.0f && real < 171.0f)) return real;
    __try {
        // Would this camera come out narrower than gameplay? Judge it through the same mapping
        // the engine demonstrably uses, so the decision is about the RENDERED angle, not the
        // camera number (a 90 deg camera renders ~58 deg/eye here - that is the whole problem).
        const float wouldRenderDeg = 2.0f * atanf(g_fovK * tanf(real * 0.00872665f)) * 57.2957795f;
        // The getter is also called by secondary aim/HUD/camcorder queries while the primary
        // gameplay view is being built.  Live Test 13 evidence showed one of those sub-70 calls
        // every frame, so using it as a cinematic detector permanently held tracked IK off even
        // though the submitted camera remained normal gameplay.  In WideFov mode screen/injection
        // state is the authoritative authored-scene gate; only the legacy non-wide path retains
        // this FOV heuristic.
        if (!g_wideFov && wouldRenderDeg < kCineNarrowDeg)
            InterlockedExchange(&g_nativeNarrowFrames, 4);
        const float targetDeg = EyeTargetDeg();
        // [WIDEFOV] off: the shipped rule, gameplay (>= kCineNarrowDeg) is left exactly alone
        // and only narrow cine/menu frames are pulled up to 88. On: one rule for every frame -
        // anything narrower than the headset is widened to the headset, gameplay's 88 included.
        // The near-target guard keeps the writes from flapping as k self-corrects.
        if (!g_wideFov && wouldRenderDeg >= kCineNarrowDeg) return real;
        if (wouldRenderDeg >= targetDeg - 1.0f) return real;
        const float w = CineFovWriteDeg(targetDeg);
        if (w <= real + 2.0f) return real;                      // only ever widen
        g_fovWrittenTan = tanf(w * 0.00872665f);
        const long n = InterlockedIncrement(&g_fovApplied);
        if (n == 1 || (n % 3000) == 0) {
            char l[190];
            sprintf_s(l, "[OLVR][%s] widening: camera %.0f -> %.0f deg (would render %.0f, "
                         "target %.0f deg/eye) frames=%ld", g_wideFov ? "WIDEFOV" : "CINEFOV",
                      real, w, wouldRenderDeg, targetDeg, n);
            OLProxyLog(l);
        }
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return real; }
}

// The mapping constant, measured live: the requested angle and what the engine actually
// rendered (off the projection matrix) are both known. Their tan ratio IS k.
static void g_cineMeasureK(float eyeTan) {
    if (g_fovWrittenTan > 0.01f) {
        const float k = eyeTan / g_fovWrittenTan;
        if (k > 0.25f && k < 1.2f) g_fovK += (k - g_fovK) * 0.2f;
        g_fovWrittenTan = 0.0f;
    }
}

// ---- [NVLIGHT] camcorder / night-vision light follows the head ------------------------------
// Reported: in camcorder mode with night vision on, head tracking does not move the light.
//
// MECHANISM (disassembled, not guessed - the earlier guess of "a screen-space post pass"
// was WRONG). The NV light is a real world SPOTLIGHT owned by the player pawn: OLGame.ini
// [OLGame.OLHero] carries NVLightZoomedInInnerAngle / OuterAngle / Radius / Brightness, i.e.
// spotlight cone parameters. UnrealScript aims it through USpotLightComponent::SetRotation,
// whose native implementation is RVA 0x2CA9F0 (found via the exec-name registration table:
// 'USpotLightComponentexecSetRotation' -> exec 0x428960 -> real setter):
//     void SetRotation(UStructComponent* this, FRotator* rot)  // writes +0x26C/+0x270/+0x274
// FRotator is {Pitch, Yaw, Roll} as ints - the same layout Hook_GetViewPoint already uses.
//
// Why the light diverges: head rotation is injected ONLY during the mod's own view-building
// passes (g_eyePhase != 0), deliberately, so AI/audio/aim keep the gamepad answer - "head moves the
// CAMERA, not the character". The light is aimed by the BODY during the game tick, so turning
// your head swings the view while the lit cone stays where the body points.
//
// FIX: correct the rotation IN FLIGHT as a parameter. The FRotator is copied, the same head
// delta the camera got is added, and the copy is handed to the engine - so nothing is ever
// written into a live engine object directly; the engine's own setter does its normal work.
//
// SCOPING - this must not rotate level spotlights. A light that is aimed along the player's view
// IS the camcorder/NV light; a level light points somewhere arbitrary. So the delta is applied
// only when the requested rotation is within kNvAimTolUU of the player's CURRENT gamepad view
// rotation (captured pre-injection in Hook_GetViewPoint). Anything else passes through untouched.
static volatile long g_nvLightOn = 1;
static const uintptr_t kSpotSetRotRva = 0x2CA9F0;
static const unsigned char kSpotSetRotPrologue[8] = { 0x8B,0x02, 0x89,0x81,0x6C,0x02,0x00,0x00 };
// IDENTIFYING THE PLAYER'S LIGHTS - from the live census, not a guess. The player's lights are
// set with { Pitch = the aim's pitch exactly, Yaw ~ 0, Roll = 0 }: the component rotation is
// RELATIVE to the pawn, which already carries the body yaw, so script only has to supply pitch.
// Measured over ~8000 calls: two player lights (pitch offset 0..594 uu, yaw -6..+2) and one
// level light (pitch offset 2000-3500 uu, 75 calls total, does not track). So the test is
// "pitch TRACKS the aim AND yaw is ~0" - which is also why the first attempt barely fired: it
// compared yaw against the ABSOLUTE aim, so it only matched while the body faced yaw~0.
static const int kNvPitchTolUU = 1200;      // ~6.6 deg - player lights sat inside 600, level at 2000+
static const int kNvYawZeroUU  = 900;       // ~5 deg  - the relative yaw is ~0 for a player light

typedef void (__fastcall *SpotSetRot_t)(void*, int*);
static SpotSetRot_t o_SpotSetRot = nullptr;

// (no learned-component state: the correction is applied per call, in flight)
static volatile long g_nvHits = 0;
// (Test43) [NVEYE] which eye pass a player light's rotation is set in, and to what (the NV black wedge)
static volatile long g_nvPhaseN[3] = { 0, 0, 0 };
static int g_nvPhaseRot[3][3] = {};
static void* g_nvPhaseComp[3] = {};

extern "C" int  OLUE3_GetNvLight() { return g_nvLightOn ? 1 : 0; }
extern "C" void OLUE3_SetNvLight(int v) { g_nvLightOn = v ? 1 : 0; }

// Signed wrap for UE3's 16-bit-style angles, so comparisons work across the 0/65536 seam.
static int WrapUE(int d) { d &= 0xFFFF; if (d > 32768) d -= 65536; return d; }

static bool NvHeadDelta(int* outPitch, int* outYaw) {
    if (!g_headTrack || !g_olPoseValid) return false;
    const float w = g_injW;                      // 0 while the cinema screen is up
    if (w <= 0.0f) return false;
    const float kToUnits = 65536.0f / 6.28318531f;
    *outPitch = (int)(g_olPitchRad * w * kToUnits);
    *outYaw   = (int)(g_olYawRad   * w * kToUnits);
    return true;
}

// [NVLIGHT] TELEMETRY. The hook fires and matches, but the light still does not follow the head
// live - so the assumption that "this spotlight's Rotation is what aims the beam" is unproven.
// Census every distinct component that gets SetRotation, with its rotation vs the player's aim,
// so one run says which light is actually being moved and whether anything else is driving it.
static void*   g_nvSeen[6]   = { 0 };
static long    g_nvSeenN[6]  = { 0 };
static int     g_nvSeenRot[6][3] = { { 0 } };
static int     g_nvDistinct  = 0;
static long    g_nvCalls     = 0;
static long    g_nvMatched   = 0;

static void NvCensus(void* comp, const int* rot) {
    int idx = -1;
    for (int i = 0; i < g_nvDistinct; ++i) if (g_nvSeen[i] == comp) { idx = i; break; }
    if (idx < 0 && g_nvDistinct < 6) { idx = g_nvDistinct++; g_nvSeen[idx] = comp; g_nvSeenN[idx] = 0; }
    if (idx >= 0) {
        ++g_nvSeenN[idx];
        g_nvSeenRot[idx][0] = rot[0]; g_nvSeenRot[idx][1] = rot[1]; g_nvSeenRot[idx][2] = rot[2];
    }
}

extern "C" void OLUE3_NvLightReport() {
    if (!g_nvCalls) { OLProxyLog("[OLVR][NVCENSUS] SetRotation has NEVER been called - this is not "
                                 "how the NV light is aimed"); return; }
    char l[240];
    sprintf_s(l, "[OLVR][NVCENSUS] calls=%ld matched=%ld distinct=%d  baseView(P=%d Y=%d) head(P=%d Y=%d)",
              g_nvCalls, g_nvMatched, g_nvDistinct, g_baseViewRot[0], g_baseViewRot[1],
              (int)(g_olPitchRad * (65536.0f / 6.28318531f)), (int)(g_olYawRad * (65536.0f / 6.28318531f)));
    OLProxyLog(l);
    for (int ph = 0; ph < 3; ++ph) {
        sprintf_s(l, "[OLVR][NVEYE] player light set in eye pass %d: %ld times, last comp %p rot (P=%d Y=%d R=%d)", ph,
                  InterlockedExchange(&g_nvPhaseN[ph], 0), g_nvPhaseComp[ph], g_nvPhaseRot[ph][0], g_nvPhaseRot[ph][1], g_nvPhaseRot[ph][2]);
        OLProxyLog(l);
    }
    for (int i = 0; i < g_nvDistinct; ++i) {
        sprintf_s(l, "[OLVR][NVCENSUS]   comp %p calls=%ld lastRot(P=%d Y=%d R=%d) offFromAim(P=%d Y=%d)",
                  g_nvSeen[i], g_nvSeenN[i], g_nvSeenRot[i][0], g_nvSeenRot[i][1], g_nvSeenRot[i][2],
                  WrapUE(g_nvSeenRot[i][0] - g_baseViewRot[0]), WrapUE(g_nvSeenRot[i][1] - g_baseViewRot[1]));
        OLProxyLog(l);
    }
}

static void __fastcall Hook_SpotSetRotation(void* comp, int* rot) {
    if (!g_nvLightOn || !rot) { o_SpotSetRot(comp, rot); return; }
    int mod[3];
    bool use = false;
    __try {
        InterlockedIncrement(&g_nvCalls);
        NvCensus(comp, rot);
        int dp = 0, dy = 0;
        if (g_baseViewFresh > 0 && NvHeadDelta(&dp, &dy)) {
            const int offP = WrapUE(rot[0] - g_baseViewRot[0]);   // does the pitch track the aim?
            const int relY = WrapUE(rot[1]);                      // is the yaw relative (~0)?
            if (offP > -kNvPitchTolUU && offP < kNvPitchTolUU &&
                relY > -kNvYawZeroUU  && relY < kNvYawZeroUU) {
                InterlockedIncrement(&g_nvMatched);
                { const long ph = g_eyePhase; if (ph >= 0 && ph <= 2) { InterlockedIncrement(&g_nvPhaseN[ph]); g_nvPhaseRot[ph][0] = rot[0]; g_nvPhaseRot[ph][1] = rot[1]; g_nvPhaseRot[ph][2] = rot[2]; g_nvPhaseComp[ph] = comp; } }
                // The yaw is RELATIVE to the body, and the head yaw is also measured from the
                // body's forward (the recenter baseline) - so it adds directly. Pitch is the
                // aim's pitch, and the head pitch is likewise a delta on it. Both just add.
                mod[0] = rot[0] + dp; mod[1] = rot[1] + dy; mod[2] = rot[2];
                use = true;
                if (InterlockedIncrement(&g_nvHits) == 1)
                    OLProxyLog("[OLVR][NVLIGHT] player light(s) identified (pitch tracks the aim, "
                               "yaw is body-relative) - they now follow your head");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { use = false; }
    o_SpotSetRot(comp, use ? mod : rot);
}

// NO per-frame re-drive, deliberately. The first cut re-applied the rotation every Present in
// case script only set it when the aim moved - but the census settles it: 7435 calls across
// ~8200 frames on the main light, i.e. script drives these lights unconditionally every frame.
// So the parameter correction above is sufficient, and dropping the re-drive removes the only
// place this feature wrote into a live engine object (and the only stale-pointer risk with it).

// ---- [CAMPROBE] logging-only camera decomposition probe ----------------------------------------
// Answers, with numbers instead of another feel-build: what does the Hero-Camera bone actually do
// per state (idle / walk / camcorder up / trigger lean), and is there a readable PAWN ROOT whose
// motion is pure locomotion? If camera = root + animation, the animation term can be scaled
// exactly (bob/sway off, locomotion untouched) and a stable camera lean finally has a base.
// Armed by [Stage3] CamProbe=1, default off = this block costs one integer compare per frame.
// Output: %LOCALAPPDATA%\OutlastVR\CamProbe.csv - per frame the base camera pos/rot (pre-
// injection) + rendered FOV, plus up to 8 candidate "root" triplets found by scanning the hooked
// actor object (direct fields at frame 600, one pointer hop at frame 660 - PC -> Pawn -> Location
// is one hop). Candidate identities (offsets) go to the main log; the CSV carries their values
// every frame so bob-vs-flat separates them in analysis. Row cap 30k (~8 min), then it stops.
static long  g_camProbe = 0;
static FILE* g_probeFile = nullptr;
static long  g_probeRows = 0, g_probeFrame = 0;
static struct { int ptrHop; unsigned offA, offB; } g_probeCand[8];
static int   g_probeCandN = 0;

static int ProbeReadF3(const void* addr, float* out) {
    __try {
        const float* f = (const float*)addr;
        const float a = f[0], b = f[1], c = f[2];
        if (!(a == a) || !(b == b) || !(c == c)) return 0;                       // NaN
        if (fabsf(a) > 1e7f || fabsf(b) > 1e7f || fabsf(c) > 1e7f) return 0;     // not a world pos
        out[0] = a; out[1] = b; out[2] = c; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* ProbeReadPtr(const void* addr) {
    __try {
        void* p = *(void* const*)addr;
        if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x00007FFFFFFF0000ull) return nullptr;
        return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static int ProbeNear(const float* v, const float* cam) {
    const float dx = v[0] - cam[0], dy = v[1] - cam[1], dz = v[2] - cam[2];
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) return 0;
    return dx * dx + dy * dy + dz * dz <= 400.0f * 400.0f;   // within 4 m of the camera
}
static float g_probeCandV[8][3];   // value at record time, to reject duplicate finds
static void ProbeScan(void* actor, const float* cam, int ptrHop) {
    char l[192];
    unsigned nextDirectOk = 0;
    for (unsigned offA = 0; offA <= 0x7F8 && g_probeCandN < 8; offA += (ptrHop ? 8 : 4)) {
        void* base = actor;
        if (ptrHop) {
            base = ProbeReadPtr((char*)actor + offA);
            if (!base || base == actor) continue;
        } else if (offA < nextDirectOk) {
            continue;                        // inside the previous hit's triplet, skip
        }
        int fromThisPtr = 0;
        for (unsigned offB = 0; offB <= (ptrHop ? 0x3F8u : 0u) && g_probeCandN < 8; offB += 4) {
            float v[3];
            const unsigned off = ptrHop ? offB : offA;
            if (!ProbeReadF3((char*)base + off, v) || !ProbeNear(v, cam)) continue;
            // The same location reached through two different pointers is not two separate findings.
            int dup = 0;
            for (int i = 0; i < g_probeCandN; ++i) {
                const float ex = v[0] - g_probeCandV[i][0], ey = v[1] - g_probeCandV[i][1],
                            ez = v[2] - g_probeCandV[i][2];
                if (ex * ex + ey * ey + ez * ez < 0.25f) { dup = 1; break; }
            }
            if (dup) { offB += 8; continue; }
            g_probeCand[g_probeCandN].ptrHop = ptrHop;
            g_probeCand[g_probeCandN].offA = offA;
            g_probeCand[g_probeCandN].offB = ptrHop ? off : 0;
            g_probeCandV[g_probeCandN][0] = v[0]; g_probeCandV[g_probeCandN][1] = v[1];
            g_probeCandV[g_probeCandN][2] = v[2];
            if (ptrHop) sprintf_s(l, "[OLVR][CAMPROBE] c%d = ptr@+0x%X -> +0x%X = (%.1f, %.1f, %.1f)  below cam %.1f uu",
                                  g_probeCandN, offA, off, v[0], v[1], v[2], cam[2] - v[2]);
            else        sprintf_s(l, "[OLVR][CAMPROBE] c%d = actor+0x%X = (%.1f, %.1f, %.1f)  below cam %.1f uu",
                                  g_probeCandN, offA, v[0], v[1], v[2], cam[2] - v[2]);
            OLProxyLog(l);
            ++g_probeCandN;
            offB += 8;                       // don't re-hit the same triplet at +4
            if (!ptrHop) { nextDirectOk = offA + 12; break; }
            if (++fromThisPtr >= 2) break;   // max 2 per pointer, keep variety
        }
    }
}
static void ProbeTick(void* actor, const float* loc, const int* rot) {
    if (g_probeRows >= 30000) return;
    if (!g_probeFile) {
        wchar_t path[MAX_PATH]; wchar_t* lad = _wgetenv(L"LOCALAPPDATA");
        if (!lad) { g_probeRows = 30000; return; }
        swprintf_s(path, L"%s\\OutlastVR\\CamProbe.csv", lad);
        g_probeFile = _wfsopen(path, L"w", _SH_DENYWR);
        if (!g_probeFile) { g_probeRows = 30000; return; }
        fprintf(g_probeFile, "ms,frame,camX,camY,camZ,pitch,yaw,roll,fovXdeg,"
                "hX,hY,hZ,armed,hPitchDeg,hYawDeg,"
                "c0x,c0y,c0z,c1x,c1y,c1z,c2x,c2y,c2z,c3x,c3y,c3z,"
                "c4x,c4y,c4z,c5x,c5y,c5z,c6x,c6y,c6z,c7x,c7y,c7z\n");
        OLProxyLog("[OLVR][CAMPROBE] armed - logging the base camera to CamProbe.csv "
                   "(root-candidate scan at ~10s and ~11s in)");
    }
    ++g_probeFrame;
    if (g_probeFrame == 600) ProbeScan(actor, loc, 0);   // direct fields on the hooked actor
    if (g_probeFrame == 660) {
        ProbeScan(actor, loc, 1);                        // one pointer hop (PC -> Pawn -> Location)
        char l[96]; sprintf_s(l, "[OLVR][CAMPROBE] scan done, %d candidate(s) in the CSV", g_probeCandN);
        OLProxyLog(l);
    }
    float fx = 0.0f, fyv = 0.0f; OLUE3_GetEyeFov(&fx, &fyv);
    // [CAMPROBE v2] the HEAD-POSITION FEED itself, next to the camera it drives. Reports of an
    // apparent movement limit while sitting still tie to these values reaching METRES -
    // this is the column set that convicts the feed (origin jump / prediction overshoot) or
    // clears it (pitch-correlated pumping = neck-orbit double-count).
    fprintf(g_probeFile, "%lu,%ld,%.2f,%.2f,%.2f,%d,%d,%d,%.1f,%.4f,%.4f,%.4f,%d,%.2f,%.2f",
            GetTickCount(), g_probeFrame, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2],
            fx * 57.2957795f,
            g_olHeadX, g_olHeadY, g_olHeadZ, OLXR_PosArmed(),
            g_olPitchRad * 57.2957795f, g_olYawRad * 57.2957795f);
    for (int i = 0; i < 8; ++i) {
        float v[3]; int ok = 0;
        if (i < g_probeCandN) {
            void* base = actor;
            unsigned off = g_probeCand[i].offA;
            if (g_probeCand[i].ptrHop) {
                base = ProbeReadPtr((char*)actor + g_probeCand[i].offA);
                off = g_probeCand[i].offB;
            }
            if (base) ok = ProbeReadF3((char*)base + off, v);
        }
        if (ok) fprintf(g_probeFile, ",%.2f,%.2f,%.2f", v[0], v[1], v[2]);
        else    fprintf(g_probeFile, ",,,");
    }
    fprintf(g_probeFile, "\n");
    if ((++g_probeRows % 60) == 0) fflush(g_probeFile);
}

// ---- [BONEPROBE] find Miles's skeleton: the ground truth for real arm motion controls ----------
// Goal: moving the controller moves Miles's arms. That needs three addresses this
// probe exists to find, log-only, one shot:
//   1. GNames - UE3's global name table, so FName indices become readable strings;
//   2. the RefSkeleton bone-name array of the skeletal mesh the view lives in (CameraBoneName=
//      Hero-Camera, so the array CONTAINING a name like that is the one the camera rides - the
//      same skeleton as the arms);
//   3. bone-count-sized transform arrays on the owning component (SpaceBases/LocalAtoms are in
//      the exe's strings) - the eventual WRITE target.
// Nothing is assumed about struct layouts: the bone array is found by trying stride x name-offset
// grids and demanding that 7 of 8 consecutive elements resolve to printable names - the same
// runtime-discovery style that found ME3's HUD element table.
// Armed by [Stage3] BoneProbe=1, default off. Everything is SEH-guarded reads; nothing is written.
static long  g_boneProbe = 0;
static void** g_bpNames = nullptr;     // GNames: flat FNameEntry* array
// [NAMESFIX] (Test36) GNames is a TArray: when a level load adds names past its capacity the engine
// MOVES the array and frees the old one. Test35 found GNames at the main menu (its menus no longer
// waited for gameplay), cached that pointer, and after the save loaded every name lookup read the
// freed block - so Miles_beheaded was never recognised and the arms never left the game's own
// animation. The in-exe slot holding {Data, Num, Max} never moves: re-read it on every lookup.
static void* const* g_bpNamesSlot = nullptr;
static unsigned g_bpNameSoff = 0;      // offset of the ANSI string inside an FNameEntry
static int   g_bpBoneCount = 0;        // bone count of the identified view skeleton

static int BP_Read(const void* a, void* out, size_t n) {
    __try { memcpy(out, a, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int BP_Printable(const char* s, int cap) {
    if (s[0] < 'A' || (s[0] > 'Z' && s[0] < 'a') || s[0] > 'z') return 0;   // names start with a letter
    for (int i = 1; i < cap; ++i) {
        const char c = s[i];
        if (!c) return 1;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 0;
}
static const char* BP_Name(uint32_t idx, char* buf, size_t bufn) {
    if (!g_bpNames || idx > 2u * 1000u * 1000u) return nullptr;
    if (g_bpNamesSlot) {
        struct { void** d; int n; int mx; } ta = {};
        if (!BP_Read(g_bpNamesSlot, &ta, sizeof(ta)) || !ta.d || ta.n <= 0 || ta.mx < ta.n) return nullptr;
        if (ta.d != g_bpNames) {
            g_bpNames = ta.d;
            static long s_moves = 0;
            if (InterlockedIncrement(&s_moves) <= 8) {
                char l[160];
                sprintf_s(l, "[OLVR][NAMESFIX] the engine moved its name table (level load): now %p, %d names", (void*)ta.d, ta.n);
                OLProxyLog(l);
            }
        }
        if ((int)idx >= ta.n) return nullptr;
    }
    void* e = nullptr;
    if (!BP_Read(g_bpNames + idx, &e, 8) || !e) return nullptr;
    char tmp[64];
    if (!BP_Read((char*)e + g_bpNameSoff, tmp, sizeof(tmp))) return nullptr;
    tmp[63] = 0;
    if (!BP_Printable(tmp, 63)) return nullptr;
    strcpy_s(buf, bufn, tmp);
    return buf;
}

// GNames: scan the exe's writable sections for a pointer to an array whose entry 0 carries the
// string "None" and entry 1 "ByteProperty" - indices 0 and 1 of every UE3 name table, which makes
// false positives effectively impossible.
static void BP_FindGNames() {
    char l[160];
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!BP_Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return;
    if (!BP_Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)(base + dos.e_lfanew +
        FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sh;
        if (!BP_Read(sec + s, &sh, sizeof(sh))) continue;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* p = base + sh.VirtualAddress;
        const uint8_t* end = p + sh.Misc.VirtualSize - 8;
        for (; p < end; p += 8) {
            void** cand = nullptr;
            if (!BP_Read(p, &cand, 8)) { p += 0x10000 - 8; continue; }   // unreadable page: skip ahead
            if ((uintptr_t)cand < 0x10000 || (uintptr_t)cand > 0x00007FFFFFFF0000ull) continue;
            void* e0 = nullptr, *e1 = nullptr;
            if (!BP_Read(cand, &e0, 8) || !e0 || !BP_Read(cand + 1, &e1, 8) || !e1) continue;
            char h0[0x40], h1[0x40];
            if (!BP_Read(e0, h0, sizeof(h0)) || !BP_Read(e1, h1, sizeof(h1))) continue;
            for (unsigned off = 0; off <= 0x38 - 5; ++off) {
                if (memcmp(h0 + off, "None", 5) == 0 && memcmp(h1 + off, "ByteProperty", 13) == 0) {
                    g_bpNames = cand; g_bpNameSoff = off;
                    // [NAMESFIX] only trust the slot as a live TArray header if it reads as one.
                    struct { void** d; int n; int mx; } ta = {};
                    const bool hdr = BP_Read(p, &ta, sizeof(ta)) && ta.d == cand && ta.n > 1000 && ta.mx >= ta.n;
                    g_bpNamesSlot = hdr ? (void* const*)p : nullptr;
                    sprintf_s(l, "[OLVR][BONEPROBE] GNames @ %p (in-exe slot %p, %s %d/%d), entry string offset 0x%X",
                              (void*)cand, (const void*)p, hdr ? "tracked, names" : "NOT a TArray header",
                              ta.n, ta.mx, off);
                    OLProxyLog(l);
                    return;
                }
            }
        }
    }
    OLProxyLog("[OLVR][BONEPROBE] GNames NOT found - no name table, cannot read bones this way");
}

// Scan one object for a TArray that reads as a bone-name table: {Data, Num, Max} sane, and 7 of
// the first 8 elements resolving to printable names on some stride/name-offset pair.
// Run 1 lesson (2026-08-17): "7 of 8 resolve" alone is a WEAK test - any array holding a
// constant dword that happens to be a valid name index passes it, which buried the log in
// false tables all reading the same name. A real skeleton never repeats a bone name, so the
// test is now 8-of-8 resolving AND at least 6 DISTINCT strings. And run 1's stride grid
// stopped at 0x40 while UE3's x64 FMeshBone (FName + flags + VJointPos + parent + children)
// is ~0x44-0x50 - the real RefSkeleton fell straight through the grid. Extended to 0x70.
static const unsigned kBpStride[]  = { 0x10, 0x14, 0x18, 0x1C, 0x20, 0x28, 0x30, 0x38, 0x40,
                                       0x44, 0x48, 0x4C, 0x50, 0x58, 0x60, 0x68, 0x70 };
static const unsigned kBpNameOff[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20,
                                       0x28, 0x2C, 0x30, 0x38 };
static int BP_ScanForBones(void* obj, const char* how) {
    if (!obj) return 0;
    char l[256], nm[64];
    int found = 0;
    for (unsigned off = 0; off <= 0x900; off += 8) {
        struct { void* d; int n; int mx; } ta;
        if (!BP_Read((char*)obj + off, &ta, 16)) continue;
        if (!ta.d || ta.n < 12 || ta.n > 512 || ta.mx < ta.n || ta.mx > 1024) continue;
        for (unsigned si = 0; si < sizeof(kBpStride) / sizeof(kBpStride[0]); ++si) {
            for (unsigned ni = 0; ni < sizeof(kBpNameOff) / sizeof(kBpNameOff[0]); ++ni) {
                const unsigned stride = kBpStride[si], noff = kBpNameOff[ni];
                if (noff + 4 > stride) continue;
                int good = 0, distinct = 0;
                char seenNm[8][64];
                for (int i = 0; i < 8; ++i) {
                    uint32_t idx = 0;
                    if (!BP_Read((char*)ta.d + (size_t)i * stride + noff, &idx, 4) ||
                        !BP_Name(idx, nm, sizeof(nm))) break;
                    ++good;
                    bool dup = false;
                    for (int j = 0; j < distinct; ++j) if (!strcmp(seenNm[j], nm)) { dup = true; break; }
                    if (!dup) strcpy_s(seenNm[distinct++], nm);
                }
                if (good < 8 || distinct < 6) continue;
                // Real find. Dump the table and look for the camera bone.
                sprintf_s(l, "[OLVR][BONEPROBE] BONE TABLE via %s: obj %p +0x%X  count=%d stride=0x%X nameOff=0x%X",
                          how, obj, off, ta.n, stride, noff);
                OLProxyLog(l);
                int camIdx = -1;
                char line[200]; int ll = 0; line[0] = 0;
                const int dumpN = ta.n < 160 ? ta.n : 160;   // arms can index past 64; dump more
                for (int i = 0; i < dumpN; ++i) {
                    uint32_t idx = 0; nm[0] = 0;
                    BP_Read((char*)ta.d + (size_t)i * stride + noff, &idx, 4);
                    if (!BP_Name(idx, nm, sizeof(nm))) strcpy_s(nm, "?");
                    if (strstr(nm, "amera")) camIdx = i;   // Hero-Camera, camera, Camera...
                    // Flush BEFORE appending when space runs low - sprintf_s on overflow is
                    // process death, not truncation (killed run 4 in the sibling dump).
                    if (ll > 140) { sprintf_s(l, "[OLVR][BONEPROBE]   %.200s", line); OLProxyLog(l); ll = 0; line[0] = 0; }
                    ll += sprintf_s(line + ll, sizeof(line) - ll, "%d:%.40s  ", i, nm);
                }
                if (ll) { sprintf_s(l, "[OLVR][BONEPROBE]   %.200s", line); OLProxyLog(l); }
                if (camIdx >= 0) {
                    sprintf_s(l, "[OLVR][BONEPROBE] *** camera bone at index %d - THIS is the view skeleton "
                                 "(the arms live here too) ***", camIdx);
                    OLProxyLog(l);
                    g_bpBoneCount = ta.n;
                }
                ++found;
                break;
            }
            if (found) break;
        }
    }
    return found;
}

// Transform arrays: any TArray on the object whose Num equals the identified bone count and whose
// data reads as floats. Those are SpaceBases/LocalAtoms shaped - the future write targets.
static void BP_ScanForTransforms(void* obj, const char* how) {
    if (!obj || !g_bpBoneCount) return;
    char l[192];
    for (unsigned off = 0; off <= 0x900; off += 8) {
        struct { void* d; int n; int mx; } ta;
        if (!BP_Read((char*)obj + off, &ta, 16)) continue;
        if (!ta.d || ta.n != g_bpBoneCount || ta.mx < ta.n || ta.mx > 2048) continue;
        float f[8];
        if (!BP_Read(ta.d, f, sizeof(f))) continue;
        int fin = 0;
        for (int i = 0; i < 8; ++i) if (f[i] == f[i] && fabsf(f[i]) < 1e7f) ++fin;
        if (fin < 8) continue;
        sprintf_s(l, "[OLVR][BONEPROBE] transform-shaped array via %s: obj %p +0x%X  num=%d "
                     "(first floats %.2f %.2f %.2f %.2f)", how, obj, off, ta.n, f[0], f[1], f[2], f[3]);
        OLProxyLog(l);
    }
}

// [BONEPROBE r3] Run 2 lesson: blind two-hop pointer walking drowns - the 1024-object budget
// went to UI localization tables and property metadata without ever touching a mesh. The engine
// keeps a global array of EVERY live object (GObjObjects), and with GNames working, it can be
// found the same way GNames was found - by shape - then simply asked for every object named
// *SkeletalMeshComponent*. No hops, no luck.
// GObjObjects discovery: a TArray{UObject** data; int num; int max} in the exe's writable data
// with a big num, where some fixed offset inside the pointed-to objects resolves to printable
// names for nearly every sampled object. That offset IS UObject::Name - discovered, not assumed.
static void** g_bpObjs = nullptr;      // GObjObjects data
static int    g_bpObjNum = 0;
static unsigned g_bpUNameOff = 0;      // UObject::Name offset, discovered by voting
static const uint8_t* g_bpObjHeader = nullptr; // live TArray header; data can move on map loads

static bool BP_RefreshObjects() {
    if (!g_bpObjHeader) return g_bpObjs && g_bpObjNum > 0;
    struct { void** d; int n; int mx; } ta = {};
    if (!BP_Read(g_bpObjHeader, &ta, sizeof(ta)) || !ta.d || ta.n < 5000 ||
        ta.mx < ta.n || ta.mx > 5000000) return false;
    if (ta.d != g_bpObjs || ta.n != g_bpObjNum) {
        const bool moved = ta.d != g_bpObjs;     // (Test45) the count changes every few seconds as things spawn
        g_bpObjs = ta.d; g_bpObjNum = ta.n;
        if (moved) {
            char l[160];
            sprintf_s(l, "[OLVR][BONEPROBE] GObjObjects refreshed after level transition: %p num=%d",
                      (void*)g_bpObjs, g_bpObjNum);
            OLProxyLog(l);
        }
    }
    return true;
}

static void BP_FindObjects() {
    char l[192], nm[64];
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!BP_Read(base, &dos, sizeof(dos)) || !BP_Read(base + dos.e_lfanew, &nt, sizeof(nt))) return;
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)(base + dos.e_lfanew +
        FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sh;
        if (!BP_Read(sec + s, &sh, sizeof(sh))) continue;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* p = base + sh.VirtualAddress;
        const uint8_t* end = p + sh.Misc.VirtualSize - 16;
        for (; p < end; p += 8) {
            struct { void** d; int n; int mx; } ta;
            if (!BP_Read(p, &ta, 16)) { p += 0x10000 - 8; continue; }
            if (!ta.d || ta.n < 5000 || ta.mx < ta.n || ta.mx > 5000000) continue;
            if ((uintptr_t)ta.d < 0x10000 || (uintptr_t)ta.d > 0x00007FFFFFFF0000ull) continue;
            // Sample 16 objects spread across the array; vote for the UObject::Name offset.
            void* smp[16]; int nsmp = 0;
            for (int k = 0; k < 16; ++k) {
                void* o = nullptr;
                if (BP_Read(ta.d + (size_t)k * (ta.n / 16), &o, 8) && o &&
                    (uintptr_t)o > 0x10000 && (uintptr_t)o < 0x00007FFFFFFF0000ull) smp[nsmp++] = o;
            }
            if (nsmp < 12) continue;
            // Run 3 lesson: "resolves to a printable name" alone voted for ObjectInternalInteger -
            // the object's own INDEX in this very array. Every small int resolves to SOME name, so
            // a counter beats the real Name on that test. Two mechanical separators:
            //   - a counter EQUALS its own array slot; a name never tracks the slot;
            //   - real names REPEAT across objects (hundreds share 'Function'); a counter cannot.
            // So: disqualify slot-tracking offsets, then require the winner to actually contain
            // objects named Function - the one name every UE3 registry holds in bulk.
            int slotOf[16];
            for (int k = 0; k < nsmp; ++k) slotOf[k] = k * (ta.n / 16);
            unsigned candOff[4] = {}; int candHits[4] = {}; int ncand = 0;
            for (unsigned no = 0x18; no <= 0xA0; no += 8) {
                int hits = 0, self = 0, rep = 0; uint32_t seenIdx[16]; int nseen = 0;
                for (int k = 0; k < nsmp; ++k) {
                    uint32_t idx = 0;
                    if (!BP_Read((char*)smp[k] + no, &idx, 4)) continue;
                    if ((int)idx == slotOf[k]) ++self;
                    if (!BP_Name(idx, nm, sizeof(nm))) continue;
                    ++hits;
                    for (int j = 0; j < nseen; ++j) if (seenIdx[j] == idx) { ++rep; break; }
                    if (nseen < 16) seenIdx[nseen++] = idx;
                }
                if (hits < nsmp - 2 || self >= nsmp / 2) continue;
                if (ncand < 4) { candOff[ncand] = no; candHits[ncand] = hits + rep; ++ncand; }
            }
            if (!ncand) continue;
            // Validate candidates against the full array: the true Name offset finds 'Function'
            // objects within the first few thousand entries; a wrong one never does.
            for (int ci = 0; ci < ncand; ++ci) {
                int fn = 0;
                const int lim = ta.n < 20000 ? ta.n : 20000;
                for (int i = 0; i < lim && fn <= 10; ++i) {
                    void* o = nullptr; uint32_t idx = 0;
                    if (!BP_Read(ta.d + i, &o, 8) || !o) continue;
                    if (!BP_Read((char*)o + candOff[ci], &idx, 4)) continue;
                    if (BP_Name(idx, nm, sizeof(nm)) && strstr(nm, "Function")) ++fn;
                }
                if (fn <= 10) continue;
                g_bpObjs = ta.d; g_bpObjNum = ta.n; g_bpUNameOff = candOff[ci];
                g_bpObjHeader = p;
                sprintf_s(l, "[OLVR][BONEPROBE] GObjObjects @ %p num=%d, UObject::Name at +0x%X "
                             "(validated: Function objects present)", (void*)ta.d, ta.n, candOff[ci]);
                OLProxyLog(l);
                // Self-diagnosing sample so a wrong pick is visible in the log without a rebuild.
                // BUFFER RULE, learned the hard way in run 4: sprintf_s TERMINATES THE PROCESS on
                // overflow, it does not truncate - the run-4 probe died exactly here, printing a
                // ~215-char sample line through a 192-byte buffer, one line before the answer.
                // So: check remaining space BEFORE appending, cap every name with %.40s, and
                // print through a buffer sized for the worst case.
                char line[200]; int ll = 0; line[0] = 0;
                for (int k = 0; k < 12 && ll < 140; ++k) {
                    void* o = nullptr; uint32_t idx = 0; nm[0] = 0;
                    BP_Read(ta.d + (size_t)k * (ta.n / 12), &o, 8);
                    if (o && BP_Read((char*)o + g_bpUNameOff, &idx, 4) && BP_Name(idx, nm, sizeof(nm)))
                        ll += sprintf_s(line + ll, sizeof(line) - ll, "%.40s  ", nm);
                }
                char big[256];
                sprintf_s(big, "[OLVR][BONEPROBE] sample names: %.200s", line);
                OLProxyLog(big);
                return;
            }
            continue;
        }
    }
    OLProxyLog("[OLVR][BONEPROBE] GObjObjects NOT found");
}

static const char* BP_ObjName(void* obj, char* buf, size_t bufn) {
    uint32_t idx = 0;
    if (!obj || !BP_Read((char*)obj + g_bpUNameOff, &idx, 4)) return nullptr;
    return BP_Name(idx, buf, bufn);
}

// One shot, from the camera hook's actor. Walk: the actor itself, every object it points at
// (depth 1), and every object THOSE point at (depth 2). The bone table usually sits two hops out
// (PC -> Pawn -> MeshComponent -> SkeletalMesh asset), so depth-2 objects also get one extra hop
// for the bone scan only.
static void BP_Run(void* actor) {
    char l[160];
    OLProxyLog("[OLVR][BONEPROBE] scanning for the skeleton (one-shot, log-only)...");
    BP_FindGNames();
    if (!g_bpNames) return;
    // Three separate buffers - run 1 used one buffer for all three %s and printed the LAST
    // evaluated call three times, which made a working GNames look broken in the log.
    char n0[64], n1[64], n2[64];
    sprintf_s(l, "[OLVR][BONEPROBE] name sanity: 0='%s' 1='%s' 2='%s'",
              BP_Name(0, n0, sizeof(n0)) ? n0 : "?",
              BP_Name(1, n1, sizeof(n1)) ? n1 : "?",
              BP_Name(2, n2, sizeof(n2)) ? n2 : "?");
    OLProxyLog(l);

    (void)actor;
    BP_FindObjects();
    if (!g_bpObjs) return;

    // Every object named *SkeletalMeshComponent* - the class object and every live instance
    // share the name's base string, and Outlast subclasses (OL...) keep the substring. A tiny
    // verdict cache keyed on the FName INDEX keeps this one string-compare per unique name, not
    // per object, across a registry that can run to hundreds of thousands of entries.
    static uint32_t s_key[16384]; static int8_t s_val[16384];
    memset(s_key, 0xFF, sizeof(s_key)); memset(s_val, 0, sizeof(s_val));
    char nm[64];
    void* comps[64]; int nc = 0;
    int scanned = 0;
    for (int i = 0; i < g_bpObjNum && nc < 64; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        uint32_t idx = 0;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        ++scanned;
        const unsigned h = idx & 16383;
        int verdict;
        if (s_key[h] == idx) verdict = s_val[h];
        else {
            verdict = (BP_Name(idx, nm, sizeof(nm)) && strstr(nm, "SkeletalMeshComponent")) ? 1 : 0;
            s_key[h] = idx; s_val[h] = (int8_t)verdict;
        }
        if (verdict) comps[nc++] = o;
    }
    sprintf_s(l, "[OLVR][BONEPROBE] %d objects scanned, %d *SkeletalMeshComponent* found%s",
              scanned, nc, nc == 64 ? " (capped)" : "");
    OLProxyLog(l);

    // For each component: its name, then every object it points at gets the bone-table scan -
    // the SkeletalMesh asset is one hop from its component. The asset's NAME is logged with the
    // table, because the asset name is what identifies which mesh is Miles's arms.
    int hits = 0;
    static void* s_done[512]; int nd = 0;   // same asset hangs off many components/fields: scan once
    auto done = [&](void* p) -> bool {
        for (int i = 0; i < nd; ++i) if (s_done[i] == p) return true;
        if (nd < 512) s_done[nd++] = p;
        return false;
    };
    for (int c = 0; c < nc; ++c) {
        char cn[64];
        if (!BP_ObjName(comps[c], cn, sizeof(cn))) strcpy_s(cn, "?");
        for (unsigned off = 0; off <= 0xB00; off += 8) {
            void* p = ProbeReadPtr((char*)comps[c] + off);
            if (!p || p == comps[c] || done(p)) continue;
            const int before = g_bpBoneCount;
            char how[160], an[64];
            if (!BP_ObjName(p, an, sizeof(an))) an[0] = 0;
            sprintf_s(how, "comp '%.40s' +0x%X -> '%.40s'", cn, off, an[0] ? an : "?");
            const int f = BP_ScanForBones(p, how);
            hits += f;
            // A table appeared under this component: its transform arrays live ON the component.
            if (f && g_bpBoneCount && g_bpBoneCount != before) {
                char tag[96]; sprintf_s(tag, "comp '%s'", cn);
                BP_ScanForTransforms(comps[c], tag);
            }
        }
    }
    sprintf_s(l, "[OLVR][BONEPROBE] done: %d bone table(s), view-skeleton bone count %d",
              hits, g_bpBoneCount);
    OLProxyLog(l);
}

// ---- [WIGGLE] prove the bone write path: oscillate Miles's right arm on purpose ----------------
// BONEPROBE r5 delivered the map: asset 'Miles_beheaded', 73 bones (R-UpperArm 34, R-Forearm 35,
// R-Hand 36), and the owning component carries two 73-element FBoneAtom arrays at +0x2D8/+0x2E8
// (quat 16 + translation 12 + scale 4 = 0x20 stride; first element reads 0,0,0,1 = identity
// quat). This probe WRITES those arrays - a bounded sine on the right arm's translation - and
// answers the question every bone-driving plan lives or dies on: DO THESE WRITES SURVIVE THE
// FRAME, or does the animation recompose over them?
// The answer is measured, not eyeballed: each frame reads back what it wrote last frame. Value
// unchanged = nobody recomposed (sticky); value changed = the anim rewrote it first (anim).
// The 3s log line carries the ratio. Sway, never a constant offset - a constant is invisible
// on success and drifts on failure; a 0.5 Hz 15 uu sway is unmistakable and self-centring.
// Armed by [Stage3] BoneWiggle (0 off, 1 = +0x2D8 only, 2 = +0x2E8 only, 3 = both). Dev only.
static long  g_boneWiggle = 0;
static void* g_wigComp[4]; static int g_wigCompN = 0;
static long  g_wigNext = 0;            // rescan countdown
static long  g_wigSticky = 0, g_wigAnim = 0;
static float g_wigLast[4][2][3];       // [comp][array][bone] last value written
static float g_wigDelta[4][2][3];      // and the delta inside it
static uint32_t g_wigMilesIdx = 0;     // FName index of 'Miles_beheaded', cached

static const unsigned kWigArr[2]  = { 0x2D8, 0x2E8 };
static const int      kWigBone[3] = { 34, 35, 36 };   // R-UpperArm, R-Forearm, R-Hand

static void WigFind() {
    g_wigCompN = 0;
    if (!g_bpObjs || !g_bpNames) return;
    char nm[64];
    for (int i = 0; i < g_bpObjNum && g_wigCompN < 4; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm)) || !strstr(nm, "SkeletalMeshComponent")) continue;
        void* mesh = ProbeReadPtr((char*)o + 0x238);
        if (!mesh) continue;
        uint32_t mIdx = 0;
        if (!BP_Read((char*)mesh + g_bpUNameOff, &mIdx, 4)) continue;
        if (g_wigMilesIdx) { if (mIdx != g_wigMilesIdx) continue; }
        else {
            char mn[64];
            if (!BP_Name(mIdx, mn, sizeof(mn)) || strcmp(mn, "Miles_beheaded")) continue;
            g_wigMilesIdx = mIdx;
        }
        // Must carry the 73-element atom arrays, or it is a different LOD/purpose component.
        struct { void* d; int n; int mx; } a0, a1;
        if (!BP_Read((char*)o + kWigArr[0], &a0, 16) || a0.n != 73) continue;
        if (!BP_Read((char*)o + kWigArr[1], &a1, 16) || a1.n != 73) continue;
        g_wigComp[g_wigCompN++] = o;
    }
    char l[128];
    sprintf_s(l, "[OLVR][WIGGLE] %d Miles component(s) armed (mode %ld)", g_wigCompN, g_boneWiggle);
    OLProxyLog(l);
    memset(g_wigLast, 0, sizeof(g_wigLast));
    memset(g_wigDelta, 0, sizeof(g_wigDelta));
}

static int WigWrite(void* addr, float v) {
    __try { *(float*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void WigTick() {
    if (--g_wigNext <= 0) { WigFind(); g_wigNext = 600; }
    if (!g_wigCompN) return;
    const float t = (float)(GetTickCount() % 100000u) * 0.001f;
    const float sway = 15.0f * sinf(t * 3.14159265f);   // 0.5 Hz, +/-15 uu
    for (int c = 0; c < g_wigCompN; ++c) {
        for (int a = 0; a < 2; ++a) {
            if (!(g_boneWiggle & (a + 1))) continue;
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)g_wigComp[c] + kWigArr[a], &ta, 16) || ta.n != 73 || !ta.d) continue;
            for (int b = 0; b < 3; ++b) {
                // FBoneAtom: quat[0..3], translation[4..6], scale[7]. Wiggle translation Z.
                float* atom = (float*)((char*)ta.d + (size_t)kWigBone[b] * 0x20);
                float cur = 0.0f;
                if (!BP_Read(atom + 6, &cur, 4)) continue;
                // Readback verdict: still exactly the last written value = nothing recomposed
                // since that write.
                if (cur == g_wigLast[c][a][b] && g_wigDelta[c][a][b] != 0.0f) {
                    ++g_wigSticky;
                    cur -= g_wigDelta[c][a][b];   // strip the previous sway before adding the new one
                } else if (g_wigDelta[c][a][b] != 0.0f) {
                    ++g_wigAnim;
                }
                const float nv = cur + sway;
                if (!WigWrite(atom + 6, nv)) continue;
                g_wigLast[c][a][b] = nv;
                g_wigDelta[c][a][b] = sway;
            }
        }
    }
    static long s_rep = 0;
    if (++s_rep >= 180) {
        s_rep = 0;
        char l[160];
        const long st = InterlockedExchange(&g_wigSticky, 0), an = InterlockedExchange(&g_wigAnim, 0);
        sprintf_s(l, "[OLVR][WIGGLE] 3s: sticky=%ld animRewrote=%ld  (sticky-dominant = this write owns the "
                     "value; anim-dominant = write must land later in the frame)", st, an);
        OLProxyLog(l);
    }
}

// ---- [BONEWATCH] who touches Miles's bone arrays, and from where -------------------------------
// WIGGLE verdict: nothing moved, and animRewrote ran at exactly 2 comps x 2 arrays x 3 bones x
// 60 fps. So the arrays are real and the anim recomposes them every frame - but a write from
// the camera hook is invisible: it lands AFTER the composed pose has already been handed
// to the renderer for that frame, and the next compose wipes it. Writing to the right memory at
// the wrong moment. The fix needs two addresses this probe exists to find:
//   - the COMPOSER: the code that writes these arrays each frame (post-hook it so the override
//     lands after the anim and before extraction), and
//   - the EXTRACTOR(S): the code that reads them for rendering (if extraction is inside the same
//     function as compose, pre-hook the extractor instead).
// Method: hardware data breakpoints (DR0-3, read|write, 4 bytes) on the right hand's translation
// in both arrays of both Miles components, installed on EVERY thread of the process by a helper
// that suspends each thread and writes its context. A vectored handler records (thread, RIP,
// value-changed?) for each hit and, for the first hit of each distinct RIP, the FUNCTION START
// and a 3-frame caller chain via RtlLookupFunctionEntry + RtlVirtualUnwind - real unwinding, never
// a stack scan. Nothing is logged inside the handler; the game thread drains the table on its
// tick. Auto-disarms after ~8 s. Function starts are what the next build hooks with MinHook.
// [Stage3] BoneWatch=1. Dev only.
static long   g_boneWatch = 0;
static long   g_bwGameTid = 0;   // only this thread gets debug registers
static volatile long g_bwArmed = 0, g_bwDisarm = 0;
static void*  g_bwAddr[4] = {};
struct BwHit { DWORD tid; uintptr_t rip; uintptr_t fnStart; uintptr_t chain[3]; long hits; long changed; int which; };
static BwHit  g_bwHits[48];
static volatile long g_bwHitN = 0;
static volatile long g_bwTotal = 0;
static float  g_bwLastVal[4] = {};
static PVOID  g_bwVeh = nullptr;

static LONG CALLBACK BwHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    int which = -1;
    for (int i = 0; i < 4; ++i) if (dr6 & (1ull << i)) { which = i; break; }
    if (which < 0) return EXCEPTION_CONTINUE_SEARCH;   // a single-step from something other than this hook
    c->Dr6 = 0;
    InterlockedIncrement(&g_bwTotal);
    // Value-changed test: the trapping instruction has already executed, so a write shows here.
    float now = 0.0f; __try { now = *(volatile float*)g_bwAddr[which]; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    const bool changed = (now != g_bwLastVal[which]);
    g_bwLastVal[which] = now;
    const uintptr_t rip = (uintptr_t)c->Rip;
    const DWORD tid = GetCurrentThreadId();
    // Find or add the (tid, rip) row.
    long n = g_bwHitN;
    for (long i = 0; i < n; ++i) {
        if (g_bwHits[i].rip == rip && g_bwHits[i].tid == tid) {
            InterlockedIncrement(&g_bwHits[i].hits);
            if (changed) InterlockedIncrement(&g_bwHits[i].changed);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (n < 48) {
        long slot = InterlockedIncrement(&g_bwHitN) - 1;
        if (slot < 48) {
            BwHit& h = g_bwHits[slot];
            h.tid = tid; h.rip = rip; h.hits = 1; h.changed = changed ? 1 : 0; h.which = which;
            h.fnStart = 0; h.chain[0] = h.chain[1] = h.chain[2] = 0;
            // Real unwind for the function start and three callers.
            __try {
                CONTEXT uc = *c;
                for (int f = 0; f < 4; ++f) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(uc.Rip, &base, nullptr);
                    if (f == 0) h.fnStart = rf ? (uintptr_t)(base + rf->BeginAddress) : 0;
                    if (!rf) break;
                    PVOID hd = nullptr; DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, uc.Rip, rf, &uc, &hd, &est, nullptr);
                    if (!uc.Rip) break;
                    if (f < 3) h.chain[f] = (uintptr_t)uc.Rip;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ARMING RULE, learned by crashing the game: NEVER blanket-arm. Run 1 armed read|write
// breakpoints on 300 threads - every read of a bone array on every thread trapped into the VEH,
// and the process died. Now: the GAME THREAD ONLY (skeletal composition is game-thread in UE3),
// WRITE-ONLY traps (the writer is the target, and reads are the overwhelming majority of accesses),
// and if that yields nothing, scope widens deliberately rather than by default.
static void BwSetOnAllThreads(bool arm) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int done = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (te.th32ThreadID != (DWORD)g_bwGameTid) continue;   // game thread only - see the rule above
            HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c)) {
                    if (arm) {
                        c.Dr0 = (DWORD64)g_bwAddr[0]; c.Dr1 = (DWORD64)g_bwAddr[1];
                        c.Dr2 = (DWORD64)g_bwAddr[2]; c.Dr3 = (DWORD64)g_bwAddr[3];
                        // L0..L3 enabled; RW=11 (read|write) LEN=11 (4 bytes) for each.
                        DWORD64 dr7 = 0;
                        for (int i = 0; i < 4; ++i) if (g_bwAddr[i]) { dr7 |= (1ull << (i * 2)); dr7 |= (0xDull << (16 + i * 4)); }   // 0xD = LEN 4 bytes, RW WRITE-ONLY
                        c.Dr7 = dr7;
                    } else {
                        c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr7 = 0;
                    }
                    c.Dr6 = 0;
                    if (SetThreadContext(h, &c)) ++done;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    char l[128];
    sprintf_s(l, "[OLVR][BONEWATCH] debug registers %s on %d thread(s)", arm ? "ARMED" : "cleared", done);
    OLProxyLog(l);
}

static DWORD WINAPI BwArmThread(LPVOID) {
    Sleep(50);
    BwSetOnAllThreads(true);
    InterlockedExchange(&g_bwArmed, 1);
    Sleep(8000);
    BwSetOnAllThreads(false);
    InterlockedExchange(&g_bwDisarm, 1);
    return 0;
}

static void BwReport() {
    const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    char l[320];
    auto rel = [&](uintptr_t a, char* out, size_t n) {
        if (!a) { strcpy_s(out, n, "-"); return; }
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m);
        if ((uintptr_t)m == exe) sprintf_s(out, n, "exe+0x%llX", (unsigned long long)(a - exe));
        else if (m) { wchar_t p[MAX_PATH]; GetModuleFileNameW(m, p, MAX_PATH); const wchar_t* b = wcsrchr(p, L'\\'); sprintf_s(out, n, "%ls+0x%llX", b ? b + 1 : p, (unsigned long long)(a - (uintptr_t)m)); }
        else sprintf_s(out, n, "%p", (void*)a);
    };
    const long n = g_bwHitN < 48 ? g_bwHitN : 48;
    sprintf_s(l, "[OLVR][BONEWATCH] %ld total hits, %ld distinct (thread,rip) sites:", g_bwTotal, n);
    OLProxyLog(l);
    for (long i = 0; i < n; ++i) {
        const BwHit& h = g_bwHits[i];
        char r[48], f[48], c0[48], c1[48], c2[48];
        rel(h.rip, r, sizeof(r)); rel(h.fnStart, f, sizeof(f));
        rel(h.chain[0], c0, sizeof(c0)); rel(h.chain[1], c1, sizeof(c1)); rel(h.chain[2], c2, sizeof(c2));
        sprintf_s(l, "[OLVR][BONEWATCH]  tid=%lu dr%d rip=%s fn=%s hits=%ld changed=%ld  <- %s <- %s <- %s",
                  h.tid, h.which, r, f, h.hits, h.changed, c0, c1, c2);
        OLProxyLog(l);
    }
}

static void BwTick() {
    static bool s_started = false, s_reported = false;
    if (!s_started) {
        if (g_wigCompN < 1) { if (--g_wigNext <= 0) { WigFind(); g_wigNext = 600; } return; }
        // DR0/1 = comp0's two arrays, DR2/3 = comp1's (or comp0 again if only one), bone 36 z.
        for (int i = 0; i < 4; ++i) {
            void* comp = g_wigComp[(i / 2) < g_wigCompN ? (i / 2) : 0];
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)comp + kWigArr[i & 1], &ta, 16) || ta.n != 73 || !ta.d) { g_bwAddr[i] = nullptr; continue; }
            g_bwAddr[i] = (char*)ta.d + (size_t)36 * 0x20 + 6 * 4;   // R-Hand translation.z
        }
        char l[200];
        g_bwGameTid = (long)GetCurrentThreadId();
        sprintf_s(l, "[OLVR][BONEWATCH] gameThread=%lu targets %p %p %p %p (write-only, this thread only)", GetCurrentThreadId(),
                  g_bwAddr[0], g_bwAddr[1], g_bwAddr[2], g_bwAddr[3]);
        OLProxyLog(l);
        g_bwVeh = AddVectoredExceptionHandler(1, BwHandler);
        CreateThread(nullptr, 0, BwArmThread, nullptr, 0, nullptr);
        s_started = true;
        return;
    }
    if (g_bwDisarm && !s_reported) {
        s_reported = true;
        BwReport();
        if (g_bwVeh) { RemoveVectoredExceptionHandler(g_bwVeh); g_bwVeh = nullptr; }
    }
}

// ---- [SKELPROP] read the SkelControl property offsets out of UE3's own reflection ---------------
// SKELCTRL found the prize: Miles's AnimTree already holds LIVE SkelControls on his arms
// (SkelControlLimb on Hero-R-Clavicle = a 2-bone IK effector; SkelControlSingleBone on both upper
// arms and forearms). The animation system ticks them itself, in the correct window - so the
// write-timing problem that beat WIGGLE and BONEWATCH is simply gone.
// To drive one, three field offsets are needed: ControlStrength (float 0..1, the on switch),
// EffectorLocation (vector, for the Limb IK) and BoneTranslation/BoneRotation (for SingleBone).
// GUESSING OFFSETS IS THE MISTAKE THAT COST TODAY, so this reads them from the engine's own
// UProperty chain: every UClass holds a Children list of UProperty objects, each carrying an
// already-resolvable name plus an Offset field. Both link offsets are DISCOVERED by scoring, never
// assumed - the correct Children/Next pair yields a long chain of real property names, and the
// correct Offset field yields values that increase along that chain.
// Log-only, no writes, no threads. [Stage3] SkelPropProbe=1.
static long g_skelProp = 0;

static void* SP_FindClass(const char* want) {
    char nm[64];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (BP_Name(idx, nm, sizeof(nm)) && !strcmp(nm, want)) return o;
    }
    return nullptr;
}

// Walk a candidate (children,next) pair; returns how many links resolved to names.
static int SP_Walk(void* cls, unsigned childOff, unsigned nextOff,
                   char out[48][64], unsigned outOff[48], unsigned propOffField) {
    void* p = ProbeReadPtr((char*)cls + childOff);
    int n = 0;
    while (p && n < 48) {
        uint32_t idx = 0;
        if (!BP_Read((char*)p + g_bpUNameOff, &idx, 4)) break;
        if (!BP_Name(idx, out[n], 64)) break;
        outOff[n] = 0;
        if (propOffField) { uint32_t v = 0; if (BP_Read((char*)p + propOffField, &v, 4)) outOff[n] = v; }
        ++n;
        void* nx = ProbeReadPtr((char*)p + nextOff);
        if (nx == p) break;
        p = nx;
    }
    return n;
}

static void SkelPropRun() {
    char l[256];
    if (!g_bpObjs || !g_bpNames) { OLProxyLog("[OLVR][SKELPROP] registry not ready"); return; }
    const char* classes[3] = { "SkelControlBase", "SkelControlSingleBone", "SkelControlLimb" };
    for (int ci = 0; ci < 3; ++ci) {
        void* cls = SP_FindClass(classes[ci]);
        if (!cls) { sprintf_s(l, "[OLVR][SKELPROP] class '%.32s' not found", classes[ci]); OLProxyLog(l); continue; }

        unsigned bestC = 0, bestN = 0; int bestLen = 0;
        char names[48][64]; unsigned offs[48];
        for (unsigned co = 0x28; co <= 0x100; co += 8) {
            if (!ProbeReadPtr((char*)cls + co)) continue;
            for (unsigned no = 0x28; no <= 0x80; no += 8) {
                const int len = SP_Walk(cls, co, no, names, offs, 0);
                if (len > bestLen) { bestLen = len; bestC = co; bestN = no; }
            }
        }
        if (bestLen < 3) { sprintf_s(l, "[OLVR][SKELPROP] '%.32s': no property chain found", classes[ci]); OLProxyLog(l); continue; }

        // UProperty::Offset = the uint32 that increases along the chain and stays object-sized.
        unsigned bestOffField = 0; int bestScore = 0;
        for (unsigned of = 0x38; of <= 0xA0; of += 4) {
            const int len = SP_Walk(cls, bestC, bestN, names, offs, of);
            int score = 0, prev = -1, sane = 1;
            for (int i = 0; i < len; ++i) {
                if (offs[i] > 0x800) { sane = 0; break; }
                if ((int)offs[i] > prev) ++score;
                prev = (int)offs[i];
            }
            if (sane && score > bestScore) { bestScore = score; bestOffField = of; }
        }
        const int len = SP_Walk(cls, bestC, bestN, names, offs, bestOffField);
        sprintf_s(l, "[OLVR][SKELPROP] === %.32s: %d props (children+0x%X next+0x%X offField+0x%X) ===",
                  classes[ci], len, bestC, bestN, bestOffField);
        OLProxyLog(l);
        char line[240]; int ll = 0; line[0] = 0;
        for (int i = 0; i < len; ++i) {
            if (ll > 150) { sprintf_s(l, "[OLVR][SKELPROP]   %.200s", line); OLProxyLog(l); ll = 0; line[0] = 0; }
            ll += sprintf_s(line + ll, sizeof(line) - ll, "%.32s@0x%X  ", names[i], offs[i]);
        }
        if (ll) { sprintf_s(l, "[OLVR][SKELPROP]   %.200s", line); OLProxyLog(l); }
    }

    // Cross-check on a LIVE Hero-bound control: whatever the reflection calls ControlStrength must
    // read as a plausible float in a real instance. This line is what proves the offsets usable.
    char nm[64];
    for (int i = 0, shown = 0; i < g_bpObjNum && shown < 3; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm))) continue;
        if (strcmp(nm, "SkelControlSingleBone") && strcmp(nm, "SkelControlLimb")) continue;
        int hero = 0; char bn[64]; bn[0] = 0;
        for (unsigned off = 0x50; off <= 0x300 && !hero; off += 4) {
            uint32_t bi = 0;
            if (BP_Read((char*)o + off, &bi, 4) && BP_Name(bi, bn, sizeof(bn)) && !strncmp(bn, "Hero-", 5)) hero = 1;
        }
        if (!hero) continue;
        ++shown;
        char line[240]; int ll = 0; line[0] = 0;
        for (unsigned off = 0x60; off <= 0x150 && ll < 150; off += 4) {
            float f = 0.0f;
            if (!BP_Read((char*)o + off, &f, 4)) continue;
            if (f != f || fabsf(f) > 1e6f) continue;
            ll += sprintf_s(line + ll, sizeof(line) - ll, "%X:%.2f ", off, f);
        }
        sprintf_s(l, "[OLVR][SKELPROP] live %.28s (%.24s)  %.170s", nm, bn, line);
        OLProxyLog(l);
    }
    OLProxyLog("[OLVR][SKELPROP] done");
}


// ---- [ARMDRIVE] the controller moves Miles's arm -----------------------------------------------
// Everything needed is now measured, none of it guessed:
//   SkelControlLimb   EffectorLocation @0xF4  EffectorLocationSpace @0x10C   (2-bone IK effector)
//   SkelControlBase   ControlStrength  @ found by reflection below (the on switch)
// A SkelControl is ticked BY the animation system, in its own correct window, so there is no hook
// and no write-timing problem - the trap that beat WIGGLE and BONEWATCH. The effector is set to
// where the player's controller is in the world and the strength is raised; the engine solves the
// elbow and moves the arm.
// The base-class property offsets could not be read in the SKELPROP run because SkelControlBase's
// Children chain mixes UFunctions in with UProperties and functions have no Offset field, which
// wrecked the scoring. The subclasses proved the Offset field is at +0x8C, so the base chain is
// re-read with that forced and only sane, increasing values accepted.
// Values must be REWRITTEN EVERY FRAME: the animation system reasserts control state each tick
// (the RE Engine lesson - anything written to a joint is rewritten every frame or it is eaten).
// [Stage3] ArmDrive=1. Hold the right grip to aim; strength ramps so the arm never snaps.
static unsigned g_scStrengthOff = 0;      // SkelControlBase::ControlStrength, from reflection
// The live log proved Outlast has no left-arm SkelControlLimb at all. Both arms instead expose
// SkelControlSingleBone controls for UpperArm and Forearm. Driving the old Limb path could never
// produce two-arm IK; use the controls the game actually ships on both sides.
static const unsigned kSingleBools    = 0xF4;
static const unsigned kSingleRotSpace = 0x105;
static const unsigned kSingleRotation = 0x110; // FRotator pitch,yaw,roll
static unsigned g_bitApplyRotation = 0;
static unsigned g_bitAddRotation = 0;
static unsigned g_bitRemoveMeshRotation = 0;
// Body estimates in Unreal units at the measured 100 uu/m. The camera is 168 uu above the pawn
// root (CamProbe), so these hang the shoulder a head's height below the eyes.
static const float kShoulderDown  = 25.0f;
static const float kShoulderRight = 18.0f;
static const float kShoulderBack  = 8.0f;
static const float kArmReachUU    = 62.0f;   // upper arm + forearm, ~62 cm
static const float kElbowBack     = 40.0f;
static const float kElbowDown     = 55.0f;
enum ArmSegment { ARM_UPPER = 0, ARM_FORE = 1 };
struct ArmSingleRef { void* control; int hand; int segment; }; // hand: 0=left, 1=right
static ArmSingleRef g_armSingle[64]; static int g_armSingleN = 0;
static float g_armRamp[2] = { 0.0f, 0.0f };
static bool g_armOwned = false;

// A UE3 UBoolProperty packs many bools into one dword and stores WHICH BIT in a BitMask field on
// the property object. The offset of that field is not known, so it is found the same way as
// everything else here: inside the property object, look for the lone power-of-two dword. Guessing
// the bit would mean flipping bAllowStretching or bInvertBoneAxis by accident.
static unsigned SP_FindBoolBit(const char* cls, const char* want) {
    void* c = SP_FindClass(cls);
    if (!c) return 0;
    char names[48][64]; unsigned offs[48];
    const int len = SP_Walk(c, 0x80, 0x60, names, offs, 0x8C);
    // Re-walk to reach the property OBJECT for the wanted name.
    void* pobj = ProbeReadPtr((char*)c + 0x80);
    for (int i = 0; i < len && pobj; ++i) {
        if (!strcmp(names[i], want)) {
            for (unsigned bo = 0x88; bo <= 0xC8; bo += 4) {
                uint32_t v = 0;
                if (!BP_Read((char*)pobj + bo, &v, 4)) continue;
                if (v && !(v & (v - 1)) && v <= 0x80000000u) return v;   // exactly one bit set
            }
            return 0;
        }
        pobj = ProbeReadPtr((char*)pobj + 0x60);
    }
    return 0;
}

static unsigned SP_FindBaseProp(const char* want) {
    void* cls = SP_FindClass("SkelControlBase");
    if (!cls) return 0;
    char names[48][64]; unsigned offs[48];
    const int len = SP_Walk(cls, 0x80, 0x60, names, offs, 0x8C);   // links proven by the subclasses
    for (int i = 0; i < len; ++i)
        if (!strcmp(names[i], want) && offs[i] >= 0x40 && offs[i] < 0xF4) return offs[i];
    return 0;
}

// ---- [PHYSCROUCH] read the pawn's crouch flag through UE3's own reflection --------------------
// Same property chain SKELPROP proved (UClass Children +0x80, UProperty Next +0x60, Offset
// +0x8C), but walked to the END of the class's own list: Pawn declares far more than the 48
// properties SP_Walk keeps.
// A bool's BitMask field position is NOT assumed. UE3 packs a class's bools into shared DWORDs,
// so every bool at the wanted bool's Offset must show its own, distinct, single-bit value at the
// right field; the field position where the most of them do is the BitMask. A lone single-bit
// value (ArrayDim = 1, say) cannot win that vote.
static bool SP_FindPropIn(void* c, const char* want, unsigned* off, unsigned* mask);
// SP_FindClass returns the FIRST object with the name, and a property can share a class's name
// (Controller has a property called "Pawn"). Try every object so named until one of them really
// carries the wanted property.
static bool SP_FindPropAny(const char* cls, const char* want, unsigned* off, unsigned* mask) {
    char nm[64];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm)) || strcmp(nm, cls)) continue;
        if (SP_FindPropIn(o, want, off, mask)) return true;
    }
    return false;
}
static bool SP_FindPropIn(void* c, const char* want, unsigned* off, unsigned* mask) {
    void* props[1024]; uint32_t offs[1024]; int n = 0, target = -1;
    char nm[64];
    for (void* p = ProbeReadPtr((char*)c + 0x80); p && n < 1024; ) {
        uint32_t idx = 0, o = 0;
        if (!BP_Read((char*)p + g_bpUNameOff, &idx, 4) || !BP_Name(idx, nm, sizeof(nm))) break;
        if (!BP_Read((char*)p + 0x8C, &o, 4)) break;
        if (target < 0 && !strcmp(nm, want)) target = n;
        props[n] = p; offs[n] = o; ++n;
        void* nx = ProbeReadPtr((char*)p + 0x60);
        if (nx == p) break;
        p = nx;
    }
    if (target < 0) return false;
    *off = offs[target];
    if (!mask) return true;
    *mask = 0;
    unsigned bestBo = 0; int bestVotes = 0;
    for (unsigned bo = 0x88; bo <= 0xD0; bo += 4) {
        if (bo == 0x8C) continue;
        uint32_t seen = 0; int votes = 0; bool clash = false;
        for (int i = 0; i < n && !clash; ++i) {
            if (offs[i] != offs[target]) continue;
            uint32_t v = 0;
            if (!BP_Read((char*)props[i] + bo, &v, 4) || !v || (v & (v - 1))) { if (i == target) clash = true; continue; }
            if (seen & v) clash = true; else { seen |= v; ++votes; }
        }
        if (!clash && votes > bestVotes) { bestVotes = votes; bestBo = bo; }
    }
    if (bestVotes < 2) return false;   // not enough evidence to trust a bit
    return BP_Read((char*)props[target] + bestBo, mask, 4) && *mask && !(*mask & (*mask - 1));
}

// -1 = unknown (not in gameplay yet, registry not found, or the lookup failed), 0 standing,
// 1 crouched. Resolved once, on first use in gameplay; logged either way.
static unsigned g_pcOwnerOff = 0, g_pcCrouchOff = 0, g_pcCrouchMask = 0;
static int      g_pcState = 0;        // 0 untried, 1 resolved, -1 failed for good (after 3 tries)
static int      g_pcTries = 0;
static ULONGLONG g_pcNextTry = 0;
extern "C" int OLUE3_PawnCrouchUnavailable() { return g_pcState < 0 ? 1 : 0; }
extern "C" int OLUE3_PawnCrouched() {
    void* comp = g_milesComp;
    if (!comp || g_milesCompFresh <= 0 || !g_bpObjs || !g_bpNames || !g_bpUNameOff) return -1;
    if (g_pcState == 0) {
        if (GetTickCount64() < g_pcNextTry) return -1;
        BP_RefreshObjects();   // the object table moves on level loads
        const bool ok = SP_FindPropAny("ActorComponent", "Owner", &g_pcOwnerOff, nullptr) &&
                        SP_FindPropAny("Pawn", "bIsCrouched", &g_pcCrouchOff, &g_pcCrouchMask) &&
                        g_pcOwnerOff >= 0x40 && g_pcOwnerOff < 0x400 &&
                        g_pcCrouchOff >= 0x40 && g_pcCrouchOff < 0x2000;
        ++g_pcTries;
        if (ok) g_pcState = 1;
        else if (g_pcTries >= 3) g_pcState = -1;
        else g_pcNextTry = GetTickCount64() + 3000;
        char l[240];
        sprintf_s(l, "[OLVR][PHYSCROUCH] reflection %s (try %d): ActorComponent.Owner=+0x%X Pawn.bIsCrouched=+0x%X mask=0x%X",
                  ok ? "resolved" : (g_pcState < 0 ? "FAILED - physical crouch unavailable" : "not found yet"),
                  g_pcTries, g_pcOwnerOff, g_pcCrouchOff, g_pcCrouchMask);
        OLProxyLog(l);
        if (!ok) return -1;
    }
    if (g_pcState < 0) return -1;
    __try {
        void* pawn = *(void**)((char*)comp + g_pcOwnerOff);
        if (!pawn) return -1;
        static bool s_named = false;
        if (!s_named) {
            s_named = true; char nm[64] = "?", l[160];
            BP_ObjName(pawn, nm, sizeof(nm));
            sprintf_s(l, "[OLVR][PHYSCROUCH] Miles's pawn is '%.60s'", nm);
            OLProxyLog(l);
        }
        return (*(const uint32_t*)((char*)pawn + g_pcCrouchOff) & g_pcCrouchMask) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// [PHYSCROUCH] 1 = Miles is in ordinary locomotion (hero+0x7BC == 0, the same gate look-to-use
// uses), 0 = locker, bed, ledge, squeeze or any other special move, -1 = unknown.
extern "C" int OLUE3_PawnNormalMove() {
    void* comp = g_milesComp;
    if (!comp || g_milesCompFresh <= 0 || g_pcState != 1) return -1;
    __try {
        const char* pawn = *(const char* const*)((const char*)comp + g_pcOwnerOff);
        if (!pawn) return -1;
        return *(const uint8_t*)(pawn + 0x7BC) == 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---- [SCENESTATE] pause / cutscene / scripted-control signals (Test35) -----------------------------
// Read from the PlayerController the camera hook sees (GetPlayerViewPoint's actor), at most every
// 15 ms, on the game thread. Every offset comes from UE3's own reflection by NAME; each signal
// resolves (or fails) on its own and is logged, so a missing one only disables its own use.
static unsigned g_ssWorldInfo = 0, g_ssPauser = 0, g_ssCine = 0, g_ssCineMask = 0,
                g_ssIgnoreMove = 0, g_ssIgnoreLook = 0, g_ssRotation = 0;
// The game's menus are one Scaleform movie, OLHUD.MenuManager (an OLUIFrontEnd), whose ViewStack
// holds the open screens - main menu, pause, options, notes, recordings, chapter select. The
// gamma and splash screens are flags on the HUD itself. The main menu runs with a live hero
// in a cinematic, so "no hero" alone never caught it.
static unsigned g_ssMyHud = 0, g_ssHudFlags = 0, g_ssGammaMask = 0, g_ssSplashMask = 0,
                g_ssMenuMgr = 0, g_ssMovieOpen = 0, g_ssMovieOpenMask = 0, g_ssViewStack = 0,
                g_ssMenuType = 0;
static void*    g_ssHudClass = nullptr;
// The UClass object called <name> (an object of class "Class"; other objects share the name).
// UObject::Class sits right after UObject::Name.
static void* BP_FindClassObj(const char* name) {
    char nm[96], cn[96];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4) || !BP_Name(idx, nm, sizeof(nm)) || strcmp(nm, name)) continue;
        void* c = ProbeReadPtr((char*)o + g_bpUNameOff + 8);
        if (c && BP_ObjName(c, cn, sizeof(cn)) && !strcmp(cn, "Class")) return o;
    }
    return nullptr;
}
static int      g_ssState = 0, g_ssTries = 0;     // 0 untried, 1 resolved (some), -1 gave up
static ULONGLONG g_ssNextTry = 0;

// A class's Children list holds FUNCTIONS as well as properties, and UE3 has functions called
// IgnoreMoveInput/IgnoreLookInput next to the byte variables bIgnoreMoveInput/bIgnoreLookInput -
// the first build read a function object as a property and got a garbage offset. So: the real
// variable names, and an offset that has to look like one.
static bool SS_Plausible(unsigned off) { return off >= 0x40 && off < 0x4000; }
static bool SS_FindByte(const char* prop, unsigned* off) {
    unsigned o = 0;
    if ((SP_FindPropAny("PlayerController", prop, &o, nullptr) && SS_Plausible(o)) ||
        (SP_FindPropAny("Controller", prop, &o, nullptr) && SS_Plausible(o))) { *off = o; return true; }
    return false;
}

static void SceneStateResolve() {
    if (g_ssState != 0 || GetTickCount64() < g_ssNextTry || !g_bpObjs || !g_bpNames || !g_bpUNameOff) return;
    BP_RefreshObjects();
    unsigned o = 0, m = 0;
    if (!g_ssWorldInfo && SP_FindPropAny("Actor", "WorldInfo", &o, nullptr) && SS_Plausible(o)) g_ssWorldInfo = o;
    if (!g_ssRotation && SP_FindPropAny("Actor", "Rotation", &o, nullptr) && SS_Plausible(o)) g_ssRotation = o;   // [STICKPITCH]
    if (!g_ssPauser && SP_FindPropAny("WorldInfo", "Pauser", &o, nullptr) && SS_Plausible(o)) g_ssPauser = o;
    if (!g_ssCine && SP_FindPropAny("PlayerController", "bCinematicMode", &o, &m) && SS_Plausible(o)) {
        g_ssCine = o; g_ssCineMask = m;
    }
    if (!g_ssIgnoreMove) SS_FindByte("bIgnoreMoveInput", &g_ssIgnoreMove);
    if (!g_ssIgnoreLook) SS_FindByte("bIgnoreLookInput", &g_ssIgnoreLook);
    if (!g_ssMyHud && SP_FindPropAny("PlayerController", "myHUD", &o, nullptr) && SS_Plausible(o)) g_ssMyHud = o;
    if (!g_ssHudClass) g_ssHudClass = BP_FindClassObj("OLHUD");
    if (!g_ssHudFlags && SP_FindPropAny("OLHUD", "bGammaCalibrationOpen", &o, &m) && SS_Plausible(o)) {
        unsigned o2 = 0, m2 = 0;
        g_ssHudFlags = o; g_ssGammaMask = m;
        if (SP_FindPropAny("OLHUD", "bSplashScreenOpen", &o2, &m2) && o2 == o) g_ssSplashMask = m2;
    }
    if (!g_ssMenuMgr && SP_FindPropAny("OLHUD", "MenuManager", &o, nullptr) && SS_Plausible(o)) g_ssMenuMgr = o;
    if (!g_ssMovieOpen && SP_FindPropAny("GFxMoviePlayer", "bMovieIsOpen", &o, &m) && SS_Plausible(o)) {
        g_ssMovieOpen = o; g_ssMovieOpenMask = m;
    }
    if (!g_ssViewStack && SP_FindPropAny("OLUIFrontEnd", "ViewStack", &o, nullptr) && SS_Plausible(o)) g_ssViewStack = o;
    if (!g_ssMenuType && SP_FindPropAny("OLUIFrontEnd", "MenuType", &o, nullptr) && SS_Plausible(o)) g_ssMenuType = o;
    ++g_ssTries;
    const bool all = g_ssWorldInfo && g_ssPauser && g_ssCine && g_ssIgnoreMove && g_ssIgnoreLook &&
                     g_ssMyHud && g_ssHudClass && g_ssHudFlags && g_ssMenuMgr && g_ssMovieOpen && g_ssViewStack;
    if (all || g_ssTries >= 3) g_ssState = (g_ssWorldInfo || g_ssCine || g_ssIgnoreMove) ? 1 : -1;
    else g_ssNextTry = GetTickCount64() + 3000;
    char l[300];
    sprintf_s(l, "[OLVR][SCENESTATE] reflection try %d: Actor.WorldInfo=+0x%X WorldInfo.Pauser=+0x%X "
                 "PC.bCinematicMode=+0x%X/0x%X IgnoreMoveInput=+0x%X IgnoreLookInput=+0x%X%s",
              g_ssTries, g_ssWorldInfo, g_ssPauser, g_ssCine, g_ssCineMask, g_ssIgnoreMove, g_ssIgnoreLook,
              g_ssState == 1 ? " -> in use" : (g_ssState < 0 ? " -> unavailable" : ""));
    OLProxyLog(l);
    sprintf_s(l, "[OLVR][SCENESTATE] menus: PC.myHUD=+0x%X OLHUD=%s flags=+0x%X gamma=0x%X splash=0x%X "
                 "MenuManager=+0x%X bMovieIsOpen=+0x%X/0x%X ViewStack=+0x%X MenuType=+0x%X",
              g_ssMyHud, g_ssHudClass ? "found" : "missing", g_ssHudFlags, g_ssGammaMask, g_ssSplashMask,
              g_ssMenuMgr, g_ssMovieOpen, g_ssMovieOpenMask, g_ssViewStack, g_ssMenuType);
    OLProxyLog(l);
}

// [DEVBRIDGE] The player controller SceneStateRead last saw, for developer commands only.
static void* volatile g_devLastPC = nullptr;
// "tp <actor> [dz]" / "tp xyz x y z": put the player's pawn at that actor's Location or at a world
// point. A raw Location write - collision catches up on the next move. Research use only.
extern "C" void OLUE3_DevTeleportTo(float x, float y, float z) {
    unsigned loc = 0; char l[200];
    void* pc = g_devLastPC;
    if (!pc || !SP_FindPropAny("Actor", "Location", &loc, nullptr) || !SS_Plausible(loc)) { OLProxyLog("[OLVR][DEVTP] no controller/Location yet"); return; }
    __try {
        char* hero = *(char**)((char*)pc + kCtrlHeroOff);
        if (!hero) { OLProxyLog("[OLVR][DEVTP] no pawn"); return; }
        float* p = (float*)(hero + loc);
        sprintf_s(l, "[OLVR][DEVTP] pawn (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)", p[0], p[1], p[2], x, y, z); OLProxyLog(l);
        p[0] = x; p[1] = y; p[2] = z;
    } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][DEVTP] FAULT"); }
}
extern "C" void OLUE3_DevTeleport(void* actor, float dz) {
    unsigned loc = 0;
    if (!SP_FindPropAny("Actor", "Location", &loc, nullptr) || !SS_Plausible(loc)) return;
    float p[3] = {};
    __try { memcpy(p, (char*)actor + loc, sizeof(p)); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][DEVTP] bad actor"); return; }
    OLUE3_DevTeleportTo(p[0], p[1], p[2] + dz);
}

// ---- [INTERACT] (Test37) engine access for ol_interact.cpp ---------------------------------------
// Everything is SEH-guarded reads plus three deliberate writes (a door's ratios). Offsets come from
// UE3 reflection by name; the two native door functions are verified by their prologue bytes.
extern "C" int OLUE3_FindProp(const char* cls, const char* prop, unsigned* off, unsigned* mask) {
    if (!g_bpObjs || !g_bpNames || !g_bpUNameOff) return 0;
    BP_RefreshObjects();
    unsigned o = 0, m = 0;
    // A mask is only asked for when wanted: for a non-bool the bit vote fails and rejects the field.
    if (!SP_FindPropAny(cls, prop, &o, mask ? &m : nullptr) || !SS_Plausible(o)) return 0;
    if (off) *off = o;
    if (mask) *mask = m;
    return 1;
}
extern "C" void* OLUE3_PlayerController() { return g_devLastPC; }
extern "C" void* OLUE3_Hero() {
    void* pc = g_devLastPC;
    if (!pc) return nullptr;
    __try {
        char* hero = *(char**)((char*)pc + kCtrlHeroOff);
        if (hero && *(void**)(hero + kHeroCtrlOff) == pc) return hero;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
// [OBJSCAN] (Test45) Every live (non-default) object of a class, from a cache a sliced scan keeps fresh.
// Test37-44 walked the whole object table (93-103k objects in the user's session) for every lookup - the
// doors every 2 s, ledges and ladders every 4 s - in one go on the game thread, a dropped frame each time
// ("the framerate is horrible during fast movement and later sections"). Now ONE pass serves every class
// asked for, kObjScanSlice objects per frame (OLUE3_ObjScanStep, from the interaction tick), and a lookup
// copies the last finished pass. A class asked for the first time is scanned in full once, right away.
static const int kObjScanMax = 1024, kObjScanSlice = 4000;
struct ObjScanClass { char name[40]; void* cls; int pubN, workN; bool skip; void* pub[kObjScanMax]; void* work[kObjScanMax]; };
static ObjScanClass g_osc[8];
static int g_oscN = 0, g_oscCursor = 0;
static void** g_oscData = nullptr;
static long g_oscPasses = 0;
static int ObjScanAll(void* c, void** out, int max) {
    int n = 0; char nm[48];
    for (int i = 0; i < g_bpObjNum && n < max; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (ProbeReadPtr((char*)o + g_bpUNameOff + 8) != c) continue;
        if (BP_ObjName(o, nm, sizeof(nm)) && !strncmp(nm, "Default__", 9)) continue;
        out[n++] = o;
    }
    return n;
}
extern "C" int OLUE3_ObjectsOfClass(const char* cls, void** out, int max) {
    if (!cls || !out || max <= 0 || !g_bpObjs || !g_bpUNameOff) return 0;
    int k = -1;
    for (int i = 0; i < g_oscN; ++i) if (!strcmp(g_osc[i].name, cls)) { k = i; break; }
    if (k < 0) {
        BP_RefreshObjects();
        void* c = BP_FindClassObj(cls);
        if (!c) return 0;
        if (g_oscN >= 8) return ObjScanAll(c, out, max);              // no room in the cache: the old way
        k = g_oscN;
        ObjScanClass& e = g_osc[k];
        strncpy_s(e.name, cls, _TRUNCATE); e.cls = c; e.workN = 0;
        e.skip = g_oscCursor > 0;                                       // the running pass started without it
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        e.pubN = ObjScanAll(c, e.pub, kObjScanMax);
        QueryPerformanceCounter(&t1);
        ++g_oscN;
        char l[200];
        sprintf_s(l, "[OLVR][OBJSCAN] %s: %d found in a full scan of %d objects (%.1f ms on the game thread) - kept fresh by a sliced scan (%d objects a frame) from now on",
                  cls, e.pubN, g_bpObjNum, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, kObjScanSlice);
        OLProxyLog(l);
    }
    const ObjScanClass& e = g_osc[k];
    const int n = e.pubN < max ? e.pubN : max;
    if (n > 0) memcpy(out, e.pub, (size_t)n * sizeof(void*));
    return n;
}
// Game thread, once per frame: the next slice of the pass.
extern "C" void OLUE3_ObjScanStep() {
    if (!g_oscN || !g_bpObjs || !g_bpUNameOff) return;
    if (g_oscCursor == 0) {
        BP_RefreshObjects();
        g_oscData = g_bpObjs;
        for (int k = 0; k < g_oscN; ++k) g_osc[k].workN = 0;
    } else if (g_bpObjs != g_oscData) {                                  // the table moved (a level load): start over
        g_oscCursor = 0;
        for (int k = 0; k < g_oscN; ++k) g_osc[k].skip = false;
        return;
    }
    const int num = g_bpObjNum;
    int end = g_oscCursor + kObjScanSlice; if (end > num) end = num;
    char nm[48];
    for (int i = g_oscCursor; i < end; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        void* c = ProbeReadPtr((char*)o + g_bpUNameOff + 8);
        if (!c) continue;
        for (int k = 0; k < g_oscN; ++k) {
            ObjScanClass& e = g_osc[k];
            if (c != e.cls) continue;
            if (e.workN < kObjScanMax && !(BP_ObjName(o, nm, sizeof(nm)) && !strncmp(nm, "Default__", 9))) e.work[e.workN++] = o;
            break;
        }
    }
    g_oscCursor = end;
    if (end >= num) {
        for (int k = 0; k < g_oscN; ++k) {
            ObjScanClass& e = g_osc[k];
            if (!e.skip) { memcpy(e.pub, e.work, (size_t)e.workN * sizeof(void*)); e.pubN = e.workN; }
            e.skip = false;
        }
        g_oscCursor = 0;
        if ((++g_oscPasses % 600) == 1) {
            char l[200]; int at = 0; at += sprintf_s(l, "[OLVR][OBJSCAN] pass %ld over %d objects:", g_oscPasses, num);
            for (int k = 0; k < g_oscN && at < 170; ++k) at += sprintf_s(l + at, sizeof(l) - at, " %s %d", g_osc[k].name, g_osc[k].pubN);
            OLProxyLog(l);
        }
    }
}
// The head-centre world position and body yaw of the newest view (0 = stale, over 250 ms old).
extern "C" int OLUE3_FrameView(float head[3], float* baseYaw) {
    if (!g_fvTick || GetTickCount64() - g_fvTick > 250) return 0;
    if (head) { head[0] = g_fvHead[0]; head[1] = g_fvHead[1]; head[2] = g_fvHead[2]; }
    if (baseYaw) *baseYaw = g_fvBaseYaw;
    return 1;
}
// OLDoor natives (RVAs from the Test30-32 research, prologues re-verified on this OLGame.exe):
//  0xCB24D0 knob world position: DoorKnobOffset (Y mirrored for bReverseDirection) through the
//           door mesh's LocalToWorld - so it already includes the current opening.
//  0xCB2300 set the open ratio: writes OpenRatio and the mesh's relative yaw.
typedef float* (__fastcall *DoorKnob_t)(void*, float*);
typedef void (__fastcall *DoorSetRatio_t)(void*, float);
static DoorKnob_t     g_doorKnobFn = nullptr;
static DoorSetRatio_t g_doorSetFn = nullptr;
static int            g_doorFnState = 0;   // 0 untried, 1 ok, -1 mismatch
static bool DoorFnsReady() {
    if (g_doorFnState) return g_doorFnState > 0;
    static const unsigned char kKnob[11] = { 0x48,0x83,0xEC,0x28,0xF6,0x81,0xC4,0x02,0x00,0x00,0x01 };
    static const unsigned char kSet[13] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0x2E,0x89,0x08,0x19,0x00,0x00 };
    unsigned char a[11] = {}, b[13] = {};
    void* pk = (void*)(g_base + 0xCB24D0); void* ps = (void*)(g_base + 0xCB2300);
    const bool ok = SafeReadBytes(pk, a, sizeof(a)) && !memcmp(a, kKnob, sizeof(a)) &&
                    SafeReadBytes(ps, b, sizeof(b)) && !memcmp(b, kSet, sizeof(b));
    g_doorFnState = ok ? 1 : -1;
    if (ok) { g_doorKnobFn = (DoorKnob_t)pk; g_doorSetFn = (DoorSetRatio_t)ps; }
    OLProxyLog(ok ? "[OLVR][DOORS] door knob/ratio natives verified (0xCB24D0, 0xCB2300)"
                  : "[OLVR][DOORS] door native prologue MISMATCH - physical doors off");
    return ok;
}
extern "C" int OLUE3_DoorKnobWorld(void* door, float out[3]) {
    if (!door || !out || !DoorFnsReady()) return 0;
    float v[4] = { 0, 0, 0, 0 };
    __try { g_doorKnobFn(door, v); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
    return 1;
}
// The hinge: the door mesh component's world origin (LocalToWorld translation, mesh+0xF0).
extern "C" int OLUE3_DoorHinge(void* door, float out[3]) {
    if (!door || !out) return 0;
    __try {
        const char* mesh = *(const char* const*)((const char*)door + 0x258);
        if (!mesh) return 0;
        memcpy(out, mesh + 0xF0, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
// [DOORPHYS] (Test40) 0xCC46A0: TargetOpenRatio = OpenRatio = r (unless the door is broken), then the
// navmesh obstacle is dropped / re-registered as it opens / closes. OLDoor's own procedural swings
// (locked rattle, blocked, auto-close) call it every tick; the hand physics does the same.
typedef void (__fastcall *DoorSetBoth_t)(void*, float);
static DoorSetBoth_t g_doorBothFn = nullptr;
static int g_doorBothState = 0;
extern "C" int OLUE3_DoorSetRatioAndTarget(void* door, float r) {
    if (!door) return 0;
    if (!g_doorBothState) {
        static const unsigned char k[13] = { 0x40,0x53,0x48,0x83,0xEC,0x20,0x80,0xB9,0x0C,0x03,0x00,0x00,0x02 };
        unsigned char b[13] = {}; void* p = (void*)(g_base + 0xCC46A0);
        g_doorBothState = (SafeReadBytes(p, b, sizeof(b)) && !memcmp(b, k, sizeof(b))) ? 1 : -1;
        if (g_doorBothState > 0) g_doorBothFn = (DoorSetBoth_t)p;
        OLProxyLog(g_doorBothState > 0 ? "[OLVR][DOORPHYS] door set-ratio native verified (0xCC46A0)"
                                       : "[OLVR][DOORPHYS] door set-ratio native MISMATCH - doors fall back to the target");
    }
    if (g_doorBothState < 0) return 0;
    __try { g_doorBothFn(door, r); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
// [CLIMBPACE] (Test41) Miles's animation speed, on both his body meshes (the visible one, Mesh +0x444,
// and the shadow proxy +0x2CE0). The climb special moves are root-motion animations - measured: the
// ladder mount takes 0.92 s at 100%, 2.68 s at 25% - so the hands pace a climb by how fast they pull.
extern "C" void OLUE3_SetHeroAnimRate(float r) {
    static unsigned s_off = 0; static int s_tries = 0;
    if (!s_off && s_tries < 1) {
        ++s_tries;
        // a float: no bit mask (asking for one makes the lookup reject a non-bool field)
        if (!OLUE3_FindProp("SkeletalMeshComponent", "GlobalAnimRateScale", &s_off, nullptr)) s_off = 0x2C4;   // this build, by reflection
        char l[120]; sprintf_s(l, "[OLVR][CLIMBPACE] SkeletalMeshComponent.GlobalAnimRateScale at +0x%X", s_off);
        OLProxyLog(l);
    }
    if (!s_off) return;
    void* hero = OLUE3_Hero();
    if (!hero) return;
    __try {
        char* meshes[2] = { *(char**)((char*)hero + 0x444), *(char**)((char*)hero + 0x2CE0) };
        for (int i = 0; i < 2; ++i) if (meshes[i]) *(float*)(meshes[i] + s_off) = r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
extern "C" int OLUE3_DoorSetRatio(void* door, float r) {
    if (!door || !DoorFnsReady()) return 0;
    __try { g_doorSetFn(door, r); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

// [DOOREVENT] (Test43) "The first jumpscare at the door does not happen when I open it with motion
// controls." OLDoor.TriggerEvent(byte EventType, Pawn Instigator) is what the game's own door code
// calls as a door is used (AOLDoor::execTriggerEvent, RVA 0xCA4980, is a thin wrapper over vtable
// +0x750); it fires the door's OLSeqEvent_Door Kismet outputs, and the level scripts' scares listen to
// those. The hand physics moves the door through 0xCC46A0 and never got there. The hook (installed on
// the first door the hands see, from that door's vtable) logs every event the game fires, so a door
// opened with X shows which ones a door expects; ol_interact fires the same ones for a door the hands open.
typedef void (__fastcall *DoorTrig_t)(void* door, unsigned char type, void* instigator);
static DoorTrig_t o_DoorTrig = nullptr;      // the original (through the hook)
static DoorTrig_t g_doorTrigFn = nullptr;    // the function itself, read from the vtable
static volatile long g_doorTrigState = 0;    // 0 untried, 1 hooked, -1 refused
static volatile long g_doorTrigOurs = 0;
static void __fastcall Hook_DoorTrig(void* door, unsigned char type, void* inst) {
    float loc[3] = {};
    __try { memcpy(loc, (char*)door + 0x80, 12); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    char l[220];
    sprintf_s(l, "[OLVR][DOOREVENT] door %p at (%.0f %.0f %.0f): event %u, instigator %s (%s)", door, loc[0], loc[1], loc[2],
              (unsigned)type, inst ? (inst == OLUE3_Hero() ? "Miles" : "someone else") : "none", g_doorTrigOurs ? "fired by the hands" : "the game's own");
    OLProxyLog(l);
    o_DoorTrig(door, type, inst);
}
extern "C" int OLUE3_DoorEventHook(void* door) {
    if (g_doorTrigState || !door) return g_doorTrigState > 0;
    void* fn = nullptr;
    __try { fn = (*(void***)door)[0x750 / 8]; } __except (EXCEPTION_EXECUTE_HANDLER) { fn = nullptr; }
    char l[200];
    if (!fn || (uintptr_t)fn < g_base || (uintptr_t)fn > g_base + 0x4000000) {
        g_doorTrigState = -1;
        sprintf_s(l, "[OLVR][DOOREVENT] door vtable +0x750 = %p is not in OLGame.exe - door events off", fn);
        OLProxyLog(l); return 0;
    }
    g_doorTrigFn = (DoorTrig_t)fn;
    if (MH_CreateHook(fn, (void*)&Hook_DoorTrig, (void**)&o_DoorTrig) == MH_OK && MH_EnableHook(fn) == MH_OK) {
        g_doorTrigState = 1;
        sprintf_s(l, "[OLVR][DOOREVENT] OLDoor.TriggerEvent at base+0x%llX hooked (from a door's vtable +0x750)", (unsigned long long)((uintptr_t)fn - g_base));
    } else {
        g_doorTrigState = 1; o_DoorTrig = g_doorTrigFn;     // calling still works without the log
        sprintf_s(l, "[OLVR][DOOREVENT] OLDoor.TriggerEvent at base+0x%llX (hook failed - fired events are not logged)", (unsigned long long)((uintptr_t)fn - g_base));
    }
    OLProxyLog(l);
    return 1;
}
extern "C" int OLUE3_DoorTriggerEvent(void* door, int type) {
    void* hero = OLUE3_Hero();
    if (!door || !hero || g_doorTrigState <= 0 || !o_DoorTrig) return 0;
    InterlockedExchange(&g_doorTrigOurs, 1);
    __try { Hook_DoorTrig(door, (unsigned char)type, hero); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_doorTrigOurs, 0); return 0; }
    InterlockedExchange(&g_doorTrigOurs, 0);
    return 1;
}

// [PHYSCLIMB] (Test43) Miles's body carried by your hands. The engine's own calls, from the script
// natives: AActor::execMove (RVA 0x409CF0) calls 0x652FF0 = Move(Delta): GWorld->MoveActor(this,
// Delta, Rotation, 0, Hit) - collision swept, touches and triggers fired like any move; and
// AActor::execSetPhysics calls vtable +0x420 = setPhysics(NewPhysics, NewFloor, NewFloorV).
typedef int (__fastcall *ActorMove_t)(void* actor, const float* delta);
typedef void (__fastcall *SetPhysics_t)(void* actor, unsigned char phys, void* floor, const float* floorV);
static ActorMove_t g_actorMove = nullptr;
static int g_actorMoveState = 0;
static bool ActorMoveReady() {
    if (g_actorMoveState) return g_actorMoveState > 0;
    static const unsigned char k[13] = { 0x48,0x8B,0xC4,0x48,0x81,0xEC,0xA8,0x00,0x00,0x00,0x0F,0x57,0xC0 };
    unsigned char b[13] = {}; void* p = (void*)(g_base + 0x652FF0);
    g_actorMoveState = (SafeReadBytes(p, b, sizeof(b)) && !memcmp(b, k, sizeof(b))) ? 1 : -1;
    if (g_actorMoveState > 0) g_actorMove = (ActorMove_t)p;
    OLProxyLog(g_actorMoveState > 0 ? "[OLVR][PHYSCLIMB] AActor::Move verified (0x652FF0) - your hands can carry Miles"
                                    : "[OLVR][PHYSCLIMB] AActor::Move prologue MISMATCH - physical climbing off (guided climbing instead)");
    return g_actorMoveState > 0;
}
extern "C" int OLUE3_HeroMoveReady() { return ActorMoveReady() ? 1 : 0; }
// Moves Miles by d (uu) with collision; out = where he ended up. 0 = could not call.
extern "C" int OLUE3_HeroMove(const float d[3], float out[3]) {
    void* hero = OLUE3_Hero();
    if (!hero || !ActorMoveReady()) return 0;
    __try {
        g_actorMove(hero, d);
        if (out) memcpy(out, (char*)hero + 0x80, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
extern "C" int OLUE3_HeroSetPhysics(int phys) {
    void* hero = OLUE3_Hero();
    if (!hero) return 0;
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    __try {
        SetPhysics_t fn = (SetPhysics_t)(*(void***)hero)[0x420 / 8];
        if ((uintptr_t)fn < g_base || (uintptr_t)fn > g_base + 0x4000000) return 0;
        fn(hero, (unsigned char)phys, nullptr, up);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static void SceneStateRead(void* pc) {
    static LARGE_INTEGER s_f = {}, s_last = {};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
    if (s_last.QuadPart && (now.QuadPart - s_last.QuadPart) * 1000 < s_f.QuadPart * 15) return;
    s_last = now;
    if (!pc) return;
    g_devLastPC = pc;
    SceneStateResolve();
    const bool refl = g_ssState == 1;   // the hero signals below need no reflection
    // Each signal in its own guard: one unreadable field must not blank the others.
    long paused = -1, cine = -1, im = -1, il = -1, mm = -1, hh = -1, menu = -1, views = -1, mtype = -1;
    const char* p = (const char*)pc;
    __try {
        if (refl && g_ssWorldInfo && g_ssPauser) {
            const char* wi = *(const char* const*)(p + g_ssWorldInfo);
            if (wi) paused = *(void* const*)(wi + g_ssPauser) ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { paused = -1; }
    __try { if (refl && g_ssCine && g_ssCineMask) cine = (*(const uint32_t*)(p + g_ssCine) & g_ssCineMask) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { cine = -1; }
    __try {
        if (refl && g_ssRotation) { g_sCtrlPitch = (long)(short)(*(const int*)(p + g_ssRotation) & 0xFFFF); g_sCtrlPitchOk = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_sCtrlPitchOk = 0; }
    __try { if (refl && g_ssIgnoreMove) im = *(const uint8_t*)(p + g_ssIgnoreMove); }
    __except (EXCEPTION_EXECUTE_HANDLER) { im = -1; }
    __try { if (refl && g_ssIgnoreLook) il = *(const uint8_t*)(p + g_ssIgnoreLook); }
    __except (EXCEPTION_EXECUTE_HANDLER) { il = -1; }
    __try {
        // Locomotion mode, via the same hero back-pointer chain look-to-use verified live. No
        // hero at all is the main menu (or a level with no player body).
        const char* hero = *(const char* const*)(p + kCtrlHeroOff);
        if (!hero) hh = 0;
        else if (*(void* const*)(hero + kHeroCtrlOff) == pc) {
            hh = 1; mm = *(const uint8_t*)(hero + kHeroMoveModeOff);
            g_sSpecialMove = *(const uint8_t*)(hero + kHeroSpecialMoveOff);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { hh = -1; mm = -1; g_sSpecialMove = -1; }
    __try {
        // Only an actual OLHUD: its field offsets mean nothing on any other HUD class.
        const char* hud = (refl && g_ssMyHud) ? *(const char* const*)(p + g_ssMyHud) : nullptr;
        if (hud && g_ssHudClass && *(void* const*)(hud + g_bpUNameOff + 8) == g_ssHudClass) {
            menu = 0;
            if (g_ssHudFlags && (*(const uint32_t*)(hud + g_ssHudFlags) & (g_ssGammaMask | g_ssSplashMask))) menu = 1;
            const char* fe = g_ssMenuMgr ? *(const char* const*)(hud + g_ssMenuMgr) : nullptr;
            if (fe) {
                if (g_ssViewStack) views = *(const int32_t*)(fe + g_ssViewStack + 8);   // TArray Num
                if (g_ssMenuType) mtype = *(const uint8_t*)(fe + g_ssMenuType);
                const bool open = g_ssMovieOpen && g_ssMovieOpenMask &&
                                  (*(const uint32_t*)(fe + g_ssMovieOpen) & g_ssMovieOpenMask);
                if (open && views > 0 && views < 64) menu = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { menu = -1; views = -1; mtype = -1; }
    static long s_views = -2, s_mtype = -2, s_sm = -2;
    const bool smChanged = g_sSpecialMove != s_sm; s_sm = g_sSpecialMove;
    const bool changed = smChanged || paused != g_sPaused || cine != g_sCinematic || im != g_sIgnoreMove ||
                         il != g_sIgnoreLook || mm != g_sMoveMode || hh != g_sHasHero ||
                         menu != g_sMenu || views != s_views || mtype != s_mtype;
    g_sPaused = paused; g_sCinematic = cine; g_sIgnoreMove = im; g_sIgnoreLook = il; g_sMoveMode = mm;
    g_sHasHero = hh; g_sMenu = menu; s_views = views; s_mtype = mtype;
    if (changed) {
        static long s_logs = 0;
        if (InterlockedIncrement(&s_logs) <= 400) {
            char l[260];
            const int amc = ArmMoveClass();
            sprintf_s(l, "[OLVR][SCENESTATE] hero=%ld paused=%ld cinematic=%ld ignoreMove=%ld ignoreLook=%ld moveMode=%ld sm=%ld "
                         "menu=%ld (views=%ld type=%ld) -> %s",
                      hh, paused, cine, im, il, mm, (long)g_sSpecialMove, menu, views, mtype,
                      SceneWantsTheater() ? "THEATER" : (!SceneScripted() ? "gameplay" :
                      (amc == 0 ? "MOVE (arms yours)" : (amc == 1 ? "CLIMB (holding hands native)" : "SCRIPTED (arms native)"))));
            OLProxyLog(l);
        }
    }
}

// [DEVBRIDGE] "objs <text>": log live objects whose name contains the text (case-insensitive),
// to find engine/UI state worth reading. Developer use only.
extern "C" void OLUE3_DevDumpObjects(const char* sub) {
    if (!sub || !*sub || !g_bpObjs || !g_bpNames || !g_bpUNameOff) { OLProxyLog("[OLVR][DEVOBJS] registry not ready"); return; }
    BP_RefreshObjects();
    char want[64]; strncpy_s(want, sub, _TRUNCATE); _strlwr_s(want);
    int shown = 0;
    char nm[96], low[96], l[200];
    for (int i = 0; i < g_bpObjNum && shown < 160; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4) || !BP_Name(idx, nm, sizeof(nm))) continue;
        strncpy_s(low, nm, _TRUNCATE); _strlwr_s(low);
        if (!strstr(low, want)) continue;
        uint32_t num = 0; BP_Read((char*)o + g_bpUNameOff + 4, &num, 4);   // FName number (_N suffix)
        sprintf_s(l, "[OLVR][DEVOBJS] %s_%u @%p", nm, num ? num - 1 : 0, o);
        OLProxyLog(l); ++shown;
    }
    sprintf_s(l, "[OLVR][DEVOBJS] '%s': %d shown (of %d objects)", sub, shown, g_bpObjNum);
    OLProxyLog(l);
}

// [DEVBRIDGE] "cls <Class>": the class's super chain and its own fields with offsets.
// "get <Class> <prop>": that field's value in every live instance of the class.
// UObject: Outer +0x40, Name +0x48 (discovered), Class +0x50. UStruct: SuperField +0x78.
static void* Dev_ClassObj(const char* name) { return BP_FindClassObj(name); }
extern "C" void OLUE3_DevClass(const char* name) {
    if (!name || !*name || !g_bpObjs || !g_bpUNameOff) return;
    BP_RefreshObjects();
    char l[260], nm[96], kn[96];
    void* cls = Dev_ClassObj(name);
    if (!cls) { sprintf_s(l, "[OLVR][DEVCLS] class '%.60s' not found", name); OLProxyLog(l); return; }
    int ll = sprintf_s(l, "[OLVR][DEVCLS] %.40s", name);
    for (void* s = ProbeReadPtr((char*)cls + 0x78); s && ll < 220; s = ProbeReadPtr((char*)s + 0x78))
        ll += sprintf_s(l + ll, sizeof(l) - ll, " : %.30s", BP_ObjName(s, nm, sizeof(nm)) ? nm : "?");
    OLProxyLog(l);
    char line[240]; int n = 0; line[0] = 0; int li = 0;
    char fline[240]; fline[0] = 0; int fi = 0;
    for (void* p = ProbeReadPtr((char*)cls + 0x80); p && n < 600; ++n) {
        if (!BP_ObjName(p, nm, sizeof(nm))) break;
        void* k = ProbeReadPtr((char*)p + g_bpUNameOff + 8);
        if (!k || !BP_ObjName(k, kn, sizeof(kn))) kn[0] = 0;
        if (!strcmp(kn, "Function")) {   // (Test40) the class's own functions, by name
            if (fi > 170) { sprintf_s(l, "[OLVR][DEVCLS]   fn: %s", fline); OLProxyLog(l); fi = 0; fline[0] = 0; }
            fi += sprintf_s(fline + fi, sizeof(fline) - fi, "%.40s  ", nm);
        }
        if (strstr(kn, "Property")) {
            uint32_t off = 0; BP_Read((char*)p + 0x8C, &off, 4);
            char ty[8]; strncpy_s(ty, kn, 3);
            if (li > 160) { sprintf_s(l, "[OLVR][DEVCLS]   %s", line); OLProxyLog(l); li = 0; line[0] = 0; }
            li += sprintf_s(line + li, sizeof(line) - li, "%.40s(%s)@%X  ", nm, ty, off);
        }
        void* nx = ProbeReadPtr((char*)p + 0x60);
        if (nx == p) break;
        p = nx;
    }
    if (li) { sprintf_s(l, "[OLVR][DEVCLS]   %s", line); OLProxyLog(l); }
    if (fi) { sprintf_s(l, "[OLVR][DEVCLS]   fn: %s", fline); OLProxyLog(l); }
}
// [DEVBRIDGE] (Test40) "enum <Class> <ByteProp>" or "enum <EnumName>": the enum's value names.
// UByteProperty keeps its UEnum* somewhere past the UProperty fields and UEnum keeps its names
// in a TArray<FName>; neither offset is known for this build, so both are found by shape.
static void DevEnumDump(void* en) {
    char l[300], nm[96];
    for (unsigned off = 0x58; off <= 0xA8; off += 8) {
        void* data = ProbeReadPtr((char*)en + off); int32_t num = 0, mx = 0;
        if (!data || !BP_Read((char*)en + off + 8, &num, 4) || !BP_Read((char*)en + off + 12, &mx, 4)) continue;
        if (num < 1 || num > 255 || mx < num || mx > 4096) continue;
        uint32_t first = 0;
        if (!BP_Read(data, &first, 4) || !BP_Name(first, nm, sizeof(nm))) continue;
        int ll = sprintf_s(l, "[OLVR][DEVENUM] %d names @+0x%X:", num, off);
        for (int i = 0; i < num && ll < 270; ++i) {
            uint32_t idx = 0; BP_Read((char*)data + i * 8, &idx, 4);
            if (!BP_Name(idx, nm, sizeof(nm))) strcpy_s(nm, "?");
            ll += sprintf_s(l + ll, sizeof(l) - ll, " %d=%.28s", i, nm);
            if (ll > 230 && i + 1 < num) { OLProxyLog(l); ll = sprintf_s(l, "[OLVR][DEVENUM]  "); }
        }
        OLProxyLog(l);
        return;
    }
    OLProxyLog("[OLVR][DEVENUM] no name array found");
}
extern "C" void OLUE3_DevEnum(const char* args) {
    if (!args || !g_bpObjs || !g_bpUNameOff) return;
    char a0[96] = { 0 }, a1[96] = { 0 }, l[260], nm[96], kn[96];
    const int na = sscanf_s(args, "%95s %95s", a0, (unsigned)sizeof(a0), a1, (unsigned)sizeof(a1));
    BP_RefreshObjects();
    if (na == 1) {
        for (int i = 0; i < g_bpObjNum; ++i) {
            void* o = nullptr;
            if (!BP_Read(g_bpObjs + i, &o, 8) || !o || !BP_ObjName(o, nm, sizeof(nm)) || _stricmp(nm, a0)) continue;
            void* k = ProbeReadPtr((char*)o + g_bpUNameOff + 8);
            if (!k || !BP_ObjName(k, kn, sizeof(kn)) || strcmp(kn, "Enum")) continue;
            sprintf_s(l, "[OLVR][DEVENUM] enum %s @%p", nm, o); OLProxyLog(l);
            DevEnumDump(o); return;
        }
        sprintf_s(l, "[OLVR][DEVENUM] enum '%.60s' not found", a0); OLProxyLog(l); return;
    }
    void* cls = Dev_ClassObj(a0);
    if (!cls) { sprintf_s(l, "[OLVR][DEVENUM] class '%.60s' not found", a0); OLProxyLog(l); return; }
    for (void* c = cls; c; c = ProbeReadPtr((char*)c + 0x78)) {
        for (void* p = ProbeReadPtr((char*)c + 0x80); p; ) {
            if (BP_ObjName(p, nm, sizeof(nm)) && !strcmp(nm, a1)) {
                for (unsigned off = 0x90; off <= 0xC8; off += 8) {
                    void* en = ProbeReadPtr((char*)p + off);
                    void* k = en ? ProbeReadPtr((char*)en + g_bpUNameOff + 8) : nullptr;
                    if (k && BP_ObjName(k, kn, sizeof(kn)) && !strcmp(kn, "Enum")) {
                        char enm[96] = "?"; BP_ObjName(en, enm, sizeof(enm));
                        sprintf_s(l, "[OLVR][DEVENUM] %s.%s -> enum %s (property +0x%X)", a0, a1, enm, off); OLProxyLog(l);
                        DevEnumDump(en); return;
                    }
                }
                sprintf_s(l, "[OLVR][DEVENUM] %s.%s has no enum", a0, a1); OLProxyLog(l); return;
            }
            void* nx = ProbeReadPtr((char*)p + 0x60);
            if (nx == p) break;
            p = nx;
        }
    }
    sprintf_s(l, "[OLVR][DEVENUM] %s.%s not found", a0, a1); OLProxyLog(l);
}
extern "C" void OLUE3_DevGet(const char* args) {
    if (!args || !g_bpObjs || !g_bpUNameOff) return;
    char cname[96] = { 0 }, prop[96] = { 0 }, l[260], nm[96], kn[96];
    if (sscanf_s(args, "%95s %95s", cname, (unsigned)sizeof(cname), prop, (unsigned)sizeof(prop)) < 2) return;
    BP_RefreshObjects();
    void* cls = Dev_ClassObj(cname);
    if (!cls) { sprintf_s(l, "[OLVR][DEVGET] class '%.60s' not found", cname); OLProxyLog(l); return; }
    unsigned off = 0, mask = 0; void* pobj = nullptr;
    for (void* c = cls; c && !pobj; c = ProbeReadPtr((char*)c + 0x78)) {
        for (void* p = ProbeReadPtr((char*)c + 0x80); p; ) {
            if (BP_ObjName(p, nm, sizeof(nm)) && !strcmp(nm, prop)) {
                void* k = ProbeReadPtr((char*)p + g_bpUNameOff + 8);
                if (k && BP_ObjName(k, kn, sizeof(kn)) && strstr(kn, "Property")) {
                    pobj = p; BP_Read((char*)p + 0x8C, &off, 4);
                    if (!strcmp(kn, "BoolProperty") && !SP_FindPropIn(c, prop, &off, &mask)) mask = 0;
                    break;
                }
            }
            void* nx = ProbeReadPtr((char*)p + 0x60);
            if (nx == p) break;
            p = nx;
        }
    }
    if (!pobj) { sprintf_s(l, "[OLVR][DEVGET] %.40s.%.40s not found", cname, prop); OLProxyLog(l); return; }
    sprintf_s(l, "[OLVR][DEVGET] %.40s.%.40s (%.20s) @+0x%X mask 0x%X", cname, prop, kn, off, mask); OLProxyLog(l);
    int shown = 0;
    for (int i = 0; i < g_bpObjNum && shown < 24; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || ProbeReadPtr((char*)o + g_bpUNameOff + 8) != cls) continue;
        uint32_t num = 0; BP_Read((char*)o + g_bpUNameOff + 4, &num, 4);
        char on[96] = "?", outer[96] = "-";
        BP_ObjName(o, on, sizeof(on));
        void* ou = ProbeReadPtr((char*)o + g_bpUNameOff - 8);
        if (ou) BP_ObjName(ou, outer, sizeof(outer));
        uint64_t raw = 0; BP_Read((char*)o + off, &raw, 8);
        char val[120];
        if (!strcmp(kn, "BoolProperty")) sprintf_s(val, "%d", mask ? (((uint32_t)raw & mask) ? 1 : 0) : -1);
        else if (!strcmp(kn, "ObjectProperty") || !strcmp(kn, "ClassProperty") || !strcmp(kn, "ComponentProperty")) {
            char pn[96] = "None"; void* pv = (void*)raw;
            if (pv) { if (!BP_ObjName(pv, pn, sizeof(pn))) strcpy_s(pn, "?"); }
            sprintf_s(val, "%p %s", pv, pn);
        } else if (!strcmp(kn, "ByteProperty")) sprintf_s(val, "%u", (unsigned)(raw & 0xFF));
        else if (!strcmp(kn, "FloatProperty")) { float f; memcpy(&f, &raw, 4); sprintf_s(val, "%.4f", f); }
        else if (!strcmp(kn, "NameProperty")) { char fn[96] = "?"; BP_Name((uint32_t)raw, fn, sizeof(fn)); sprintf_s(val, "%s", fn); }
        else if (!strcmp(kn, "ArrayProperty")) { int32_t num = -1; BP_Read((char*)o + off + 8, &num, 4); sprintf_s(val, "Num=%d data=%p", num, (void*)raw); }
        else if (!strcmp(kn, "StructProperty")) {
            // vectors/rotators: the first three 4-byte fields as floats and as ints
            float fv[3] = {}; int iv[3] = {}; BP_Read((char*)o + off, fv, 12); memcpy(iv, fv, 12);
            sprintf_s(val, "(%.1f %.1f %.1f) ints(%d %d %d)", fv[0], fv[1], fv[2], iv[0], iv[1], iv[2]);
        }
        else sprintf_s(val, "%d (0x%llX)", (int)(uint32_t)raw, (unsigned long long)raw);
        sprintf_s(l, "[OLVR][DEVGET]   %s_%u (in %s) @%p = %s", on, num ? num - 1 : 0, outer, o, val);
        OLProxyLog(l); ++shown;
    }
    sprintf_s(l, "[OLVR][DEVGET] %d instance(s)", shown); OLProxyLog(l);
}

// True when the object's class chain contains `base` (UObject Class at Name+8, SuperField +0x78).
static bool ObjIsA(void* o, const char* base) {
    char nm[96];
    void* c = o ? ProbeReadPtr((char*)o + g_bpUNameOff + 8) : nullptr;
    for (int i = 0; c && i < 32; ++i, c = ProbeReadPtr((char*)c + 0x78))
        if (BP_ObjName(c, nm, sizeof(nm)) && !strcmp(nm, base)) return true;
    return false;
}
// [DEVBRIDGE] "oget <hexaddr> <prop>": one field of one object, found through its class chain.
extern "C" void OLUE3_DevObjGet(const char* args) {
    unsigned long long addr = 0; char prop[96] = { 0 }, l[300], nm[96], kn[96], cn[96] = "?";
    if (!args || !g_bpObjs || !g_bpUNameOff) return;
    if (sscanf_s(args, "%llx %95s", &addr, prop, (unsigned)sizeof(prop)) < 2 || addr < 0x10000) return;
    void* o = (void*)(uintptr_t)addr;
    void* cls = ProbeReadPtr((char*)o + g_bpUNameOff + 8);
    if (!cls || !BP_ObjName(cls, cn, sizeof(cn))) { sprintf_s(l, "[OLVR][DEVOGET] %p is not an object", o); OLProxyLog(l); return; }
    unsigned off = 0, mask = 0; bool found = false;
    for (void* c = cls; c && !found; c = ProbeReadPtr((char*)c + 0x78)) {
        for (void* p = ProbeReadPtr((char*)c + 0x80); p; ) {
            if (BP_ObjName(p, nm, sizeof(nm)) && !strcmp(nm, prop)) {
                void* k = ProbeReadPtr((char*)p + g_bpUNameOff + 8);
                if (k && BP_ObjName(k, kn, sizeof(kn)) && strstr(kn, "Property")) {
                    found = true; BP_Read((char*)p + 0x8C, &off, 4);
                    if (!strcmp(kn, "BoolProperty") && !SP_FindPropIn(c, prop, &off, &mask)) mask = 0;
                    break;
                }
            }
            void* nx = ProbeReadPtr((char*)p + 0x60);
            if (nx == p) break;
            p = nx;
        }
    }
    if (!found) { sprintf_s(l, "[OLVR][DEVOGET] %s %p: no field '%.40s'", cn, o, prop); OLProxyLog(l); return; }
    uint64_t raw = 0; BP_Read((char*)o + off, &raw, 8);
    char val[160];
    if (!strcmp(kn, "BoolProperty")) sprintf_s(val, "%d (word 0x%08X mask 0x%X)", mask ? (((uint32_t)raw & mask) ? 1 : 0) : -1, (uint32_t)raw, mask);
    else if (!strcmp(kn, "ObjectProperty") || !strcmp(kn, "ClassProperty") || !strcmp(kn, "ComponentProperty")) {
        char pn[96] = "None"; void* pv = (void*)raw;
        if (pv && !BP_ObjName(pv, pn, sizeof(pn))) strcpy_s(pn, "?");
        sprintf_s(val, "%p %s", pv, pn);
    } else if (!strcmp(kn, "ByteProperty")) sprintf_s(val, "%u", (unsigned)(raw & 0xFF));
    else if (!strcmp(kn, "FloatProperty")) { float f; memcpy(&f, &raw, 4); sprintf_s(val, "%.4f", f); }
    else if (!strcmp(kn, "NameProperty")) { char fn[96] = "?"; BP_Name((uint32_t)raw, fn, sizeof(fn)); sprintf_s(val, "%s", fn); }
    else if (!strcmp(kn, "ArrayProperty")) { int32_t num = -1; BP_Read((char*)o + off + 8, &num, 4); sprintf_s(val, "Num=%d data=%p", num, (void*)raw); }
    else if (!strcmp(kn, "StructProperty")) {
        float fv[3] = {}; BP_Read((char*)o + off, fv, 12);
        sprintf_s(val, "(%.2f %.2f %.2f)", fv[0], fv[1], fv[2]);
    } else sprintf_s(val, "%d (0x%llX)", (int)(uint32_t)raw, (unsigned long long)raw);
    sprintf_s(l, "[OLVR][DEVOGET] %s %p .%s (%s @+0x%X) = %s", cn, o, prop, kn, off, val); OLProxyLog(l);
}
// [DEVBRIDGE] "raw <hexaddr> <bytes>": qwords, with object names where a qword is an object.
extern "C" void OLUE3_DevRaw(const char* args) {
    unsigned long long addr = 0; unsigned n = 64; char l[300], nm[96];
    if (!args || sscanf_s(args, "%llx %u", &addr, &n) < 1 || addr < 0x10000) return;
    if (n > 512) n = 512;
    for (unsigned i = 0; i < n; i += 8) {
        uint64_t q = 0;
        if (!BP_Read((char*)(uintptr_t)addr + i, &q, 8)) { sprintf_s(l, "[OLVR][DEVRAW] +0x%03X unreadable", i); OLProxyLog(l); break; }
        float f[2]; memcpy(f, &q, 8);
        char obj[100] = "";
        if (q > 0x10000 && g_bpUNameOff && BP_ObjName((void*)(uintptr_t)q, nm, sizeof(nm))) sprintf_s(obj, " -> %s", nm);
        else { char n0[64] = "", n1[64] = ""; if (BP_Name((uint32_t)q, n0, sizeof(n0)) && (uint32_t)q > 0) sprintf_s(obj, " name?%s", n0); (void)n1; }
        sprintf_s(l, "[OLVR][DEVRAW] +0x%03X %016llX  f(%.3f %.3f)%s", i, (unsigned long long)q, f[0], f[1], obj);
        OLProxyLog(l);
    }
}

// [HOLSTER] UPrimitiveComponent::SetHiddenGame(bool): virtual slot 0x4C0. execSetHidden (RVA 0x2516D0)
// reads its bool and calls [vtbl+0x4C0]. Game thread only - it reattaches the component.
static const unsigned char kExecSetHiddenPrologue[] = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                                       0x48,0x83,0xEC,0x20,0x48,0x8B,0x42,0x24 };
static int g_setHiddenOk = 0;   // 0 untried, 1 verified, -1 mismatch
static bool SetHiddenReady() {
    if (g_setHiddenOk) return g_setHiddenOk > 0;
    unsigned char have[sizeof(kExecSetHiddenPrologue)] = {};
    void* ex = (void*)(g_base + 0x2516D0);
    const bool ok = SafeReadBytes(ex, have, sizeof(have)) && !memcmp(have, kExecSetHiddenPrologue, sizeof(have));
    g_setHiddenOk = ok ? 1 : -1;
    OLProxyLog(ok ? "[OLVR][HOLSTER] SetHiddenGame (vtable 0x4C0) verified through execSetHidden"
                  : "[OLVR][HOLSTER] execSetHidden prologue MISMATCH - the camcorder model stays the game's");
    return ok;
}
extern "C" int OLUE3_SetHiddenGame(void* comp, int hidden) {
    if (!comp || !SetHiddenReady() || !ObjIsA(comp, "PrimitiveComponent")) return 0;
    __try {
        void** vt = *(void***)comp;
        typedef void (__fastcall *SetHidden_t)(void*, int);
        SetHidden_t f = (SetHidden_t)vt[0x4C0 / 8];
        if ((uintptr_t)f < g_base || (uintptr_t)f > g_base + 0x4000000) return 0;   // inside OLGame.exe
        f(comp, hidden ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
// [DEVBRIDGE] "hide <hexaddr> <0|1>" runs on the game thread at the end of the CPU split.
static volatile LONG64 g_devHideComp = 0;
static volatile LONG   g_devHideVal = 0;
extern "C" void OLUE3_DevQueueHide(void* comp, int hidden) {
    InterlockedExchange(&g_devHideVal, hidden ? 1 : 0);
    InterlockedExchange64(&g_devHideComp, (LONG64)(uintptr_t)comp);
}
static void DevGameThreadWork() {
    void* c = (void*)(uintptr_t)InterlockedExchange64(&g_devHideComp, 0);
    if (!c) return;
    char l[200], nm[96] = "?";
    if (g_bpUNameOff) BP_ObjName(c, nm, sizeof(nm));
    const int ok = OLUE3_SetHiddenGame(c, g_devHideVal);
    sprintf_s(l, "[OLVR][DEVBRIDGE] SetHiddenGame(%s %p, %ld) %s", nm, c, g_devHideVal, ok ? "done" : "REFUSED (not a primitive component?)");
    OLProxyLog(l);
}

// Every SkelControlSingleBone carrying a Hero left/right upper-arm or forearm reference. Several
// variants exist; driving all of them is safer than guessing which current mesh/LOD is attached.
static void ArmFind() {
    BP_RefreshObjects();
    g_armSingleN = 0;
    int found[2][2] = {};
    char nm[64], bn[64];
    for (int i = 0; i < g_bpObjNum && g_armSingleN < 64; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm)) || strcmp(nm, "SkelControlSingleBone")) continue;
        int sideScore[2] = { 0, 0 }, segScore[2] = { 0, 0 };
        for (unsigned off = 0xF0; off <= 0x300; off += 4) {
            uint32_t bi = 0;
            if (!BP_Read((char*)o + off, &bi, 4) || !BP_Name(bi, bn, sizeof(bn))) continue;
            if (strncmp(bn, "Hero-", 5)) continue;
            if (strstr(bn, "Hero-L-") || strstr(bn, "Hero_Left") || strstr(bn, "Hero-L_")) ++sideScore[0];
            if (strstr(bn, "Hero-R-") || strstr(bn, "Hero_Right") || strstr(bn, "Hero-R_")) ++sideScore[1];
            if (strstr(bn, "UpperArm")) ++segScore[ARM_UPPER];
            if (strstr(bn, "Forearm"))  ++segScore[ARM_FORE];
        }
        int hand = sideScore[0] > sideScore[1] ? 0 : (sideScore[1] > sideScore[0] ? 1 : -1);
        int seg = segScore[ARM_UPPER] > segScore[ARM_FORE] ? ARM_UPPER :
                  (segScore[ARM_FORE] > segScore[ARM_UPPER] ? ARM_FORE : -1);
        if (hand < 0 || seg < 0) continue;
        g_armSingle[g_armSingleN++] = { o, hand, seg };
        ++found[hand][seg];
    }
    char l[240];
    sprintf_s(l, "[OLVR][ARMDRIVE] %d live two-arm controls: L upper=%d fore=%d, R upper=%d fore=%d; "
                 "strength@0x%X applyRot=0x%X addRot=0x%X",
               g_armSingleN, found[0][0], found[0][1], found[1][0], found[1][1],
               g_scStrengthOff, g_bitApplyRotation, g_bitAddRotation);
    OLProxyLog(l);
    if (!found[0][0] || !found[0][1] || !found[1][0] || !found[1][1])
        OLProxyLog("[OLVR][ARMDRIVE] WARNING: complete L/R upper+forearm controls not found");
}

static int ArmWriteF(void* addr, float v) {
    __try { *(float*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ArmWriteB(void* addr, unsigned char v) {
    __try { *(unsigned char*)addr = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void ArmReleaseOwnership() {
    if (!g_armOwned || !g_scStrengthOff) return;
    for (int i = 0; i < g_armSingleN; ++i)
        ArmWriteF((char*)g_armSingle[i].control + g_scStrengthOff, 0.0f);
    g_armOwned = false;
}

// camLoc/baseYaw come from the camera hook, so the target is built in the same body frame the
// lean already uses and at the same measured 100 uu/m.
static void ArmTick(const float* camLoc, float baseYaw) {
    if (!g_armSingleN || !g_scStrengthOff || !g_bitApplyRotation) return;
    OLXrPad pad;
    const bool padReady = OLXR_MotionControlsReady() && OLXR_GetPad(&pad);

    // GAMEPLAY-ONLY OWNERSHIP. The original camera FOV identifies menus, cinematics and deep
    // camcorder views before CINEFOV widens them. Those states, and any explicit interaction,
    // belong to Outlast's authored animation. Stop writing immediately so the native AnimTree
    // can take both arms without fighting our IK; ordinary locomotion remains controller-driven.
    static long nativeHold = 0;
    if (padReady && ((pad.buttons & (OLXR_BTN_A | OLXR_BTN_B | OLXR_BTN_X | OLXR_BTN_Y)) ||
                     pad.gripR > 0.60f))
        nativeHold = 90;                       // roughly 1.5 seconds at the game tick
    else if (nativeHold > 0)
        --nativeHold;
    const bool gameplay = padReady && !OLXR_ScreenActive() && g_nativeNarrowFrames == 0 &&
                          nativeHold == 0 && g_injW > 0.95f;
    if (!gameplay) {
        g_armRamp[0] = g_armRamp[1] = 0.0f;
        ArmReleaseOwnership();                 // one-frame handoff, then native animation owns it
        return;
    }

    const float fx = cosf(baseYaw), fy = sinf(baseYaw);
    const float rx = -sinf(baseYaw), ry = cosf(baseYaw);

    struct HandTarget {
        bool valid; float wx, wy, wz, shx, shy, shz, ex, ey, ez;
        int rot[2][3]; float reach;
    } t[2] = {};

    const float kToUU = 65536.0f / 6.28318531f;
    auto dirToRot = [kToUU](float x, float y, float z, int roll, int* out) {
        const float flat = sqrtf(x * x + y * y);
        out[0] = (int)(atan2f(z, flat) * kToUU);
        out[1] = (int)(atan2f(y, x) * kToUU);
        out[2] = roll;
    };

    for (int hand = 0; hand < 2; ++hand) {
        if (!(pad.handValid & (1u << hand))) { g_armRamp[hand] = 0.0f; continue; }
        g_armRamp[hand] += (1.0f - g_armRamp[hand]) * 0.20f;
        if (g_armRamp[hand] > 0.999f) g_armRamp[hand] = 1.0f;

        const float hx = pad.handPose[hand].px, hy = pad.handPose[hand].py, hz = pad.handPose[hand].pz;
        const float dR = hx * kMetersToUU, dU = hy * kMetersToUU, dF = -hz * kMetersToUU;
        t[hand].wx = camLoc[0] + rx * dR + fx * dF;
        t[hand].wy = camLoc[1] + ry * dR + fy * dF;
        t[hand].wz = camLoc[2] + dU;

        const float side = hand ? 1.0f : -1.0f;
        t[hand].shx = camLoc[0] + rx * (kShoulderRight * side) - fx * kShoulderBack;
        t[hand].shy = camLoc[1] + ry * (kShoulderRight * side) - fy * kShoulderBack;
        t[hand].shz = camLoc[2] - kShoulderDown;
        float ax = t[hand].wx - t[hand].shx, ay = t[hand].wy - t[hand].shy;
        float az = t[hand].wz - t[hand].shz;
        float d = sqrtf(ax * ax + ay * ay + az * az);
        if (d > kArmReachUU && d > 0.001f) {
            const float k = kArmReachUU / d;
            t[hand].wx = t[hand].shx + ax * k;
            t[hand].wy = t[hand].shy + ay * k;
            t[hand].wz = t[hand].shz + az * k;
            d = kArmReachUU;
        }
        t[hand].reach = d;
        // Solve an elbow on a stable down/back/outward bend plane. Equal 31uu segments match the
        // measured 62uu reach and preserve the game's native shoulder placement.
        ax = t[hand].wx - t[hand].shx; ay = t[hand].wy - t[hand].shy;
        az = t[hand].wz - t[hand].shz;
        d = sqrtf(ax * ax + ay * ay + az * az);
        if (d < 1.0f) d = 1.0f;
        const float ux = ax / d, uy = ay / d, uz = az / d;
        float hxv = -fx * kElbowBack + rx * (12.0f * side);
        float hyv = -fy * kElbowBack + ry * (12.0f * side);
        float hzv = -kElbowDown;
        const float hp = hxv * ux + hyv * uy + hzv * uz;
        hxv -= hp * ux; hyv -= hp * uy; hzv -= hp * uz;
        float hn = sqrtf(hxv * hxv + hyv * hyv + hzv * hzv);
        if (hn < 0.001f) { hxv = rx * side; hyv = ry * side; hzv = -1.0f; hn = sqrtf(2.0f); }
        hxv /= hn; hyv /= hn; hzv /= hn;
        const float along = d * 0.5f;
        const float bend2 = 31.0f * 31.0f - along * along;
        const float bend = bend2 > 0.0f ? sqrtf(bend2) : 0.0f;
        t[hand].ex = t[hand].shx + ux * along + hxv * bend;
        t[hand].ey = t[hand].shy + uy * along + hyv * bend;
        t[hand].ez = t[hand].shz + uz * along + hzv * bend;

        const float qx = pad.handPose[hand].qx, qy = pad.handPose[hand].qy;
        const float qz = pad.handPose[hand].qz, qw = pad.handPose[hand].qw;
        const float handRoll = atan2f(2.0f * (qx * qy + qw * qz),
                                      1.0f - 2.0f * (qx * qx + qz * qz));
        dirToRot(t[hand].ex - t[hand].shx, t[hand].ey - t[hand].shy,
                 t[hand].ez - t[hand].shz, 0, t[hand].rot[ARM_UPPER]);
        dirToRot(t[hand].wx - t[hand].ex, t[hand].wy - t[hand].ey,
                 t[hand].wz - t[hand].ez, (int)(handRoll * kToUU),
                 t[hand].rot[ARM_FORE]);
        t[hand].valid = true;
    }

    for (int i = 0; i < g_armSingleN; ++i) {
        const int hand = g_armSingle[i].hand, seg = g_armSingle[i].segment;
        char* c = (char*)g_armSingle[i].control;
        if (hand < 0 || hand > 1 || seg < 0 || seg > 1 || !t[hand].valid) {
            ArmWriteF(c + g_scStrengthOff, 0.0f);
            continue;
        }
        __try {
            unsigned flags = *(unsigned*)(c + kSingleBools);
            flags |= g_bitApplyRotation;
            if (g_bitAddRotation) flags &= ~g_bitAddRotation;
            if (g_bitRemoveMeshRotation) flags &= ~g_bitRemoveMeshRotation;
            *(unsigned*)(c + kSingleBools) = flags;
            *(unsigned char*)(c + kSingleRotSpace) = 0; // BCS_WorldSpace
            *(int*)(c + kSingleRotation + 0) = t[hand].rot[seg][0];
            *(int*)(c + kSingleRotation + 4) = t[hand].rot[seg][1];
            *(int*)(c + kSingleRotation + 8) = t[hand].rot[seg][2];
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        ArmWriteF(c + g_scStrengthOff, g_armRamp[hand]);
    }
    g_armOwned = true;
    static long s_rep = 0;
    if ((++s_rep % 180) == 1) {
        char l[220];
        sprintf_s(l, "[OLVR][ARMDRIVE] gameplay single-bone IK n=%d valid=0x%X rampL=%.2f rampR=%.2f reachL=%.0f reachR=%.0f",
                  g_armSingleN, pad.handValid, g_armRamp[0], g_armRamp[1],
                  t[0].valid ? t[0].reach : 0.0f, t[1].valid ? t[1].reach : 0.0f);
        OLProxyLog(l);
    }
}

// ---- [VISIBLEIK] controller IK on the mesh that is actually rendered -------------------------
// The AnimTree SkelControl objects discovered above accept writes but are templates/inactive
// instances for this first-person mesh. The visible Miles component is composed by the native
// function at RVA 0x6F8910. Hooking AFTER that function gives us the final LocalAtoms/SpaceBases
// window: change the arm locals, ask the native composer to rebuild the hierarchy once, then
// restore the source locals so the next authored animation tick starts from untouched game data.
// Fingers and hand children follow because the engine performs the second hierarchy compose.
struct VIk3 { float x, y, z; };
struct QIk4 { float x, y, z, w; };
struct IkAtom { QIk4 q; VIk3 t; float scale; };
struct IkArray { IkAtom* d; int n, mx; };
static_assert(sizeof(IkAtom) == 0x20, "UE3 FBoneAtom size changed");

typedef void (__fastcall *PoseCompose_t)(void*);
static PoseCompose_t o_PoseCompose = nullptr;
static const uintptr_t kPoseComposeRva = 0x6F8910;
static const unsigned char kPoseComposePrologue[11] = {
    0x4C,0x8B,0xDC,0x53,0x48,0x81,0xEC,0x30,0x02,0x00,0x00
};
static __declspec(thread) int g_poseRecompose = 0;
// USkeletalMeshComponent::UpdateSkelPose(float DeltaTime, bool bTickFaceFX).
// Unlike the partial composer above, every visible skeletal component passes through this full
// update. Applying after it returns is the final game-thread window before render extraction.
typedef void (__fastcall *SkelUpdate_t)(void*, float, int);
static SkelUpdate_t o_SkelUpdate = nullptr;
static const uintptr_t kSkelUpdateRva = 0x6FFC90;
static const unsigned char kSkelUpdatePrologue[14] = {
    0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57
};
static long g_skelUpdateHookOk = 0;
static void* g_poseMesh = nullptr;
static int g_poseBone[2][3] = { { -1,-1,-1 }, { -1,-1,-1 } }; // upper, forearm, hand
static int g_poseCameraBone = -1;
static float g_poseRamp[2] = { 0.0f, 0.0f };
static long g_poseApplied = 0;
// A frozen, component-derived Outlast 1 reference pose.  Only the upper-body driver bones use
// it; native torso/legs remain live and native finger curls are carried along by the solved hand.
static IkAtom g_poseRefLocal[73] = {};
static void*  g_poseRefMesh = nullptr;
static bool   g_poseRefValid = false;
// Wrist axes are calibrated once from Miles's own frozen reference pose.  Unlike Test 15 this
// is an absolute grip-to-wrist mapping; it does not reinterpret whatever angle the player happened
// to hold the controllers at on the first gameplay frame as "neutral".
static QIk4  g_poseRefCameraQ = {0,0,0,1};
static QIk4  g_poseRefWristQ[2] = { {0,0,0,1},{0,0,0,1} };
static QIk4  g_poseWristOffset[2] = { {0,0,0,1},{0,0,0,1} };
static bool   g_poseWristOffsetValid = false;
static VIk3  g_poseTargetSmooth[2] = {};
static bool   g_poseTargetSmoothValid[2] = { false,false };
static VIk3  g_posePoleSmooth[2] = {};
static bool   g_posePoleSmoothValid[2] = { false,false };
static long   g_poseRecenterSeen = -1;

// ---- [ARMTUNE] Test33 corrections to the Test16 solver -----------------------------------------
// The solver itself (reference pose, two-bone solve, pole, wrist mapping, helper rebase) is not
// touched. These change only WHICH inputs it gets and HOW its three filters advance. Each is a
// separate switch, and all three off is exactly Test16.
//  HandSync  - hands are located at the head's display time and frozen with it (ol_xr
//              [HANDSYNC]) instead of the live pad sample one display period older.
//  LeanFix   - the hand offset is measured from the RAW head and moved by the lean the camera
//              actually applied. Test16 measured it from the clamped head and ignored the lean,
//              so leaning in by d drew the hands d short of where the real hands are - d too
//              close to the eyes that had just moved in.
//  SmoothDt  - target follow, pole blend and ramp-in advance per SECOND, matching Test16
//              exactly at 60 fps. Test16 advanced them per FRAME, so the arms felt floatier at
//              40 fps and twitchier at 90.
static volatile long g_armHandSync = 0, g_armLeanFix = 0, g_armSmoothDt = 0;
extern "C" void OLUE3_GetArmFlags(int* handSync, int* leanFix, int* smoothDt) {
    if (handSync) *handSync = g_armHandSync ? 1 : 0;
    if (leanFix)  *leanFix  = g_armLeanFix ? 1 : 0;
    if (smoothDt) *smoothDt = g_armSmoothDt ? 1 : 0;
}
// 0 = all off (Test16), 1 = all on, 2 = mixed (set individually in the ini).
extern "C" int OLUE3_GetArmImproved() {
    const long n = (g_armHandSync ? 1 : 0) + (g_armLeanFix ? 1 : 0) + (g_armSmoothDt ? 1 : 0);
    return n == 0 ? 0 : (n == 3 ? 1 : 2);
}
extern "C" void OLUE3_SetArmImproved(int on) {
    g_armHandSync = g_armLeanFix = g_armSmoothDt = on ? 1 : 0;
    OLProxyLog(on ? "[OLVR][ARMTUNE] Test33 arm corrections ON (hand sync, lean fix, per-second smoothing)"
                  : "[OLVR][ARMTUNE] Test33 arm corrections OFF - exact Test16 arm behaviour");
}
// A per-frame blend factor f (tuned at 60 fps) advanced by `frames` 60 fps frames' worth of time.
// frames == 1 returns f unchanged, so SmoothDt off is bit-for-bit Test16.
static float ArmPer(float f, float frames) {
    if (frames == 1.0f) return f;
    if (f >= 1.0f) return 1.0f;
    if (f <= 0.0f) return 0.0f;
    return 1.0f - powf(1.0f - f, frames);
}

// ---- [PHASE1] camera comfort and the shared lean -----------------------------------------------
// [PHYSCROUCH] While a physical crouch holds the GAME crouched, the game's own crouch already
// lowers the camera, so this much of the player's real drop is handed back (ramped by
// ol_motion.cpp) instead of lowering the view twice.
static volatile float g_crouchLiftM = 0.0f;
extern "C" void OLUE3_SetCrouchLift(float m) { g_crouchLiftM = (m > 0.0f && m < 1.0f) ? m : 0.0f; }
// [COMFORT] Head-bob reduction strength (0 off .. 1) and the level horizon switch.
static volatile float g_bobReduce = 0.0f;
static volatile long  g_levelHorizon = 0;
static volatile float g_bobCorrZ = 0.0f;   // uu, applied to the view this frame; the arms follow it
extern "C" float OLUE3_GetBobReduce() { return g_bobReduce; }
extern "C" void  OLUE3_SetBobReduce(float v) { g_bobReduce = (v > 0.0f) ? (v < 1.0f ? v : 1.0f) : 0.0f; }
extern "C" int   OLUE3_GetLevelHorizon() { return g_levelHorizon ? 1 : 0; }
extern "C" void  OLUE3_SetLevelHorizon(int v) { g_levelHorizon = v ? 1 : 0; }

// What the camera hook adds to the view for the lean this frame: metres, recenter frame (+X right,
// +Y up, +Z back). ONE function so the camera and the arm solver can never disagree about it.
static void ViewLeanM(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!(g_headTrack && g_olPoseValid && g_headPos && OLXR_PosArmed())) return;
    float h[3] = { g_olHeadX, g_olHeadY, g_olHeadZ };
    const float lift = g_crouchLiftM;
    if (lift > 0.0f) { h[1] += lift; if (h[1] > 0.0f) h[1] = 0.0f; }   // never above standing
    for (int i = 0; i < 3; ++i) {
        float v = h[i];
        if (v < -kPosSanityM) v = -kPosSanityM; else if (v > kPosSanityM) v = kPosSanityM;
        out[i] = v * g_leanGain * g_injW / g_worldSize;   // (Beta 1.1) [WORLDSIZE]
    }
}

// [COMFORT] Head bob is the Hero-Camera bone riding the walk cycle. A level+trend (Holt) filter
// follows stairs, slopes and crouch moves without lag, so only the oscillation is removed, and the
// correction is capped at 10 uu (10 cm) so jumps, falls and scripted moves always pass through.
// Called once per frame, at the first eye; both eyes use the same value.
static float BobCorrection(float z, float w) {
    static bool s_init = false;
    static float s_level = 0.0f, s_trend = 0.0f;
    static LARGE_INTEGER s_last = {}, s_f = {};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
    float dt = s_last.QuadPart ? (float)((double)(now.QuadPart - s_last.QuadPart) / (double)s_f.QuadPart) : 0.0f;
    s_last = now;
    if (g_bobReduce <= 0.001f || w <= 0.0f || dt <= 0.0f || dt > 0.25f || !s_init ||
        fabsf(z - s_level) > 60.0f) {
        s_level = z; s_trend = 0.0f; s_init = true;
        return 0.0f;
    }
    if (dt < 0.002f) dt = 0.002f;
    const float a = 1.0f - expf(-dt / 0.20f);   // level: removes the ~2 Hz walk bob
    const float b = 1.0f - expf(-dt / 0.50f);   // trend: tracks stairs and slopes
    const float prev = s_level;
    const float pred = s_level + s_trend * dt;
    s_level = pred + (z - pred) * a;
    s_trend += ((s_level - prev) / dt - s_trend) * b;
    float corr = s_level - z;
    if (corr > 10.0f) corr = 10.0f; else if (corr < -10.0f) corr = -10.0f;
    return corr * g_bobReduce * w;
}

// [BOB2] (Beta 1.1) Testers: "an option to disable head bobbing when running", "couldn't disable the
// viewbob". The filter above only takes the camera bone's up-down bounce out. A stride also rocks the
// view (the bone pitches and yaws) and sways it sideways and back and forth. Each is measured against
// what does not bob - the controller's own rotation and Miles's position - and split the same way:
// what moves slowly (turning, a deliberate camera move, the camera leading a sprint) stays, what
// swings with the stride goes, scaled by the same ReduceHeadBob strength. A jump bigger than a
// stride's swing passes untouched. Plain walking and running only; climbs and scenes are the game's.
static volatile int   g_bobRotCorr[2] = { 0, 0 };          // pitch, yaw (UE3 units) added to the view
static volatile float g_bobXYCorr[2] = { 0.0f, 0.0f };     // uu added to the view location (world X, Y)
static volatile float g_bobSF[2] = { 0.0f, 0.0f };         // the same, along the body: right, forward (uu)
static unsigned g_bobLocOff = 0;
static bool g_bobLocTried = false;
static struct { float maxOsc[4], maxCorr[4]; long frames, passed; } g_bobStat = {};
static void BobStabilize(void* pc, const float* loc, const int* rot, float w) {
    static bool s_init = false;
    static float s_lp[4] = { 0, 0, 0, 0 };   // pitch, yaw (units), side, forward (uu)
    static LARGE_INTEGER s_last = {}, s_f = {};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
    float dt = s_last.QuadPart ? (float)((double)(now.QuadPart - s_last.QuadPart) / (double)s_f.QuadPart) : 0.0f;
    s_last = now;
    g_bobRotCorr[0] = g_bobRotCorr[1] = 0;
    g_bobXYCorr[0] = g_bobXYCorr[1] = 0.0f;
    g_bobSF[0] = g_bobSF[1] = 0.0f;
    if (!g_bobLocTried && g_ssRotation) {
        g_bobLocTried = true;
        unsigned o = 0;
        if (SP_FindPropAny("Actor", "Location", &o, nullptr) && SS_Plausible(o)) g_bobLocOff = o;
    }
    int crot[3] = { 0, 0, 0 };
    float hloc[3] = { 0, 0, 0 };
    bool ok = g_bobReduce > 0.001f && w > 0.0f && dt > 0.0f && dt < 0.25f && g_ssRotation && g_bobLocOff &&
              ArmMoveClass() == 0 && g_sCinematic <= 0 && pc;
    if (ok) {
        __try {
            memcpy(crot, (const char*)pc + g_ssRotation, sizeof(crot));
            const char* hero = *(const char* const*)((const char*)pc + kCtrlHeroOff);
            if (!hero || *(void* const*)(hero + kHeroCtrlOff) != pc) ok = false;
            else memcpy(hloc, hero + g_bobLocOff, sizeof(hloc));
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    }
    if (!ok) { s_init = false; return; }
    const float yawR = (float)(crot[1] & 0xFFFF) * (6.28318531f / 65536.0f);
    const float fx = cosf(yawR), fy = sinf(yawR), rx = -sinf(yawR), ry = cosf(yawR);
    const float ox = loc[0] - hloc[0], oy = loc[1] - hloc[1];
    const float v[4] = { (float)WrapUE(rot[0] - crot[0]), (float)WrapUE(rot[1] - crot[1]),
                         ox * rx + oy * ry, ox * fx + oy * fy };
    static int s_prevCtrl[2] = { 0, 0 };
    const float ctrlRate[2] = { fabsf((float)WrapUE(crot[0] - s_prevCtrl[0])) / (dt > 0.002f ? dt : 0.002f),
                                fabsf((float)WrapUE(crot[1] - s_prevCtrl[1])) / (dt > 0.002f ? dt : 0.002f) };
    s_prevCtrl[0] = crot[0]; s_prevCtrl[1] = crot[1];
    if (!s_init) { for (int i = 0; i < 4; ++i) s_lp[i] = v[i]; s_init = true; return; }
    if (dt < 0.002f) dt = 0.002f;
    // While the controller itself turns (a snap or smooth turn, the pitch leveler) the bone trails it a
    // little: that gap is the turn, not a stride, and taking it out would make the turn end with a jerk.
    for (int i = 0; i < 2; ++i) if (ctrlRate[i] > 3640.0f) s_lp[i] = v[i];   // 20 deg/s
    const float a = 1.0f - expf(-dt / 0.30f);   // a stride is ~0.3-0.5 s; turning and leading are slower
    static const float kSwing[4] = { 1456.0f, 1456.0f, 14.0f, 14.0f };   // 8 deg, 8 deg, 14 cm, 14 cm
    float c[4];
    for (int i = 0; i < 4; ++i) {
        s_lp[i] += (v[i] - s_lp[i]) * a;
        float osc = v[i] - s_lp[i];
        if (fabsf(osc) > kSwing[i]) { s_lp[i] = v[i]; osc = 0.0f; ++g_bobStat.passed; }   // a real move
        c[i] = -osc * g_bobReduce * w;
        if (fabsf(osc) > g_bobStat.maxOsc[i]) g_bobStat.maxOsc[i] = fabsf(osc);
        if (fabsf(c[i]) > g_bobStat.maxCorr[i]) g_bobStat.maxCorr[i] = fabsf(c[i]);
    }
    ++g_bobStat.frames;
    g_bobRotCorr[0] = (int)c[0];
    g_bobRotCorr[1] = (int)c[1];
    g_bobXYCorr[0] = rx * c[2] + fx * c[3];
    g_bobXYCorr[1] = ry * c[2] + fy * c[3];
    g_bobSF[0] = c[2]; g_bobSF[1] = c[3];
    static ULONGLONG s_rep = 0;
    const ULONGLONG t = GetTickCount64();
    if (t - s_rep > 5000 && g_bobStat.frames > 30) {
        s_rep = t;
        char l[300];
        sprintf_s(l, "[OLVR][BOB2] last 5 s (%ld frames): swing pitch %.1f yaw %.1f deg, side %.1f fwd %.1f uu | "
                     "taken out pitch %.1f yaw %.1f deg, side %.1f fwd %.1f uu | real moves passed %ld | strength %.2f",
                  g_bobStat.frames, g_bobStat.maxOsc[0] * (360.0f / 65536.0f), g_bobStat.maxOsc[1] * (360.0f / 65536.0f),
                  g_bobStat.maxOsc[2], g_bobStat.maxOsc[3], g_bobStat.maxCorr[0] * (360.0f / 65536.0f),
                  g_bobStat.maxCorr[1] * (360.0f / 65536.0f), g_bobStat.maxCorr[2], g_bobStat.maxCorr[3],
                  g_bobStat.passed, g_bobReduce);
        OLProxyLog(l);
        memset(&g_bobStat, 0, sizeof(g_bobStat));
    }
}

static VIk3 IVSub(VIk3 a, VIk3 b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
static VIk3 IVAdd(VIk3 a, VIk3 b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
static VIk3 IVScale(VIk3 a, float s) { return { a.x*s, a.y*s, a.z*s }; }
static float IVDot(VIk3 a, VIk3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static VIk3 IVCross(VIk3 a, VIk3 b) {
    return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
static float IVLen(VIk3 a) { return sqrtf(IVDot(a,a)); }
static VIk3 IVNorm(VIk3 a) {
    const float n = IVLen(a); return n > 1e-6f ? IVScale(a, 1.0f/n) : VIk3{1,0,0};
}
static QIk4 IQNorm(QIk4 q) {
    const float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    return n > 1e-6f ? QIk4{q.x/n,q.y/n,q.z/n,q.w/n} : QIk4{0,0,0,1};
}
static QIk4 IQConj(QIk4 q) { return { -q.x,-q.y,-q.z,q.w }; }
static QIk4 IQMul(QIk4 a, QIk4 b) {
    return IQNorm({
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    });
}
static VIk3 IQRotate(QIk4 q, VIk3 p) {
    const VIk3 v{q.x,q.y,q.z};
    return IVAdd(p, IVAdd(IVScale(IVCross(v,p),2.0f*q.w),
                          IVScale(IVCross(v,IVCross(v,p)),2.0f)));
}
static QIk4 IQFromTo(VIk3 from, VIk3 to) {
    from = IVNorm(from); to = IVNorm(to);
    const float d = IVDot(from, to);
    if (d < -0.9995f) {
        VIk3 axis = IVCross(from, fabsf(from.x) < 0.8f ? VIk3{1,0,0} : VIk3{0,1,0});
        axis = IVNorm(axis); return { axis.x,axis.y,axis.z,0.0f };
    }
    const VIk3 c = IVCross(from, to);
    return IQNorm({ c.x,c.y,c.z,1.0f+d });
}
static QIk4 IQBlend(QIk4 a, QIk4 b, float t) {
    if (a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w < 0.0f)
        b = { -b.x,-b.y,-b.z,-b.w };
    return IQNorm({ a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t,
                    a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t });
}

static int PoseParent(void* mesh, int bone) {
    if (!mesh || bone < 0) return -1;
    __try {
        const char* refs = *(const char**)((char*)mesh + 0xC4);
        const int n = *(int*)((char*)mesh + 0xCC);
        if (!refs || bone >= n) return -1;
        return *(const int*)(refs + (size_t)bone * 0x50 + 0x40);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static bool PoseDescendant(void* mesh, int bone, int root, int count) {
    if (!mesh || bone < 0 || root < 0 || bone >= count || root >= count) return false;
    for (int depth=0; depth<count; ++depth) {
        if (bone == root) return true;
        if (bone <= 0) return false;
        const int parent = PoseParent(mesh,bone);
        if (parent < 0 || parent == bone || parent >= count) return false;
        bone = parent;
    }
    return false;
}

// Outlast 2's stable arm solver works on the completed component-space pose.
// Rotate the entire descendant subtree so twist, helper, palm and finger bones
// remain rigidly connected to their native animation instead of rebuilding only
// three local joints with assumptions about the game's bone axes.
static void PoseTransformTree(IkArray& space, void* mesh, int root, QIk4 delta,
                              VIk3 translation = {0,0,0}) {
    if (!space.d || root < 0 || root >= space.n) return;
    const VIk3 pivot = space.d[root].t;
    for (int i=0; i<space.n; ++i) if (PoseDescendant(mesh,i,root,space.n)) {
        space.d[i].t = IVAdd(pivot,IVAdd(IQRotate(delta,IVSub(space.d[i].t,pivot)),translation));
        space.d[i].q = IQMul(delta,space.d[i].q);
    }
}

// [ARMFIX] 8 (Test42): each twist helper bone (and its chain) relative to its arm driver, in the reference pose
static QIk4 g_helperRefQ[73]; static VIk3 g_helperRefT[73]; static bool g_helperRefOk = false;
static const int kTwistHelper[4][2] = { {29,11},{31,10},{53,35},{55,34} };   // helper root, driver
static void PoseResetTrackingCalibration() {
    for (int hand=0; hand<2; ++hand) {
        g_poseTargetSmoothValid[hand]=false;
        g_posePoleSmoothValid[hand]=false;
        g_poseRamp[hand]=0.0f;
    }
}

// Capture one verified Miles pose as LOCAL bone transforms.  Bone translations/lengths come
// from the actual Outlast 1 mesh, not estimated body constants.  Freezing the locals once means
// ordinary flat-game arm animation can no longer drag the VR shoulders around every frame.
static bool PoseCaptureReference(const IkArray& space, void* mesh) {
    if (!space.d || space.n!=73 || !mesh) return false;
    __try {
        for (int i=0;i<73;++i) {
            const int parent=PoseParent(mesh,i);
            if (parent<0 || parent>=73) {
                g_poseRefLocal[i]=space.d[i];
                continue;
            }
            const QIk4 inv=IQConj(space.d[parent].q);
            const float parentScale=fabsf(space.d[parent].scale)>1e-5f ? space.d[parent].scale : 1.0f;
            g_poseRefLocal[i].q=IQMul(inv,space.d[i].q);
            g_poseRefLocal[i].t=IVScale(IQRotate(inv,IVSub(space.d[i].t,space.d[parent].t)),
                                        1.0f/parentScale);
            g_poseRefLocal[i].scale=space.d[i].scale/parentScale;
        }
        const float l1L=IVLen(IVSub(space.d[11].t,space.d[10].t));
        const float l2L=IVLen(IVSub(space.d[12].t,space.d[11].t));
        const float l1R=IVLen(IVSub(space.d[35].t,space.d[34].t));
        const float l2R=IVLen(IVSub(space.d[36].t,space.d[35].t));
        const float shoulderSpan=IVLen(IVSub(space.d[34].t,space.d[10].t));
        if (l1L<8.0f || l1L>80.0f || l2L<8.0f || l2L>80.0f ||
            l1R<8.0f || l1R>80.0f || l2R<8.0f || l2R>80.0f ||
            shoulderSpan<15.0f || shoulderSpan>100.0f) return false;
        // [ARMFIX] 8: the twist helpers' relation to their drivers in this calm pose
        g_helperRefOk=true;
        for (int k=0;k<4;++k) {
            const int h=kTwistHelper[k][0], d=kTwistHelper[k][1];
            const QIk4 inv=IQConj(space.d[d].q);
            for (int i=0;i<73;++i) if (PoseDescendant(mesh,i,h,73)) {
                g_helperRefQ[i]=IQMul(inv,space.d[i].q);
                g_helperRefT[i]=IQRotate(inv,IVSub(space.d[i].t,space.d[d].t));
            }
        }
        g_poseRefCameraQ=space.d[g_poseCameraBone].q;
        g_poseRefWristQ[0]=space.d[g_poseBone[0][2]].q;
        g_poseRefWristQ[1]=space.d[g_poseBone[1][2]].q;
        g_poseWristOffsetValid=false;
        g_poseRefMesh=mesh; g_poseRefValid=true;
        PoseResetTrackingCalibration();
        char l[260];
        sprintf_s(l,"[OLVR][VISIBLEIK] Outlast 1 reference captured: shoulderSpan=%.1f armL=%.1f+%.1f armR=%.1f+%.1f (mesh units)",
                  shoulderSpan,l1L,l2L,l1R,l2R);
        OLProxyLog(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_poseRefValid=false; g_poseRefMesh=nullptr; return false;
    }
}

// Rebuild one bone from its frozen local reference under the CURRENT native parent.  The torso
// can still walk, jump and crouch, but the requested arm bone no longer inherits native arm
// animation.  Descendants move rigidly so native finger curls survive until their own driver is
// solved later in the chain.
static bool PoseApplyReferenceBone(IkArray& space, void* mesh, int bone) {
    if (!g_poseRefValid || !space.d || bone<0 || bone>=space.n) return false;
    const int parent=PoseParent(mesh,bone);
    if (parent<0 || parent>=space.n) return false;
    const IkAtom& p=space.d[parent];
    const IkAtom& local=g_poseRefLocal[bone];
    const QIk4 wantedQ=IQMul(p.q,local.q);
    const VIk3 wantedT=IVAdd(p.t,IQRotate(p.q,IVScale(local.t,p.scale)));
    const QIk4 delta=IQMul(wantedQ,IQConj(space.d[bone].q));
    const VIk3 translation=IVSub(wantedT,space.d[bone].t);
    PoseTransformTree(space,mesh,bone,delta,translation);
    space.d[bone].scale=p.scale*local.scale;
    return true;
}

static QIk4 PoseUprightHand(VIk3 forward) {
    forward.z=0.0f; forward=IVNorm(forward);
    const QIk4 aim=IQFromTo({1,0,0},forward);
    return IQMul(IQFromTo(IQRotate(aim,{0,0,1}),{0,0,1}),aim);
}

static QIk4 PoseGripCanonical(const OLXrPose& grip) {
    // OpenXR grip (+X right,+Y up,-Z forward) -> UE component canonical
    // (+X forward,+Y right,+Z up).  The live body basis is applied separately below.
    return IQNorm({grip.qz,-grip.qx,-grip.qy,grip.qw});
}

// [ARMFIX] (Test42) "The arms deform when moving around a lot or crouching". Three causes, each a
// switch (dev "armfix <mask>", default all):
//  1 the upper arm turned by the shortest rotation onto its new direction - fine near Miles's
//    reference pose, but the further the arm went (up, across, far out) the more it rolled about its
//    own axis, so the elbow then had to bend sideways and wrung the elbow / shoulder skin. Now the upper
//    arm is turned so the elbow's hinge axis lies across the bend: the forearm then turns about that
//    hinge only, like a real elbow.
//  2 the whole wrist roll went into the forearm from the elbow out (candy wrapper at the elbow). Now
//    half of it turns the forearm and the rest happens at the wrist, as in a real forearm.
//  4 the collarbone never moved: reaching high or far stretched the shoulder. Now it turns up to 25 deg
//    toward a raised or far reach.
//  8 the forearm and upper-arm twist helpers (the bones that shape the sleeve and skin) copied the
//    GAME's own twist relation every frame - and the game swings Miles's arms as he walks, runs and
//    crouches: measured 12-19 deg of drift walking / sprinting, up to 31 deg crouching / crouch-walking,
//    under 5 standing. Copied onto arms that follow your hands, that wrung and wobbled the skin. Now
//    they keep the relation from Miles's calm reference pose (captured with the arm reference).
static int g_armFix = 15;
extern "C" void OLUE3_DevArmFix(int mask) {
    g_armFix = mask & 15;
    char l[140]; sprintf_s(l, "[OLVR][ARMFIX] hinge=%d twistSplit=%d collarbone=%d steadyHelpers=%d", g_armFix & 1, (g_armFix >> 1) & 1, (g_armFix >> 2) & 1, (g_armFix >> 3) & 1);
    OLProxyLog(l);
}
// The rotation that takes direction a0 onto a1 exactly and b0 (square to a0) onto b1 as far as it goes.
static QIk4 IQFromFrames(VIk3 a0, VIk3 b0, VIk3 a1, VIk3 b1) {
    const QIk4 q1 = IQFromTo(a0, a1);
    a1 = IVNorm(a1);
    VIk3 br = IQRotate(q1, b0); br = IVSub(br, IVScale(a1, IVDot(br, a1)));
    VIk3 bw = IVSub(b1, IVScale(a1, IVDot(b1, a1)));
    if (IVLen(br) < 1e-4f || IVLen(bw) < 1e-4f) return q1;
    br = IVNorm(br); bw = IVNorm(bw);
    QIk4 q2;
    if (IVDot(br, bw) < -0.9999f) q2 = { a1.x, a1.y, a1.z, 0.0f };      // half a turn about a1
    else q2 = IQFromTo(br, bw);                                          // both square to a1: a roll about it
    return IQNorm(IQMul(q2, q1));
}
// The elbow's hinge axis: across the plane of shoulder, elbow and wrist (cross of the shoulder->wrist
// line and the elbow's sideways offset from it). `fallback` is used for a straight arm.
static VIk3 ArmHinge(VIk3 shoulder, VIk3 elbow, VIk3 wrist, VIk3 fallbackBend) {
    VIk3 aim = IVSub(wrist, shoulder);
    if (IVLen(aim) < 0.01f) return { 0, 0, 0 };
    aim = IVNorm(aim);
    VIk3 bend = IVSub(elbow, shoulder); bend = IVSub(bend, IVScale(aim, IVDot(bend, aim)));
    if (IVLen(bend) < 0.75f) { bend = IVSub(fallbackBend, IVScale(aim, IVDot(fallbackBend, aim))); if (IVLen(bend) < 0.01f) return { 0, 0, 0 }; }
    return IVNorm(IVCross(aim, IVNorm(bend)));
}

static void PoseOrientArm(IkArray& space, void* mesh, int lower, int hand,
                          QIk4 target, float alpha) {
    target=IQBlend(space.d[hand].q,target,alpha);
    const VIk3 axis=IVNorm(IVSub(space.d[hand].t,space.d[lower].t));
    const QIk4 delta=IQMul(target,IQConj(space.d[hand].q));
    const float projection=delta.x*axis.x+delta.y*axis.y+delta.z*axis.z;
    if (projection*projection+delta.w*delta.w>1e-8f) {
        QIk4 twist=IQNorm({axis.x*projection,axis.y*projection,axis.z*projection,delta.w});
        if (g_armFix & 2) twist=IQBlend({0,0,0,1},twist,0.5f);   // [ARMFIX] half in the forearm, half at the wrist
        PoseTransformTree(space,mesh,lower,twist);
    }
    PoseTransformTree(space,mesh,hand,IQMul(target,IQConj(space.d[hand].q)));
}

// [ARMFIX] 8: a twist helper chain placed on its (solved) driver by the reference pose's relation.
static void PoseRebaseHelperRef(IkArray& space, void* mesh, int helper, int driver) {
    if (!space.d || helper<0 || helper>=space.n || driver<0 || driver>=space.n) return;
    const IkAtom& dv=space.d[driver];
    for (int i=0;i<space.n;++i) if (PoseDescendant(mesh,i,helper,space.n)) {
        space.d[i].q=IQMul(dv.q,g_helperRefQ[i]);
        space.d[i].t=IVAdd(dv.t,IQRotate(dv.q,g_helperRefT[i]));
    }
}
static void PoseRebaseHelper(IkArray& space, const IkAtom original[73], void* mesh,
                             int helper, int driver) {
    if (!space.d || helper<0 || helper>=space.n || driver<0 || driver>=space.n) return;
    const QIk4 delta=IQMul(space.d[driver].q,IQConj(original[driver].q));
    for (int i=0;i<space.n;++i) if (PoseDescendant(mesh,i,helper,space.n)) {
        space.d[i].t=IVAdd(space.d[driver].t,
                           IQRotate(delta,IVSub(original[i].t,original[driver].t)));
        space.d[i].q=IQMul(delta,original[i].q);
        space.d[i].scale=original[i].scale;
    }
}

static bool PoseTwoBone(VIk3 root, VIk3 target, VIk3 pole, float upperLength,
                        float lowerLength, VIk3& joint, VIk3& end) {
    if (upperLength < 0.01f || lowerLength < 0.01f) return false;
    VIk3 aim = IVSub(target,root); float distance = IVLen(aim);
    if (distance < 0.001f) return false;
    aim = IVScale(aim,1.0f/distance);
    const float minimumDistance=fabsf(upperLength-lowerLength)+0.5f;
    const float maximumDistance=upperLength+lowerLength-0.5f;
    if (distance<minimumDistance) distance=minimumDistance;
    if (distance>maximumDistance) distance=maximumDistance;
    VIk3 bend = IVSub(IVSub(pole,root),IVScale(aim,IVDot(IVSub(pole,root),aim)));
    if (IVLen(bend) < 0.01f)
        bend = IVCross(aim,fabsf(aim.z)<0.8f?VIk3{0,0,1}:VIk3{0,1,0});
    bend = IVNorm(bend);
    const float along=(distance*distance+upperLength*upperLength-lowerLength*lowerLength)/(2.0f*distance);
    const float heightSquared=upperLength*upperLength-along*along;
    const float height=sqrtf(heightSquared>0.0f?heightSquared:0.0f);
    joint=IVAdd(root,IVAdd(IVScale(aim,along),IVScale(bend,height)));
    end=IVAdd(root,IVScale(aim,distance));
    return true;
}

// (Test47) After Trager Miles's visible body is 'Miles_beheaded_fingerless' while his shadow body can still be
// 'Miles_beheaded': two assets, one skeleton (73 bones, same names in the same order - checked in memory).
// Both pass through here every frame; keyed on the asset pointer, the bone cache and the reference pose were
// thrown away and recaptured on every switch, which held the arms' blend-in at a third (measured: the hand
// never reached the controller). Any Miles body with the 73-bone skeleton is the same body to the arms.
static bool MilesBody(void* mesh) {
    if (!mesh || !g_bpNames || !g_bpUNameOff) return false;
    __try {
        char nm[64];
        if (!BP_Name(*(uint32_t*)((char*)mesh + g_bpUNameOff), nm, sizeof(nm)) || strncmp(nm, "Miles_beheaded", 14)) return false;
        return *(int*)((char*)mesh + 0xCC) == 73;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Every skeletal component in the level passes through PoseResolveBones each frame: Miles's bodies are
// remembered once checked, so any other component still costs one name lookup, as before.
// A remembered entry must still carry the same name and 73 bones: after a level change the memory of a
// freed body can hold some other object.
static struct { void* m; uint32_t name; } g_milesKnown[4] = {};
static bool MilesBodyCached(void* m) {
    if (!m) return false;
    for (auto& k : g_milesKnown) {
        if (k.m != m) continue;
        uint32_t nm = 0; int bones = 0;
        if (BP_Read((char*)m + g_bpUNameOff, &nm, 4) && BP_Read((char*)m + 0xCC, &bones, 4) && nm == k.name && bones == 73) return true;
        k.m = nullptr; break;
    }
    if (!MilesBody(m)) return false;
    static int s_next = 0;
    g_milesKnown[s_next].m = m; BP_Read((char*)m + g_bpUNameOff, &g_milesKnown[s_next].name, 4); s_next = (s_next + 1) & 3;
    return true;
}
static bool SameMilesBody(void* a, void* b) { return a == b || (a && b && MilesBodyCached(a) && MilesBodyCached(b)); }
static bool PoseResolveBones(void* mesh) {
    if (!mesh) return false;
    if (mesh != g_poseMesh && !MilesBodyCached(mesh)) return false;   // not Miles: one lookup, as before
    if (SameMilesBody(mesh, g_poseMesh) && g_poseCameraBone >= 0 && g_poseBone[0][0] >= 0 &&
        g_poseBone[0][1] >= 0 && g_poseBone[0][2] >= 0 && g_poseBone[1][0] >= 0 &&
        g_poseBone[1][1] >= 0 && g_poseBone[1][2] >= 0) return true;
    if (!mesh || !g_bpNames || !g_bpUNameOff) return false;
    __try {
        uint32_t meshIdx = *(uint32_t*)((char*)mesh + g_bpUNameOff);
        char meshName[64];
        // (Test47) A tester: after Trager's wheelchair scene the VR hands stopped working.
        // From then on Miles is 'Miles_beheaded_fingerless' (OLHero.FingerlessMesh) - the exact-name
        // check turned the arms off for the rest of the game. Every Miles_beheaded* body counts.
        if (!BP_Name(meshIdx, meshName, sizeof(meshName)) || strncmp(meshName, "Miles_beheaded", 14))
            return false;
        // Do not let the hundreds of non-player skeletal components evict this cache every frame.
        g_poseMesh = mesh; g_poseCameraBone = -1;
        for (int h=0; h<2; ++h) for (int s=0; s<3; ++s) g_poseBone[h][s] = -1;
        const char* refs = *(const char**)((char*)mesh + 0xC4);
        const int n = *(int*)((char*)mesh + 0xCC);
        if (!refs || n != 73) return false;
        int handNameLength[2] = { 0x7fffffff,0x7fffffff };
        for (int i=0; i<n; ++i) {
            const uint32_t ni = *(const uint32_t*)(refs + (size_t)i * 0x50);
            char nm[64]; if (!BP_Name(ni, nm, sizeof(nm))) continue;
            if (strstr(nm, "Camera")) g_poseCameraBone = i;
            int hand = -1;
            if (strstr(nm, "Hero-L-") || strstr(nm, "Hero_Left") || strstr(nm, "Hero-L_")) hand = 0;
            if (strstr(nm, "Hero-R-") || strstr(nm, "Hero_Right") || strstr(nm, "Hero-R_")) hand = 1;
            if (hand < 0) continue;
            if (strstr(nm, "UpperArm")) g_poseBone[hand][0] = i;
            else if (strstr(nm, "Forearm") && !strstr(nm,"Twist")) g_poseBone[hand][1] = i;
            else if (strstr(nm, "-Hand") || strstr(nm, "_Hand")) {
                // Finger/helper names also contain "Hand". Test 9 selected the
                // final such child (28/52) as the wrist and twisted the mesh.
                // As in Outlast 2, the shortest semantic hand name is the wrist.
                const int length=(int)strlen(nm);
                if (length < handNameLength[hand]) {
                    handNameLength[hand]=length; g_poseBone[hand][2]=i;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    const bool ok = g_poseCameraBone >= 0 && g_poseBone[0][0] >= 0 && g_poseBone[0][1] >= 0 &&
                    g_poseBone[0][2] >= 0 && g_poseBone[1][0] >= 0 &&
                    g_poseBone[1][1] >= 0 && g_poseBone[1][2] >= 0 &&
                    PoseDescendant(mesh,g_poseBone[0][1],g_poseBone[0][0],73) &&
                    PoseDescendant(mesh,g_poseBone[0][2],g_poseBone[0][1],73) &&
                    PoseDescendant(mesh,g_poseBone[1][1],g_poseBone[1][0],73) &&
                    PoseDescendant(mesh,g_poseBone[1][2],g_poseBone[1][1],73);
    char l[220];
    sprintf_s(l, "[OLVR][VISIBLEIK] bone map %s: camera=%d L=%d/%d/%d R=%d/%d/%d",
              ok ? "ready" : "INCOMPLETE", g_poseCameraBone,
              g_poseBone[0][0],g_poseBone[0][1],g_poseBone[0][2],
              g_poseBone[1][0],g_poseBone[1][1],g_poseBone[1][2]);
    OLProxyLog(l);
    return ok;
}

// [IKDIAG] (Test36) Test35 played a whole gameplay stretch with the gate open and no arm solve at
// all, and logged nothing about why. Every ~5 s of open gate without a single solve now says
// which step stopped it, and names the skeletal meshes it saw (one of them should be Miles).
static volatile long g_ikMiss[5] = { 0, 0, 0, 0, 0 };   // noPad, noHands, notMiles, badSpace, solved
static void* g_ikMeshes[12] = {};
static long  g_ikMeshN = 0;
static void IkNote(int why, void* mesh) {
    InterlockedIncrement(&g_ikMiss[why]);
    if (why == 2 && mesh && g_ikMeshN < 12) {
        bool seen = false;
        for (long i = 0; i < g_ikMeshN; ++i) if (g_ikMeshes[i] == mesh) { seen = true; break; }
        if (!seen) g_ikMeshes[g_ikMeshN++] = mesh;
    }
    static LARGE_INTEGER s_f = {}, s_t0 = {};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
    if (!s_t0.QuadPart) { s_t0 = now; return; }
    if ((now.QuadPart - s_t0.QuadPart) < s_f.QuadPart * 5) return;
    s_t0 = now;
    long m[5]; for (int i = 0; i < 5; ++i) m[i] = InterlockedExchange(&g_ikMiss[i], 0);
    static bool s_wasSolving = true;
    const bool solving = m[4] > 0;
    if (!solving) {
        static long s_logs = 0;
        if (++s_logs <= 40) {
            char l[700]; int n = sprintf_s(l, "[OLVR][IKDIAG] gate open, no arm solve in 5 s: noPad=%ld noHands=%ld notMiles=%ld badSpace=%ld | meshes:",
                                           m[0], m[1], m[2], m[3]);
            for (long i = 0; i < g_ikMeshN && n < 640; ++i) {
                char nm[64] = "?"; int refs = -1;
                __try {
                    uint32_t idx = *(uint32_t*)((char*)g_ikMeshes[i] + g_bpUNameOff);
                    if (!g_bpNames || !BP_Name(idx, nm, sizeof(nm))) strcpy_s(nm, "?");
                    refs = *(int*)((char*)g_ikMeshes[i] + 0xCC);
                } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(nm, "<gone>"); }
                n += sprintf_s(l + n, sizeof(l) - n, " %.40s(%d)", nm, refs);
            }
            OLProxyLog(l);
        }
    } else if (!s_wasSolving) {
        char l[160]; sprintf_s(l, "[OLVR][IKDIAG] arms solving again (%ld solves in 5 s)", m[4]); OLProxyLog(l);
    }
    s_wasSolving = solving;
    g_ikMeshN = 0;
}

// ---- [HOLSTER] (Test38) the game's own handycam: on your chest, in your hand, gliding between ----------
// Outlast keeps OLHero.CameraMesh (+0x2CF0, SkeletalMesh "handycam") attached to Miles's body mesh at
// the Hero-R-Hand_aux bone and HIDDEN in first person whether the camcorder is up or down (checked
// live: HiddenGame stays 1 through a raise and a lower). Its shadow twin CameraMeshShadowProxy
// (+0x2CF8) hangs off the hidden ShadowProxy body the same way. While ol_interact asks for it the mod
// shows the model and places it every frame by rewriting that attachment's RelativeLocation /
// RelativeRotation right here - after the arm solve, before the engine updates the mesh's attached
// children - so it follows this frame's final hand bone with no lag:
//   in the hand: the right controller's grip pose (the arm's own mapping) times kCamHeld,
//   on the chest: the holster point from ol_interact, upright, facing the body's way,
// blended by ol_interact's eased 0..1. At your eye it is hidden (you are looking through it), and
// whenever ol_interact steps back (menus, cutscenes, holster off) the game's state is put back.
#pragma pack(push, 4)
struct UEAttachment { void* comp; uint32_t boneName, boneNum; float loc[3]; int32_t rot[3]; float scale[3]; };
#pragma pack(pop)
static_assert(sizeof(UEAttachment) == 0x34, "UE3 FAttachment layout changed");
struct UEAttachArray { UEAttachment* d; int32_t n, mx; };
static const unsigned kCompAttachmentsOff = 0x3EC;          // SkeletalMeshComponent.Attachments
static const unsigned kHeroCameraMeshOff = 0x2CF0, kHeroCameraShadowOff = 0x2CF8;
static const unsigned kPrimHiddenWordOff = 0x178, kPrimHiddenGameMask = 0x4;   // PrimitiveComponent.HiddenGame
// The model in the hand, in the grip's canonical frame (+x forward, +y right, +z up): rotation as
// roll/pitch/yaw degrees and offset in uu. On the chest: an extra tilt. Tuned live ("camtune").
static float g_camHeldRot[3]  = { 0.0f, 0.0f, 0.0f };
// [CAMHOLD] (Test40) How the camcorder sits in the hand, always in the game's own strap grip (Miles's
// hand through the strap on its right side, fingers over the top, the screen flipped open):
//  3 (default): held the way the game itself shows it - its raise animation, captured frame by frame
//    with the game's own arms (Test40): from your eyes you see the flip-out screen face on and the
//    camcorder's left side (ports, dial), Miles's fingers over its top, the body turned 60 degrees
//    across from the way he faces, with a slight roll. It follows the way Miles faces, turned / tilted
//    a little toward where your hand is; moving your hand moves it, turning the controller does not;
//    Miles's wrist turns to fit. g_camHold3: share of the hand's tilt, roll, turn (camtune 4).
//  2: it points where your controller points and stays upright (the first Test40 build - "unnatural").
//  0: Test39 - every bit of the controller's angle, roll included.
//  1: the hand follows the controller like an empty hand and the camcorder sits in it (tried in
//    testing: with the arm calibration's wrist that put the lens sideways or toward you).
//  4 (Test43 default, "free hold"): it moves and turns exactly with your controller, like holding a
//    real one ("you don't have much free movement with it" - mode 3 ignored the controller's angle).
//    The game's grip is its resting angle: g_camHold4 (camtune 5) turns the controller's frame into the
//    camcorder's so a relaxed handshake hold points the lens ahead, upright, the flip screen toward you.
//  5 (Beta 1.1 test 1 only, "real camcorder" - NOT shipped: the user found it broken in the headset; the default
//    is 4 again and 5 is reachable only through the dev bridge "camhold 5" until it is reworked): a tester - "holding the camcorder has the wrong rotation ... correct
//    rotation should let you have the top end of your controller facing towards you like how you hold a real
//    camcorder". The free hold turned over about the controller's own left-right axis: the controller's top
//    (its ring) toward your face is the camcorder's back, the lens points away from you along the handle,
//    the camcorder upright, Miles's hand in the game's own grip around it. The sliders adjust it the same way.
static int g_camHoldMode = 4;
static float g_camHold3[3] = { 0.35f, 12.0f, -60.0f };   // pitch share, roll (deg), turn across (deg)
static float g_camHold4[3] = { 0.0f, 0.0f, 0.0f };      // roll, pitch, yaw (deg) from the controller to the camcorder
// [CAMADJUST] (Test45) "We need to get the camcorder in the right position and maybe add a setting where I can
// position it myself." Free hold now takes the controller's AIM pose (where it points) instead of its grip pose:
// the grip's forward runs along the handle - up and forward in a relaxed hold - so the lens pointed up past
// your view with the camcorder's top toward you (the user's screenshot). The player's own adjustment, from the
// VR settings: g_camHold4 above (roll, tilt, turn) and g_camAdjOff (forward, up, right, cm) - the hand and the
// camcorder in it move together, in the frame of the way you face.
static float g_camAdjOff[3] = { 0.0f, 0.0f, 0.0f };
static volatile long g_camAdjLog = 1;                     // report where it sits the next time it is in the hand
extern "C" void OLUE3_SetCamAdjust(const float rot[3], const float off[3]) {
    // + tilt raises the lens, + turn swings it right, + roll leans its top right (about +Y a positive angle dips +X)
    g_camHold4[0] = -rot[2]; g_camHold4[1] = -rot[0]; g_camHold4[2] = rot[1];
    for (int i = 0; i < 3; ++i) g_camAdjOff[i] = off[i];
    g_camAdjLog = 1;
    char l[200];
    sprintf_s(l, "[OLVR][CAMADJUST] camcorder in the hand: tilt %+.0f, turn %+.0f, roll %+.0f deg; forward %+.0f, up %+.0f, right %+.0f cm",
              rot[0], rot[1], rot[2], off[0], off[1], off[2]);
    OLProxyLog(l);
}
extern "C" void OLUE3_DevCamHold(int mode) {
    g_camHoldMode = (mode >= 0 && mode <= 5) ? mode : 4;
    OLProxyLog(g_camHoldMode == 5 ? "[OLVR][CAMHOLD] real camcorder hold: the controller's top toward you, the lens away" :
               g_camHoldMode == 4 ? "[OLVR][CAMHOLD] free hold: the camcorder moves and turns with your controller" :
               g_camHoldMode == 3 ? "[OLVR][CAMHOLD] camcorder held like the game shows it: screen to your eyes, upright" :
               g_camHoldMode == 2 ? "[OLVR][CAMHOLD] camcorder points where the controller points, kept upright" :
               g_camHoldMode == 1 ? "[OLVR][CAMHOLD] camcorder sits in the hand, hand follows the controller (experiment)" :
                                    "[OLVR][CAMHOLD] camcorder copies the controller angle, roll included (Test39)");
}
// The same forward, rolled so the camcorder's top is as close to straight up as that forward allows
// (comp space: +X forward, +Z up). Pointing nearly straight up or down, the roll is left alone.
static QIk4 CamUpright(QIk4 q) {
    const VIk3 f = IVNorm(IQRotate(q, { 1, 0, 0 })), u = IQRotate(q, { 0, 0, 1 });
    const float fz = fabsf(f.z);
    if (fz > 0.97f) return q;
    VIk3 want = IVSub({ 0, 0, 1 }, IVScale(f, f.z));              // world up, square to the forward
    want = IVNorm(want);
    QIk4 fix = IQFromTo(u, want);
    if (IVDot(u, want) < -0.999f) fix = IQNorm({ f.x, f.y, f.z, 0.0f });   // upside down: half a turn about the forward
    const float w = 1.0f - (fz < 0.85f ? 0.0f : (fz - 0.85f) / 0.12f);
    return IQBlend(q, IQMul(fix, q), w);
}
static float g_camHeldOff[3]  = { 6.0f, 0.0f, -1.0f };
static float g_camChestRot[3] = { 0.0f, 20.0f, 0.0f };
// (Test39) On Miles's right hip, from his right thigh joint: up, forward, right (uu, body frame) -
// clipped to the outside of the hip, lens forward and tipped a little down.
static float g_camChestOff[3] = { 0.0f, -1.0f, 21.0f };
// Where the holstered model is, back in the recenter frame (metres), for ol_interact's glow and grab.
static float g_camChestTrack[3] = {};
static volatile ULONGLONG g_camChestMs = 0;
extern "C" int OLUE3_HolsterVisual(float out[3]) {
    if (!g_camChestMs || GetTickCount64() - g_camChestMs > 300) return 0;
    out[0] = g_camChestTrack[0]; out[1] = g_camChestTrack[1]; out[2] = g_camChestTrack[2];
    return 1;
}
static bool  g_camVisible = false;                         // the mod made the model visible
static void* g_camMeshSeen = nullptr;
static UEAttachment* g_camEntry[2] = { nullptr, nullptr }; // main, shadow: offsets the mod wrote
static void* g_camEntryComp[2] = { nullptr, nullptr };
extern "C" void OLUE3_DevCamTune(int which, float a, float b, float c) {
    float* t = which == 0 ? g_camHeldRot : (which == 1 ? g_camHeldOff : (which == 2 ? g_camChestRot : (which == 3 ? g_camChestOff : (which == 4 ? g_camHold3 : g_camHold4))));
    t[0] = a; t[1] = b; t[2] = c;
    char l[160]; sprintf_s(l, "[OLVR][HOLSTER] tune %s = (%.2f %.2f %.2f)", which == 0 ? "heldRot" : (which == 1 ? "heldOff" : (which == 2 ? "chestRot" : (which == 3 ? "chestOff" : (which == 4 ? "hold3 (pitch share, roll, turn)" : "hold4 (roll, pitch, yaw)")))), a, b, c);
    OLProxyLog(l);
}
static QIk4 IQAxis(VIk3 axis, float rad) { const float s = sinf(rad * 0.5f); return IQNorm({ axis.x * s, axis.y * s, axis.z * s, cosf(rad * 0.5f) }); }
static QIk4 IQEulerDeg(const float e[3]) {   // roll about X, then pitch about Y, then yaw about Z
    const float d = 3.14159265f / 180.0f;
    return IQMul(IQAxis({ 0, 0, 1 }, e[2] * d), IQMul(IQAxis({ 0, 1, 0 }, e[1] * d), IQAxis({ 1, 0, 0 }, e[0] * d)));
}
// FQuat -> FRotator exactly as UE3's FMatrix::Rotator() reads the rotated axes.
static void IQToRotator(QIk4 q, int32_t out[3]) {
    const VIk3 X = IQRotate(q, { 1, 0, 0 }), Y = IQRotate(q, { 0, 1, 0 }), Z = IQRotate(q, { 0, 0, 1 });
    const float pitch = atan2f(X.z, sqrtf(X.x * X.x + X.y * X.y));
    const float yaw = atan2f(X.y, X.x);
    const VIk3 sy = { -sinf(yaw), cosf(yaw), 0.0f };
    const float roll = atan2f(IVDot(Z, sy), IVDot(Y, sy));
    const float k = 32768.0f / 3.14159265f;
    out[0] = (int32_t)lroundf(pitch * k); out[1] = (int32_t)lroundf(yaw * k); out[2] = (int32_t)lroundf(roll * k);
}
// ---- [NATIVEGRIP] (Test39) the game's own hand pose for the camcorder -------------------------------
// Whenever the camcorder is up at your eye the right arm plays Outlast's own camcorder animation
// (OLInteract_RightArmNative). ~0.9 s into it, the right hand's finger bones (everything under the
// wrist) are read as LOCAL transforms, and the hand auxiliary bone the handycam hangs from is read
// relative to the wrist. Holding the camcorder lowered then puts exactly that grip back on Miles's
// hand: the fingers wrap the camcorder the way the game animates them, the camcorder sits where the
// game puts it in the hand (attachment offset zero), and the wrist turns so its lens points where your
// controller points. A grip baked from this build's capture is used until the first live one.
static IkAtom g_gripLocal[73];
static bool   g_gripHas[73];
static IkAtom g_gripAux;           // the hand auxiliary bone relative to the wrist
static bool   g_gripValid = false, g_gripLive = false;
static int    g_rAux = -1, g_lAux = -1, g_hipBone = -1;
static IkAtom IkLocalOf(const IkArray& s, int child, int parent) {
    IkAtom l; const IkAtom& p = s.d[parent]; const IkAtom& c = s.d[child];
    const QIk4 inv = IQConj(p.q);
    const float ps = fabsf(p.scale) > 1e-5f ? p.scale : 1.0f;
    l.q = IQMul(inv, c.q); l.t = IVScale(IQRotate(inv, IVSub(c.t, p.t)), 1.0f / ps); l.scale = c.scale / ps;
    return l;
}
static IkAtom IkCompose(const IkAtom& p, const IkAtom& l) {
    IkAtom c; c.q = IQMul(p.q, l.q); c.t = IVAdd(p.t, IQRotate(p.q, IVScale(l.t, p.scale))); c.scale = p.scale * l.scale;
    return c;
}
static int BoneFind(void* mesh, const char* const* subs, int nsubs) {
    const char* refs = (const char*)ProbeReadPtr((char*)mesh + 0xC4);
    int n = 0; BP_Read((char*)mesh + 0xCC, &n, 4);
    if (!refs || n <= 0 || n > 256) return -1;
    for (int k = 0; k < nsubs; ++k)
        for (int i = 0; i < n; ++i) {
            uint32_t ni = 0; char nm[64];
            if (BP_Read(refs + (size_t)i * 0x50, &ni, 4) && BP_Name(ni, nm, sizeof(nm)) && strstr(nm, subs[k])) return i;
        }
    return -1;
}
static void GripBones(void* mesh) {
    static void* s_mesh = nullptr;
    if (s_mesh == mesh) return;
    s_mesh = mesh;
    static const char* const kRA[] = { "R-Hand_aux", "R_Hand_aux" }, *const kLA[] = { "L-Hand_aux", "L_Hand_aux" };
    static const char* const kHip[] = { "R-Thigh", "R_Thigh", "RThigh", "R-UpLeg", "Pelvis" };
    g_rAux = BoneFind(mesh, kRA, 2); g_lAux = BoneFind(mesh, kLA, 2); g_hipBone = BoneFind(mesh, kHip, 5);
    char l[200]; sprintf_s(l, "[OLVR][NATIVEGRIP] bones: right hand aux %d, left hand aux %d, hip %d", g_rAux, g_lAux, g_hipBone);
    OLProxyLog(l);
}
// [NATIVEGRIP] the grip this build captured (bone index, local quaternion, local translation)
struct GripBake { int bone; float qx, qy, qz, qw, tx, ty, tz; };
// Captured 2026-10-14 from Outlast's own camcorder-raise animation (gripdump), right hand 37-51.
static const GripBake kGripBake[] = {
    { 37, -0.42135f, 0.28703f, -0.33896f, -0.79069f, 2.621f, -1.309f, 2.024f },
    { 38, 0.00541f, -0.03127f, 0.30361f, -0.95227f, 5.138f, -0.000f, 0.000f },
    { 39, 0.00001f, -0.00002f, 0.16986f, -0.98547f, 3.571f, -0.001f, -0.000f },
    { 40, -0.04228f, 0.04170f, 0.00061f, -0.99823f, 10.645f, 0.206f, 3.175f },
    { 41, 0.00000f, 0.00000f, 0.36028f, -0.93284f, 4.163f, -0.000f, 0.000f },
    { 42, -0.00000f, 0.00000f, 0.31403f, -0.94941f, 2.140f, -0.000f, 0.000f },
    { 43, 0.06727f, 0.01969f, 0.11267f, -0.99116f, 11.083f, 0.201f, 0.845f },
    { 44, 0.00003f, 0.00008f, 0.38477f, -0.92301f, 4.338f, -0.000f, 0.000f },
    { 45, 0.00003f, -0.00007f, 0.35836f, -0.93358f, 2.920f, 0.000f, -0.000f },
    { 46, 0.10000f, -0.00295f, 0.19536f, -0.97562f, 9.954f, 0.755f, -1.683f },
    { 47, 0.00000f, -0.00000f, 0.20463f, -0.97884f, 4.016f, 0.000f, 0.000f },
    { 48, -0.00000f, 0.00000f, 0.33754f, -0.94131f, 2.661f, -0.000f, 0.000f },
    { 49, 0.13735f, -0.00261f, 0.06874f, -0.98813f, 9.210f, 0.216f, -4.120f },
    { 50, -0.01820f, -0.00023f, 0.19279f, -0.98107f, 2.730f, -0.000f, 0.000f },
    { 51, -0.00000f, 0.00000f, 0.30675f, -0.95179f, 1.943f, -0.000f, 0.000f },
    { 52, 0.01805f, -0.77427f, -0.04382f, -0.63108f, 10.492f, -4.129f, -2.288f },
};
static const GripBake kGripAuxBake = { 52, 0.01805f, -0.77427f, -0.04382f, -0.63108f, 10.492f, -4.129f, -2.288f };
static void GripUseBake() {
    if (g_gripValid || kGripAuxBake.bone < 0) return;
    for (const GripBake& b : kGripBake) {
        if (b.bone < 0 || b.bone >= 73) continue;
        g_gripLocal[b.bone].q = { b.qx, b.qy, b.qz, b.qw }; g_gripLocal[b.bone].t = { b.tx, b.ty, b.tz }; g_gripLocal[b.bone].scale = 1.0f;
        g_gripHas[b.bone] = true;
    }
    g_gripAux.q = { kGripAuxBake.qx, kGripAuxBake.qy, kGripAuxBake.qz, kGripAuxBake.qw };
    g_gripAux.t = { kGripAuxBake.tx, kGripAuxBake.ty, kGripAuxBake.tz }; g_gripAux.scale = 1.0f;
    g_gripValid = true;
    OLProxyLog("[OLVR][NATIVEGRIP] using the camcorder grip captured for this build (a live one replaces it)");
}
static void GripCapture(const IkArray& space, void* mesh) {
    const int wrist = g_poseBone[1][2];
    if (wrist < 0 || g_rAux < 0 || g_rAux >= space.n) return;
    int n = 0;
    for (int i = 0; i < space.n; ++i) {
        g_gripHas[i] = false;
        if (i == wrist || !PoseDescendant(mesh, i, wrist, space.n)) continue;
        const int p = PoseParent(mesh, i);
        if (p < 0 || p >= space.n) continue;
        g_gripLocal[i] = IkLocalOf(space, i, p); g_gripHas[i] = true; ++n;
    }
    g_gripAux = IkLocalOf(space, g_rAux, wrist);
    const bool first = !g_gripLive;
    g_gripValid = g_gripLive = true;
    if (first) {
        char l[220]; sprintf_s(l, "[OLVR][NATIVEGRIP] camcorder grip captured from the game's own animation: %d finger bones, camcorder bone %d at (%.1f %.1f %.1f) from the wrist",
                               n, g_rAux, g_gripAux.t.x, g_gripAux.t.y, g_gripAux.t.z);
        OLProxyLog(l);
    }
}
// Put the captured grip on the right hand, `w` of the way (0 = as solved, 1 = the game's grip).
static void GripApply(IkArray& space, void* mesh, float w) {
    const int wrist = g_poseBone[1][2];
    if (!g_gripValid || wrist < 0 || w <= 0.0f) return;
    IkAtom loc[73]; bool sub[73] = {};
    for (int i = 0; i < space.n && i < 73; ++i) {
        if (i == wrist || !PoseDescendant(mesh, i, wrist, space.n)) continue;
        const int p = PoseParent(mesh, i);
        if (p < 0 || p >= space.n) continue;
        loc[i] = IkLocalOf(space, i, p); sub[i] = true;
        if (g_gripHas[i]) {
            loc[i].q = IQBlend(loc[i].q, g_gripLocal[i].q, w);
            loc[i].t = IVAdd(loc[i].t, IVScale(IVSub(g_gripLocal[i].t, loc[i].t), w));
        }
    }
    for (int i = 0; i < space.n && i < 73; ++i) if (sub[i]) space.d[i] = IkCompose(space.d[PoseParent(mesh, i)], loc[i]);
    if (g_rAux >= 0 && g_rAux < space.n) {
        IkAtom rel = IkLocalOf(space, g_rAux, wrist);
        rel.q = IQBlend(rel.q, g_gripAux.q, w);
        rel.t = IVAdd(rel.t, IVScale(IVSub(g_gripAux.t, rel.t), w));
        const IkAtom want = IkCompose(space.d[wrist], rel);
        const QIk4 dq = IQMul(want.q, IQConj(space.d[g_rAux].q));
        PoseTransformTree(space, mesh, g_rAux, dq, IVSub(want.t, space.d[g_rAux].t));
    }
}
// [DOORGRIP] (Test39) A hand on a door handle closes into the same grip. The left hand gets the right
// hand's grip mirrored: the reference pose tells which local axis the rig mirrors (left and right
// finger bones' local offsets agree on two axes and disagree on one), and a rotation mirrored across
// that axis keeps that component and negates the other two.
static IkAtom g_gripLeft[73];
static bool   g_gripLeftHas[73];
static int    g_gripLeftState = 0;      // 0 not built, 1 built, -1 rig not mirrored
static void GripBuildLeft(void* mesh) {
    if (g_gripLeftState || !g_gripValid || !g_poseRefValid) return;
    const int lw = g_poseBone[0][2], rw = g_poseBone[1][2];
    if (lw < 0 || rw < 0) return;
    // left finger i <-> right finger i + (rw - lw): the two hands' bone lists run in the same order
    const int off = rw - lw;
    double dot[3] = { 0, 0, 0 };
    for (int i = lw + 1; i < 73 && i + off < 73; ++i) {
        if (!PoseDescendant(mesh, i, lw, 73) || !PoseDescendant(mesh, i + off, rw, 73)) continue;
        const VIk3 a = g_poseRefLocal[i].t, b = g_poseRefLocal[i + off].t;
        dot[0] += a.x * b.x; dot[1] += a.y * b.y; dot[2] += a.z * b.z;
    }
    int flip = -1, flips = 0;
    for (int k = 0; k < 3; ++k) if (dot[k] < 0) { flip = k; ++flips; }
    if (flips != 1) {
        g_gripLeftState = -1;
        char l[200]; sprintf_s(l, "[OLVR][DOORGRIP] left hand: rig not a one-axis mirror (%.0f %.0f %.0f) - left hand stays open", dot[0], dot[1], dot[2]);
        OLProxyLog(l); return;
    }
    int n = 0;
    for (int i = lw + 1; i < 73 && i + off < 73; ++i) {
        g_gripLeftHas[i] = false;
        const int j = i + off;
        if (!g_gripHas[j] || j == g_rAux || !PoseDescendant(mesh, i, lw, 73)) continue;
        IkAtom m = g_gripLocal[j];
        float* q = &m.q.x; float* t = &m.t.x;
        for (int k = 0; k < 3; ++k) { if (k != flip) q[k] = -q[k]; else t[k] = -t[k]; }
        g_gripLeft[i] = m; g_gripLeftHas[i] = true; ++n;
    }
    g_gripLeftState = 1;
    char l[160]; sprintf_s(l, "[OLVR][DOORGRIP] left-hand grip mirrored from the right across local axis %d (%d bones)", flip, n);
    OLProxyLog(l);
}
// The camcorder grip is a flat hand inside the strap; round a door handle the fingers close further.
// Every finger joint of this rig bends about its own local -Z (the captured grip is pure Z there), and
// the left hand's mirror keeps that axis, so one table of extra bend serves both hands:
// [chain][joint] degrees, chain 0 = thumb, 1-4 = index..little; joint 0 = knuckle.
static float g_doorCurl[2][3] = { { 0.0f, 18.0f, 22.0f }, { 38.0f, 48.0f, 32.0f } };
extern "C" void OLUE3_DevCurl(float t0, float t1, float t2, float f0, float f1, float f2) {
    g_doorCurl[0][0] = t0; g_doorCurl[0][1] = t1; g_doorCurl[0][2] = t2;
    g_doorCurl[1][0] = f0; g_doorCurl[1][1] = f1; g_doorCurl[1][2] = f2;
}
// Close hand `h` round a handle, `w` of the way. Fingers only (not the camcorder bone).
static void GripFingers(IkArray& space, void* mesh, int h, float w) {
    const int wrist = g_poseBone[h][2];
    if (w <= 0.0f || wrist < 0 || !g_gripValid) return;
    if (h == 0) { GripBuildLeft(mesh); if (g_gripLeftState != 1) return; }
    const IkAtom* grip = h ? g_gripLocal : g_gripLeft;
    const bool* has = h ? g_gripHas : g_gripLeftHas;
    const int aux = h ? g_rAux : g_lAux;
    IkAtom loc[73]; bool sub[73] = {};
    for (int i = 0; i < space.n && i < 73; ++i) {
        if (i == wrist || i == aux || !PoseDescendant(mesh, i, wrist, space.n)) continue;
        const int p = PoseParent(mesh, i);
        if (p < 0 || p >= space.n) continue;
        loc[i] = IkLocalOf(space, i, p); sub[i] = true;
        if (has[i]) {
            const int k = i - (wrist + 1);
            const int chain = k / 3, joint = k % 3;
            QIk4 q = grip[i].q;
            if (chain >= 0 && chain <= 4) {
                const float deg = g_doorCurl[chain == 0 ? 0 : 1][joint];
                if (deg != 0.0f) q = IQMul(q, IQAxis({ 0.0f, 0.0f, -1.0f }, deg * 0.0174532925f));
            }
            loc[i].q = IQBlend(loc[i].q, q, w);
            loc[i].t = IVAdd(loc[i].t, IVScale(IVSub(grip[i].t, loc[i].t), w));
        }
    }
    for (int i = 0; i < space.n && i < 73; ++i) if (sub[i]) space.d[i] = IkCompose(space.d[PoseParent(mesh, i)], loc[i]);
}

// Retarget runtime finger curls to Miles's existing three-joint chains. Object grips and
// authored scenes retain priority; never write wrist/auxiliary bones or change bone lengths.
static void TrackedFingers(IkArray& space, void* mesh, int h, const OLXrPad& pad, float weight) {
    if (!g_poseRefValid || !pad.fingerValid[h] || !(pad.indexHands & (1u << h))) return;
    const int wrist = g_poseBone[h][2];
    if (wrist < 0 || wrist + 15 >= space.n || wrist + 15 >= 73) return;
    const float fingerDegrees[3] = { 65, 85, 60 };
    const float thumbDegrees[3] = { 10, 40, 50 };
    for (int f = 0; f < 5; ++f) {
        if (!(pad.fingerValid[h] & (1u << f))) continue;
        // Capture locals before composing the chain, so the live parent is never mixed
        // with the previous frame's child when applying the blend.
        IkAtom local[3]; bool valid = true;
        for (int j = 0; j < 3; ++j) {
            const int bone = wrist + 1 + f * 3 + j, parent = PoseParent(mesh, bone);
            if (parent != (j ? bone - 1 : wrist)) { valid = false; break; }
            local[j] = IkLocalOf(space, bone, parent);
        }
        if (!valid) continue;
        for (int j = 0; j < 3; ++j) {
            const int bone = wrist + 1 + f * 3 + j;
            const float degrees = (f == 0 ? thumbDegrees[j] : fingerDegrees[j]) * pad.fingerCurl[h][f];
            const QIk4 q = IQMul(g_poseRefLocal[bone].q, IQAxis({0, 0, -1}, degrees * 0.0174532925f));
            local[j].q = IQBlend(local[j].q, q, weight);
            space.d[bone] = IkCompose(space.d[PoseParent(mesh, bone)], local[j]);
        }
    }
}

extern "C" void OLUE3_DevGripDump() {
    char l[240];
    sprintf_s(l, "[OLVR][NATIVEGRIP] grip valid=%d live=%d aux=%d: q(%.5f %.5f %.5f %.5f) t(%.3f %.3f %.3f)", g_gripValid, g_gripLive, g_rAux,
              g_gripAux.q.x, g_gripAux.q.y, g_gripAux.q.z, g_gripAux.q.w, g_gripAux.t.x, g_gripAux.t.y, g_gripAux.t.z);
    OLProxyLog(l);
    for (int i = 0; i < 73; ++i) if (g_gripHas[i]) {
        sprintf_s(l, "[OLVR][NATIVEGRIP]   { %d, %.5ff, %.5ff, %.5ff, %.5ff, %.3ff, %.3ff, %.3ff },", i,
                  g_gripLocal[i].q.x, g_gripLocal[i].q.y, g_gripLocal[i].q.z, g_gripLocal[i].q.w, g_gripLocal[i].t.x, g_gripLocal[i].t.y, g_gripLocal[i].t.z);
        OLProxyLog(l);
    }
}
extern "C" void OLUE3_DevBones() {
    void* mesh = g_poseMesh; void* comp = g_milesComp;
    if (!mesh) { OLProxyLog("[OLVR][DEVBONES] Miles's mesh not seen yet"); return; }
    const char* refs = (const char*)ProbeReadPtr((char*)mesh + 0xC4);
    int n = 0; BP_Read((char*)mesh + 0xCC, &n, 4);
    IkArray space = {}; if (comp) BP_Read((char*)comp + 0x2D8, &space, sizeof(space));
    char l[240];
    for (int i = 0; refs && i < n && i < 256; ++i) {
        uint32_t ni = 0; char nm[64] = "?";
        BP_Read(refs + (size_t)i * 0x50, &ni, 4); BP_Name(ni, nm, sizeof(nm));
        IkAtom a = {}; if (space.d && i < space.n) BP_Read(space.d + i, &a, sizeof(a));
        sprintf_s(l, "[OLVR][DEVBONES] %2d %-28s parent %2d  at (%.1f %.1f %.1f)", i, nm, PoseParent(mesh, i), a.t.x, a.t.y, a.t.z);
        OLProxyLog(l);
    }
}

static bool CamHidden(void* m) { uint32_t w = 0; return BP_Read((char*)m + kPrimHiddenWordOff, &w, 4) ? (w & kPrimHiddenGameMask) != 0 : true; }
// [FLIPSCREEN] (Test43) The flip-out screen on the model: its centre from the camcorder's own origin
// (lens +X, right +Y, up +Z; uu), its turn about the camcorder's up (deg), and its size (m). The
// compositor (ol_xr.cpp) shows a live picture there. Tuned live with the dev "flipscr".
static float g_flipScr[6] = { 1.5f, -8.2f, 3.2f, 0.0f, 0.062f, 0.046f };   // matched to the model in headset captures
static int g_flipOn = 1;
extern "C" void OLUE3_SetFlipScreenOn(int on) { g_flipOn = on ? 1 : 0; }
// [LIVESCREEN] (Test46) where the camcorder's lens is in the WORLD this frame (CamPlace, game thread) - the
// capture camera is put there. Lens offset in the camcorder model's frame (+x lens, +y right, +z up), uu.
static float g_lsLensOff[3] = { 12.0f, 0.0f, 3.0f };
static float g_lsLensPos[3] = { 0, 0, 0 };
static int   g_lsLensRot[3] = { 0, 0, 0 };
static ULONGLONG g_lsLensMs = 0;
static bool  g_lsInHand = false;
static volatile long g_lsLive = 0;                 // the lens capture is built (the overlay shows it - [LENSCAM] ol_xr.cpp)
static ULONGLONG g_lsSeenMs = 0;                   // the flip screen last faced your eyes
static bool CompMatrix(void* comp, float m[16]);
static bool CompToWorld(void* comp, VIk3 p, float w[3]);
extern "C" void OLUE3_DevFlipScreen(const float v[6]) {
    for (int i = 0; i < 6; ++i) g_flipScr[i] = v[i];
    char l[200]; sprintf_s(l, "[OLVR][FLIPSCREEN] (dev) screen at (%.1f %.1f %.1f) uu, turned %.0f deg, %.3f x %.3f m", v[0], v[1], v[2], v[3], v[4], v[5]);
    OLProxyLog(l);
}
static QIk4 IQFromAxes(VIk3 x, VIk3 y, VIk3 z) {   // the rotation whose columns are x, y, z (orthonormal)
    const float tr = x.x + y.y + z.z;
    QIk4 q;
    if (tr > 0.0f) { const float s = sqrtf(tr + 1.0f) * 2.0f; q = { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s }; }
    else if (x.x > y.y && x.x > z.z) { const float s = sqrtf(1.0f + x.x - y.y - z.z) * 2.0f; q = { 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s }; }
    else if (y.y > z.z) { const float s = sqrtf(1.0f + y.y - x.x - z.z) * 2.0f; q = { (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s }; }
    else { const float s = sqrtf(1.0f + z.z - x.x - y.y) * 2.0f; q = { (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s }; }
    return IQNorm(q);
}
static void CamRestore() {
    { OLFlipScreen off = {}; OLXR_SetFlipScreen(&off); }      // [FLIPSCREEN]
    if (!g_camVisible && !g_camEntry[0] && !g_camEntry[1]) return;
    void* hero = OLUE3_Hero();
    void* camMesh = hero ? ProbeReadPtr((char*)hero + kHeroCameraMeshOff) : nullptr;
    if (g_camVisible && camMesh && camMesh == g_camMeshSeen && !CamHidden(camMesh)) OLUE3_SetHiddenGame(camMesh, 1);
    g_camVisible = false;
    for (int i = 0; i < 2; ++i) {
        if (!g_camEntry[i]) continue;
        __try {
            if (g_camEntry[i]->comp == g_camEntryComp[i]) {
                memset(g_camEntry[i]->loc, 0, sizeof(g_camEntry[i]->loc));
                memset(g_camEntry[i]->rot, 0, sizeof(g_camEntry[i]->rot));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_camEntry[i] = nullptr; g_camEntryComp[i] = nullptr;
    }
    OLProxyLog("[OLVR][HOLSTER] camcorder model handed back to the game (hidden, offsets cleared)");
}
// `comp` is one of Miles's two body meshes (the visible one carries CameraMesh, the hidden shadow body
// the shadow twin). headRef/headRaw/grip: exactly what the arm solve used this frame.
static void CamPlace(void* comp, const IkArray& space, void* mesh, VIk3 cam, VIk3 fwd, VIk3 right,
                     const float headRef[3], const float headRaw[3], const OLXrPose& grip, bool gripOk) {
    OLCamVisual v;
    if (!OLInteract_CamVisual(&v) || v.mode == 0) { CamRestore(); return; }
    void* hero = OLUE3_Hero();
    if (!hero) return;
    void* camMesh = ProbeReadPtr((char*)hero + kHeroCameraMeshOff);
    void* camShadow = ProbeReadPtr((char*)hero + kHeroCameraShadowOff);
    if (!camMesh) return;
    UEAttachArray at = {};
    if (!BP_Read((char*)comp + kCompAttachmentsOff, &at, sizeof(at)) || !at.d || at.n <= 0 || at.n > 64) return;
    UEAttachment* a = nullptr; int which = -1;
    for (int i = 0; i < at.n; ++i) {
        void* c = at.d[i].comp;
        if (c == camMesh) { a = &at.d[i]; which = 0; break; }
        if (camShadow && c == camShadow) { a = &at.d[i]; which = 1; break; }
    }
    if (!a) return;
    // the bone the model hangs from, by name (cached per mesh asset)
    static void* s_boneMesh = nullptr; static uint32_t s_boneName = 0; static int s_bone = -1;
    if (s_boneMesh != mesh || s_boneName != a->boneName) {
        s_boneMesh = mesh; s_boneName = a->boneName; s_bone = -1;
        const char* refs = (const char*)ProbeReadPtr((char*)mesh + 0xC4);
        int n = 0; BP_Read((char*)mesh + 0xCC, &n, 4);
        for (int i = 0; refs && i < n && i < 256; ++i) {
            uint32_t ni = 0;
            if (BP_Read(refs + (size_t)i * 0x50, &ni, 4) && ni == a->boneName) { s_bone = i; break; }
        }
        char bn[64] = "?", l[200]; BP_Name(a->boneName, bn, sizeof(bn));
        sprintf_s(l, "[OLVR][HOLSTER] handycam hangs from bone %d (%s); the mod places it from here on", s_bone, bn);
        OLProxyLog(l);
    }
    if (s_bone < 0 || s_bone >= space.n) return;
    if (which == 0) {
        g_camMeshSeen = camMesh;
        if (v.mode == 2) {                                      // at your eye
            if (g_camVisible && !CamHidden(camMesh)) OLUE3_SetHiddenGame(camMesh, 1);
            g_camVisible = false;
            return;
        }
        if (CamHidden(camMesh) && OLUE3_SetHiddenGame(camMesh, 0)) {
            if (!g_camVisible) OLProxyLog("[OLVR][HOLSTER] camcorder model shown");
        }
        g_camVisible = true;
    } else if (v.mode == 2) return;
    // the two targets, in Miles's component space
    const VIk3 up = { 0, 0, 1 };
    auto map = [&](float px, float py, float pz) -> VIk3 {
        const float dR = (px - headRef[0]) * kMetersToUU, dU = (py - headRef[1]) * kMetersToUU, dF = -(pz - headRef[2]) * kMetersToUU;
        VIk3 t = IVAdd(cam, IVAdd(IVScale(right, dR + g_bobSF[0]), IVAdd(IVScale(up, dU), IVScale(fwd, dF + g_bobSF[1]))));
        t.z += g_bobCorrZ;
        return t;
    };
    const QIk4 heldRot = IQEulerDeg(g_camHeldRot);
    // Holstered: on Miles's own right hip, hung from his right thigh joint, so looking down it is on
    // him. (Outlast's camera bone sits well in front of his body: the point at YOUR hip, where you
    // reach, maps in front of his. ol_interact accepts a grab at either place; the glide covers it.)
    GripBones(mesh);
    VIk3 chestT;
    if (g_hipBone >= 0 && g_hipBone < space.n) {
        chestT = IVAdd(space.d[g_hipBone].t, IVAdd(IVScale(up, g_camChestOff[0]), IVAdd(IVScale(fwd, g_camChestOff[1]), IVScale(right, g_camChestOff[2]))));
    } else {
        chestT = map(headRaw[0] + v.holsterRel[0], headRaw[1] + v.holsterRel[1], headRaw[2] + v.holsterRel[2]);
    }
    {   // back into the recenter frame, for the glow and the grab
        const VIk3 d = IVSub(chestT, cam);
        g_camChestTrack[0] = headRef[0] + (IVDot(d, right) - g_bobSF[0]) / kMetersToUU;
        g_camChestTrack[1] = headRef[1] + (d.z - g_bobCorrZ) / kMetersToUU;
        g_camChestTrack[2] = headRef[2] - (IVDot(d, fwd) - g_bobSF[1]) / kMetersToUU;
        g_camChestMs = GetTickCount64();
    }
    const QIk4 chestQ = IQMul(IQMul(PoseUprightHand(fwd), IQEulerDeg(g_camChestRot)), heldRot);
    VIk3 handT = chestT; QIk4 handQ = chestQ;
    if (g_gripValid && s_bone == g_rAux) {
        // [NATIVEGRIP] in the hand exactly where the game's own grip holds it: no offset from the bone
        // ([CAMHOLD] plus the in-hand fine turn, camtune 0)
        handT = space.d[s_bone].t; handQ = g_camHoldMode == 1 ? IQMul(space.d[s_bone].q, heldRot) : space.d[s_bone].q;
    } else if (gripOk) {
        const QIk4 gq = IQMul(PoseUprightHand(fwd), PoseGripCanonical(grip));
        handT = IVAdd(map(grip.px, grip.py, grip.pz), IQRotate(gq, { g_camHeldOff[0], g_camHeldOff[1], g_camHeldOff[2] }));
        handQ = IQMul(gq, heldRot);
    }
    const float b = v.blend < 0.0f ? 0.0f : (v.blend > 1.0f ? 1.0f : v.blend);
    const VIk3 T = IVAdd(chestT, IVScale(IVSub(handT, chestT), b));
    const QIk4 Q = IQBlend(chestQ, handQ, b);
    if (which == 0) {
        // [FLIPSCREEN] (Test43) the screen's pose and the lens, in the recenter frame, for the compositor
        OLFlipScreen fsc = {};
        {   // [LIVESCREEN] the lens in the world, for the capture camera
            g_lsInHand = b > 0.9f;
            float m[16], w[3];
            const VIk3 ax = IQRotate(Q, { 1, 0, 0 }), ay = IQRotate(Q, { 0, 1, 0 }), az = IQRotate(Q, { 0, 0, 1 });
            const VIk3 lc = IVAdd(T, IVAdd(IVScale(ax, g_lsLensOff[0]), IVAdd(IVScale(ay, g_lsLensOff[1]), IVScale(az, g_lsLensOff[2]))));
            if (g_lsInHand && CompMatrix(comp, m) && CompToWorld(comp, lc, w)) {
                auto dirW = [&](VIk3 d) -> VIk3 { return IVNorm({ d.x * m[0] + d.y * m[4] + d.z * m[8], d.x * m[1] + d.y * m[5] + d.z * m[9], d.x * m[2] + d.y * m[6] + d.z * m[10] }); };
                const VIk3 X = dirW(ax), Y = dirW(ay), Z = dirW(az);
                const float pitch = atan2f(X.z, sqrtf(X.x * X.x + X.y * X.y)), yaw = atan2f(X.y, X.x);
                const VIk3 sy = { -sinf(yaw), cosf(yaw), 0.0f };
                const float roll = atan2f(IVDot(Z, sy), IVDot(Y, sy)), k = 32768.0f / 3.14159265f;
                g_lsLensPos[0] = w[0]; g_lsLensPos[1] = w[1]; g_lsLensPos[2] = w[2];
                g_lsLensRot[0] = (int)lroundf(pitch * k); g_lsLensRot[1] = (int)lroundf(yaw * k); g_lsLensRot[2] = (int)lroundf(roll * k);
                g_lsLensMs = GetTickCount64();
            }
        }
        if (g_flipOn && b > 0.85f) {
            const VIk3 ax = IQRotate(Q, { 1, 0, 0 }), ay = IQRotate(Q, { 0, 1, 0 }), az = IQRotate(Q, { 0, 0, 1 });
            const VIk3 sc = IVAdd(T, IVAdd(IVScale(ax, g_flipScr[0]), IVAdd(IVScale(ay, g_flipScr[1]), IVScale(az, g_flipScr[2]))));
            auto dirT = [&](VIk3 d) -> VIk3 { return IVNorm({ IVDot(d, right), d.z, -IVDot(d, fwd) }); };
            const VIk3 dp = IVSub(sc, cam);
            fsc.pos[0] = headRef[0] + (IVDot(dp, right) - g_bobSF[0]) / kMetersToUU;
            fsc.pos[1] = headRef[1] + (dp.z - g_bobCorrZ) / kMetersToUU;
            fsc.pos[2] = headRef[2] - (IVDot(dp, fwd) - g_bobSF[1]) / kMetersToUU;
            const VIk3 lens = dirT(ax), up = dirT(az), side = dirT(IVScale(ay, -1.0f));
            const float yr = g_flipScr[3] * 0.0174533f;
            VIk3 n = IVNorm(IVAdd(IVScale(lens, -cosf(yr)), IVScale(side, sinf(yr))));      // the screen faces back, turned toward its side
            VIk3 rr = IVNorm(IVCross(up, n)); const VIk3 uu = IVCross(n, rr);
            const QIk4 qs = IQFromAxes(rr, uu, n);
            fsc.q[0] = qs.x; fsc.q[1] = qs.y; fsc.q[2] = qs.z; fsc.q[3] = qs.w;
            fsc.w = g_flipScr[4]; fsc.h = g_flipScr[5];
            fsc.lens[0] = lens.x; fsc.lens[1] = lens.y; fsc.lens[2] = lens.z;
            const VIk3 toEye = { headRaw[0] - fsc.pos[0], headRaw[1] - fsc.pos[1], headRaw[2] - fsc.pos[2] };
            fsc.on = IVDot(n, toEye) > 0.0f ? 1 : 0;                                      // only from its front
            if (fsc.on) g_lsSeenMs = GetTickCount64();                                    // [LIVESCREEN] worth capturing
            fsc.nv = OLInteract_HandNV();                                                 // [HANDNV] night vision or colour
        }
        OLXR_SetFlipScreen(&fsc);
        // [CAMADJUST] where it sits, once each time it comes to the hand (and after every adjustment)
        static bool s_inHand = false;
        const bool inHand = b > 0.9f;
        if (inHand && (!s_inHand || g_camAdjLog)) {
            g_camAdjLog = 0;
            const VIk3 ax = IQRotate(Q, { 1, 0, 0 }), az = IQRotate(Q, { 0, 0, 1 }), d = IVSub(T, cam);
            char l[260];
            sprintf_s(l, "[OLVR][CAMADJUST] in your hand: lens points %.2f ahead, %+.2f right, %+.2f up; its top %+.2f up; it sits %.0f cm ahead, %+.0f right, %+.0f up of your eyes",
                      IVDot(ax, fwd), IVDot(ax, right), ax.z, az.z, IVDot(d, fwd), IVDot(d, right), d.z);
            OLProxyLog(l);
        }
        s_inHand = inHand;
    }
    // ... as an offset from the bone: loc = B^-1 (T - B.t) / scale, rot = B^-1 Q
    const IkAtom& B = space.d[s_bone];
    const QIk4 inv = IQConj(B.q);
    const float s = fabsf(B.scale) > 1e-4f ? B.scale : 1.0f;
    const VIk3 rl = IVScale(IQRotate(inv, IVSub(T, B.t)), 1.0f / s);
    int32_t rr[3]; IQToRotator(IQMul(inv, Q), rr);
    __try {
        a->loc[0] = rl.x; a->loc[1] = rl.y; a->loc[2] = rl.z;
        a->rot[0] = rr[0]; a->rot[1] = rr[1]; a->rot[2] = rr[2];
        g_camEntry[which] = a; g_camEntryComp[which] = a->comp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static ULONGLONG s_logMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (which == 0 && now - s_logMs > 5000) {
        s_logMs = now;
        char l[260];
        sprintf_s(l, "[OLVR][HOLSTER] model blend %.2f: offset (%.1f %.1f %.1f) uu rot (%d %d %d) from bone %d",
                  b, rl.x, rl.y, rl.z, rr[0], rr[1], rr[2], s_bone);
        OLProxyLog(l);
    }
}

// [ANCHOR] (Test40) World points <-> Miles's component space, through the component's LocalToWorld
// (UPrimitiveComponent: rows X Y Z T at +0xC0; the door hinge reads the same matrix's T at +0xF0).
static bool CompMatrix(void* comp, float m[16]) {
    __try { memcpy(m, (const char*)comp + 0xC0, 16 * sizeof(float)); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < 3; ++i) {
        const float* a = m + i * 4; const float l2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
        if (!(l2 > 1e-4f && l2 < 1e4f)) return false;
    }
    return true;
}
static bool WorldToComp(void* comp, const float w[3], VIk3* out) {
    float m[16]; if (!CompMatrix(comp, m)) return false;
    const float d[3] = { w[0] - m[12], w[1] - m[13], w[2] - m[14] };
    float r[3];
    for (int i = 0; i < 3; ++i) {
        const float* a = m + i * 4;
        r[i] = (d[0] * a[0] + d[1] * a[1] + d[2] * a[2]) / (a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    }
    out->x = r[0]; out->y = r[1]; out->z = r[2];
    return true;
}
static bool CompToWorld(void* comp, VIk3 p, float w[3]) {
    float m[16]; if (!CompMatrix(comp, m)) return false;
    for (int k = 0; k < 3; ++k) w[k] = p.x * m[k] + p.y * m[4 + k] + p.z * m[8 + k] + m[12 + k];
    return true;
}
// [ANCHOR] (Test40) Miles's camera bone in the world, for the interaction tick's own check
static volatile float g_camBoneWorld[3] = { 0, 0, 0 };
static volatile ULONGLONG g_camBoneWorldMs = 0;
extern "C" int OLUE3_CameraBoneWorld(float out[3]) {
    if (!g_camBoneWorldMs || GetTickCount64() - g_camBoneWorldMs > 500) return 0;
    out[0] = g_camBoneWorld[0]; out[1] = g_camBoneWorld[1]; out[2] = g_camBoneWorld[2];
    return 1;
}

static bool PoseApplyVisible(void* comp) {
    const bool screen=OLXR_ScreenActive()!=0;
    const bool controls=OLXR_MotionControlsReady()!=0;
    if (!g_armDrive || !g_poseHookOk || g_poseNativeHold > 0 || g_nativeNarrowFrames > 0 ||
        screen || g_injW < 0.95f || !controls || ArmMoveClass() == 2 || SceneWantsTheater()) {
        static long blocked=0; const long count=InterlockedIncrement(&blocked);
        if (g_armDrive && (count==1 || (count%600)==0)) {
            char l[240]; sprintf_s(l,"[OLVR][VISIBLEIK] blocked: hook=%ld nativeHold=%ld narrow=%ld screen=%d inj=%.2f controls=%d calls=%ld",
                g_poseHookOk,g_poseNativeHold,g_nativeNarrowFrames,screen?1:0,g_injW,controls?1:0,count);
            OLProxyLog(l);
        }
        g_poseRamp[0] = g_poseRamp[1] = 0.0f;
        CamRestore();   // [HOLSTER] the game's own camcorder state while the arms are blocked
        return false;
    }
    OLXR_FrameLatch();   // [FRAMETAG] the arms use the same frozen head pose as this frame's camera
    OLXrPad pad = {};
    const int padOk = OLXR_GetPad(&pad);
    if (!padOk || !pad.handValid) { IkNote(padOk ? 1 : 0, nullptr); return false; }
    __try {
        void* mesh = *(void**)((char*)comp + 0x238);
        if (!PoseResolveBones(mesh)) { IkNote(2, mesh); return false; }
        IkArray space = *(IkArray*)((char*)comp + 0x2D8);
        if (!space.d || space.n != 73) { IkNote(3, mesh); return false; }
        g_milesComp = comp; InterlockedExchange(&g_milesCompFresh, 30);   // [PHYSCROUCH]

        // [ARMTUNE] Time since the last Miles solve, as a count of 60 fps frames. Measured here,
        // after the mesh check, because every skeletal component in the level passes through
        // the SkelUpdate hook. A long gap means the arms were blocked (menu, cutscene, screen)
        // and the ramp restarts from zero anyway, so it counts as one frame.
        static LARGE_INTEGER s_qpf = {}, s_lastSolve = {};
        LARGE_INTEGER nowQpc; QueryPerformanceCounter(&nowQpc);
        if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
        float dt = s_lastSolve.QuadPart
            ? (float)((double)(nowQpc.QuadPart - s_lastSolve.QuadPart) / (double)s_qpf.QuadPart)
            : (1.0f / 60.0f);
        s_lastSolve = nowQpc;
        if (dt > 0.25f) dt = 1.0f / 60.0f;
        if (dt < 0.002f) dt = 0.002f; else if (dt > 0.066f) dt = 0.066f;
        const float frames = g_armSmoothDt ? dt * 60.0f : 1.0f;

        // [ARMTUNE] HandSync: the hands sampled for the same instant as the frozen head pose.
        OLXrFrameHands frameHands = {};
        const bool useFrameHands = g_armHandSync && OLXR_GetFrameHands(&frameHands) && frameHands.valid;
        if (useFrameHands) {
            pad.handPose[0] = frameHands.hand[0]; pad.handPose[1] = frameHands.hand[1];
            pad.handValid = frameHands.valid;
        }
        // [HOLSTER] the right controller as the arm solve sees it, before the eye rule below drops it
        const OLXrPose camGrip = pad.handPose[1];
        const bool camGripOk = (pad.handValid & 2u) != 0;
        // [NATIVEGRIP] how much the right hand is holding the camcorder (0 holstered .. 1 in the hand)
        OLCamVisual camVis = {};
        const float camHold = (OLInteract_CamVisual(&camVis) && camVis.mode == 1) ? camVis.blend : 0.0f;
        GripBones(mesh);
        GripUseBake();
        // [INTERACT] (Test37) The camcorder held up to your eye: Miles's right arm plays the game's own
        // camcorder pose. Following the controller there put his sleeve right across the lens.
        // (Beta 1.1) [CAMTUCK] But that pose holds a camcorder that is hidden at the eye, so in the headset's wide
        // view the EMPTY hand hung beside the picture: "the hand I put it there with sticks out during the
        // camera view". While you look through it the right arm now rests at Miles's side instead.
        const bool camTuck = OLInteract_RightArmNative() != 0;
        // [CLIMB] (Test40) in a climb, a hand that holds on (the edge, a rung) plays the game's own arm
        const int moveClass = ArmMoveClass();
        if (moveClass == 1)
            for (int h = 0; h < 2; ++h) if (OLInteract_ClimbHold(h)) pad.handValid &= ~(1u << h);
        // [ARMTUNE] LeanFix: where the hand offset is measured FROM. Test16 used the clamped,
        // published head and no lean. The camera hook moved the view by the sanity-clamped
        // published head * LeanGain * injW, so the hands must be measured from the raw head
        // and then carried by that same amount: target = cam + (hand - rawHead + applied).
        float headRef[3] = { g_olHeadX, g_olHeadY, g_olHeadZ };
        float leanApplied[3] = { 0.0f, 0.0f, 0.0f };
        if (g_armLeanFix) {
            if (useFrameHands) {
                headRef[0] = frameHands.headRaw[0]; headRef[1] = frameHands.headRaw[1];
                headRef[2] = frameHands.headRaw[2];
            }
            ViewLeanM(leanApplied);
            for (int i = 0; i < 3; ++i) headRef[i] -= leanApplied[i];
        }

        if (camTuck) {   // [CAMTUCK] right hand low at the side, pointing down (recenter frame, metres)
            pad.handPose[1] = { -0.7071f, 0.0f, 0.0f, 0.7071f, headRef[0] + 0.20f, headRef[1] - 0.64f, headRef[2] + 0.05f };
            pad.handValid |= 2u;
        }
        if (!g_poseRefValid || !SameMilesBody(g_poseRefMesh, mesh)) {
            if (!PoseCaptureReference(space,mesh)) return false;
            g_poseRecenterSeen=OLXR_RecenterGeneration();
        }
        const long recenterGeneration=OLXR_RecenterGeneration();
        if (recenterGeneration!=g_poseRecenterSeen) {
            g_poseRecenterSeen=recenterGeneration;
            PoseResetTrackingCalibration();
            OLProxyLog("[OLVR][VISIBLEIK] controller smoothing reset after OpenXR recenter; O1 anatomical wrist calibration preserved");
        }

        // Save the engine pose before overriding the arm drivers.  Outlast 2's proven solver uses
        // the LIVE helper/finger relationships for skin deformation, while the actual clavicle,
        // upper arm, forearm and wrist are rebuilt from a stable reference.  Test 15 froze the
        // helper roots as well, which made the surface deformation look twisted and artificial.
        IkAtom nativePose[73]; memcpy(nativePose,space.d,sizeof(nativePose));
        static const int restoreBone[2][4] = {
            { 9,10,11,12 },
            { 33,34,35,36 }
        };
        for (int hand=0;hand<2;++hand) if (pad.handValid & (1u<<hand))
            for (int i=0;i<4;++i)
                if (!PoseApplyReferenceBone(space,mesh,restoreBone[hand][i])) return false;

        const VIk3 cam=space.d[g_poseCameraBone].t;
        {   // [ANCHOR] where the camera bone is in the world (logged against the view by ol_interact)
            float cw[3];
            if (CompToWorld(comp, cam, cw)) { g_camBoneWorld[0] = cw[0]; g_camBoneWorld[1] = cw[1]; g_camBoneWorld[2] = cw[2]; g_camBoneWorldMs = GetTickCount64(); }
        }
        VIk3 cameraForward=IQRotate(space.d[g_poseCameraBone].q,{1,0,0});
        cameraForward.z=0.0f; cameraForward=IVNorm(cameraForward);
        const VIk3 cameraRight={-cameraForward.y,cameraForward.x,0.0f};
        const VIk3 worldUp={0,0,1};

        // Calibrate the Quest grip axes against Outlast 1's own wrist rotations.  This follows the
        // successful absolute mapping used in Outlast 2, but the offsets come from Miles's O1 rig
        // rather than copied O2 constants or the player's arbitrary first-frame controller angle.
        if (!g_poseWristOffsetValid) {
            VIk3 refForward=IQRotate(g_poseRefCameraQ,{1,0,0});
            refForward.z=0.0f; refForward=IVNorm(refForward);
            const QIk4 refBasis=PoseUprightHand(refForward);
            for (int hand=0;hand<2;++hand)
                g_poseWristOffset[hand]=IQMul(IQConj(refBasis),g_poseRefWristQ[hand]);
            g_poseWristOffsetValid=true;
            OLProxyLog("[OLVR][VISIBLEIK] Outlast 1 absolute grip-to-wrist axes calibrated from Miles reference pose");
        }
        const QIk4 liveBodyBasis=PoseUprightHand(cameraForward);
        bool changed[2] = { false,false };
        for (int hand=0; hand<2; ++hand) {
            if (!(pad.handValid & (1u << hand))) { g_poseRamp[hand] = 0.0f; continue; }
            const int up=g_poseBone[hand][0], fo=g_poseBone[hand][1], ha=g_poseBone[hand][2];
            const int clav=hand ? 33 : 9;
            if (clav<0 || up<0 || fo<0 || ha<0 ||
                !PoseDescendant(mesh,up,clav,space.n)) {
                static long badClav[2] = {0,0};
                if (InterlockedIncrement(&badClav[hand]) == 1) {
                    char l[190];
                    sprintf_s(l,"[OLVR][VISIBLEIK] custom shoulder rejected: hand=%d clav=%d upper=%d parent=%d",
                              hand,clav,up,PoseParent(mesh,up));
                    OLProxyLog(l);
                }
                continue;
            }
            g_poseRamp[hand]+=(1.0f-g_poseRamp[hand])*ArmPer(0.45f,frames);
            if (g_poseRamp[hand]>0.999f) g_poseRamp[hand]=1.0f;
            const float side=hand ? 1.0f : -1.0f;
            if ((g_armFix & 4) && g_poseTargetSmoothValid[hand]) {
                // [ARMFIX] the collarbone turns a little toward a raised or far reach (last frame's target)
                const VIk3 c=space.d[clav].t, s=space.d[up].t, T=g_poseTargetSmooth[hand];
                const float armLen=IVLen(IVSub(space.d[fo].t,s))+IVLen(IVSub(space.d[ha].t,space.d[fo].t));
                const VIk3 cs=IVSub(s,c), st=IVSub(T,s);
                if (armLen>16.0f && IVLen(cs)>1.0f) {
                    float up01=st.z/armLen; up01=up01<0.0f?0.0f:(up01>1.0f?1.0f:up01);
                    float far01=(IVLen(st)-0.8f*armLen)/(0.25f*armLen); far01=far01<0.0f?0.0f:(far01>1.0f?1.0f:far01);
                    const float w=0.45f*up01+0.25f*far01;
                    if (w>0.01f) {
                        const VIk3 want=IVSub(T,c);
                        QIk4 turn=IQFromTo(cs,want);
                        const float ang=2.0f*acosf(fminf(1.0f,fabsf(turn.w)));
                        const float cap=0.436f;                                   // 25 degrees at most
                        float share=w*g_poseRamp[hand];
                        if (ang*share>cap) share=cap/ang;
                        PoseTransformTree(space,mesh,clav,IQBlend({0,0,0,1},turn,share));
                    }
                }
            }
            const VIk3 shoulder=space.d[up].t, elbow0=space.d[fo].t, wrist0=space.d[ha].t;
            const float l1=IVLen(IVSub(elbow0,shoulder)), l2=IVLen(IVSub(wrist0,elbow0));
            if (l1 < 8.0f || l1 > 80.0f || l2 < 8.0f || l2 > 80.0f) continue;

            // Controller and head poses share one recentered tracking frame.  Place their
            // RELATIVE vector in Miles's CURRENT component-space camera basis.  Do not subtract
            // cumulative game yaw here: SpaceBases are component-local, so doing so counter-
            // rotated the hands during smooth turning in Test 15 instead of carrying them with
            // the body.  This is the same body-relative construction used by Outlast 2 VR.
            const float dR=(pad.handPose[hand].px-headRef[0])*kMetersToUU+g_bobSF[0];   // [BOB2] the steadied view
            const float dU=(pad.handPose[hand].py-headRef[1])*kMetersToUU;
            const float dF=-(pad.handPose[hand].pz-headRef[2])*kMetersToUU+g_bobSF[1];
            VIk3 rawTarget=IVAdd(cam,IVAdd(IVScale(cameraRight,dR),
                                     IVAdd(IVScale(worldUp,dU),
                                           IVScale(cameraForward,dF))));
            rawTarget.z+=g_bobCorrZ;   // [COMFORT] the view was moved off the bone by this much
            if (hand == 1 && camHold > 0.0f && (g_camAdjOff[0] != 0.0f || g_camAdjOff[1] != 0.0f || g_camAdjOff[2] != 0.0f)) {
                // [CAMADJUST] the player's own placement: the hand, and the camcorder in it, moved by this much
                const VIk3 o = IVAdd(IVScale(cameraForward, g_camAdjOff[0]), IVAdd(IVScale(worldUp, g_camAdjOff[1]), IVScale(cameraRight, g_camAdjOff[2])));
                rawTarget = IVAdd(rawTarget, IVScale(o, camHold));
            }
            float anchorW = 0.0f;
            {   // [ANCHOR] (Test40) a hand holding a door handle, a ledge or a rung reaches there
                float aw[3]; VIk3 ac;
                const float w = OLInteract_HandAnchorWorld(hand, aw);
                anchorW = w;
                if (w > 0.0f && WorldToComp(comp, aw, &ac)) rawTarget = IVAdd(rawTarget, IVScale(IVSub(ac, rawTarget), w > 1.0f ? 1.0f : w));
            }
            {   // [ARMRESET] (Beta 1.1) "the arm bends unnaturally holding a door and doesn't return": the elbow
                // side below is kept from frame to frame, and a handle swung beside or behind the body could
                // leave it on the wrong side for good. Letting go starts the elbow from its natural side again.
                static float s_prevAnchorW[2] = { 0.0f, 0.0f };
                if (s_prevAnchorW[hand] > 0.0f && anchorW <= 0.0f) g_posePoleSmoothValid[hand] = false;
                s_prevAnchorW[hand] = anchorW;
            }

            // Low-amplitude smoothing removes controller jitter, while a deliberate movement
            // quickly raises follow to 1.0.  The two-bone solver below still enforces the actual
            // measured upper+forearm reach, so no hand can stretch the mesh.
            if (!g_poseTargetSmoothValid[hand]) {
                g_poseTargetSmooth[hand]=rawTarget; g_poseTargetSmoothValid[hand]=true;
            } else {
                const VIk3 error=IVSub(rawTarget,g_poseTargetSmooth[hand]);
                float follow=0.45f+0.55f*(IVLen(error)/25.0f);
                if (follow>1.0f) follow=1.0f;
                follow=ArmPer(follow,frames);
                g_poseTargetSmooth[hand]=IVAdd(g_poseTargetSmooth[hand],IVScale(error,follow));
            }
            const VIk3 target=g_poseTargetSmooth[hand];

            // Use Miles's own reference elbow as the primary bend guide, just as the working O2
            // solver uses the authored elbow.  Blend in a small anatomical down/outward guide for
            // overhead and cross-body reaches, then keep the previous hemisphere to prevent flips.
            const VIk3 aim=IVNorm(IVSub(target,shoulder));
            VIk3 referenceBend=IVSub(elbow0,shoulder);
            referenceBend=IVSub(referenceBend,IVScale(aim,IVDot(referenceBend,aim)));
            VIk3 anatomical=IVAdd(IVScale(cameraRight,side*0.52f),
                                  IVAdd(IVScale(worldUp,-0.72f),
                                        IVScale(cameraForward,-0.10f)));
            anatomical=IVSub(anatomical,IVScale(aim,IVDot(anatomical,aim)));
            if (IVLen(referenceBend)<0.01f) referenceBend=anatomical;
            if (IVLen(anatomical)<0.01f) anatomical=referenceBend;
            const float crossBody=IVDot(aim,cameraRight)*side;
            const float anatomicalWeight=crossBody<0.0f ? 0.42f : 0.24f;
            VIk3 wantedPole=IVNorm(IVAdd(IVScale(IVNorm(referenceBend),1.0f-anatomicalWeight),
                                         IVScale(IVNorm(anatomical),anatomicalWeight)));
            if (!g_posePoleSmoothValid[hand]) {
                g_posePoleSmooth[hand]=wantedPole; g_posePoleSmoothValid[hand]=true;
            } else {
                // Keep the previous side against a flip - but only while that side is a natural one (down
                // and out, like the anatomical guide) or the hand is held somewhere. A stored side pointing
                // up or inward is a leftover (a handle behind the body, a cross-body reach): let it go.
                if (IVDot(wantedPole,g_posePoleSmooth[hand])<0.0f &&
                    (anchorW > 0.0f || IVDot(g_posePoleSmooth[hand],IVNorm(anatomical)) > 0.0f))
                    wantedPole=IVScale(wantedPole,-1.0f);
                const float poleT=ArmPer(0.35f,frames);
                const float poleKeep=(frames==1.0f)?0.65f:1.0f-poleT;   // exact Test16 constants
                VIk3 blended=IVAdd(IVScale(g_posePoleSmooth[hand],poleKeep),
                                    IVScale(wantedPole,poleT));
                if (IVLen(blended)>0.05f) g_posePoleSmooth[hand]=IVNorm(blended);
            }
            const VIk3 customPole=IVAdd(shoulder,IVScale(g_posePoleSmooth[hand],100.0f));
            VIk3 elbow{}, solvedWrist{};
            if (!PoseTwoBone(shoulder,target,customPole,l1,l2,elbow,solvedWrist)) continue;

            // Apply the solve to complete component-space descendant trees, never isolated
            // joints.  Segment lengths therefore remain exactly those measured from Miles.
            QIk4 upperDelta=IQFromTo(IVSub(elbow0,shoulder),IVSub(elbow,shoulder));
            if (g_armFix & 1) {
                // [ARMFIX] turn the upper arm so the elbow's hinge lies across the new bend
                const VIk3 n0=ArmHinge(shoulder,elbow0,wrist0,anatomical);
                const VIk3 n1=ArmHinge(shoulder,elbow,solvedWrist,IVSub(customPole,shoulder));
                if (IVLen(n0)>0.5f && IVLen(n1)>0.5f)
                    upperDelta=IQFromFrames(IVSub(elbow0,shoulder),n0,IVSub(elbow,shoulder),n1);
            }
            upperDelta=IQBlend({0,0,0,1},upperDelta,g_poseRamp[hand]);
            PoseTransformTree(space,mesh,up,upperDelta);

            const VIk3 elbowNow=space.d[fo].t, wristNow=space.d[ha].t;
            QIk4 foreDelta=IQFromTo(IVSub(wristNow,elbowNow),IVSub(solvedWrist,elbowNow));
            foreDelta=IQBlend({0,0,0,1},foreDelta,g_poseRamp[hand]);
            PoseTransformTree(space,mesh,fo,foreDelta);

            // Absolute controller orientation in the live body basis plus Miles's measured wrist
            // offset.  This removes Test 15's startup-angle dependency and turns smoothly with the
            // body while retaining the O2-style forearm twist distribution.
            QIk4 wristTarget=IQMul(IQMul(liveBodyBasis,
                                         PoseGripCanonical(pad.handPose[hand])),
                                   g_poseWristOffset[hand]);
            if (hand == 1 && camHold > 0.0f && g_gripValid && g_camHoldMode != 1) {
                // [NATIVEGRIP] turn the wrist so the camcorder in the game's grip points where the
                // controller points: camcorder = wrist * gripAux  =>  wrist = camcorder * gripAux^-1
                // ([CAMHOLD] 2: kept upright)
                QIk4 camQ = IQMul(IQMul(liveBodyBasis, PoseGripCanonical(pad.handPose[hand])), IQEulerDeg(g_camHeldRot));
                if (g_camHoldMode == 4 || g_camHoldMode == 5) { // [CAMHOLD] free hold: the lens where the controller POINTS ([CAMADJUST] aim pose)
                    const OLXrPose& aim = (pad.aimValid & 2u) ? pad.aimPose[1] : pad.handPose[hand];
                    QIk4 hold = IQMul(IQEulerDeg(g_camHold4), IQEulerDeg(g_camHeldRot));
                    if (g_camHoldMode == 5) {   // (Beta 1.1) real camcorder: turned over about the controller's left-right axis
                        static const float kOver[3] = { 0.0f, 180.0f, 0.0f };
                        hold = IQMul(IQEulerDeg(kOver), hold);
                    }
                    camQ = IQMul(IQMul(liveBodyBasis, PoseGripCanonical(aim)), hold);
                }
                if (g_camHoldMode == 2) camQ = CamUpright(camQ);
                if (g_camHoldMode == 3) {
                    // the way Miles faces, turned / tilted a little toward the hand (eye -> hand)
                    const VIk3 look = IVSub(target, cam);
                    const float hl = sqrtf(look.x * look.x + look.y * look.y);
                    if (hl > 1.0f) {
                        const VIk3 lh = { look.x / hl, look.y / hl, 0.0f };
                        const VIk3 h = IVNorm(IVAdd(IVScale(cameraForward, 0.75f), IVScale(lh, 0.25f)));
                        const float pitch = atan2f(look.z, hl) * g_camHold3[0];
                        const VIk3 f = IVNorm(IVAdd(h, { 0.0f, 0.0f, tanf(pitch) }));
                        const float roll[3] = { g_camHold3[1], 0.0f, 0.0f }, turn[3] = { 0.0f, 0.0f, g_camHold3[2] };
                        camQ = IQMul(IQMul(IQMul(CamUpright(IQFromTo({ 1, 0, 0 }, f)), IQEulerDeg(roll)), IQEulerDeg(turn)), IQEulerDeg(g_camHeldRot));
                    }
                }
                wristTarget = IQBlend(wristTarget, IQMul(camQ, IQConj(g_gripAux.q)), camHold);
            }
            PoseOrientArm(space,mesh,fo,ha,wristTarget,g_poseRamp[hand]);
            changed[hand]=true;
        }
        // [NATIVEGRIP] the camcorder up at your eye: the right arm is the game's - learn its grip
        {
            static ULONGLONG s_nativeSince = 0; static bool s_took = false;
            if (OLInteract_RightArmNative()) {
                const ULONGLONG nowMs = GetTickCount64();
                if (!s_nativeSince) { s_nativeSince = nowMs; s_took = false; }
                else if (!s_took && nowMs - s_nativeSince > 900) { GripCapture(space, mesh); s_took = true; }
            } else s_nativeSince = 0;
        }
        // Outlast 1's helper roots are sibling skinning chains.  Preserve their LIVE native
        // driver-relative shape, as the working Outlast 2 solver does, then attach that shape to
        // the solved arm.  Only the arm drivers themselves use the frozen reference.
        if (changed[0]) {
            PoseRebaseHelper(space,nativePose,mesh,28,12); // L hand auxiliary
            if ((g_armFix & 8) && g_helperRefOk) {         // [ARMFIX] steady twist helpers
                PoseRebaseHelperRef(space,mesh,29,11);
                PoseRebaseHelperRef(space,mesh,31,10);
            } else {
                PoseRebaseHelper(space,nativePose,mesh,29,11); // L forearm twist chain
                PoseRebaseHelper(space,nativePose,mesh,31,10); // L upper-arm twist chain
            }
        }
        if (changed[1]) {
            PoseRebaseHelper(space,nativePose,mesh,52,36); // R hand auxiliary
            if ((g_armFix & 8) && g_helperRefOk) {         // [ARMFIX] steady twist helpers
                PoseRebaseHelperRef(space,mesh,53,35);
                PoseRebaseHelperRef(space,mesh,55,34);
            } else {
                PoseRebaseHelper(space,nativePose,mesh,53,35); // R forearm twist chain
                PoseRebaseHelper(space,nativePose,mesh,55,34); // R upper-arm twist chain
            }
            if (camHold > 0.0f) GripApply(space, mesh, camHold);   // [NATIVEGRIP]
        }
        {   // [DOORGRIP] a hand holding a door handle
            float dg[2] = { 0.0f, 0.0f };
            OLInteract_HandGrip(dg);
            if (changed[0] && dg[0] <= 0.0f) TrackedFingers(space, mesh, 0, pad, g_poseRamp[0]);
            if (changed[1] && dg[1] <= 0.0f && camHold <= 0.0f) TrackedFingers(space, mesh, 1, pad, g_poseRamp[1]);
            if (changed[0] && dg[0] > 0.0f) GripFingers(space, mesh, 0, dg[0]);
            if (changed[1] && dg[1] > 0.0f && camHold <= 0.0f) GripFingers(space, mesh, 1, dg[1]);
        }
        // [HOLSTER] (Test38) place the game's handycam (hip, hand, or hidden at the eye) - after the
        // helpers, so the hand bone it hangs from is final (Test38 placed it before them)
        {
            float headRaw[3] = { g_olHeadX, g_olHeadY, g_olHeadZ };
            if (useFrameHands) { headRaw[0] = frameHands.headRaw[0]; headRaw[1] = frameHands.headRaw[1]; headRaw[2] = frameHands.headRaw[2]; }
            if (!g_armLeanFix) { headRaw[0] = headRef[0]; headRaw[1] = headRef[1]; headRaw[2] = headRef[2]; }
            CamPlace(comp, space, mesh, cam, cameraForward, cameraRight, headRef, headRaw, camGrip, camGripOk);
        }
        if (!changed[0] && !changed[1]) return false;
        const long n=InterlockedIncrement(&g_poseApplied);
        if (n==1 || (n%300)==0) {
            char l[300]; sprintf_s(l,"[OLVR][VISIBLEIK] body-relative O1 anatomical IK: calls=%ld hands=0x%X ramp=%.2f/%.2f cameraForward=(%.2f %.2f) wrists=%d/%d liveHelpers=1",
                                   n,pad.handValid,g_poseRamp[0],g_poseRamp[1],
                                   cameraForward.x,cameraForward.y,
                                   g_poseBone[0][2],g_poseBone[1][2]); OLProxyLog(l);
            char t[300]; sprintf_s(t,"[OLVR][ARMTUNE] handSync=%ld(frameHands=%d) leanFix=%ld lean=(%.3f %.3f %.3f)m smoothDt=%ld dt=%.1fms",
                                   g_armHandSync,useFrameHands?1:0,g_armLeanFix,
                                   leanApplied[0],leanApplied[1],leanApplied[2],
                                   g_armSmoothDt,dt*1000.0f); OLProxyLog(t);
        }
        IkNote(4, nullptr);   // [IKDIAG] solved
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_poseRecompose=0;
        return false;
    }
}

static void __fastcall Hook_PoseCompose(void* comp) {
    o_PoseCompose(comp);
    // This is an internal/partial composer and is skipped by some first-person update states.
    // It remains hooked only so PoseApplyVisible can safely ask the engine to rebuild children.
}

static void __fastcall Hook_SkelUpdate(void* comp, float deltaTime, int tickFaceFx) {
    o_SkelUpdate(comp, deltaTime, tickFaceFx);
    // [PHYSCROUCH] Remember Miles's component whatever the arm solver decides this frame.
    if (g_poseMesh) {
        __try {
            if (*(void**)((char*)comp + 0x238) == g_poseMesh) {
                g_milesComp = comp; InterlockedExchange(&g_milesCompFresh, 30);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!g_poseRecompose) PoseApplyVisible(comp);
}

// ---- [LOOKUSE] use what you look at (Test34) ------------------------------------------------------
// Outlast 1 decides what X can use from the hero's cached EyeForward (hero+0x2BD8). The engine
// rebuilds it from the Hero-Camera bone once per frame (RVA 0xD5D9F0), and the interaction scans
// that build the prompt list (pickups, beds, lockers, doors) all read it inside
// AOLPlayerController::NativePlayerMove (RVA 0xD90060, ctrl in rcx, DeltaTime in xmm1). The head
// only turns the RENDER camera, so until now the prompt followed Miles's body, not your eyes.
// For the length of that one call EyeForward is turned by exactly the head yaw/pitch the camera
// shows (or set to the right controller's pointing ray), then put back. Nothing integrates:
// next frame rebuilds it from the bone, so a missed restore heals itself, and no rotation the
// controller keeps is ever touched. (Outlast 2's GetViewRotation hook is deliberately NOT used:
// AOLPlayerController::Tick copies that into the control rotation every tick - a feedback loop.)
// Every write is gated on the hero's own back-pointer to this controller, normal locomotion
// (hero+0x7BC == 0, so lockers, beds and ledges are untouched) and a unit-length vector already
// sitting at the offset.
typedef void (__fastcall *NativePlayerMove_t)(void*, float);
static NativePlayerMove_t o_NativePlayerMove = nullptr;
static const uintptr_t kNativePlayerMoveRva = 0xD90060;
static const unsigned char kNativePlayerMovePrologue[15] = {
    0x40,0x55,0x53,0x48,0x8D,0x6C,0x24,0xB1,0x48,0x81,0xEC,0xE8,0x00,0x00,0x00
};
static volatile long g_lookUse = 0;        // 0 the game's own (body), 1 head, 2 right hand
static volatile long g_lookUseHookOk = 0;
extern "C" int  OLUE3_GetLookUse() { return (int)g_lookUse; }
extern "C" void OLUE3_SetLookUse(int v) { g_lookUse = v < 0 ? 0 : (v > 2 ? 2 : v); }
extern "C" int  OLUE3_LookUseReady() { return g_lookUseHookOk ? 1 : 0; }

// ---- [LIVESCREEN] (Test46) the camcorder's own flip screen, live ----------------------------------------
// The game's script functions, called by name through UObject::ProcessEvent (vtable +0x218 - read from the
// script event the FOV getter 0x60F4F0 dispatches: FindFunctionChecked 0xCE240, then call [vtbl+0x218]).
// Game thread only, from the player tick.
typedef void (__fastcall *ProcessEvent_t)(void* obj, void* fn, void* parms, void* result);
static void* LS_Outer(void* o) { return ProbeReadPtr((char*)o + g_bpUNameOff - 8); }
static void* LS_ClassOf(void* o) { return ProbeReadPtr((char*)o + g_bpUNameOff + 8); }
static bool  LS_NameIs(void* o, const char* s) { char nm[96]; return o && BP_ObjName(o, nm, sizeof(nm)) && !strcmp(nm, s); }
// The UFunction <fn> declared by class <cls> (an object called fn, of class Function, whose Outer is cls).
static void* LS_FindFunction(const char* cls, const char* fn) {
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || !LS_NameIs(o, fn)) continue;
        if (LS_NameIs(LS_ClassOf(o), "Function") && LS_NameIs(LS_Outer(o), cls)) return o;
    }
    return nullptr;
}
static void* LS_FindObject(const char* name) {
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr;
        if (BP_Read(g_bpObjs + i, &o, 8) && o && LS_NameIs(o, name)) return o;
    }
    return nullptr;
}
static int LS_ParamOff(void* fn, const char* p) { unsigned o = 0; return (fn && SP_FindPropIn(fn, p, &o, nullptr)) ? (int)o : -1; }
// UE3's ProcessEvent refuses a function that still has an old native index (UFunction.iNative, the WORD at
// +0xD4, after FunctionFlags +0xD0, before FriendlyName +0xD8): Actor.SetLocation (267) and SetRotation (299)
// did nothing at all - the lens camera stayed where it was spawned. The index is cleared for the call (game
// thread, the only one running script) and put back. Checked: FriendlyName equals the object's own name.
static const unsigned kFuncFlagsOff = 0xD0, kFuncNativeOff = 0xD4, kFuncFriendlyOff = 0xD8;
static bool LS_Call(void* obj, void* fn, void* parms) {
    if (!obj || !fn) return false;
    uint16_t* nat = nullptr; uint16_t was = 0;
    __try {
        if (*(uint32_t*)((char*)fn + kFuncFriendlyOff) == *(uint32_t*)((char*)fn + g_bpUNameOff) &&
            (*(uint32_t*)((char*)fn + kFuncFlagsOff) & 0x400) && (was = *(uint16_t*)((char*)fn + kFuncNativeOff)) != 0) {
            nat = (uint16_t*)((char*)fn + kFuncNativeOff); *nat = 0;
        }
        void** vt = *(void***)obj; ((ProcessEvent_t)vt[0x218 / 8])(obj, fn, parms, nullptr);
        if (nat) *nat = was;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { if (nat) *nat = was; return false; }
}
// a function's parameters, for the log
static void LS_LogParams(const char* cls, const char* fn) {
    char l[300], nm[64], kn[64];
    void* f = LS_FindFunction(cls, fn);
    int at = sprintf_s(l, "[OLVR][LIVESCREEN] %s.%s %s", cls, fn, f ? "found:" : "NOT FOUND");
    for (void* p = f ? ProbeReadPtr((char*)f + 0x80) : nullptr; p && at < 260; ) {
        uint32_t off = 0;
        if (!BP_ObjName(p, nm, sizeof(nm))) break;
        void* k = LS_ClassOf(p); if (!k || !BP_ObjName(k, kn, sizeof(kn))) kn[0] = 0;
        BP_Read((char*)p + 0x8C, &off, 4);
        if (strstr(kn, "Property")) at += sprintf_s(l + at, sizeof(l) - at, " %s(%.6s)@%X", nm, kn, off);
        void* nx = ProbeReadPtr((char*)p + 0x60); if (nx == p) break; p = nx;
    }
    OLProxyLog(l);
}
static void LS_LogMaterial(const char* what, void* m, int depth) {
    char l[300], nm[96] = "?", cn[64] = "?";
    if (!m) { sprintf_s(l, "[OLVR][LIVESCREEN]   %s: none", what); OLProxyLog(l); return; }
    BP_ObjName(m, nm, sizeof(nm)); BP_ObjName(LS_ClassOf(m), cn, sizeof(cn));
    sprintf_s(l, "[OLVR][LIVESCREEN]   %s: %s (%s) %p", what, nm, cn, m); OLProxyLog(l);
    unsigned po = 0, to = 0;
    if (strstr(cn, "MaterialInstance")) {
        if (SP_FindPropAny("MaterialInstanceConstant", "TextureParameterValues", &to, nullptr)) {
            struct { char* d; int n, mx; } ta = {};
            if (BP_Read((char*)m + to, &ta, 16) && ta.d && ta.n > 0 && ta.n < 64) {
                for (int i = 0; i < ta.n; ++i) {
                    uint32_t pn = 0; void* tex = nullptr; char pnm[64] = "?", tnm[96] = "-";
                    BP_Read(ta.d + i * 32, &pn, 4); BP_Read(ta.d + i * 32 + 8, &tex, 8);
                    BP_Name(pn, pnm, sizeof(pnm)); if (tex) BP_ObjName(tex, tnm, sizeof(tnm));
                    sprintf_s(l, "[OLVR][LIVESCREEN]     texture parameter '%s' = %s", pnm, tnm); OLProxyLog(l);
                }
            } else { sprintf_s(l, "[OLVR][LIVESCREEN]     no texture parameters (n=%d)", ta.n); OLProxyLog(l); }
        }
        unsigned so2 = 0;
        if (SP_FindPropAny("MaterialInstanceConstant", "ScalarParameterValues", &so2, nullptr)) {
            struct { char* d; int n, mx; } sa = {};
            if (BP_Read((char*)m + so2, &sa, 16) && sa.d && sa.n > 0 && sa.n < 64) {
                int at = sprintf_s(l, "[OLVR][LIVESCREEN]     scalars:");
                for (int i = 0; i < sa.n && at < 260; ++i) { uint32_t pn = 0; float v = 0; char pnm[48] = "?"; BP_Read(sa.d + i * 28, &pn, 4); BP_Read(sa.d + i * 28 + 8, &v, 4); BP_Name(pn, pnm, sizeof(pnm)); at += sprintf_s(l + at, sizeof(l) - at, " '%s'=%.2f", pnm, v); }
                OLProxyLog(l);
            }
        }
        if (depth < 3 && SP_FindPropAny("MaterialInstance", "Parent", &po, nullptr)) LS_LogMaterial("parent", ProbeReadPtr((char*)m + po), depth + 1);
    }
}
static void LS_Info() {
    BP_RefreshObjects();
    OLProxyLog("[OLVR][LIVESCREEN] ---- what the game offers ----");
    LS_LogParams("TextureRenderTarget2D", "Create");
    LS_LogParams("Actor", "Spawn");
    LS_LogParams("Actor", "SetLocation");
    LS_LogParams("Actor", "SetRotation");
    LS_LogParams("SceneCapture2DComponent", "SetCaptureParameters");
    LS_LogParams("SceneCapture2DComponent", "SetView");
    LS_LogParams("SceneCaptureComponent", "SetEnabled");
    LS_LogParams("SceneCaptureComponent", "SetFrameRate");
    LS_LogParams("MeshComponent", "CreateAndSetMaterialInstanceConstant");
    LS_LogParams("MeshComponent", "GetMaterial");
    LS_LogParams("MeshComponent", "SetMaterial");
    LS_LogParams("MaterialInstanceConstant", "SetTextureParameterValue");
    LS_LogParams("MaterialInstanceConstant", "SetParent");
    char l[260];
    const char* cls[] = { "SceneCapture2DActor", "SceneCaptureActor", "SceneCapture2DComponent", "TextureRenderTarget2D", "Default__TextureRenderTarget2D" };
    for (const char* c : cls) { void* o = LS_FindObject(c); sprintf_s(l, "[OLVR][LIVESCREEN] object %s: %p", c, o); OLProxyLog(l); }
    void* hero = OLUE3_Hero();
    void* cam = hero ? ProbeReadPtr((char*)hero + kHeroCameraMeshOff) : nullptr;
    char nm[96] = "?", cn[64] = "?";
    if (cam) { BP_ObjName(cam, nm, sizeof(nm)); BP_ObjName(LS_ClassOf(cam), cn, sizeof(cn)); }
    sprintf_s(l, "[OLVR][LIVESCREEN] the camcorder model: %s (%s) %p", nm, cn, cam); OLProxyLog(l);
    if (!cam) return;
    unsigned mo = 0, so = 0, smo = 0;
    if (SP_FindPropAny("MeshComponent", "Materials", &mo, nullptr)) {
        struct { void** d; int n, mx; } ta = {};
        BP_Read((char*)cam + mo, &ta, 16);
        sprintf_s(l, "[OLVR][LIVESCREEN] component material overrides: %d", ta.n); OLProxyLog(l);
        for (int i = 0; ta.d && i < ta.n && i < 16; ++i) { void* m = nullptr; BP_Read(ta.d + i, &m, 8); char w[32]; sprintf_s(w, "override %d", i); LS_LogMaterial(w, m, 0); }
    }
    if (SP_FindPropAny("SkeletalMeshComponent", "SkeletalMesh", &so, nullptr) && SP_FindPropAny("SkeletalMesh", "Materials", &smo, nullptr)) {
        void* sm = ProbeReadPtr((char*)cam + so);
        if (sm) BP_ObjName(sm, nm, sizeof(nm));
        struct { void** d; int n, mx; } ta = {};
        if (sm) BP_Read((char*)sm + smo, &ta, 16);
        sprintf_s(l, "[OLVR][LIVESCREEN] the mesh %s: %d materials", sm ? nm : "?", ta.n); OLProxyLog(l);
        for (int i = 0; ta.d && i < ta.n && i < 16; ++i) { void* m = nullptr; BP_Read(ta.d + i, &m, 8); char w[32]; sprintf_s(w, "element %d", i); LS_LogMaterial(w, m, 0); }
    }
    // texture parameter names in use anywhere: candidates for a screen
    unsigned to = 0;
    if (SP_FindPropAny("MaterialInstanceConstant", "TextureParameterValues", &to, nullptr)) {
        void* micCls = BP_FindClassObj("MaterialInstanceConstant");
        uint32_t seen[64]; int seenN = 0, mics = 0;
        for (int i = 0; i < g_bpObjNum && seenN < 64; ++i) {
            void* o = nullptr;
            if (!BP_Read(g_bpObjs + i, &o, 8) || !o || LS_ClassOf(o) != micCls) continue;
            ++mics;
            struct { char* d; int n, mx; } ta = {};
            if (!BP_Read((char*)o + to, &ta, 16) || !ta.d || ta.n <= 0 || ta.n > 32) continue;
            for (int j = 0; j < ta.n && seenN < 64; ++j) {
                uint32_t pn = 0; BP_Read(ta.d + j * 32, &pn, 4);
                bool have = false; for (int k = 0; k < seenN; ++k) if (seen[k] == pn) have = true;
                if (!have) {
                    seen[seenN++] = pn;
                    char pnm[64] = "?", on[96] = "?"; BP_Name(pn, pnm, sizeof(pnm)); BP_ObjName(o, on, sizeof(on));
                    sprintf_s(l, "[OLVR][LIVESCREEN] texture parameter '%s' (first seen on %s)", pnm, on); OLProxyLog(l);
                }
            }
        }
        sprintf_s(l, "[OLVR][LIVESCREEN] %d material instances scanned, %d distinct texture parameter names", mics, seenN); OLProxyLog(l);
    }
}
// The live screen: a render target, a scene-capture camera at the lens, and the flip screen's material
// (element 1, CameraScreen_INST) replaced by an instance of the game's own security-monitor screen
// material - the one with a 'CameraTexture' slot (SecurCamAdmin-01_Mat_INST) - fed the render target.
// Rendered only while the camcorder is in your hand, 30 times a second.
enum { LF_CREATE, LF_SPAWN, LF_SETCAP, LF_SETVIEW, LF_ENABLE, LF_RATE, LF_MICNEW, LF_GETMAT, LF_SETMAT, LF_SETTEX, LF_SETPARENT, LF_SETLOC, LF_SETROT, LF_N };
static void* g_lsFn[LF_N];
static bool  g_lsFnOk = false;
static unsigned g_lsCaptureOff = 0, g_lsActorLocOff = 0, g_lsTexParamsOff = 0, g_lsUpdOff = 0, g_lsUpdMask = 0;
static struct {
    int state;                    // 0 not built, 1 live, -1 failed (tried again after a while)
    void* hero; void* cam; void* rt; void* actor; void* comp; void* mic; void* orig;
    bool enabled; float fov; ULONGLONG retryAt, lastView;
} g_ls = {};
static volatile long g_lsWanted = 0;           // dev bridge "livescreen on" without the setting
extern "C" void OLUE3_SetLiveScreen(int on) { g_lsWanted = on ? 1 : 0; }
// [LENSCAM] 352x264: 4:3, and a size nothing else in the game uses - the proxy knows the lens's GPU texture by it
static const int kLsW = 352, kLsH = 264, kLsElement = 1;
extern "C" void OLProxy_LensExpect(unsigned w, unsigned h);
extern "C" int OLUE3_LensLive();
static uint32_t g_lsParam = 0;                 // FName index of 'CameraTexture'
static bool LS_ResolveFns() {
    if (g_lsFnOk) return true;
    const char* f[LF_N][2] = {
        { "TextureRenderTarget2D", "Create" }, { "Actor", "Spawn" }, { "SceneCapture2DComponent", "SetCaptureParameters" },
        { "SceneCapture2DComponent", "SetView" }, { "SceneCaptureComponent", "SetEnabled" }, { "SceneCaptureComponent", "SetFrameRate" },
        { "MeshComponent", "CreateAndSetMaterialInstanceConstant" }, { "MeshComponent", "GetMaterial" }, { "MeshComponent", "SetMaterial" },
        { "MaterialInstanceConstant", "SetTextureParameterValue" }, { "MaterialInstanceConstant", "SetParent" },
        { "Actor", "SetLocation" }, { "Actor", "SetRotation" } };
    for (int i = 0; i < LF_N; ++i) if (!(g_lsFn[i] = LS_FindFunction(f[i][0], f[i][1]))) {
        char l[160]; sprintf_s(l, "[OLVR][LIVESCREEN] %s.%s not found - the live screen is off", f[i][0], f[i][1]); OLProxyLog(l); return false;
    }
    if (!SP_FindPropAny("SceneCaptureActor", "SceneCapture", &g_lsCaptureOff, nullptr) ||
        !SP_FindPropAny("Actor", "Location", &g_lsActorLocOff, nullptr) ||
        !SP_FindPropAny("MaterialInstanceConstant", "TextureParameterValues", &g_lsTexParamsOff, nullptr)) {
        OLProxyLog("[OLVR][LIVESCREEN] reflection incomplete - the live screen is off"); return false;
    }
    // SceneCapture2DComponent.bUpdateMatrices: true = the view follows the owner actor every frame and
    // SetView is overwritten; cleared, SetView puts the camera at the lens.
    // the only bool in its dword (ViewMatrix follows at +0xC), so its bit is bit 0 - the mask vote needs two
    if (SP_FindPropAny("SceneCapture2DComponent", "bUpdateMatrices", &g_lsUpdOff, nullptr) && g_lsUpdOff >= 0x80 && g_lsUpdOff < 0x200) g_lsUpdMask = 1;
    else { g_lsUpdOff = 0; g_lsUpdMask = 0; }
    { char l[160]; sprintf_s(l, "[OLVR][LIVESCREEN] SceneCapture2DComponent.bUpdateMatrices +0x%X mask 0x%X", g_lsUpdOff, g_lsUpdMask); OLProxyLog(l); }
    g_lsFnOk = true;
    return true;
}
// A loaded material instance with a 'CameraTexture' slot: the game's security-camera monitor screen.
static void* LS_FindScreenParent(uint32_t* paramName) {
    void* micCls = BP_FindClassObj("MaterialInstanceConstant");
    char pnm[64];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || LS_ClassOf(o) != micCls) continue;
        struct { char* d; int n, mx; } ta = {};
        if (!BP_Read((char*)o + g_lsTexParamsOff, &ta, 16) || !ta.d || ta.n <= 0 || ta.n > 32) continue;
        for (int j = 0; j < ta.n; ++j) {
            uint32_t pn = 0;
            if (BP_Read(ta.d + j * 32, &pn, 4) && BP_Name(pn, pnm, sizeof(pnm)) && !strcmp(pnm, "CameraTexture")) { *paramName = pn; return o; }
        }
    }
    return nullptr;
}
// [LIVESCREEN] The camcorder's own screen material (CameraScreen_INST: emissive, with the camcorder HUD
// 'camerahud' over it and its own night-vision switch 'nvOn') draws its picture from the texture
// Screen_camera, baked in (no parameter, and a material instance made at runtime under it does not render).
// So the live render target is put where that texture's GPU texture is: FTextureResource holds the
// IDirect3DTexture9 the shaders sample - found by asking each pointer field for IID_IDirect3DTexture9 - and
// that pointer is swapped for the render target's (AddRef'd: when the level frees the screen texture it
// frees our reference, never more). The real screen, HUD and all, then shows the lens.
extern "C" IMAGE_DOS_HEADER __ImageBase;   // this DLL (our proxy is also called d3d9.dll)
static unsigned g_lsResOff = 0;
static void* g_lsScreenRes = nullptr;
static int   g_lsScreenTexOff = -1;
static void* g_lsOrigD3D = nullptr;
static void* g_lsRtD3D = nullptr;
static int   g_lsSwapTries = 0;
static bool  g_lsSwapped = false;
static int   g_lsSlotPath[2] = { -1, -1 };     // resource field, then (wrapped) the field inside it
static void* g_lsScreenTex = nullptr;          // the screen texture object swapped
static char  g_lsTarget[64] = "Screen_camera";   // [DEVBRIDGE] "lstarget <texture>" 
typedef long (__stdcall *QI_t)(void*, const GUID&, void**);
typedef unsigned long (__stdcall *AR_t)(void*);
static const GUID kIID_D3DTex9 = { 0x85C31227, 0x3DE5, 0x4F00, { 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 } };
// Only a pointer whose vtable lives in Direct3D 9's own DLL is ever called: a game object's first virtual is
// often its destructor (the first build of this probe called those - and the game died).
static bool LS_InD3D9(const void* a) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
    if (m == (HMODULE)&__ImageBase) return false;          // our own proxy is called d3d9.dll too
    return !_stricmp(b, "d3d9.dll") || !_stricmp(b, "d3d9on12.dll");
}
static bool LS_IsD3DTex(void* p) {
    if (!p) return false;
    __try {
        void** vt = *(void***)p;
        if (!LS_InD3D9(vt) || !LS_InD3D9(vt[0])) return false;
        void* q = nullptr;
        if (((QI_t)vt[0])(p, kIID_D3DTex9, &q) != 0 || !q) return false;
        ((AR_t)(*(void***)q)[2])(q);   // Release
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The IDirect3DTexture9 a texture resource draws with: in a field of the resource, or (UE3's D3D9 RHI wraps
// textures in its own objects) in a field of an object it points at. Returns the address of that field.
static void** LS_D3DTexSlot(void* res, int* o1, int* o2) {
    for (int a = 8; a <= 0x200; a += 8) {
        void* p = ProbeReadPtr((char*)res + a);
        if (!p) continue;
        if (LS_IsD3DTex(p)) { *o1 = a; *o2 = -1; return (void**)((char*)res + a); }
        for (int b = 0; b <= 0x60; b += 8) {
            void* q = ProbeReadPtr((char*)p + b);
            if (q && LS_IsD3DTex(q)) { *o1 = a; *o2 = b; return (void**)((char*)p + b); }
        }
    }
    return nullptr;
}
static bool LS_Swap() {
    if (!g_lsResOff && !SP_FindPropAny("Texture", "Resource", &g_lsResOff, nullptr)) return false;
    void* rres = g_ls.rt ? ProbeReadPtr((char*)g_ls.rt + g_lsResOff) : nullptr;
    const bool tell = g_lsSwapTries == 1 || g_lsSwapTries == 600 || g_lsSwapTries == 2999;
    char l0[200];
    if (!rres) { if (tell) { sprintf_s(l0, "[OLVR][LIVESCREEN] swap try %d: the render target has no resource yet (Resource +0x%X)", g_lsSwapTries, g_lsResOff); OLProxyLog(l0); } return false; }
    int r1 = -1, r2 = -1, s1 = -1, s2 = -1;
    void** rslot = LS_D3DTexSlot(rres, &r1, &r2);
    if (!rslot) { if (tell) { sprintf_s(l0, "[OLVR][LIVESCREEN] swap try %d: the render target's GPU texture is not there yet (resource %p)", g_lsSwapTries, rres); OLProxyLog(l0); } return false; }
    // every texture called Screen_camera: the one that is actually on the GPU
    void* texCls = BP_FindClassObj("Texture2D");
    void** sslot = nullptr; void* screenTex = nullptr; int cands = 0;
    for (int i = 0; i < g_bpObjNum && !sslot; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || LS_ClassOf(o) != texCls || !LS_NameIs(o, g_lsTarget)) continue;
        ++cands;
        void* sres = ProbeReadPtr((char*)o + g_lsResOff);
        if (sres && (sslot = LS_D3DTexSlot(sres, &s1, &s2)) != nullptr) screenTex = o;
    }
    if (!sslot) {
        if (tell) { sprintf_s(l0, "[OLVR][LIVESCREEN] swap try %d: %d %s texture(s), none on the GPU (render target's at +0x%X/+0x%X)", g_lsSwapTries, cands, g_lsTarget, r1, r2); OLProxyLog(l0); }
        return false;
    }
    void* rt = *rslot; void* orig = *sslot;
    __try {
        ((AR_t)(*(void***)rt)[1])(rt);                                   // AddRef: the screen texture now holds one
        InterlockedExchangePointer(sslot, rt);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    g_lsScreenRes = (void*)sslot; g_lsScreenTexOff = 0; g_lsOrigD3D = orig; g_lsRtD3D = rt; g_lsSwapped = true;
    g_lsSlotPath[0] = s1; g_lsSlotPath[1] = s2; g_lsScreenTex = screenTex;
    char l[240];
    sprintf_s(l, "[OLVR][LIVESCREEN] the camcorder screen's texture (%s) now draws the live render target (D3D texture at +0x%X/+0x%X, render target's at +0x%X/+0x%X)", g_lsTarget, s1, s2, r1, r2);
    OLProxyLog(l);
    return true;
}
// Put the screen's own texture back. The slot is found again from the Screen_camera object (a stored
// address could be a freed resource after a level change); if the texture is gone, so is our reference.
static void LS_Unswap() {
    if (!g_lsSwapped) return;
    g_lsSwapped = false;
    void* tex = g_lsScreenTex;
    void* res = tex ? ProbeReadPtr((char*)tex + g_lsResOff) : nullptr;
    if (!res) return;
    void** slot = (void**)((char*)res + g_lsSlotPath[0]);
    if (g_lsSlotPath[1] >= 0) { void* w = ProbeReadPtr(slot); if (!w) return; slot = (void**)((char*)w + g_lsSlotPath[1]); }
    if ((void*)slot != g_lsScreenRes || ProbeReadPtr(slot) != g_lsRtD3D) return;   // not ours any more
    __try {
        InterlockedExchangePointer(slot, g_lsOrigD3D);
        ((AR_t)(*(void***)g_lsRtD3D)[2])(g_lsRtD3D);                    // Release ours
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static const float kLsFps = 30.0f, kLsViewDist = 4000.0f;   // capture rate; how far it draws (uu, 40 m)
static void LS_Fail(const char* why) {
    if (g_ls.comp && g_ls.enabled) { alignas(16) char q[0x10] = {}; LS_Call(g_ls.comp, g_lsFn[LF_ENABLE], q); g_ls.enabled = false; }   // never left capturing
    g_ls.state = -1; g_ls.retryAt = GetTickCount64() + 20000; InterlockedExchange(&g_lsLive, 0);
    char l[200]; sprintf_s(l, "[OLVR][LIVESCREEN] not live: %s (the overlay picture stands in; trying again in 20 s)", why); OLProxyLog(l);
}
static void LS_Build() {
    BP_RefreshObjects();
    if (!LS_ResolveFns()) { LS_Fail("the game functions it needs were not found"); return; }
    void* hero = OLUE3_Hero();
    void* cam = hero ? ProbeReadPtr((char*)hero + kHeroCameraMeshOff) : nullptr;
    if (!cam) { LS_Fail("no camcorder model"); return; }
    void* cdo = LS_FindObject("Default__TextureRenderTarget2D");
    void* capCls = BP_FindClassObj("SceneCapture2DActor");
    if (!cdo || !capCls) { LS_Fail("render target / capture classes missing"); return; }
    alignas(16) char p[0x60];
    // the render target
    memset(p, 0, sizeof(p));
    *(int*)(p + 0) = kLsW; *(int*)(p + 4) = kLsH; p[8] = 2;           // PF_A8R8G8B8
    *(float*)(p + 0x18) = 1.0f;                                         // clear colour alpha
    OLProxy_LensExpect(kLsW, kLsH);                                     // [LENSCAM] its GPU texture is made on the render thread
    if (!LS_Call(cdo, g_lsFn[LF_CREATE], p) || !(g_ls.rt = *(void**)(p + 0x20))) { LS_Fail("TextureRenderTarget2D.Create failed"); return; }
    // the capture camera
    memset(p, 0, sizeof(p));
    *(void**)(p + 0) = capCls; *(void**)(p + 8) = hero;
    BP_Read((char*)hero + g_lsActorLocOff, p + 0x18, 12);
    *(int*)(p + 0x38) = 1; *(int*)(p + 0x3C) = 1;                     // bNoCollisionFail, bNoFail
    if (!LS_Call(hero, g_lsFn[LF_SPAWN], p) || !(g_ls.actor = *(void**)(p + 0x40))) { LS_Fail("could not spawn the capture camera"); return; }
    g_ls.comp = ProbeReadPtr((char*)g_ls.actor + g_lsCaptureOff);
    if (!g_ls.comp) { LS_Fail("the capture camera has no capture component"); return; }
    // the view follows the capture ACTOR (bUpdateMatrices stays set): the actor is moved to the lens every
    // frame - SetView alone left the render-thread probe where it was spawned (one frame, never again)
    memset(p, 0, sizeof(p));
    *(void**)(p + 0) = g_ls.rt; *(float*)(p + 8) = 70.0f; *(float*)(p + 0xC) = 6.0f; *(float*)(p + 0x10) = 0.0f;
    LS_Call(g_ls.comp, g_lsFn[LF_SETCAP], p); g_ls.fov = 70.0f;
    memset(p, 0, sizeof(p)); *(float*)p = kLsFps; LS_Call(g_ls.comp, g_lsFn[LF_RATE], p);
    memset(p, 0, sizeof(p)); *(int*)p = 0; LS_Call(g_ls.comp, g_lsFn[LF_ENABLE], p); g_ls.enabled = false;
    // the capture: the game's own post-process (its exposure and colour) so the picture is not near black
    {
        unsigned bo = 0, m1 = 0, m2 = 0;
        if (SP_FindPropAny("SceneCaptureComponent", "bEnablePostProcess", &bo, &m1) && SP_FindPropAny("SceneCaptureComponent", "bUseMainScenePostProcessSettings", &bo, &m2))
            __try { *(uint32_t*)((char*)g_ls.comp + bo) |= (m1 | m2); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // cheaper: a 6 cm screen does not need the far end of a corridor (the game is render-thread bound)
        unsigned vd = 0;
        if (SP_FindPropAny("SceneCaptureComponent", "MaxViewDistanceOverride", &vd, nullptr) && vd >= 0x80 && vd < 0x200)
            __try { *(float*)((char*)g_ls.comp + vd) = kLsViewDist; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    memset(p, 0, sizeof(p)); *(int*)p = kLsElement; LS_Call(cam, g_lsFn[LF_GETMAT], p); g_ls.orig = *(void**)(p + 4);   // CameraScreen_INST
    g_ls.hero = hero; g_ls.cam = cam; g_ls.state = 1; g_lsSwapTries = 0; InterlockedExchange(&g_lsLive, 1);
    char l[200];
    sprintf_s(l, "[OLVR][LIVESCREEN] live: %dx%d render target %p, capture camera %p at the lens, %.0f fps - shown on the flip screen", kLsW, kLsH, g_ls.rt, g_ls.actor, kLsFps);
    OLProxyLog(l);
}
static void LS_Undo(const char* why) {
    LS_Unswap();
    if (g_ls.state == 1 && g_ls.cam == ProbeReadPtr((char*)OLUE3_Hero() + kHeroCameraMeshOff)) {
        alignas(16) char p[0x20] = {};
        LS_Call(g_ls.comp, g_lsFn[LF_ENABLE], p);
    }
    if (g_ls.state != 0) { char l[160]; sprintf_s(l, "[OLVR][LIVESCREEN] off (%s)", why); OLProxyLog(l); }
    g_ls.state = 0; g_ls.enabled = false; InterlockedExchange(&g_lsLive, 0);
}
static void LS_Frame() {
    void* hero = OLUE3_Hero();
    const bool wanted = g_flipOn || g_lsWanted;
    if (!wanted || !hero) { if (g_ls.state == 1) LS_Undo(wanted ? "no hero" : "switched off"); return; }
    if (g_ls.state == 1 && hero != g_ls.hero) { LS_Unswap(); g_ls.state = 0; InterlockedExchange(&g_lsLive, 0); OLProxyLog("[OLVR][LIVESCREEN] a new level / Miles - rebuilding"); }
    const bool fresh = g_lsLensMs && GetTickCount64() - g_lsLensMs < 250;
    if (g_ls.state == 0 && fresh && g_lsInHand) LS_Build();
    else if (g_ls.state == -1 && GetTickCount64() > g_ls.retryAt && fresh && g_lsInHand) g_ls.state = 0;
    if (g_ls.state != 1) return;
    alignas(16) char p[0x20];
    // rendered only while the camcorder is in your hand with its screen toward you: it costs a view
    const bool want = fresh && g_lsInHand && GetTickCount64() - g_lsSeenMs < 600;
    if (want != g_ls.enabled) {
        memset(p, 0, sizeof(p)); *(int*)p = want ? 1 : 0;
        LS_Call(g_ls.comp, g_lsFn[LF_ENABLE], p); g_ls.enabled = want;
    }
    if (!want) return;
    memset(p, 0, sizeof(p));
    memset(p, 0, sizeof(p)); memcpy(p, g_lsLensPos, 12); LS_Call(g_ls.actor, g_lsFn[LF_SETLOC], p);
    memset(p, 0, sizeof(p)); memcpy(p, g_lsLensRot, 12); LS_Call(g_ls.actor, g_lsFn[LF_SETROT], p);
    static ULONGLONG s_logMs = 0;
    if (GetTickCount64() - s_logMs > 5000) {
        s_logMs = GetTickCount64();
        uint32_t upd = 0; if (g_lsUpdOff) BP_Read((char*)g_ls.comp + g_lsUpdOff, &upd, 4);
        char l[200];
        sprintf_s(l, "[OLVR][LIVESCREEN] capturing from (%.0f %.0f %.0f) pitch %d yaw %d roll %d, follow-owner bit %d",
                  g_lsLensPos[0], g_lsLensPos[1], g_lsLensPos[2], g_lsLensRot[0], g_lsLensRot[1], g_lsLensRot[2], (upd & g_lsUpdMask) ? 1 : 0);
        OLProxyLog(l);
    }
}
extern "C" int OLUE3_LensLive() { return (g_ls.state == 1 && g_ls.enabled) ? 1 : 0; }
static void LS_InfoMine() {
    char l[200];
    sprintf_s(l, "[OLVR][LIVESCREEN] mine: state %d, render target %p, capture %p, material instance %p", g_ls.state, g_ls.rt, g_ls.comp, g_ls.mic);
    OLProxyLog(l);
    if (g_ls.state == 1 && g_ls.mic) LS_LogMaterial("our screen material", g_ls.mic, 0);
    if (g_ls.state == 1 && g_ls.cam) {
        alignas(16) char p[0x20] = {}; *(int*)p = kLsElement; LS_Call(g_ls.cam, g_lsFn[LF_GETMAT], p);
        LS_LogMaterial("the flip screen element now", *(void**)(p + 4), 0);
    }
}
static char g_lsTexName[96] = "";
static void LS_SetTex() {
    if (g_ls.state != 1 || !g_ls.mic) return;
    void* t = !strcmp(g_lsTexName, "rt") ? g_ls.rt : LS_FindObject(g_lsTexName);
    alignas(16) char p[0x20] = {}; *(uint32_t*)p = g_lsParam; *(void**)(p + 8) = t;
    LS_Call(g_ls.mic, g_lsFn[LF_SETTEX], p);
    char l[160]; sprintf_s(l, "[OLVR][LIVESCREEN] (dev) screen texture -> %s (%p)", g_lsTexName, t); OLProxyLog(l);
}
// [DEVBRIDGE] (Test46) which texture slot the flip screen's own material reads: try names, watch the screen
static uint32_t LS_NameIndex(const char* want) {
    struct { void** d; int n; int mx; } ta = {};
    if (!g_bpNamesSlot || !BP_Read(g_bpNamesSlot, &ta, sizeof(ta)) || ta.n <= 0) return 0;
    char nm[64];
    for (int i = 1; i < ta.n && i < 2000000; ++i) if (BP_Name((uint32_t)i, nm, sizeof(nm)) && !_stricmp(nm, want)) return (uint32_t)i;
    return 0;
}
static char g_lsArg[96] = "", g_lsArg2[96] = "";
static void LS_DevParent() {
    if (g_ls.state != 1 || !g_ls.mic) return;
    void* par = !strcmp(g_lsArg, "orig") ? g_ls.orig : LS_FindObject(g_lsArg);
    alignas(16) char p[0x20] = {}; *(void**)p = par; LS_Call(g_ls.mic, g_lsFn[LF_SETPARENT], p);
    char l[200], pn[96] = "?"; if (par) BP_ObjName(par, pn, sizeof(pn));
    sprintf_s(l, "[OLVR][LIVESCREEN] (dev) screen material parent -> %s (%p)", pn, par); OLProxyLog(l);
}
static void LS_DevSlots(bool all, bool onOrig = false) {
    void* target = g_ls.mic;
    if (onOrig) {   // the material the flip screen has right now (the camcorder's own CameraScreen_INST when the live screen is off)
        void* hero = OLUE3_Hero(); void* cam = hero ? ProbeReadPtr((char*)hero + kHeroCameraMeshOff) : nullptr;
        if (!cam || !LS_ResolveFns()) return;
        alignas(16) char q[0x20] = {}; *(int*)q = kLsElement; LS_Call(cam, g_lsFn[LF_GETMAT], q); target = *(void**)(q + 4);
        if (!g_ls.rt) { memset(q, 0, sizeof(q)); }
    } else if (g_ls.state != 1 || !g_ls.mic) return;
    void* tex = !strcmp(g_lsArg2, "rt") ? g_ls.rt : LS_FindObject(g_lsArg2);
    static const char* kGuess[] = { "Texture", "Screen", "ScreenTexture", "ScreenTex", "Emissive", "EmissiveTexture", "EmissiveMap", "Display",
        "Video", "Movie", "LCD", "RenderTarget", "RT", "Tex", "Diffuse", "DiffuseMap", "DiffuseTexture", "Image", "CamTex", "CameraTex",
        "CameraTexture", "Feed", "UI", "HUD", "Picture", "Overlay", "Noise", "Scanlines", "Lines", "Glow", "Self illum", "Emissive2",
        "EmissiveUV", "Lit", "Base", "BaseTexture", "Color", "ScreenImage", "Camera", "Viewfinder", "Monitor", "TV", "Static" };
    int n = 0; char l[220];
    for (int k = 0; k < (int)(sizeof(kGuess) / sizeof(kGuess[0])); ++k) {
        if (!all && _stricmp(kGuess[k], g_lsArg)) continue;
        const uint32_t ni = LS_NameIndex(kGuess[k]);
        if (!ni) continue;
        alignas(16) char p[0x20] = {}; *(uint32_t*)p = ni; *(void**)(p + 8) = tex;
        LS_Call(target, g_lsFn[LF_SETTEX], p); ++n;
    }
    if (!all && !n) { const uint32_t ni = LS_NameIndex(g_lsArg); if (ni) { alignas(16) char p[0x20] = {}; *(uint32_t*)p = ni; *(void**)(p + 8) = tex; LS_Call(target, g_lsFn[LF_SETTEX], p); n = 1; } }
    sprintf_s(l, "[OLVR][LIVESCREEN] (dev) %d texture slot name%s set to %s (%p)", n, n == 1 ? "" : "s", g_lsArg2, tex); OLProxyLog(l);
}
// [DEVBRIDGE] (Test46) "lsmat <material>": what feeds its emissive / diffuse (if the cooked graph is kept)
static void LS_DumpExpr(const char* what, void* e, int depth) {
    char l[300], nm[96] = "?", cn[96] = "?";
    if (!e) { sprintf_s(l, "[OLVR][LSMAT] %*s%s: none", depth * 2, "", what); OLProxyLog(l); return; }
    BP_ObjName(e, nm, sizeof(nm)); BP_ObjName(LS_ClassOf(e), cn, sizeof(cn));
    int at = sprintf_s(l, "[OLVR][LSMAT] %*s%s: %s (%s)", depth * 2, "", what, nm, cn);
    unsigned o = 0;
    if (strstr(cn, "TextureSample")) {
        if (SP_FindPropAny("MaterialExpressionTextureSample", "Texture", &o, nullptr)) { void* t = ProbeReadPtr((char*)e + o); char tn[96] = "-"; if (t) BP_ObjName(t, tn, sizeof(tn)); at += sprintf_s(l + at, sizeof(l) - at, " texture=%s", tn); }
        if (SP_FindPropAny("MaterialExpressionParameter", "ParameterName", &o, nullptr) || SP_FindPropAny("MaterialExpressionTextureSampleParameter", "ParameterName", &o, nullptr)) {
            uint32_t pn = 0; char pnm[64] = "-"; if (BP_Read((char*)e + o, &pn, 4)) BP_Name(pn, pnm, sizeof(pnm)); at += sprintf_s(l + at, sizeof(l) - at, " parameter='%s'", pnm);
        }
    }
    OLProxyLog(l);
    if (depth >= 4) return;
    // follow the expression's own inputs (FExpressionInput = { Expression*, ... }): A, B, Input, Coordinates
    const char* ins[] = { "A", "B", "Input", "Coordinates", "Alpha", "Base", "Exponent" };
    for (const char* in : ins) {
        if (!SP_FindPropIn(LS_ClassOf(e), in, &o, nullptr)) {
            // the property lives on the expression's own class or a parent; try by name on the class chain
            void* c = LS_ClassOf(e); bool f = false;
            for (int k = 0; k < 4 && c && !f; ++k) { if (SP_FindPropIn(c, in, &o, nullptr)) f = true; else c = ProbeReadPtr((char*)c + 0x78); }
            if (!f) continue;
        }
        void* sub = ProbeReadPtr((char*)e + o);
        if (sub && sub != e) LS_DumpExpr(in, sub, depth + 1);
    }
}
static void LS_DevMat() {
    void* m = LS_FindObject(g_lsArg);
    char l[200], cn[96] = "?";
    if (!m) { sprintf_s(l, "[OLVR][LSMAT] %s not found", g_lsArg); OLProxyLog(l); return; }
    BP_ObjName(LS_ClassOf(m), cn, sizeof(cn));
    unsigned o = 0, mask = 0;
    if (SP_FindPropAny("Material", "bUsedWithSkeletalMesh", &o, &mask)) {
        uint32_t v = 0; BP_Read((char*)m + o, &v, 4);
        sprintf_s(l, "[OLVR][LSMAT] %s (%s): bUsedWithSkeletalMesh=%d", g_lsArg, cn, (v & mask) ? 1 : 0); OLProxyLog(l);
    }
    const char* ins[] = { "EmissiveColor", "DiffuseColor", "Opacity" };
    for (const char* in : ins) if (SP_FindPropAny("Material", in, &o, nullptr)) LS_DumpExpr(in, ProbeReadPtr((char*)m + o), 0);
}
// [DEVBRIDGE] (Test46) "lsfind": materials compiled for skeletal meshes whose emissive input is kept
static void LS_DevFind() {
    void* matCls = BP_FindClassObj("Material");
    unsigned so = 0, sm = 0, eo = 0;
    if (!matCls || !SP_FindPropAny("Material", "bUsedWithSkeletalMesh", &so, &sm) || !SP_FindPropAny("Material", "EmissiveColor", &eo, nullptr)) { OLProxyLog("[OLVR][LSMAT] find: reflection missing"); return; }
    int mats = 0, skel = 0, shown = 0; char l[200], nm[96];
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || LS_ClassOf(o) != matCls) continue;
        ++mats;
        uint32_t v = 0; if (!BP_Read((char*)o + so, &v, 4) || !(v & sm)) continue;
        ++skel;
        void* e = ProbeReadPtr((char*)o + eo);
        if (!e || shown >= 40) continue;
        ++shown;
        BP_ObjName(o, nm, sizeof(nm));
        sprintf_s(l, "[OLVR][LSMAT] skeletal + emissive: %s", nm); OLProxyLog(l);
        LS_DumpExpr("EmissiveColor", e, 1);
    }
    sprintf_s(l, "[OLVR][LSMAT] find: %d materials, %d for skeletal meshes, %d with a kept emissive graph", mats, skel, shown); OLProxyLog(l);
    // material instances whose BASE material is compiled for skeletal meshes, with their texture slots
    void* micCls = BP_FindClassObj("MaterialInstanceConstant");
    unsigned po = 0, to = 0;
    if (!micCls || !SP_FindPropAny("MaterialInstance", "Parent", &po, nullptr) || !SP_FindPropAny("MaterialInstanceConstant", "TextureParameterValues", &to, nullptr)) return;
    int n = 0;
    for (int i = 0; i < g_bpObjNum && n < 60; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || LS_ClassOf(o) != micCls) continue;
        struct { char* d; int n, mx; } ta = {};
        if (!BP_Read((char*)o + to, &ta, 16) || !ta.d || ta.n <= 0 || ta.n > 32) continue;
        void* base = o;
        for (int k = 0; k < 6 && base && LS_ClassOf(base) != matCls; ++k) base = ProbeReadPtr((char*)base + po);
        if (!base || LS_ClassOf(base) != matCls) continue;
        uint32_t v = 0; if (!BP_Read((char*)base + so, &v, 4) || !(v & sm)) continue;
        char bn[96] = "?"; BP_ObjName(o, nm, sizeof(nm)); BP_ObjName(base, bn, sizeof(bn));
        int at = sprintf_s(l, "[OLVR][LSMAT] skeletal instance %s (base %s):", nm, bn);
        for (int j = 0; j < ta.n && at < 180; ++j) { uint32_t pn = 0; char pnm[48] = "?"; BP_Read(ta.d + j * 32, &pn, 4); BP_Name(pn, pnm, sizeof(pnm)); at += sprintf_s(l + at, sizeof(l) - at, " '%s'", pnm); }
        OLProxyLog(l); ++n;
    }
}
// [DEVBRIDGE] (Test46) "lscreate <parent> <slot>": the flip screen gets <parent> first, THEN a new instance is
// made from it (its parent set at creation, never changed) with <slot> = the render target
static void LS_DevCreate() {
    if (g_ls.state != 1 || !g_ls.cam) return;
    void* par = !strcmp(g_lsArg, "orig") ? g_ls.orig : LS_FindObject(g_lsArg);
    alignas(16) char p[0x20] = {};
    *(int*)p = kLsElement; *(void**)(p + 4) = par; LS_Call(g_ls.cam, g_lsFn[LF_SETMAT], p);
    memset(p, 0, sizeof(p)); *(int*)p = kLsElement;
    void* mic = nullptr;
    if (LS_Call(g_ls.cam, g_lsFn[LF_MICNEW], p)) mic = *(void**)(p + 4);
    uint32_t ni = g_lsArg2[0] ? LS_NameIndex(g_lsArg2) : 0;
    if (mic && ni) { memset(p, 0, sizeof(p)); *(uint32_t*)p = ni; *(void**)(p + 8) = g_ls.rt; LS_Call(mic, g_lsFn[LF_SETTEX], p); }
    if (mic) g_ls.mic = mic;
    char l[200], pn[96] = "?"; if (par) BP_ObjName(par, pn, sizeof(pn));
    sprintf_s(l, "[OLVR][LIVESCREEN] (dev) screen = new instance of %s (%p), slot '%s' -> render target", pn, mic, g_lsArg2); OLProxyLog(l);
}
// [DEVBRIDGE] (Test46) "lscap <byte offset hex> <value>": poke a byte of the capture component (ViewMode @0x90...)
static void LS_DevCapByte() {
    if (g_ls.state != 1 || !g_ls.comp) return;
    unsigned off = (unsigned)strtoul(g_lsArg, nullptr, 16); int v = atoi(g_lsArg2);
    if (off < 0x80 || off > 0x200) return;
    __try { *((unsigned char*)g_ls.comp + off) = (unsigned char)v; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    char l[120]; sprintf_s(l, "[OLVR][LIVESCREEN] (dev) capture +0x%X = %d", off, v); OLProxyLog(l);
}
static void* g_lsFnScalar = nullptr;
static void LS_DevScalar() {
    if (g_ls.state != 1 || !g_ls.mic) return;
    if (!g_lsFnScalar) g_lsFnScalar = LS_FindFunction("MaterialInstanceConstant", "SetScalarParameterValue");
    const uint32_t ni = LS_NameIndex(g_lsArg);
    alignas(16) char p[0x20] = {}; *(uint32_t*)p = ni; *(float*)(p + 8) = (float)atof(g_lsArg2);
    LS_Call(g_ls.mic, g_lsFnScalar, p);
    char l[160]; sprintf_s(l, "[OLVR][LIVESCREEN] (dev) scalar '%s' (%u) = %s", g_lsArg, ni, g_lsArg2); OLProxyLog(l);
}
static void LS_DevMatInfo() { void* m = LS_FindObject(g_lsArg); LS_LogMaterial(g_lsArg, m, 0); }
// [DEVBRIDGE] (Test46) "lstexlist <substring>": loaded textures by name
static void LS_DevTexList() {
    char l[300], nm[96], cn[64];
    int at = sprintf_s(l, "[OLVR][LSTEX] '%s':", g_lsArg), n = 0;
    for (int i = 0; i < g_bpObjNum && n < 60; ++i) {
        void* o = nullptr;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o || !BP_ObjName(o, nm, sizeof(nm))) continue;
        char lo[96]; strcpy_s(lo, nm); _strlwr_s(lo); char want[96]; strcpy_s(want, g_lsArg); _strlwr_s(want);
        if (!strstr(lo, want)) continue;
        if (!BP_ObjName(LS_ClassOf(o), cn, sizeof(cn)) || !strstr(cn, "Texture")) continue;
        if (at > 230) { OLProxyLog(l); at = sprintf_s(l, "[OLVR][LSTEX]   "); }
        at += sprintf_s(l + at, sizeof(l) - at, " %s(%s)", nm, cn); ++n;
    }
    OLProxyLog(l);
}
// [DEVBRIDGE] (Test46) "lsres": the layout of the render target's and Screen_camera's texture resources
static const char* LS_ModOf(const void* a, char* buf, size_t n) {
    HMODULE m = nullptr; buf[0] = 0;
    if (!a || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) return buf;
    char path[MAX_PATH] = {}; GetModuleFileNameA(m, path, MAX_PATH);
    const char* b = strrchr(path, '\\'); strcpy_s(buf, n, b ? b + 1 : path);
    return buf;
}
static void LS_DumpObj(const char* what, void* o, int depth) {
    char l[400], mod[64];
    int at = sprintf_s(l, "[OLVR][LSRES] %*s%s %p:", depth * 2, "", what, o);
    void* subs[16]; int ns = 0;
    for (int i = 0; i < 14; ++i) {
        void* v = ProbeReadPtr((char*)o + i * 8);
        void* vt = v ? ProbeReadPtr(v) : nullptr;
        LS_ModOf(vt, mod, sizeof(mod));
        if (!v) at += sprintf_s(l + at, sizeof(l) - at, " %X:-", i * 8);
        else if (mod[0]) { at += sprintf_s(l + at, sizeof(l) - at, " %X:%s", i * 8, mod); if (ns < 16) subs[ns++] = v; }
        else { uint64_t raw = 0; BP_Read((char*)o + i * 8, &raw, 8); at += sprintf_s(l + at, sizeof(l) - at, " %X:%llX", i * 8, raw); }
        if (at > 360) break;
    }
    OLProxyLog(l);
    if (depth >= 1) return;
    for (int k = 0; k < ns && k < 4; ++k) { char w[32]; sprintf_s(w, "->obj%d", k); LS_DumpObj(w, subs[k], depth + 1); }
}
static void LS_DevRes() {
    if (!g_lsResOff && !SP_FindPropAny("Texture", "Resource", &g_lsResOff, nullptr)) { OLProxyLog("[OLVR][LSRES] Texture.Resource not found"); return; }
    char l[160]; sprintf_s(l, "[OLVR][LSRES] Texture.Resource at +0x%X", g_lsResOff); OLProxyLog(l);
    void* st = LS_FindObject("Screen_camera");
    if (g_ls.rt) LS_DumpObj("render target resource", ProbeReadPtr((char*)g_ls.rt + g_lsResOff), 0);
    if (st) LS_DumpObj("Screen_camera resource", ProbeReadPtr((char*)st + g_lsResOff), 0);
    for (int k2 = 0; k2 < 2; ++k2) {
        void* res = k2 ? (st ? ProbeReadPtr((char*)st + g_lsResOff) : nullptr) : (g_ls.rt ? ProbeReadPtr((char*)g_ls.rt + g_lsResOff) : nullptr);
        if (!res) continue;
        int a = -1, b = -1; void** sl = LS_D3DTexSlot(res, &a, &b);
        char l3[160]; sprintf_s(l3, "[OLVR][LSRES] %s: D3D texture %s +0x%X/+0x%X", k2 ? "screen" : "rt", sl ? "at" : "NOT FOUND up to", sl ? a : 0x200, b);
        OLProxyLog(l3);
    }
    // the screen resource around the render target's texture field (+0x98), and each pointee's first qwords
    if (st) {
        void* res = ProbeReadPtr((char*)st + g_lsResOff);
        for (int f = 0x70; f <= 0xD0 && res; f += 8) {
            void* w = ProbeReadPtr((char*)res + f);
            char l4[300], m1[48], m2[48]; int at = sprintf_s(l4, "[OLVR][LSRES] screen res+0x%X = %p:", f, w);
            for (int i = 0; w && i < 6 && at < 260; ++i) {
                void* v = ProbeReadPtr((char*)w + i * 8);
                LS_ModOf(v, m1, sizeof(m1)); LS_ModOf(v ? ProbeReadPtr(v) : nullptr, m2, sizeof(m2));
                at += sprintf_s(l4 + at, sizeof(l4) - at, " %X:%p[%s|%s]", i * 8, v, m1, m2);
            }
            OLProxyLog(l4);
        }
    }
    // what the raw pointers at +0x10 / +0x18 lead to: each qword, the module it points into, the module its target points into
    void* rs[2] = { g_ls.rt ? ProbeReadPtr((char*)g_ls.rt + g_lsResOff) : nullptr, st ? ProbeReadPtr((char*)st + g_lsResOff) : nullptr };
    for (int k = 0; k < 2; ++k) for (int f = 0x10; f <= 0x18 && rs[k]; f += 8) {
        void* w = ProbeReadPtr((char*)rs[k] + f);
        if (!w) continue;
        char l2[420], m1[48], m2[48]; int at = sprintf_s(l2, "[OLVR][LSRES] %s +0x%X -> %p:", k ? "screen" : "rt", f, w);
        for (int i = 0; i < 10 && at < 380; ++i) {
            void* v = ProbeReadPtr((char*)w + i * 8);
            LS_ModOf(v, m1, sizeof(m1)); LS_ModOf(v ? ProbeReadPtr(v) : nullptr, m2, sizeof(m2));
            at += sprintf_s(l2 + at, sizeof(l2) - at, " %X:%p[%s|%s]%s", i * 8, v, m1, m2, LS_IsD3DTex(v) ? "*TEX*" : "");
        }
        OLProxyLog(l2);
    }
}
static volatile LONG g_lsReq = 0;   // 1 info
extern "C" void OLUE3_DevLiveScreen(int what) { InterlockedExchange(&g_lsReq, what); }
extern "C" void OLUE3_DevLiveScreenTarget(const char* name) { LS_Unswap(); strncpy_s(g_lsTarget, name, _TRUNCATE); g_lsSwapTries = 0; if (g_ls.state == -1) g_ls.state = 0; }
extern "C" void OLUE3_DevLiveScreenTex(const char* name) { strncpy_s(g_lsTexName, name, _TRUNCATE); InterlockedExchange(&g_lsReq, 5); }
extern "C" void OLUE3_DevLiveScreenOp(int op, const char* a, const char* b) {
    strncpy_s(g_lsArg, a ? a : "", _TRUNCATE); strncpy_s(g_lsArg2, b ? b : "", _TRUNCATE); InterlockedExchange(&g_lsReq, op);
}
extern "C" void OLUE3_DevLensOff(float x, float y, float z) { g_lsLensOff[0] = x; g_lsLensOff[1] = y; g_lsLensOff[2] = z; }
// [DEVBRIDGE] (Test46) "con <command>": a console command through PlayerController.ConsoleCommand on the game
// thread - "scale set DepthOfField False" etc. change system settings live (the shipping game has no console).
static char g_conCmd[256] = { 0 };
static volatile LONG g_conReq = 0;
extern "C" void OLUE3_DevConsole(const char* c) { strncpy_s(g_conCmd, c, _TRUNCATE); InterlockedExchange(&g_conReq, 1); }
static void Con_Run() {
    static void* fn = nullptr; static int oCmd = -1, oLog = -1;
    if (!fn) { BP_RefreshObjects(); fn = LS_FindFunction("PlayerController", "ConsoleCommand"); oCmd = LS_ParamOff(fn, "Command"); oLog = LS_ParamOff(fn, "bWriteToLog"); }
    void* hero = OLUE3_Hero();
    void* ctrl = hero ? ProbeReadPtr((char*)hero + kHeroCtrlOff) : nullptr;
    char l[360];
    if (!fn || oCmd < 0 || !ctrl) { sprintf_s(l, "[OLVR][CONSOLE] cannot run '%s' (function %p, controller %p)", g_conCmd, fn, ctrl); OLProxyLog(l); return; }
    static wchar_t w[256]; size_t n = 0; mbstowcs_s(&n, w, g_conCmd, _TRUNCATE);
    alignas(16) char p[0x80] = {};
    *(wchar_t**)(p + oCmd) = w; *(int*)(p + oCmd + 8) = (int)n; *(int*)(p + oCmd + 12) = (int)n;   // FString: data, Num (with the 0), Max
    if (oLog >= 0) *(int*)(p + oLog) = 1;
    const bool ok = LS_Call(ctrl, fn, p);
    sprintf_s(l, "[OLVR][CONSOLE] '%s' %s", g_conCmd, ok ? "run" : "FAILED"); OLProxyLog(l);
}
// [DEVBRIDGE] (Test47) "fingerless": give Miles the body the game gives him after Trager (OLHero.FingerlessMesh,
// SkeletalMeshComponent.SetSkeletalMesh on his body mesh) - tests the VR arms on it without playing that far.
static void Dev_Fingerless() {
    BP_RefreshObjects();
    void* fn = LS_FindFunction("SkeletalMeshComponent", "SetSkeletalMesh");
    unsigned fo = 0; void* hero = OLUE3_Hero();
    if (!fn || !hero || !SP_FindPropAny("OLHero", "FingerlessMesh", &fo, nullptr)) { OLProxyLog("[OLVR][FINGERLESS] not available"); return; }
    void* comp = ProbeReadPtr((char*)hero + 0x444);
    void* mesh = ProbeReadPtr((char*)hero + fo);
    const int po = LS_ParamOff(fn, "NewMesh");
    if (!comp || !mesh || po < 0) { OLProxyLog("[OLVR][FINGERLESS] no body mesh / fingerless mesh"); return; }
    alignas(16) char p[0x20] = {};
    *(void**)(p + po) = mesh;
    const bool ok = LS_Call(comp, fn, p);
    char nm[96] = "?"; void* now = ProbeReadPtr((char*)comp + 0x238); if (now) BP_ObjName(now, nm, sizeof(nm));
    char l[200]; sprintf_s(l, "[OLVR][FINGERLESS] SetSkeletalMesh %s - Miles's body is now %s", ok ? "called" : "FAILED", nm); OLProxyLog(l);
}
static void LS_Tick() {
    if (InterlockedExchange(&g_conReq, 0)) { __try { Con_Run(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][CONSOLE] EXCEPTION"); } }
    const LONG r = InterlockedExchange(&g_lsReq, 0);
    if (r == 1) { __try { LS_Info(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LIVESCREEN] info: EXCEPTION"); } }
    if (r == 5) { __try { LS_SetTex(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 6) { __try { LS_DevParent(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 7) { __try { LS_DevSlots(true); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 8) { __try { LS_DevSlots(false); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 9) { __try { LS_DevSlots(true, true); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 19) { __try { Dev_Fingerless(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][FINGERLESS] EXCEPTION"); } }
    if (r == 18) { __try { LS_DevRes(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LSRES] EXCEPTION"); } }
    if (r == 17) { __try { LS_DevTexList(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 15) { __try { LS_DevScalar(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 16) { __try { LS_DevMatInfo(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 14) { __try { LS_DevCapByte(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 13) { __try { LS_DevCreate(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LIVESCREEN] create EXCEPTION"); } }
    if (r == 12) { __try { LS_DevFind(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LSMAT] find EXCEPTION"); } }
    if (r == 11) { __try { LS_DevMat(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LSMAT] EXCEPTION"); } }
    if (r == 10) { __try { LS_DevSlots(false, true); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (r == 4) { __try { LS_InfoMine(); } __except (EXCEPTION_EXECUTE_HANDLER) { OLProxyLog("[OLVR][LIVESCREEN] mine: EXCEPTION"); } }
    if (r == 2) g_lsWanted = 1;
    if (r == 3) { g_lsWanted = 0; g_flipOn = 0; }
    __try { LS_Frame(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_ls.state = -1; g_ls.retryAt = GetTickCount64() + 60000; InterlockedExchange(&g_lsLive, 0); OLProxyLog("[OLVR][LIVESCREEN] EXCEPTION - off for a minute"); }
}

// [DIRECTTURN] (Test46) "I can't turn even at its full speed": turning went through the game's own right
// stick, so full stick was the game's turn rate and no faster (and a 30 deg snap took a second). Smooth and
// snap turns now turn Miles's controller rotation (Actor.Rotation of the PlayerController, by reflection)
// at the start of the player tick - the way a mouse turns him: any speed, and a snap is one frame. Only
// while he walks or falls in ordinary play with look input allowed; elsewhere (lockers, ledges, beds,
// scripted looks) the stick goes to the game as before, so its own view limits hold.
static volatile float g_turnRate = 0.0f;           // deg/s, + = right; from ol_motion at the pad poll
static volatile ULONGLONG g_turnRateMs = 0;
static volatile LONG g_turnStepMilli = 0;          // a pending snap, millidegrees
static float g_turnFrac = 0.0f;                    // the part of a yaw unit not yet applied (game thread)
extern "C" int OLUE3_DirectTurnReady() {
    return (g_ssRotation && g_sHasHero > 0 && (g_sMoveMode == 0 || g_sMoveMode == 1) && g_sCinematic <= 0 &&
            g_sIgnoreLook <= 0 && OLUE3_GameplayActive()) ? 1 : 0;
}
extern "C" void OLUE3_TurnRate(float degPerSec) { g_turnRate = degPerSec; g_turnRateMs = GetTickCount64(); }
extern "C" void OLUE3_TurnStep(float deg) { InterlockedExchangeAdd(&g_turnStepMilli, (LONG)(deg * 1000.0f)); }
static void TurnApply(void* ctrl, float dt) {
    float deg = (float)InterlockedExchange(&g_turnStepMilli, 0) * 0.001f;
    if (GetTickCount64() - g_turnRateMs < 150 && dt > 0.0f && dt < 0.2f) deg += g_turnRate * dt;
    {   // the first few turns asked for: why they do or do not happen
        static ULONGLONG s_ms = 0; static long s_n = 0;
        if (deg != 0.0f && s_n < 6 && GetTickCount64() - s_ms > 2000) {
            s_ms = GetTickCount64(); ++s_n; char l[200];
            sprintf_s(l, "[OLVR][DIRECTTURN] %.2f deg this tick (rate %.0f deg/s, dt %.3f): rot+0x%X hero %ld move %ld cine %ld ignoreLook %ld play %d",
                      deg, (float)g_turnRate, dt, g_ssRotation, g_sHasHero, g_sMoveMode, g_sCinematic, g_sIgnoreLook, OLUE3_GameplayActive());
            OLProxyLog(l);
        }
    }
    if (deg == 0.0f || !ctrl || !OLUE3_DirectTurnReady()) { if (deg == 0.0f) g_turnFrac = 0.0f; return; }
    const float units = deg * (65536.0f / 360.0f) + g_turnFrac;
    const int whole = (int)units;
    g_turnFrac = units - (float)whole;
    if (!whole) return;
    // the controller AND Miles himself: Outlast copies his body's yaw back to the controller every tick
    __try {
        int* rot = (int*)((char*)ctrl + g_ssRotation); rot[1] += whole;
        void* hero = OLUE3_Hero();
        if (hero) { int* hr = (int*)((char*)hero + g_ssRotation); hr[1] += whole; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static long s_logged = 0;
    if (s_logged < 3 && fabsf(deg) > 5.0f) { ++s_logged; char l[120]; sprintf_s(l, "[OLVR][DIRECTTURN] turned %.1f deg in one step", deg); OLProxyLog(l); }
}

// [TURNSCALE] (Test46d) NOT USED: measured, LookRightScale does not change the gamepad turn (the game turned
// ~258 deg/s whatever it was set to); turning is the game's own stick turn, scaled in ol_motion. Kept for reference:
// Turning goes through the game's own stick turn again, at the speed asked for, by
// scaling PlayerInput.LookRightScale (the game's look-speed setting; 300 = its own ~258 deg/s at full stick).
// [DIRECTTURN] above wrote Miles's rotation directly, and Outlast's own body turn never ran: while he turned
// his body lagged and the right arm was solved against it - "the right hand is twisted and wrong looking"
// (measured: the arm drops away from the controller during a slow turn - a thumb resting on the stick turned
// him slowly all the time - and is right again once he stops). Applied only in play; in menus the game's own
// value is back, so its options screen can never save ours. Called from the pad poll (game thread).
static volatile float g_turnScale = 1.0f;
static unsigned g_tsInputOff = 0, g_tsLookOff = 0;
static bool  g_tsTried = false;
static float g_tsBase = 0.0f, g_tsWritten = -1.0f;
extern "C" void OLUE3_SetTurnScale(float s) { g_turnScale = s < 0.1f ? 0.1f : (s > 10.0f ? 10.0f : s); }
extern "C" void OLUE3_TurnScaleTick() {
    if (!g_tsTried) {
        if (!g_bpObjNum) return;
        g_tsTried = true;
        if (!SP_FindPropAny("PlayerController", "PlayerInput", &g_tsInputOff, nullptr) ||
            !SP_FindPropAny("PlayerInput", "LookRightScale", &g_tsLookOff, nullptr)) {
            g_tsInputOff = 0; OLProxyLog("[OLVR][TURNSCALE] PlayerInput.LookRightScale not found - turning at the game's own speed");
        } else {
            char l[160]; sprintf_s(l, "[OLVR][TURNSCALE] PlayerController.PlayerInput +0x%X, PlayerInput.LookRightScale +0x%X", g_tsInputOff, g_tsLookOff); OLProxyLog(l);
        }
    }
    if (!g_tsInputOff) return;
    void* hero = OLUE3_Hero();
    void* ctrl = hero ? ProbeReadPtr((char*)hero + kHeroCtrlOff) : nullptr;
    void* pi = ctrl ? ProbeReadPtr((char*)ctrl + g_tsInputOff) : nullptr;
    if (!pi) return;
    float* p = (float*)((char*)pi + g_tsLookOff);
    float cur = 0.0f;
    if (!BP_Read(p, &cur, 4)) return;
    if (cur != g_tsWritten && cur > 1.0f && cur < 100000.0f) g_tsBase = cur;   // the game's own (options) value
    if (g_tsBase <= 0.0f) return;
    const float want = OLUE3_GameplayActive() ? g_tsBase * g_turnScale : g_tsBase;
    if (fabsf(want - cur) > 0.01f) {
        __try { *p = want; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        static long s_logs = 0;
        if (s_logs < 4) { ++s_logs; char l[160]; sprintf_s(l, "[OLVR][TURNSCALE] look speed %.0f -> %.0f (x%.2f)", cur, want, want / g_tsBase); OLProxyLog(l); }
    }
    g_tsWritten = want;
}

// [GRAIN] (Test47) Outlast's film grain on or off ([Render] FilmGrain, VR settings). Testers asked for a switch
// (one found the console line "set OLUberPostProcessEffect GrainOpacity 0"): in a headset the grain reads as blur. Off by
// default. Every frame of play, each of the game's uber post-process effects gets GrainOpacity 0 - or, back
// on, the game's own value again (the first non-zero one seen), so night vision or a new level cannot undo it.
static volatile long g_grainOn = 0;
static unsigned g_grainOff = 0;
static bool  g_grainTried = false;
static float g_grainOrig = -1.0f;
extern "C" int  OLUE3_GetFilmGrain() { return g_grainOn ? 1 : 0; }
extern "C" void OLUE3_SetFilmGrain(int on) { g_grainOn = on ? 1 : 0; }
static void GrainTick() {
    if (!g_grainTried) {
        if (!g_bpObjNum) return;
        g_grainTried = true;
        if (!SP_FindPropAny("OLUberPostProcessEffect", "GrainOpacity", &g_grainOff, nullptr)) {
            g_grainOff = 0; OLProxyLog("[OLVR][GRAIN] OLUberPostProcessEffect.GrainOpacity not found - the film grain stays the game's");
        } else {
            char l[120]; sprintf_s(l, "[OLVR][GRAIN] OLUberPostProcessEffect.GrainOpacity +0x%X, film grain %s", g_grainOff, g_grainOn ? "ON" : "OFF"); OLProxyLog(l);
        }
    }
    if (!g_grainOff) return;
    void* fx[16];
    const int n = OLUE3_ObjectsOfClass("OLUberPostProcessEffect", fx, 16);
    for (int i = 0; i < n; ++i) {
        float* p = (float*)((char*)fx[i] + g_grainOff);
        float v = 0.0f;
        if (!BP_Read(p, &v, 4) || !(v >= 0.0f && v < 100.0f)) continue;
        if (v > 0.0f && g_grainOrig < 0.0f) {
            g_grainOrig = v;
            char l[100]; sprintf_s(l, "[OLVR][GRAIN] the game's grain opacity is %.3f", v); OLProxyLog(l);
        }
        if (!g_grainOn) { if (v != 0.0f) __try { *p = 0.0f; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        else if (v == 0.0f && g_grainOrig > 0.0f) __try { *p = g_grainOrig; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// (Beta 1.1) [BODYTURN] The user: "the arms should reset if it goes off its position, this way no more having to
// recenter all the time". The usual reason they go off: you turn your own body (a swivel chair, standing and
// shifting) and Miles's body still faces the old way - your hands are then off to one side of his shoulders,
// the arms cross his chest and the hip holster is no longer at your hip; a recenter put him straight again.
// Now, once you have faced more than 20 deg away from where Miles faces for 2 s, he turns to face your way
// (30 deg/s) and the recenter frame turns with him - the view does not move, the arms come round. Never while
// a hand holds a door, an edge or a rung. [Motion] BodyFollowsHead=0 turns it off.
extern "C" void OLXR_TurnBaseline(float rightDeg);
extern "C" float OLInteract_HandAnchorWorld(int h, float out[3]);
static volatile long g_bodyTurn = 1;
extern "C" int  OLUE3_GetBodyTurn() { return g_bodyTurn ? 1 : 0; }
extern "C" void OLUE3_SetBodyTurn(int on) { g_bodyTurn = on ? 1 : 0; }
static void BodyTurnFollow(void* ctrl, float dt) {
    static const float kStartDeg = 20.0f, kRateDegPerSec = 30.0f, kDoneDeg = 2.0f;
    static const ULONGLONG kDwellMs = 2000;
    static ULONGLONG s_offSince = 0;
    static bool s_turning = false;
    static float s_frac = 0.0f, s_total = 0.0f;
    const ULONGLONG now = GetTickCount64();
    float aw[3];
    const bool anchored = OLInteract_HandAnchorWorld(0, aw) > 0.0f || OLInteract_HandAnchorWorld(1, aw) > 0.0f;
    if (!g_bodyTurn || !ctrl || !g_olPoseValid || !g_headTrack || anchored || !OLUE3_DirectTurnReady() ||
        !(dt > 0.0f && dt < 0.2f)) {
        s_offSince = 0; s_turning = false; s_frac = 0.0f; s_total = 0.0f;
        return;
    }
    const float head = g_olYawRad * 57.2957795f;   // + right: where you face, from where Miles faces
    if (!s_turning) {
        if (fabsf(head) <= kStartDeg) { s_offSince = 0; return; }
        if (!s_offSince) s_offSince = now;
        if (now - s_offSince < kDwellMs) return;
        s_turning = true; s_total = 0.0f;
    }
    if (fabsf(head) < kDoneDeg) {
        char l[160];
        sprintf_s(l, "[OLVR][BODYTURN] Miles turned %.0f deg to face the way you face (view unchanged)", s_total);
        OLProxyLog(l);
        s_turning = false; s_offSince = 0; s_frac = 0.0f;
        return;
    }
    float step = kRateDegPerSec * dt;
    if (step > fabsf(head) - 1.0f) step = fabsf(head) - 1.0f;   // the pose catching up a frame late must not overshoot
    if (step <= 0.0f) return;
    const float units = (head > 0.0f ? step : -step) * (65536.0f / 360.0f) + s_frac;
    const int whole = (int)units;
    s_frac = units - (float)whole;
    if (!whole) return;
    bool done = false;
    __try {
        int* rot = (int*)((char*)ctrl + g_ssRotation); rot[1] += whole;
        void* hero = OLUE3_Hero();
        if (hero) { int* hr = (int*)((char*)hero + g_ssRotation); hr[1] += whole; }
        done = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (done) {
        const float deg = (float)whole * (360.0f / 65536.0f);
        OLXR_TurnBaseline(deg);
        s_total += deg;
    }
}

static void __fastcall Hook_NativePlayerMove(void* ctrl, float dt) {
    float* fwd = nullptr;
    float saved[3] = {}, wrote[3] = {};
    bool swapped = false;
    const long mode = g_lookUse;
    OLXR_FrameLatch();   // [FRAMETAG] the player tick is the first reader of a new frame's pose
    OLInteract_RoomWalkStep();   // (Test44) [ROOMWALK] Miles takes this pose's room steps before he moves
    TurnApply(ctrl, dt);         // (Test46) [DIRECTTURN] smooth / snap turn
    BodyTurnFollow(ctrl, dt);    // (Beta 1.1) [BODYTURN] Miles faces the way you face
    LS_Tick();                   // (Test46) [LIVESCREEN]
    __try { GrainTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // (Test47) [GRAIN]
    if (mode && ctrl && OLUE3_GameplayActive() && g_olPoseValid && g_headTrack) {
        __try {
            char* hero = *(char**)((char*)ctrl + kCtrlHeroOff);
            if (hero && *(void**)(hero + kHeroCtrlOff) == ctrl &&
                *(const uint8_t*)(hero + kHeroMoveModeOff) == 0) {
                fwd = (float*)(hero + kHeroEyeFwdOff);
                const float len2 = fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2];
                if (len2 > 0.9f && len2 < 1.1f) {
                    float z = fwd[2]; if (z > 1.0f) z = 1.0f; else if (z < -1.0f) z = -1.0f;
                    float yaw = atan2f(fwd[1], fwd[0]), pitch = asinf(z);
                    bool ok = true;
                    if (mode == 2) {
                        // Right controller's pointing ray, in the same body frame the head uses:
                        // yaw relative to Miles's body, pitch absolute (the frame is level).
                        OLXrPad p;
                        if (OLXR_GetPad(&p) && (p.aimValid & 2u)) {
                            const OLXrPose& q = p.aimPose[1];
                            const float ax = -2.0f * (q.qx * q.qz + q.qw * q.qy);
                            const float ay = -2.0f * (q.qy * q.qz - q.qw * q.qx);
                            const float az = -(1.0f - 2.0f * (q.qx * q.qx + q.qy * q.qy));
                            yaw += atan2f(ax, -az);
                            pitch = asinf(ay < -1.0f ? -1.0f : (ay > 1.0f ? 1.0f : ay));
                        } else ok = false;
                    } else {
                        yaw   += g_olYawRad * g_injW;     // the same offsets the camera hook adds
                        pitch += g_olPitchRad * g_injW;
                    }
                    if (pitch > 1.53f) pitch = 1.53f; else if (pitch < -1.53f) pitch = -1.53f;
                    if (ok) {
                        const float cp = cosf(pitch);
                        wrote[0] = cp * cosf(yaw); wrote[1] = cp * sinf(yaw); wrote[2] = sinf(pitch);
                        for (int i = 0; i < 3; ++i) { saved[i] = fwd[i]; fwd[i] = wrote[i]; }
                        swapped = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { swapped = false; }
    }
    o_NativePlayerMove(ctrl, dt);
    if (swapped) {
        __try {
            if (fwd[0] == wrote[0] && fwd[1] == wrote[1] && fwd[2] == wrote[2])
                for (int i = 0; i < 3; ++i) fwd[i] = saved[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        static long s_logged = 0;
        if (InterlockedIncrement(&s_logged) == 1) {
            char l[240];
            sprintf_s(l, "[OLVR][LOOKUSE] live (%s): body (%.2f %.2f %.2f) -> aim (%.2f %.2f %.2f)",
                      mode == 2 ? "right hand" : "head", saved[0], saved[1], saved[2], wrote[0], wrote[1], wrote[2]);
            OLProxyLog(l);
        }
    }
}

// ---- [HIRES] render at the headset's own resolution inside a desktop-sized window (Test35) -------
// Each eye was drawn from a 960x1080 half of a desktop-sized frame, then stretched ~2.2x to the
// headset - the blur. Every earlier attempt (Tests 17-24) tried to grow the WINDOW, which Windows
// and UE3 both clamp to the desktop. This changes what the engine asks the RHI for instead.
// FViewport::UpdateViewportRHI (RVA 0x3BA960; FViewport* in rcx, bDestroyed dl, X r8d, Y r9d,
// bFullscreen on the stack) is the single place a game viewport's size reaches the D3D9 RHI: it
// writes FViewport::SizeX/SizeY (+0x9C/+0xA0), then RHIResizeViewport, whose
// UpdateD3DDeviceFromViewports Resets the device with a WINDOWED backbuffer of that size. Every
// consumer then follows by itself: CalcSceneView's view rects (GetSizeX/Y), the scene render
// targets (grown per frame from the RT size), the canvas/HUD and Scaleform (notified by this
// very function). Windowed Present stretches the big backbuffer into the small client, so the
// desktop shows a squeezed mirror - the headset gets every pixel.
// Windowed only: exclusive fullscreen must match a real display mode, so fs passes through.
typedef void (__fastcall *UpdateViewportRHI_t)(void*, char, unsigned, unsigned, char);
static UpdateViewportRHI_t o_UpdateViewportRHI = nullptr;
static const uintptr_t kUpdateViewportRhiRva = 0x3BA960;
static const unsigned char kUpdateViewportRhiPrologue[18] = {
    0x40,0x56,0x57,0x41,0x54,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24,0x30,0xFE,0xFF,0xFF,0xFF
};
static const uintptr_t kWindowsViewportVtblRva = 0x1A4F300;
static volatile long g_hiResOn = 0, g_hiResHookOk = 0;
static volatile long g_hiResW = 3456, g_hiResH = 1824;   // whole SBS backbuffer (both eyes)
static volatile long g_hiResEyeW = 1728, g_hiResEyeH = 1824;
static float g_hiResScale = 1.0f;
static volatile long g_hiResAppliedW = 0, g_hiResAppliedH = 0;
extern "C" int OLUE3_HiResActive() { return (g_hiResOn && g_hiResHookOk) ? 1 : 0; }
extern "C" int OLUE3_HiResApplied() {
    return (g_hiResAppliedW == g_hiResW && g_hiResAppliedH == g_hiResH) ? 1 : 0;
}
static void HiResRecompute() {
    float s = g_hiResScale; if (!(s >= 0.4f)) s = 0.4f; if (s > 1.5f) s = 1.5f;
    long w = (long)(g_hiResEyeW * s + 0.5f) & ~7L, h = (long)(g_hiResEyeH * s + 0.5f) & ~7L;
    if (w < 480) w = 480; if (h < 480) h = 480;
    g_hiResW = w * 2; g_hiResH = h;
}
// The runtime's recommended per-eye size, once the XR session knows it. Returns 1 if the target
// changed (the caller then nudges the engine to re-run its resize).
extern "C" int OLUE3_SetHiResEye(unsigned eyeW, unsigned eyeH) {
    if (eyeW < 480 || eyeH < 480 || eyeW > 4096 || eyeH > 4096) return 0;
    if ((long)eyeW == g_hiResEyeW && (long)eyeH == g_hiResEyeH) return 0;
    g_hiResEyeW = (long)eyeW; g_hiResEyeH = (long)eyeH;
    HiResRecompute();
    char l[160]; sprintf_s(l, "[OLVR][HIRES] headset asks %ux%u per eye -> engine target %ldx%ld", eyeW, eyeH, g_hiResW, g_hiResH);
    OLProxyLog(l);
    return 1;
}
extern "C" float OLUE3_GetRenderScale() { return g_hiResScale; }
extern "C" int OLUE3_SetRenderScale(float s) {
    if (!(s >= 0.4f)) s = 0.4f; if (s > 1.5f) s = 1.5f;
    if (fabsf(s - g_hiResScale) < 0.001f) return 0;
    g_hiResScale = s; HiResRecompute();
    return 1;
}
extern "C" void OLUE3_GetHiResTarget(long* w, long* h) { if (w) *w = g_hiResW; if (h) *h = g_hiResH; }

static void __fastcall Hook_UpdateViewportRHI(void* vp, char destroyed, unsigned x, unsigned y, char fs) {
    if (g_hiResOn && !destroyed && vp) {
        __try {
            if (*(void**)vp == (void*)(g_base + kWindowsViewportVtblRva)) {
                if (fs) {
                    // Exclusive fullscreen can only be a real display mode (desktop-sized or less:
                    // the game also re-applies a saved 1280x720 fullscreen after startup). In VR
                    // the monitor does not matter, so keep the viewport windowed at our size and
                    // clear its own fullscreen flag so WM_SIZE/cursor handling stays coherent.
                    fs = 0;
                    *(unsigned char*)((char*)vp + 0xA8) &= (unsigned char)~1u;
                    static long s_fsLogged = 0;
                    if (InterlockedIncrement(&s_fsLogged) <= 5)
                        OLProxyLog("[OLVR][HIRES] fullscreen request turned into the windowed high-res render");
                }
                {
                    const unsigned curX = *(const unsigned*)((char*)vp + 0x9C);
                    const unsigned curY = *(const unsigned*)((char*)vp + 0xA0);
                    const bool curFs = (*(const unsigned char*)((char*)vp + 0xA8) & 1) != 0;
                    void* rhi = *(void**)((char*)vp + 0x94);
                    // Every WM_SIZE re-runs Resize because the client rect never matches; when
                    // nothing would change, skip the viewport re-init (it flushes the render thread).
                    if (rhi && !curFs && curX == (unsigned)g_hiResW && curY == (unsigned)g_hiResH) return;
                    char l[200];
                    sprintf_s(l, "[OLVR][HIRES] engine viewport %ux%u (asked %ux%u) -> %ldx%ld (%ldx%ld per eye)",
                              curX, curY, x, y, g_hiResW, g_hiResH, g_hiResW / 2, g_hiResH);
                    OLProxyLog(l);
                    x = (unsigned)g_hiResW; y = (unsigned)g_hiResH;
                    g_hiResAppliedW = g_hiResW; g_hiResAppliedH = g_hiResH;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    o_UpdateViewportRHI(vp, destroyed, x, y, fs);
}

// ---- [SKELCTRL] the UE3-native way to move a bone, which should have been the first approach ---
// Two failures point at one mistake. WIGGLE wrote SpaceBases/LocalAtoms directly and the anim
// recomposed over it (right memory, wrong moment). BONEWATCH then tried to find that moment with
// hardware breakpoints and crashed the game - 300 threads armed at once, without counting first.
// Both were fighting the animation system. UE3 has a supported way to bend a bone that the
// animation system APPLIES ITSELF, every frame, in the correct window, by design: SkelControl.
// The exe carries the whole family (SkelControlSingleBone, SkelControlLimb - a 2-bone IK with an
// effector, i.e. "put this hand here" - plus BoneRotation/ControlStrength property names).
// If Miles's AnimTree already contains SkelControls on the arms, driving them needs NO hooking
// and NO timing games: set ControlStrength and the target, and the engine does the rest.
// This probe just looks. For every object whose name contains SkelControl or AnimTree it logs
// the name, and - the useful part - scans the object for FName fields resolving to a 'Hero-*'
// string, which is what identifies a control bound to MILES's skeleton rather than an NPC's.
// It also dumps raw floats from a bone-atom array so the ATOM STRIDE can be confirmed rather
// than assumed: WIGGLE used 0x20, and if the real stride is 0x30 then every write went into a
// finger instead of the arm, which would look exactly like "nothing moved".
// [Stage3] SkelCtrlProbe=1. Log-only. No threads, no breakpoints, no writes.
static long g_skelProbe = 0;

static void SkelProbeRun() {
    char l[300], nm[64];
    if (!g_bpObjs || !g_bpNames) { OLProxyLog("[OLVR][SKELCTRL] registry not ready"); return; }

    // --- 1. atom stride, measured. A real atom array is quat-then-translation, so at the correct
    // stride every element's first 4 floats are a UNIT quaternion. Score each candidate stride by
    // how many of 40 elements have |q| ~ 1.
    for (int c = 0; c < g_wigCompN && c < 1; ++c) {
        for (int a = 0; a < 2; ++a) {
            struct { void* d; int n; int mx; } ta;
            if (!BP_Read((char*)g_wigComp[c] + kWigArr[a], &ta, 16) || ta.n != 73 || !ta.d) continue;
            int bestS = 0, bestHit = 0;
            const unsigned strides[] = { 0x20, 0x30, 0x40 };
            for (unsigned si = 0; si < 3; ++si) {
                int hit = 0;
                for (int i = 0; i < 40; ++i) {
                    float q[4];
                    if (!BP_Read((char*)ta.d + (size_t)i * strides[si], q, 16)) break;
                    const float m = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                    if (m > 0.97f && m < 1.03f) ++hit;
                }
                if (hit > bestHit) { bestHit = hit; bestS = (int)strides[si]; }
            }
            sprintf_s(l, "[OLVR][SKELCTRL] atom array +0x%X: best stride 0x%X (%d/40 unit quats)"
                         "%s", kWigArr[a], bestS, bestHit,
                      bestS == 0x20 ? "  <- WIGGLE assumed 0x20, confirmed"
                                    : "  <- WIGGLE ASSUMED 0x20 AND WAS WRONG");
            OLProxyLog(l);
            // Show the bone actually hit at the assumed stride vs the correct one.
            if (bestS) {
                float t20[3] = {}, tb[3] = {};
                BP_Read((char*)ta.d + (size_t)36 * 0x20 + 16, t20, 12);
                BP_Read((char*)ta.d + (size_t)36 * (size_t)bestS + 16, tb, 12);
                sprintf_s(l, "[OLVR][SKELCTRL]   bone36 translation @0x20-stride (%.1f %.1f %.1f) "
                             "vs @0x%X-stride (%.1f %.1f %.1f)",
                          t20[0], t20[1], t20[2], bestS, tb[0], tb[1], tb[2]);
                OLProxyLog(l);
            }
        }
    }

    // --- 2. every SkelControl / AnimTree object, and which skeleton it is bound to.
    int nSkel = 0, nHero = 0;
    for (int i = 0; i < g_bpObjNum; ++i) {
        void* o = nullptr; uint32_t idx = 0;
        if (!BP_Read(g_bpObjs + i, &o, 8) || !o) continue;
        if (!BP_Read((char*)o + g_bpUNameOff, &idx, 4)) continue;
        if (!BP_Name(idx, nm, sizeof(nm))) continue;
        const bool isSkel = strstr(nm, "SkelControl") != nullptr;
        const bool isTree = strstr(nm, "AnimTree") != nullptr;
        if (!isSkel && !isTree) continue;
        ++nSkel;
        // Bound bone: any FName field on this object resolving to a Hero-* / NPC* bone string.
        char bound[64]; bound[0] = 0; unsigned boundOff = 0;
        for (unsigned off = 0x50; off <= 0x300 && !bound[0]; off += 4) {
            uint32_t bi = 0; char bn[64];
            if (!BP_Read((char*)o + off, &bi, 4)) continue;
            if (!BP_Name(bi, bn, sizeof(bn))) continue;
            if (strncmp(bn, "Hero-", 5) == 0 || strncmp(bn, "NPC", 3) == 0) {
                strcpy_s(bound, bn); boundOff = off;
            }
        }
        if (bound[0] && strncmp(bound, "Hero-", 5) == 0) ++nHero;
        if (nSkel <= 40 || bound[0]) {
            sprintf_s(l, "[OLVR][SKELCTRL] %p '%.40s'%s%.40s%s", o, nm,
                      bound[0] ? "  bone=" : "", bound[0] ? bound : "",
                      bound[0] ? "" : "  (no bone name found)");
            if (bound[0]) {
                char l2[300];
                sprintf_s(l2, "%s  @+0x%X", l, boundOff);
                OLProxyLog(l2);
            } else OLProxyLog(l);
        }
    }
    sprintf_s(l, "[OLVR][SKELCTRL] done: %d SkelControl/AnimTree objects, %d bound to Hero-* bones",
              nSkel, nHero);
    OLProxyLog(l);
}

// MENU CAMERA FREEZE. While the menu is open the world must hold still, or you cannot read it.
// Outlast is UE3 and reads the mouse through DirectInput, which a window-message swallow cannot
// see (ME1 learned this and froze the camera in the ENGINE instead). This hook already owns the
// camera source, so the same approach applies: latch the game's view the instant the menu opens and replay it every
// frame. Head tracking and the eye offset still apply ON TOP, so you can glance around the panel
// and it stays in stereo - only the mouse/stick look is frozen.
static long  g_menuLatched = 0;
static float g_menuLoc[3] = { 0, 0, 0 };
static int   g_menuRot[3] = { 0, 0, 0 };

static void __fastcall Hook_GetViewPoint(void* actor, float* outLoc, int* outRot) {
    o_GetViewPoint(actor, outLoc, outRot);
    // [NVLIGHT] The engine's own answer, before ANY injection - this is the gamepad/body
    // aim, and it is what tells the spotlight hook which light is the player's. Captured on every
    // call (including the tick calls otherwise ignored), so it is never stale.
    if (outRot) {
        g_baseViewRot[0] = outRot[0]; g_baseViewRot[1] = outRot[1]; g_baseViewRot[2] = outRot[2];
        InterlockedExchange(&g_baseViewFresh, 30);
    }
    SceneStateRead(actor);   // [SCENESTATE] throttled; the actor here is the PlayerController
    if (!g_eyePhase || !outLoc || !outRot) return;
    // [CAMPROBE] one row per frame (first eye only), BASE values before any injection below.
    if (g_camProbe && g_eyePhase == 1) ProbeTick(actor, outLoc, outRot);
    // [BONEPROBE] one shot, ~12s into gameplay so every level actor exists.
    if (g_boneProbe && g_eyePhase == 1) {
        static long s_bpFrames = 0;
        if (++s_bpFrames == 700) BP_Run(actor);
    }
    
// [WIGGLE] / [BONEWATCH] / [SKELCTRL] every frame once the level is up; all need the registry
    // first. EVERY probe that lives in this block MUST be named in this gate - SKELCTRL was added
    // inside it but left out of the condition, so arming it alone made it silently unreachable and
    // burned a whole test run producing an empty log.
    if ((g_boneWiggle || g_boneWatch || g_skelProbe || g_skelProp || g_armDrive) && g_eyePhase == 1) {
        static bool s_boot = false;
        // The old path intentionally waited 300 rendered frames before discovering the registry.
        // That made the hands remain native for several seconds after every launch.  GNames and
        // GObjObjects are already live once CalcSceneView is producing a gameplay eye, so resolve
        // them on the first eligible frame and let visible IK start immediately.
        if (!s_boot) {
            s_boot = true;
            if (!g_bpNames) BP_FindGNames();
            if (g_bpNames && !g_bpObjs) BP_FindObjects();
        }
        if (g_bpObjs) {
                if (g_boneWiggle) WigTick();
                if (g_boneWatch)  BwTick();
                if (g_skelProbe) {
                    static bool s_sk = false;
                    if (!s_sk) { s_sk = true; if (!g_wigCompN) WigFind(); SkelProbeRun(); }
                }
                if (g_skelProp) {
                    static bool s_sp = false;
                    if (!s_sp) { s_sp = true; SkelPropRun(); }
                }
                if (g_armDrive) {
                    // The old SkelControl objects were proven inert for the visible first-person
                    // component. Registry discovery stays here because VISIBLEIK uses its verified
                    // GNames/UObject metadata, but all arm writes now occur in Hook_PoseCompose.
                    static bool s_ad = false;
                    if (!s_ad) {
                        s_ad = true;
                        OLProxyLog(g_poseHookOk
                            ? "[OLVR][VISIBLEIK] registry ready; waiting for Miles_beheaded full skeletal update"
                            : "[OLVR][VISIBLEIK] pose hook unavailable; arm driving disabled safely");
                    }
                }
        }
    }
    __try {
        if (OLMenu_Visible()) {
            if (!g_menuLatched) {
                g_menuLatched = 1;
                for (int i = 0; i < 3; ++i) { g_menuLoc[i] = outLoc[i]; g_menuRot[i] = outRot[i]; }
            }
            for (int i = 0; i < 3; ++i) { outLoc[i] = g_menuLoc[i]; outRot[i] = g_menuRot[i]; }
        } else {
            g_menuLatched = 0;
        }
        // UE3 FRotator is int16-style units: 65536 = 360 deg. Left-handed, Z-up, X-forward,
        // Y-right. With roll ignored, the camera's right axis is (-sin(yaw), cos(yaw), 0).
        const float kToRad = 6.28318531f / 65536.0f;
        const float kToUnits = 65536.0f / 6.28318531f;

        // The head offset is applied in the BASE (gamepad) frame, so it must be read before the
        // head rotation goes in - that is what makes leaning sideways stay sideways relative to
        // where the stick is pointing, instead of rotating with your own head.
        // [BOB2] (Beta 1.1) the stride's rock and sway come out first, so the base yaw, the lean and
        // the hands all use the steadied camera.
        if (g_eyePhase == 1) BobStabilize(actor, outLoc, outRot, g_injW);
        outRot[0] += g_bobRotCorr[0]; outRot[1] += g_bobRotCorr[1];
        outLoc[0] += g_bobXYCorr[0];  outLoc[1] += g_bobXYCorr[1];
        const float baseYaw = (float)(outRot[1] & 0xFFFF) * kToRad;

        const float injW = g_injW;   // one read; the tick thread may move it between calls
        // [COMFORT] Gameplay only (the weight is 0 on the cinema screen): drop the game's own
        // camera roll, and take the walk-cycle bob out of the camera height.
        if (g_levelHorizon) {
            const int roll = (short)(outRot[2] & 0xFFFF);   // signed: 65200 is -2 deg, not +358
            outRot[2] = (int)((float)roll * (1.0f - injW));
        }
        if (g_eyePhase == 1) g_bobCorrZ = BobCorrection(outLoc[2], injW);
        outLoc[2] += g_bobCorrZ;
        if (g_headTrack && g_olPoseValid && injW > 0.0f) {
            outRot[0] += (int)(g_olPitchRad * injW * kToUnits);   // up is positive in both systems
            // YAW SIGN, corrected 2026-07-22 after left/right was found inverted in testing. UE3 yaw
            // rotates the forward vector from +X toward +Y - and in UE3 **+Y IS RIGHT** (the
            // line above says so), so increasing yaw turns you RIGHT. In OpenXR a left turn is a
            // positive rotation about +Y, which makes g_olYawRad = atan2(fwd.x,-fwd.z) NEGATIVE.
            // Left turn -> negative reading -> UE3 needs a negative delta to turn left. Straight
            // addition. (The first version subtracted: right derivation of the XR side, wrong
            // reading of which world direction UE3's +Y points.)
            outRot[1] += (int)(g_olYawRad   * injW * kToUnits);
            if (g_headRoll) outRot[2] += (int)(g_olRollRad * injW * kToUnits);   // off by default

            // [LEAN100] Positional tracking at the MEASURED scale (100 uu/m - see the note at
            // kMetersToUU; every earlier round rendered half the declared movement and the
            // mismatch is what read as swimming). Raw position, applied in the body (gamepad)
            // frame read before the head rotation, gated on the armed baseline. The declared
            // pose is untouched: at gain 1.0 declared == rendered by construction.
            if (g_headPos && OLXR_PosArmed()) {
                // Same numbers the arm solver uses ([PHASE1] ViewLeanM), crouch lift included;
                // with no lift this is exactly the previous clamp * LeanGain * injW.
                float lean[3]; ViewLeanM(lean);
                const float dR = lean[0] * kMetersToUU;                 // +right
                const float dU = lean[1] * kMetersToUU;                 // +up
                const float dF = -lean[2] * kMetersToUU;                // XR -z = forward, 1:1
                const float fx = cosf(baseYaw), fy = sinf(baseYaw);     // forward (body, pre-head)
                const float rx = -sinf(baseYaw), ry = cosf(baseYaw);    // right
                outLoc[0] += rx * dR + fx * dF;
                outLoc[1] += ry * dR + fy * dF;
                outLoc[2] += dU;
            }
        }

        // [INTERACT] (Test37) the head-centre world position and the body (gamepad) yaw of this frame's
        // view: what maps the tracked hands into the world for doors and markers.
        if (g_eyePhase == 1) {
            g_fvHead[0] = outLoc[0]; g_fvHead[1] = outLoc[1]; g_fvHead[2] = outLoc[2];
            g_fvBaseYaw = baseYaw; g_fvTick = GetTickCount64();
        }
        // Eye separation goes along the FINAL view direction, head rotation included.
        const float yaw = (float)(outRot[1] & 0xFFFF) * kToRad;
        const float erx = -sinf(yaw), ery = cosf(yaw);
        float side = (g_eyePhase == 2) ? 1.0f : -1.0f;
        float d = EyeHalfUU() * side * (g_eyeSwap ? -1.0f : 1.0f);   // (Beta 1.1) [WORLDSIZE] your IPD / world size
        outLoc[0] += erx * d;
        outLoc[1] += ery * d;
        // Z deliberately untouched: eyes separate horizontally in the world, never vertically.
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- the Outlast-custom occlusion tracker ------------------------------------------------
// Outlast added an engine source file stock UE3 does not have: PrimitiveSceneProxyOcclusionTracker
// (16 path strings in OLGame_R.exe). Ghidra (2026-07-22, this session): the tracker's
// UpdateAndGetCoverage - R 0x670940, fingerprinted into shipping at 0x29AEF0 by its two
// 0x20000000000 flag tests + the 0x7FC/0x800 config pair, then confirmed by a line-for-line
// decompile match - reads the primitive's hardware occlusion query, projects the bounds to
// screen, and smooths coverage into a SINGLE float at tracker+0x50. One value per OBJECT,
// not per view. Its 511-byte caller (a DrawDynamicElements-shaped virtual) SKIPS THE DRAW
// when it returns 0.
//
// That is the same-frame flicker mechanism: two views per frame feed one smoothed coverage
// two conflicting query answers, it oscillates, and discrete props (chairs/windows/doors -
// the primitives with these proxies) wink in and out. Ground/BSP has no such tracker and
// never flickers. AER is one view per frame = one answer = clean. It also explains why the
// per-eye view-state fixes (section 4.2/section 4.3 in the handoff) changed nothing: the shared value
// lives on the OBJECT, not in any view state.
//
// The test: force coverage = 1.0 and "updated" so nothing is ever skipped or faded by the
// tracker. If the flicker dies with this ON and returns with it OFF, the cause is confirmed
// and a view-aware gate can replace the blunt force. Costs the tracker's culling/fade while on.
typedef unsigned long long (__fastcall *OcclUpdate_t)(void*, void*, void*, void*,
                                                      unsigned long long, unsigned long long);
static OcclUpdate_t o_OcclUpdate = nullptr;
static uintptr_t g_occlRva = 0;
static volatile long g_occlForce = 0;      // seeded from the ini, OcclForce
static volatile long g_occlCalls = 0, g_occlForced = 0;

// FALSIFIED 2026-07-22 (live): never called in gameplay (calls stayed 0 across a full run).
// Kept as a passive counter so a scene that DOES use it would show up in the toggle log.
static unsigned long long __fastcall Hook_OcclUpdate(void* tracker, void* p2, void* p3, void* view,
                                                     unsigned long long p5, unsigned long long p6) {
    long n = InterlockedIncrement(&g_occlCalls);
    if (n == 1) OLProxyLog("[OLVR][OCCL] occlusion tracker is LIVE (first UpdateAndGetCoverage call)");
    return o_OcclUpdate(tracker, p2, p3, view, p5, p6);
}

// ---- precomputed visibility (PVS) --------------------------------------------------------
// 2026-07-22. The tracker hypothesis was FALSIFIED live: hook installed, force ON,
// calls=0 - the tracker is never invoked in gameplay. Video evidence instead: whole meshes
// (the window assembly) skipped in the RIGHT eye only, camera stationary, quasi-periodic.
//
// New mechanism, read out of OLGame_R 0x1111030 (SceneVisibility.cpp, PrecomputedVisibility
// strings): UE3's PRECOMPUTED per-cell visibility. The view's ORIGIN picks a baked cell; the
// cell's zlib chunk holds a bitfield of which static primitives are visible from that cell;
// the caller (InitViews) culls whole meshes with it. The eyes' origins differ by the stereo
// offset, so near a cell boundary - with the camcorder sway moving the camera - one eye's
// lookup lands in a different cell whose baked data hides the mesh. One view per frame (AER)
// = one verdict = clean; two views = two verdicts = props wink out in one eye.
//
// Fingerprinted into shipping at 0x8A7810 (unique match on the scene+0x5178 handler read +
// view+0x3A0 origin reads; decompile-verified, single caller = InitViews at 0x8CE050).
//
// The test: return NULL - the legitimate "no baked data for this cell" answer, which the
// caller must treat as fully visible. Shares the OcclForce flag with the tracker bypass above.
typedef long long (__fastcall *PvsGet_t)(void*, void*, void*);
static PvsGet_t o_PvsGet = nullptr;
static uintptr_t g_pvsRva = 0;
static volatile long g_pvsCalls = 0, g_pvsForced = 0;

// FALSIFIED 2026-07-22 (live): bypassed for whole toggle windows (forced counter ran),
// flicker unchanged. Passive counter only now.
static long long __fastcall Hook_PvsGet(void* state, void* view, void* scene) {
    long n = InterlockedIncrement(&g_pvsCalls);
    if (n == 1) OLProxyLog("[OLVR][PVS] precomputed visibility is LIVE (first lookup)");
    // [FRAMETAG] Render thread, once per view of the frame being drawn: the view is the renderer's
    // copy of the FSceneView, so its matrices identify which game frame this is.
    if (view) {
        __try { OLXR_RenderViewSeen((const char*)view + kViewMatOff); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return o_PvsGet(state, view, scene);
}

// OcclForce now arms exactly ONE thing: the d3d9-level hardware occlusion-query result
// force in d3d9_proxy.cpp (see [HWQ] there). One experiment at a time, per the standing rule.
extern "C" int OLUE3_OcclBypassOn() { return g_occlForce ? 1 : 0; }
extern "C" void OLUE3_SetOcclForce(int on) { g_occlForce = on ? 1 : 0; }
extern "C" void OLProxy_HwqStats(long* created, long* forced);

void OLUE3_ToggleOcclForce() {
    g_occlForce = g_occlForce ? 0 : 1;
    long qc = 0, qf = 0;
    OLProxy_HwqStats(&qc, &qf);
    char line[260];
    sprintf_s(line, "[OLVR][HWQ] occlusion-query bypass %s (queries created=%ld, results forced=%ld; "
                    "tracker calls=%ld, PVS calls=%ld) - %s",
              g_occlForce ? "ON" : "OFF", qc, qf, g_occlCalls, g_pvsCalls,
              g_occlForce ? "every query answers 'visible', nothing query-culled"
                          : "game's query culling back in charge");
    OLProxyLog(line);
}

// ---- diagnostic: which VIEW SLOT is the artifact attached to? ---------------------------
// CalcSceneView appends each view it builds to the family, so the FIRST one built becomes
// Views[0] (the primary) and the second becomes Views[1] (the secondary). If the flicker and
// colour drift really are "the engine under-processes the secondary view", then building the
// RIGHT eye first must move the artifact to the LEFT eye. If the artifact stays on the right
// regardless of slot, the secondary-view theory is wrong and the cause is on this mod's side --
// the eye offset, the capture, or the submit. One toggle, one run, and the answer redirects
// everything after it.
static volatile long g_buildRightFirst = 0;
extern "C" int OLUE3_BuildRightFirst() { return g_buildRightFirst ? 1 : 0; }

void OLUE3_ToggleBuildOrder() {
    g_buildRightFirst = g_buildRightFirst ? 0 : 1;
    char line[200];
    sprintf_s(line, "[OLVR][S3] build order: %s is now the PRIMARY view (Views[0])",
              g_buildRightFirst ? "RIGHT eye" : "LEFT eye");
    OLProxyLog(line);
}

// Live A/B between the two stereo mechanisms, so one run can answer whether the flicker needs
// two views to exist at all:
//   SPLIT = two views per frame (both eyes in one frame, half-width each)
//   AER   = ONE view per frame, full screen, camera alternating left/right
// If the flicker survives AER, it cannot be caused by having two views - and every remaining
// theory I have is about having two views.
void OLUE3_ToggleAer() {
    if (!g_drawLo) { OLProxyLog("[OLVR][AER] needs DrawRva - not available"); return; }
    g_aerMode = g_aerMode ? 0 : 1;
    if (g_aerMode) g_splitOn = 0;          // mutually exclusive
    char line[160];
    sprintf_s(line, "[OLVR] MODE = %s", g_aerMode ? "AER (one full-screen view per frame)"
                                                  : "off (mono, no stereo)");
    OLProxyLog(line);
}

void OLUE3_ToggleSplit() {
    g_splitOn = g_splitOn ? 0 : 1;
    if (g_splitOn) g_aerMode = 0;          // mutually exclusive
    char line[160];
    sprintf_s(line, "[OLVR][S3] SPLIT %s (frames split=%ld, failed=%ld)",
              g_splitOn ? "ON - expect side-by-side" : "off", g_splitFrames, g_splitFail);
    OLProxyLog(line);
}

// ---- SFR: Same-Frame Rendering ------------------------------------------------------------
// The split renders one PRIMARY view and one SECONDARY view into halves of a frame. UE3 does
// not treat those two equally - post-processing, exposure and assorted per-view effects get the
// full treatment on the primary and a partial one on the secondary. That is the whole family of
// "wrong on the right eye only" artifacts (ME1 hit it as right-eye bloom and black characters),
// and it is NOT fixable by giving the secondary its own view state - tried, it changed nothing.
//
// SFR renders the whole frame TWICE per present, each pass a full-screen PRIMARY view with the
// camera shifted for that eye. Neither pass is ever a secondary, so there is nothing to
// mistreat. Costs a second full render; buys correctness. This is the shape ME1 settled on
// permanently after fighting the same artifacts.
//
// Pass 1 (left) is copied off the backbuffer before pass 2 (right) overwrites it.
typedef void* (__fastcall *Draw_t)(void*, void*, void*);
static Draw_t o_Draw = nullptr;
static uintptr_t g_drawRva = 0;
static volatile long g_sfrMode = 0;      // set when Mode=sfr
static volatile long g_inDraw = 0;
static long g_sfrFrames = 0, g_sfrMiss = 0;

extern "C" int OLProxy_CaptureLeftEye();
extern "C" int OLUE3_StereoMode() { return g_aerMode ? 3 : (g_sfrMode ? 2 : (g_splitOn ? 1 : 0)); }

static void* __fastcall Hook_Draw(void* self, void* viewport, void* canvas) {
    if (!g_sfrMode || g_inDraw) return o_Draw(self, viewport, canvas);

    InterlockedExchange(&g_inDraw, 1);
    void* stateL = nullptr;

    // pass 1 - LEFT eye, full screen, primary view
    InterlockedExchange(&g_eyePhase, 1);
    o_Draw(self, viewport, canvas);
    int got = OLProxy_CaptureLeftEye();

    // pass 2 - RIGHT eye, full screen, primary view. Its own view state so the two passes do
    // not ping-pong each other's exposure/occlusion history.
    InterlockedExchange(&g_eyePhase, 2);
    void* lp = g_localPlayer;
    if (g_stateR && lp) { stateL = SafeReadPtr(lp, kViewStateOff); SafeWritePtr(lp, kViewStateOff, g_stateR); }
    void* r = o_Draw(self, viewport, canvas);
    if (stateL && lp) SafeWritePtr(lp, kViewStateOff, stateL);

    InterlockedExchange(&g_eyePhase, 0);
    InterlockedExchange(&g_inDraw, 0);

    if (got) { ++g_sfrFrames; InterlockedExchange(&g_splitFresh, 30); } else ++g_sfrMiss;
    if (g_sfrFrames == 1) OLProxyLog("[OLVR][SFR] first same-frame pair rendered (two full primary views)");
    if (g_sfrFrames && (g_sfrFrames % 600) == 0) {
        char l[160]; sprintf_s(l, "[OLVR][SFR] alive: pairs=%ld captureMiss=%ld", g_sfrFrames, g_sfrMiss);
        OLProxyLog(l);
    }
    return r;
}

// ---- AER: alternate-eye rendering ----------------------------------------------------------
// Evidence (2026-07-22): swapping which eye is built first did NOT move the flicker, so it is
// not the engine mistreating a secondary view. The artifact follows the right HALF OF THE
// SCREEN - UE3's post chain misbehaves in the second half of a side-by-side frame. ME1 hit the
// same thing and its notes rule out patching the SBS data side.
//
// So: stop rendering halves. AER renders ONE full-screen view per frame with the camera on the
// left eye, then the next frame on the right, and the headset holds the other eye's last image.
// Every frame is an ordinary full-viewport primary render - the exact thing the engine is happy
// with - which sidesteps both the half-viewport post bug and the double-Draw black screen.
//
// The cost is honest: the two eyes are one frame apart, so fast motion can shear or shimmer
// unless the frame rate is steady. That is a real trade, not a free win.
extern "C" int OLUE3_CurrentEye() { return g_aerEye; }

// ---- the hook -------------------------------------------------------------------------
// ---- [LENSFIT] each eye rendered with its own lens frustum (Test36) ---------------------------
// The Quest's eyes are not centred: Virtual Desktop reports -54/+40 deg (left eye) horizontally
// and about +44/-55 vertically. A symmetric camera has to reach the far edge of both, so about a
// third of every eye picture fell outside the lens and was cropped away, and the rest was
// stretched ~1.24x to fill the eye - GPU time spent on pixels nobody sees, and a softer picture.
// FSceneView's constructor (RVA 0x8BF640, called from CalcSceneView at 0x62B07D) copies the
// projection it is handed (arg 18; arg 17 is the view matrix - disassembly: [rsp+C8h] ->
// view+0xC0, read with movaps, so it must be 16-byte aligned) and derives everything else from it:
// frustum culling, view-projection, screen mapping. During the split's eye passes only, it now
// gets the eye's own off-centre frustum (UE3 row vectors, view space x right / y up / z forward):
//   x_ndc = P00*x/z + P20   P00 = 2/(r-l)  P20 = -(r+l)/(r-l)
//   y_ndc = P11*y/z + P21   P11 = 2/(u-d)  P21 = -(u+d)/(u-d)
// l,r,u,d = tangents of the eye the bridge routes this half to. Depth terms untouched. The bridge
// then maps each half 1:1 onto its eye ([LENSFIT] in ol_xr.cpp). [Render] LensFit=0 = the old
// symmetric render.
extern "C" void OLXR_FrameLatch();
extern "C" void OLXR_FrameCommit(float fovX, float fovY, int lensFit, int viewfinder, const float* key32);
extern "C" void OLXR_RenderViewSeen(const void* key128);
extern "C" int  OLXR_LensFitTangents(int half, float* l, float* r, float* u, float* d);
static volatile long g_lensFitEye[2] = { 0, 0 };
static volatile float g_lensP20[2] = { 0.0f, 0.0f }, g_lensP21[2] = { 0.0f, 0.0f };   // [SCISSORFIX] each half's off-centre terms
static volatile long g_lensFitFrame = 0;
static volatile long g_lensFitHookOk = 0;
extern "C" int OLUE3_LensFitLive() { return g_lensFitFrame ? 1 : 0; }
static const uintptr_t kSceneViewCtorRva = 0x8BF640;
static const uintptr_t kCalcSceneViewLo = 0x62A020, kCalcSceneViewHi = 0x62B290;
static const unsigned char kSceneViewCtorPrologue[15] = {
    0x48,0x89,0x4C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x30, 0x48,0xC7,0x44,0x24,0x20
};
typedef unsigned long long u64;
typedef void* (__fastcall *SceneViewCtor_t)(void*, void*, void*, u64,
    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64,
    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
static SceneViewCtor_t o_SceneViewCtor = nullptr;

// (Test43) [NVEYES] "In night vision there are black spots, and it displays with a small visual issue."
// Measured on the test headset: with each eye's off-centre lens frustum, the camcorder's night-vision
// lamp lit only the inner part of each eye - the left eye's left wall and the right eye's right wall
// stayed black, so the two eyes disagreed (the "visual issue"). The lamp is aimed once per frame,
// outside the eye passes (logged), and the light rectangles cover the whole eye, so it is the engine's
// per-view lighting that assumes a centred view. With symmetric eyes both are lit alike: while the
// game's night vision is up, the eyes render symmetric (like the zoomed viewfinder already did).
extern "C" int OLProxy_NightVisionActive();
static void* __fastcall Hook_SceneViewCtor(void* self, void* fam, void* st, u64 a4,
    u64 a5, u64 a6, u64 a7, u64 a8, u64 a9, u64 a10, u64 a11, u64 a12, u64 a13, u64 a14,
    u64 a15, u64 a16, u64 a17, u64 a18, u64 a19, u64 a20, u64 a21, u64 a22, u64 a23, u64 a24,
    u64 a25, u64 a26, u64 a27, u64 a28, u64 a29, u64 a30) {
    __declspec(align(16)) float lens[16];
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    const bool nvUp = OLProxy_NightVisionActive() != 0;
    { static bool s_nvWas = false;
      if (nvUp != s_nvWas) { s_nvWas = nvUp; OLProxyLog(nvUp ? "[OLVR][NVEYES] night vision up: symmetric eyes, so its lamp lights both alike"
                                                              : "[OLVR][NVEYES] night vision down: the lens-shaped eyes are back"); } }
    if (g_inSplit && g_eyePhase && a18 && OLInteract_ViewfinderZoom() <= 0.0f && !nvUp &&
        ra >= g_base + kCalcSceneViewLo && ra < g_base + kCalcSceneViewHi) {
        const int half = (g_eyePhase == 2) ? 1 : 0;
        float l = 0, r = 0, u = 0, d = 0;
        if (OLXR_LensFitTangents(half, &l, &r, &u, &d) &&
            SafeReadBytes((const void*)(uintptr_t)a18, lens, sizeof(lens)) &&
            lens[0] > 0.05f && lens[5] > 0.05f && lens[11] > 0.5f) {
            lens[0] = 2.0f / (r - l); lens[8] = -(r + l) / (r - l);
            lens[5] = 2.0f / (u - d); lens[9] = -(u + d) / (u - d);
            g_lensP20[half] = lens[8]; g_lensP21[half] = lens[9];
            a18 = (u64)(uintptr_t)lens;
            g_lensFitEye[half] = 1;
            static long s_logged = 0;
            if (InterlockedIncrement(&s_logged) <= 2) {
                char line[220];
                sprintf_s(line, "[OLVR][LENSFIT] half %d rendered with its eye's frustum: %.1f/%.1f deg horizontal, "
                                "%.1f/%.1f vertical (P20=%.3f P21=%.3f)", half,
                          atanf(l) * 57.2957795f, atanf(r) * 57.2957795f, atanf(u) * 57.2957795f,
                          atanf(d) * 57.2957795f, lens[8], lens[9]);
                OLProxyLog(line);
            }
        }
    }
    return o_SceneViewCtor(self, fam, st, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16,
                           a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30);
}

// [SCISSORFIX] (Test43) "In night vision there are black spots, and it displays with a small visual
// issue." UE3 limits each light's drawing to a screen rectangle it works out from the light's bounds
// (RHISetScissorRect - FD3D9DynamicRHI::SetScissorRect, RVA 0xBF4C00, in the RHI's vtable) - and that
// sum assumes a CENTRED projection. [LENSFIT] gives each eye its lens's off-centre frustum, so every
// light rectangle landed shifted by the eye's off-centre term: the camcorder's night-vision lamp drew
// only part of its pool - a black wedge, a different one in each eye (measured on the test headset:
// the left eye lit the corridor's right wall, the right eye left it black; with LensFit off both lit
// it). A rectangle inside one eye's half is now moved by that eye's own off-centre (P20, P21) - where
// the light really is - with a margin. (dev "scissorfix": 0 off, 1 moved, 2 the whole eye)
typedef void (__fastcall *SetScissor_t)(void* rhi, unsigned en, unsigned x0, unsigned y0, unsigned x1, unsigned y1);
static SetScissor_t o_SetScissor = nullptr;
// Tried for the night-vision black wedge (Test43) and ruled out (the lamp's rectangle is the whole eye):
// off by default; kept for testing other lights.
static volatile long g_scissorFix = 0, g_scCalls = 0, g_scMoved = 0;
static const uintptr_t kSetScissorRva = 0xBF4C00;
static const unsigned char kSetScissorPrologue[10] = { 0x40,0x53,0x57,0x48,0x83,0xEC,0x48,0x48,0x8B,0x05 };
extern "C" void OLUE3_DevScissorFix(int m) {
    g_scissorFix = (m >= 0 && m <= 2) ? m : 1;
    char l[120]; sprintf_s(l, "[OLVR][SCISSORFIX] (dev) mode %ld (0 off, 1 moved to the eye's centre, 2 the whole eye)", g_scissorFix); OLProxyLog(l);
}
extern "C" void OLUE3_ScissorReport();
static void __fastcall Hook_SetScissor(void* rhi, unsigned en, unsigned x0, unsigned y0, unsigned x1, unsigned y1) {
    InterlockedIncrement(&g_scCalls);
    OLUE3_ScissorReport();
    const long fix = g_scissorFix;
    if (en && fix && g_lensFitFrame && x1 > x0 && y1 > y0) {
        const long W = g_hiResAppliedW > 64 ? g_hiResAppliedW : g_hiResW, H = g_hiResAppliedH > 64 ? g_hiResAppliedH : g_hiResH;
        const long half = W / 2;
        if (W > 64 && H > 64) {
            const int e = (long)x0 >= half ? 1 : 0;
            const long ex0 = e ? half : 0, ex1 = e ? W : half;
            if ((long)x1 <= ex1 + 1 && (long)y1 <= H + 1 && !((long)x0 <= ex0 && (long)x1 >= ex1 && y0 == 0 && (long)y1 >= H)) {
                long nx0, nx1, ny0, ny1;
                if (fix == 2) { nx0 = ex0; nx1 = ex1; ny0 = 0; ny1 = H; }
                else {
                    const long sx = (long)(g_lensP20[e] * (float)half * 0.5f), sy = (long)(-g_lensP21[e] * (float)H * 0.5f), m = 32;
                    nx0 = (long)x0 + sx - m; nx1 = (long)x1 + sx + m; ny0 = (long)y0 + sy - m; ny1 = (long)y1 + sy + m;
                }
                if (nx0 < ex0) nx0 = ex0; if (nx1 > ex1) nx1 = ex1; if (ny0 < 0) ny0 = 0; if (ny1 > H) ny1 = H;
                if (nx1 > nx0 && ny1 > ny0) { x0 = (unsigned)nx0; x1 = (unsigned)nx1; y0 = (unsigned)ny0; y1 = (unsigned)ny1; InterlockedIncrement(&g_scMoved); }
            }
        }
    }
    o_SetScissor(rhi, en, x0, y0, x1, y1);
}
extern "C" void OLUE3_ScissorReport() {
    if (!g_scissorFix) return;
    static ULONGLONG s_ms = 0; const ULONGLONG now = GetTickCount64();
    if (now - s_ms < 15000) return;
    s_ms = now;
    char l[200]; sprintf_s(l, "[OLVR][SCISSORFIX] light rectangles /15 s: %ld set, %ld moved to their eye's lens (mode %ld, P20 %.3f / %.3f)",
                           InterlockedExchange(&g_scCalls, 0), InterlockedExchange(&g_scMoved, 0), g_scissorFix, g_lensP20[0], g_lensP20[1]);
    OLProxyLog(l);
}

static void* __fastcall Hook_CalcSceneView(void* self, void* family, void* outLoc, void* outRot,
                                           void* viewport, void* drawer) {
    long n = InterlockedIncrement(&g_calls);
    g_localPlayer = self;

    // AER: single full-screen render, camera alternating per frame. No rect rewrite, no second
    // CalcSceneView call - the frame is exactly what the game would render on its own, just from
    // one eye's position.
    if (g_aerMode) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        if (ra >= g_drawLo && ra < g_drawHi) {
            long f = InterlockedIncrement(&g_aerFrame);
            long eye = (f & 1) ? 1 : 0;
            InterlockedExchange(&g_aerEye, eye);
            InterlockedExchange(&g_eyePhase, eye ? 2 : 1);   // shifts the camera +/- halfEye
            void* v = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
            InterlockedExchange(&g_eyePhase, 0);
            CaptureRenderedFov(v);
            InterlockedExchange(&g_splitFresh, 30);
            if (f == 1) OLProxyLog("[OLVR][AER] alternate-eye rendering live (full-screen per frame)");
            return v;
        }
    }

    // SFR: no rect rewrite at all - each pass is already a full-screen primary view. Only the
    // eye offset (applied in GetPlayerViewPoint) and the rendered FOV are needed.
    if (g_sfrMode) {
        if (!g_stateTried && o_AllocViewState) {
            g_stateTried = true;
            __try { g_stateR = o_AllocViewState(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_stateR = nullptr; }
            char l[160];
            sprintf_s(l, "[OLVR][SFR] second view state %s (%p)", g_stateR ? "allocated" : "FAILED", g_stateR);
            OLProxyLog(l);
        }
        void* v = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
        CaptureRenderedFov(v);
        return v;
    }

    // [THEATER] Paused, or the settings menu open: render ONE normal view, exactly like the main
    // menu, and drop the split freshness at once so the headset switches to the theater screen
    // on this very frame instead of showing a flat frame as two eyes for half a second.
    if (g_splitOn && !g_inSplit && SceneWantsTheater()) {
        InterlockedExchange(&g_splitFresh, 0);
        static long s_monoLogged = 0;
        if (InterlockedIncrement(&s_monoLogged) == 1)
            OLProxyLog("[OLVR][THEATER] paused/menu: flat single view for the theater screen");
        return o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
    }
    if (g_splitOn && !g_inSplit) {
        uintptr_t ra = (uintptr_t)_ReturnAddress();
        if (ra >= g_drawLo && ra < g_drawHi) {
            float save[4] = { 0, 0, 0, 0 };
            if (SafeReadBytes((char*)self + kRectOff, save, sizeof(save)) &&
                save[2] > 0.9f && save[3] > 0.9f) {          // only split a full-viewport player
                InterlockedExchange(&g_inSplit, 1);
                OLXR_FrameLatch();                    // [FRAMETAG] both eyes from one frozen pose
                g_lensFitEye[0] = g_lensFitEye[1] = 0;
                const float L[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
                const float R[4] = { 0.5f, 0.0f, 0.5f, 1.0f };
                void* left = nullptr; void* right = nullptr;
                void* stateL = SafeReadPtr(self, kViewStateOff);   // the game's own state = left eye
                if (!g_stateTried && o_AllocViewState && stateL) {
                    g_stateTried = true;
                    __try { g_stateR = o_AllocViewState(); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { g_stateR = nullptr; }
                    char l[160];
                    sprintf_s(l, "[OLVR][S3] right-eye view state %s (%p) - each eye now keeps its own "
                                 "exposure/occlusion history", g_stateR ? "ALLOCATED" : "alloc FAILED", g_stateR);
                    OLProxyLog(l);
                }

                const float* firstRect  = g_buildRightFirst ? R : L;
                const float* secondRect = g_buildRightFirst ? L : R;
                const long   firstPhase  = g_buildRightFirst ? 2 : 1;
                const long   secondPhase = g_buildRightFirst ? 1 : 2;
                void** firstOut  = g_buildRightFirst ? &right : &left;
                void** secondOut = g_buildRightFirst ? &left  : &right;

                if (SafeWriteRect(self, firstRect)) {
                    InterlockedExchange(&g_eyePhase, firstPhase);
                    *firstOut = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
                }
                if (SafeWriteRect(self, secondRect)) {
                    InterlockedExchange(&g_eyePhase, secondPhase);
                    if (g_stateR) SafeWritePtr(self, kViewStateOff, g_stateR);
                    *secondOut = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
                }
                InterlockedExchange(&g_eyePhase, 0);
                if (g_stateR && stateL) SafeWritePtr(self, kViewStateOff, stateL);   // ALWAYS restore
                SafeWriteRect(self, save);                    // ALWAYS restore, even on failure
                InterlockedExchange(&g_inSplit, 0);
                if (left && right) {
                    ++g_splitFrames;
                    CaptureRenderedFov(right);       // both eyes share it; right is the last built
                    InterlockedExchange(&g_splitFresh, 30);
                    // [LENSFIT] the frame counts as lens-shaped only if BOTH eyes got their frustum.
                    g_lensFitFrame = (g_lensFitEye[0] && g_lensFitEye[1]) ? 1 : 0;
                    // [FRAMETAG] what this frame's views were built with, keyed by the left view's
                    // view+projection matrices (the render thread finds the frame by them).
                    float key[32];
                    if (SafeReadBytes((char*)left + kViewMatOff, key, sizeof(key)))
                        OLXR_FrameCommit(g_fovX, g_fovY, g_lensFitFrame, OLInteract_ViewfinderZoom() > 0.0f, key);
                    // [INTERACT] (Test37) once per stereo frame, right after this frame's head
                    // position and body yaw were recorded: doors, raise-to-eye, battery, markers.
                    DevGameThreadWork();
                    OLInteract_Tick();
                } else ++g_splitFail;
                if (g_splitFrames == 1)
                    OLProxyLog("[OLVR][S3] first split done - two views built in one frame");
                if ((g_splitFrames % 600) == 0) {
                    char l[160]; sprintf_s(l, "[OLVR][S3] split alive: ok=%ld fail=%ld", g_splitFrames, g_splitFail);
                    OLProxyLog(l);
                }
                if (left) return left;                        // hand the engine the LEFT view
            }
        }
    }

    // Pass-through path. Also the layout dump: the first few calls log where the rect and the
    // FSceneView matrices actually live, so the offsets above are re-confirmed on every run
    // rather than trusted from a previous session.
    {
        void* view = o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
        if (n <= 3 && InterlockedIncrement(&g_logged) <= 3) {
            char line[256];
            sprintf_s(line, "[OLVR][S3] CalcSceneView call#%ld this=%p family=%p viewport=%p -> view=%p", n, self, family, viewport, view);
            OLProxyLog(line);
            {   // which view-state source is live?
                unsigned char flag = 0; SafeReadBytes((char*)self + 0xC8, &flag, 1);
                unsigned int gflag = 0; SafeReadBytes((const void*)(g_base + 0x200A3DC), &gflag, 4);
                sprintf_s(line, "[OLVR][S3] viewstate branch: this+0xC8&1=%u globalFlag=%u -> %s "
                                "(sharedGetter hits=%ld, oursReturned=%ld)",
                          (unsigned)(flag & 1), gflag,
                          ((flag & 1) == 0 || gflag != 0) ? "LocalPlayer+0xB0" : "SHARED global",
                          g_sharedHits, g_sharedGaveOurs);
                OLProxyLog(line);
            }
            ScanForRect(self);
            ScanSceneView(view);
        }
        if ((n % 600) == 0) { char l[128]; sprintf_s(l, "[OLVR][S3] CalcSceneView calls=%ld (alive)", n); OLProxyLog(l); }
        return view;
    }

    return o_CalcSceneView(self, family, outLoc, outRot, viewport, drawer);
}

// ---- setup ----------------------------------------------------------------------------
// An RVA is only valid for the exact build it came from. Hooking base+RVA in the WRONG exe
// lands on an arbitrary function and crashes the game. Two gates before anything is touched:
//   1. the running exe's filename must match the ini's Exe= key
//   2. the bytes at base+RVA must match the ini's Prologue= key (as read from that exe on disk)
static wchar_t g_wantExe[64] = { 0 };
static unsigned char g_wantBytes[32] = { 0 };
static int g_wantLen = 0;

static bool ExeMatches() {
    if (!g_wantExe[0]) return true;               // no constraint set
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return _wcsicmp(name, g_wantExe) == 0;
}
static bool PrologueMatches(const void* target) {
    if (g_wantLen <= 0) return true;              // no constraint set
    unsigned char got[32] = { 0 };
    if (!SafeReadBytes(target, got, (size_t)g_wantLen)) return false;
    return memcmp(got, g_wantBytes, (size_t)g_wantLen) == 0;
}
static int ParseHexInto(const wchar_t* hex, unsigned char* out, int cap) {
    int len = 0;
    for (const wchar_t* p = hex; p[0] && p[1] && len < cap; ) {
        if (*p == L' ') { ++p; continue; }
        wchar_t byte[3] = { p[0], p[1], 0 };
        out[len++] = (unsigned char)wcstoul(byte, nullptr, 16);
        p += 2;
    }
    return len;
}
static void ParseHex(const wchar_t* hex) { g_wantLen = ParseHexInto(hex, g_wantBytes, 32); }

// The occlusion-tracker hook has its own prologue gate: a second RVA means a second chance to
// hook garbage on a wrong build, so it gets the same exe+prologue treatment as CalcSceneView.
static unsigned char g_occlWantBytes[32] = { 0 };
static int g_occlWantLen = 0;
static unsigned char g_pvsWantBytes[32] = { 0 };
static int g_pvsWantLen = 0;

// (Test45) The Wwise hooks (ol_sound.cpp) only serve cover-your-mouth, which is off and out of the menus:
// they are not installed unless [Motion] CoverMouth=1.
static long g_soundWanted = 0;
static void ReadConfig() {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%soutlastvr.ini", exe);

    wchar_t val[64] = { 0 };
    g_soundWanted = GetPrivateProfileIntW(L"Motion", L"CoverMouth", 0, ini) ? 1 : 0;   // (Test45) [SOUND]
    GetPrivateProfileStringW(L"Stage3", L"CalcSceneViewRva", L"0", val, 64, ini);
    g_cvRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"Mode", L"probe", val, 64, ini);
    g_splitMode = (_wcsicmp(val, L"split") == 0);
    g_sfrMode   = (_wcsicmp(val, L"sfr") == 0) ? 1 : 0;
    g_aerMode   = (_wcsicmp(val, L"aer") == 0) ? 1 : 0;
    GetPrivateProfileStringW(L"Stage3", L"SharedStateRva", L"0", val, 64, ini);
    g_sharedRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"StateCreatorRva", L"0", val, 64, ini);
    g_creatorRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"AllocViewStateRva", L"0", val, 64, ini);
    g_avsRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"HalfEyeUU", L"1.6", val, 64, ini);
    g_halfEyeUU = (float)_wtof(val);
    GetPrivateProfileStringW(L"Render", L"WorldSize", L"1.00", val, 64, ini);   // (Beta 1.1) [WORLDSIZE]
    OLUE3_SetWorldSize((float)_wtof(val));
    GetPrivateProfileStringW(L"Stage3", L"WorldToMeters", L"50", val, 64, ini);
    { float w = (float)_wtof(val); if (w >= 5.0f && w <= 500.0f) g_worldToMeters = w; }
    g_headTrack = GetPrivateProfileIntW(L"Stage3", L"HeadTracking", 1, ini) ? 1 : 0;
    g_headRoll  = GetPrivateProfileIntW(L"Stage3", L"HeadRoll", 0, ini) ? 1 : 0;
    g_headPos   = GetPrivateProfileIntW(L"Stage3", L"HeadPosition", 1, ini) ? 1 : 0;
    g_bodyTurn  = GetPrivateProfileIntW(L"Motion", L"BodyFollowsHead", 1, ini) ? 1 : 0;   // (Beta 1.1) [BODYTURN]
    g_camProbe  = GetPrivateProfileIntW(L"Stage3", L"CamProbe", 0, ini) ? 1 : 0;   // [CAMPROBE] dev only
    g_boneProbe = GetPrivateProfileIntW(L"Stage3", L"BoneProbe", 0, ini) ? 1 : 0;  // [BONEPROBE] dev only
    g_boneWiggle = GetPrivateProfileIntW(L"Stage3", L"BoneWiggle", 0, ini);        // [WIGGLE] dev only
    g_boneWatch = GetPrivateProfileIntW(L"Stage3", L"BoneWatch", 0, ini) ? 1 : 0;  // [BONEWATCH] dev only
    g_skelProbe = GetPrivateProfileIntW(L"Stage3", L"SkelCtrlProbe", 0, ini) ? 1 : 0;  // [SKELCTRL] dev only
    g_skelProp = GetPrivateProfileIntW(L"Stage3", L"SkelPropProbe", 0, ini) ? 1 : 0;  // [SKELPROP] dev only
    g_armDrive = GetPrivateProfileIntW(L"Stage3", L"ArmDrive", 0, ini) ? 1 : 0;   // [ARMDRIVE]
    // [ARMTUNE] Test33 arm-tracking corrections. Missing keys = 0 = exact Test16 behaviour.
    g_armHandSync = GetPrivateProfileIntW(L"Stage3", L"ArmHandSync", 0, ini) ? 1 : 0;
    g_armLeanFix  = GetPrivateProfileIntW(L"Stage3", L"ArmLeanFix", 0, ini) ? 1 : 0;
    g_armSmoothDt = GetPrivateProfileIntW(L"Stage3", L"ArmSmoothDt", 0, ini) ? 1 : 0;
    // [HIRES] missing keys = the old desktop-sized render
    g_hiResOn = GetPrivateProfileIntW(L"Render", L"HighResRender", 0, ini) ? 1 : 0;
    g_hiResEyeW = (long)GetPrivateProfileIntW(L"Render", L"EyeWidth", 1728, ini);
    g_hiResEyeH = (long)GetPrivateProfileIntW(L"Render", L"EyeHeight", 1824, ini);
    { wchar_t rv[32]; GetPrivateProfileStringW(L"Render", L"RenderScale", L"1.0", rv, 32, ini);
      g_hiResScale = (float)_wtof(rv); }
    HiResRecompute();
    g_lookUse = GetPrivateProfileIntW(L"Input", L"LookToUse", 0, ini);   // [LOOKUSE] missing = off
    if (g_lookUse < 0 || g_lookUse > 2) g_lookUse = 0;
    // [COMFORT] Missing keys = off = the previous camera exactly.
    g_levelHorizon = GetPrivateProfileIntW(L"Comfort", L"LevelHorizon", 0, ini) ? 1 : 0;
    { wchar_t bv[32]; GetPrivateProfileStringW(L"Comfort", L"ReduceHeadBob", L"1", bv, 32, ini);
      OLUE3_SetBobReduce((float)_wtof(bv)); }
    GetPrivateProfileStringW(L"Stage3", L"LeanGain", L"1.0", val, 64, ini);
    OLUE3_SetLeanGain((float)_wtof(val));
    GetPrivateProfileStringW(L"Stage3", L"GetViewPointRva", L"0", val, 64, ini);
    g_vpRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"DrawRva", L"0", val, 64, ini);
    uintptr_t drawRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"DrawSize", L"0", val, 64, ini);
    uintptr_t drawSize = (uintptr_t)wcstoull(val, nullptr, 0);
    g_drawRva = drawRva;
    g_drawLo = drawRva ? g_base + drawRva : 0;
    g_drawHi = drawRva ? g_base + drawRva + drawSize : 0;
    GetPrivateProfileStringW(L"Stage3", L"Exe", L"", g_wantExe, 64, ini);
    wchar_t hex[80] = { 0 };
    GetPrivateProfileStringW(L"Stage3", L"Prologue", L"", hex, 80, ini);
    ParseHex(hex);
    GetPrivateProfileStringW(L"Stage3", L"OcclRva", L"0", val, 64, ini);
    g_occlRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"OcclPrologue", L"", hex, 80, ini);
    g_occlWantLen = ParseHexInto(hex, g_occlWantBytes, 32);
    g_occlForce = GetPrivateProfileIntW(L"Stage3", L"OcclForce", 0, ini) ? 1 : 0;
    g_cineFovOn = GetPrivateProfileIntW(L"Stage3", L"CineFov", 1, ini) ? 1 : 0;   // [CINEFOV]
    g_wideFov = GetPrivateProfileIntW(L"Render", L"WideFov", 0, ini) ? 1 : 0;     // [WIDEFOV]
    g_grainOn = GetPrivateProfileIntW(L"Render", L"FilmGrain", 0, ini) ? 1 : 0;   // (Test47) [GRAIN]
    if (g_wideFov) OLProxyLog("[OLVR][WIDEFOV] ON - rendering at the headset's own width "
                              "(costs sharpness, fills the eye; [Render] WideFov=0 reverts)");
    GetPrivateProfileStringW(L"Stage3", L"PvsRva", L"0", val, 64, ini);
    g_pvsRva = (uintptr_t)wcstoull(val, nullptr, 0);
    GetPrivateProfileStringW(L"Stage3", L"PvsPrologue", L"", hex, 80, ini);
    g_pvsWantLen = ParseHexInto(hex, g_pvsWantBytes, 32);

    char line[512];
    char inia[MAX_PATH]; size_t n = 0; wcstombs_s(&n, inia, ini, _TRUNCATE);
    sprintf_s(line, "[OLVR][S3] config %s -> CalcSceneViewRva=0x%llx mode=%s drawRange=[%p,%p)",
              inia, (unsigned long long)g_cvRva, g_splitMode ? "split" : "probe",
              (void*)g_drawLo, (void*)g_drawHi);
    OLProxyLog(line);
}

void OLUE3_Init() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    char line[256];
    OLProxyLog("[OLVR] BUILD=OUTLAST1VR-BETA-1.1-20261022 (BETA 1.1: Quest Link picture where you look (true pose declared), head bob fully off incl. running, world size + your own IPD, focus 0, body turns with you, doors let go when you turn away + arm resets, doors stop at Miles, auto ledge grab option, hand tucked at the eye, big battery gauge, recenter hint; Test49: camcorder GRIP BUTTON option (seated play); Test48: sprint + A vaults, a running squeeze at the hip is sprint not an edge, stick drift fix + controller layout in the log (Pimax / SteamVR); Test47: streaming freezes gone (texture copies in RAM), film grain switch, arms after Trager, readouts in from the edges; Test46: Quest Link fixed (lens-shaped eyes off there), render thread ~1/3 faster - monitor at 30 fps + forced occlusion tests skipped, turn speed through the game's own turn (arms stay right) + quick snap, live screen off for now, zoom fill without double vision, zoom only on a clear up/down push and filling the view, vault fix, async log + hitch times; Test45: camcorder aims with the controller + adjustable, ladders without the barrier, smoother scan; Test44: walk around for real; Test43: physical climbing, free-hold camcorder with a live screen, door scares fire, cover your mouth)");
    sprintf_s(line, "[OLVR][S3] module base=%p", (void*)g_base);
    OLProxyLog(line);

    ReadConfig();
    if (!g_cvRva) { OLProxyLog("[OLVR][S3] no CalcSceneViewRva set -> Stage 3 idle (flat XR only)"); return; }

    if (!ExeMatches()) {
        char want[64]; size_t wn = 0; wcstombs_s(&wn, want, g_wantExe, _TRUNCATE);
        sprintf_s(line, "[OLVR][S3] SKIPPED: this RVA belongs to %s, not the exe that's running", want);
        OLProxyLog(line); return;
    }
    void* target = (void*)(g_base + g_cvRva);
    if (!PrologueMatches(target)) {
        OLProxyLog("[OLVR][S3] SKIPPED: bytes at base+RVA do not match the expected prologue (wrong build?)");
        return;
    }
    OLProxyLog("[OLVR][S3] target verified (exe + prologue match)");
    if (g_aerMode && !g_drawLo) {
        OLProxyLog("[OLVR][AER] Mode=aer needs DrawRva to identify the render call site -> AER off");
        g_aerMode = 0;
    }
    if (g_aerMode) OLProxyLog("[OLVR][AER] mode=aer: one full-screen eye per frame");
    if (g_splitMode && !g_drawLo) {
        OLProxyLog("[OLVR][S3] Mode=split but no DrawRva set -> split DISABLED (it would split "
                   "every call site, including the query views). Set DrawRva/DrawSize.");
        g_splitMode = false;
    }
    g_splitOn = g_splitMode ? 1 : 0;
    // ALREADY_INITIALIZED is success, not failure: the resolution hooks in d3d9_proxy.cpp run
    // first and initialise MinHook. Treating it as fatal here skipped the ENTIRE engine hook,
    // which killed the split and left only a mono flat panel - "no VR anymore", 2026-07-22.
    {
        MH_STATUS ms = MH_Initialize();
        if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
            char l[128]; sprintf_s(l, "[OLVR][S3] MH_Initialize FAILED (status=%d)", (int)ms);
            OLProxyLog(l); return;
        }
    }
    if (MH_CreateHook(target, (void*)&Hook_CalcSceneView, (void**)&o_CalcSceneView) != MH_OK) {
        sprintf_s(line, "[OLVR][S3] MH_CreateHook FAILED at %p", target); OLProxyLog(line); return;
    }
    if (MH_EnableHook(target) != MH_OK) { OLProxyLog("[OLVR][S3] MH_EnableHook FAILED"); return; }
    sprintf_s(line, "[OLVR][S3] CalcSceneView hooked at %p (base+0x%llx) mode=%s",
              target, (unsigned long long)g_cvRva, g_splitMode ? "split" : "probe");
    OLProxyLog(line);

    // Final visible-pose composer. This exact-build hook is independently byte-gated; if the
    // executable differs, controller IK stands down while every renderer/camera feature remains.
    if (g_armDrive) {
        void* pc = (void*)(g_base + kPoseComposeRva);
        unsigned char have[sizeof(kPoseComposePrologue)] = {};
        if (!SafeReadBytes(pc, have, sizeof(have)) ||
            memcmp(have, kPoseComposePrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][VISIBLEIK] pose composer prologue MISMATCH at %p - IK disabled", pc);
        } else if (MH_CreateHook(pc, (void*)&Hook_PoseCompose, (void**)&o_PoseCompose) == MH_OK &&
                   MH_EnableHook(pc) == MH_OK) {
            InterlockedExchange(&g_poseHookOk, 1);
            sprintf_s(line, "[OLVR][VISIBLEIK] final pose composer hooked at base+0x%llX",
                      (unsigned long long)kPoseComposeRva);
        } else {
            sprintf_s(line, "[OLVR][VISIBLEIK] final pose composer hook FAILED at %p - IK disabled", pc);
        }
        OLProxyLog(line);

        // Hook the complete skeletal update as the ownership point. Runtime evidence from Test 8
        // proved that Miles's visible first-person component does not necessarily enter the
        // partial composer hook above, while static disassembly shows every complete pose update
        // ending here before render extraction.
        void* su = (void*)(g_base + kSkelUpdateRva);
        unsigned char suHave[sizeof(kSkelUpdatePrologue)] = {};
        if (!SafeReadBytes(su, suHave, sizeof(suHave)) ||
            memcmp(suHave, kSkelUpdatePrologue, sizeof(suHave)) != 0) {
            sprintf_s(line, "[OLVR][VISIBLEIK] UpdateSkelPose prologue MISMATCH at %p - IK disabled", su);
        } else if (MH_CreateHook(su, (void*)&Hook_SkelUpdate, (void**)&o_SkelUpdate) == MH_OK &&
                   MH_EnableHook(su) == MH_OK) {
            InterlockedExchange(&g_skelUpdateHookOk, 1);
            sprintf_s(line, "[OLVR][VISIBLEIK] final UpdateSkelPose hooked at base+0x%llX",
                      (unsigned long long)kSkelUpdateRva);
        } else {
            sprintf_s(line, "[OLVR][VISIBLEIK] UpdateSkelPose hook FAILED at %p - IK disabled", su);
        }
        OLProxyLog(line);
    }

    // [HIRES] Engine render size. Installed as early as Stage 3 allows; if the viewport already
    // exists, the proxy nudges a resize once the device is up (OLProxy side).
    if (g_hiResOn) {
        void* uv = (void*)(g_base + kUpdateViewportRhiRva);
        unsigned char have[sizeof(kUpdateViewportRhiPrologue)] = {};
        if (!SafeReadBytes(uv, have, sizeof(have)) || memcmp(have, kUpdateViewportRhiPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][HIRES] UpdateViewportRHI prologue MISMATCH at %p - desktop-sized render", uv);
        } else if (MH_CreateHook(uv, (void*)&Hook_UpdateViewportRHI, (void**)&o_UpdateViewportRHI) == MH_OK &&
                   MH_EnableHook(uv) == MH_OK) {
            InterlockedExchange(&g_hiResHookOk, 1);
            sprintf_s(line, "[OLVR][HIRES] UpdateViewportRHI hooked at base+0x%llX - target %ldx%ld (%ldx%ld per eye, scale %.2f)",
                      (unsigned long long)kUpdateViewportRhiRva, g_hiResW, g_hiResH, g_hiResW / 2, g_hiResH, g_hiResScale);
        } else {
            sprintf_s(line, "[OLVR][HIRES] UpdateViewportRHI hook FAILED at %p", uv);
        }
        OLProxyLog(line);
    }

    // [SCISSORFIX] (Test43) light rectangles follow the off-centre eyes
    {
        void* ss = (void*)(g_base + kSetScissorRva);
        unsigned char have[sizeof(kSetScissorPrologue)] = {};
        if (!SafeReadBytes(ss, have, sizeof(have)) || memcmp(have, kSetScissorPrologue, sizeof(have)) != 0)
            sprintf_s(line, "[OLVR][SCISSORFIX] SetScissorRect prologue MISMATCH at %p - light rectangles left alone", ss);
        else if (MH_CreateHook(ss, (void*)&Hook_SetScissor, (void**)&o_SetScissor) == MH_OK && MH_EnableHook(ss) == MH_OK)
            sprintf_s(line, "[OLVR][SCISSORFIX] SetScissorRect hooked at base+0x%llX (pass-through; dev \"scissorfix\" tries moved light rectangles)", (unsigned long long)kSetScissorRva);
        else
            sprintf_s(line, "[OLVR][SCISSORFIX] SetScissorRect hook FAILED at %p", ss);
        OLProxyLog(line);
    }

    // [LENSFIT] FSceneView constructor: a pass-through except inside the split's own eye passes.
    {
        void* sv = (void*)(g_base + kSceneViewCtorRva);
        unsigned char have[sizeof(kSceneViewCtorPrologue)] = {};
        if (!SafeReadBytes(sv, have, sizeof(have)) || memcmp(have, kSceneViewCtorPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][LENSFIT] FSceneView constructor prologue MISMATCH at %p - symmetric eyes", sv);
        } else if (MH_CreateHook(sv, (void*)&Hook_SceneViewCtor, (void**)&o_SceneViewCtor) == MH_OK &&
                   MH_EnableHook(sv) == MH_OK) {
            InterlockedExchange(&g_lensFitHookOk, 1);
            sprintf_s(line, "[OLVR][LENSFIT] FSceneView constructor hooked at base+0x%llX",
                      (unsigned long long)kSceneViewCtorRva);
        } else {
            sprintf_s(line, "[OLVR][LENSFIT] FSceneView constructor hook FAILED at %p - symmetric eyes", sv);
        }
        OLProxyLog(line);
    }

    // [LOOKUSE] Interaction aim. Same verify-then-hook discipline as everything else here.
    {
        void* npm = (void*)(g_base + kNativePlayerMoveRva);
        unsigned char have[sizeof(kNativePlayerMovePrologue)] = {};
        if (!SafeReadBytes(npm, have, sizeof(have)) ||
            memcmp(have, kNativePlayerMovePrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][LOOKUSE] NativePlayerMove prologue MISMATCH at %p - interactions stay body-aimed", npm);
        } else if (MH_CreateHook(npm, (void*)&Hook_NativePlayerMove, (void**)&o_NativePlayerMove) == MH_OK &&
                   MH_EnableHook(npm) == MH_OK) {
            InterlockedExchange(&g_lookUseHookOk, 1);
            sprintf_s(line, "[OLVR][LOOKUSE] NativePlayerMove hooked at base+0x%llX (mode %ld: %s)",
                      (unsigned long long)kNativePlayerMoveRva, g_lookUse,
                      g_lookUse == 2 ? "right hand" : (g_lookUse == 1 ? "head" : "off"));
        } else {
            sprintf_s(line, "[OLVR][LOOKUSE] NativePlayerMove hook FAILED at %p", npm);
        }
        OLProxyLog(line);
    }

    // [PROMPTS] (Test37) the game's button hints name the Quest 3 controls (ol_prompts.cpp).
    OLPrompts_Install(g_base);

    // [SOUND] (Test43) Wwise, through the exports OLGame.exe carries by name (ol_sound.cpp) - (Test45) only
    // for cover-your-mouth.
    if (g_soundWanted) OLSound_Install();
    else OLProxyLog("[OLVR][SOUND] cover-your-mouth is off - the Wwise hooks are not installed");

    // GetPlayerViewPoint = the parallax injection point. Without it the split still works, it
    // just has no depth, so a failure here degrades to flat rather than breaking the split.
    if (g_vpRva) {
        void* vp = (void*)(g_base + g_vpRva);
        if (MH_CreateHook(vp, (void*)&Hook_GetViewPoint, (void**)&o_GetViewPoint) == MH_OK &&
            MH_EnableHook(vp) == MH_OK) {
            sprintf_s(line, "[OLVR][S3] GetPlayerViewPoint hooked at %p (base+0x%llx) halfEye=%.2fuu",
                      vp, (unsigned long long)g_vpRva, g_halfEyeUU);
        } else {
            sprintf_s(line, "[OLVR][S3] GetPlayerViewPoint hook FAILED at %p - split will be flat", vp);
        }
        OLProxyLog(line);
    } else {
        OLProxyLog("[OLVR][S3] no GetViewPointRva -> split will render two identical views (no depth)");
    }

    // [CINEFOV] the FOV getter CalcSceneView calls (0x60F4F0, from the disassembly). Hooking
    // the RETURN VALUE is why this needs no memory scan and no restore. Prologue-verified, so a
    // patched or different build simply leaves it uninstalled instead of hooking a wrong address.
    {
        void* fg = (void*)(g_base + kFovGetterRva);
        unsigned char have[sizeof(kFovGetterPrologue)] = { 0 };
        if (!SafeReadBytes((char*)fg, have, sizeof(have)) ||
            memcmp(have, kFovGetterPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter prologue MISMATCH at %p - not hooking "
                            "(cutscenes stay at the game's own width)", fg);
        } else if (MH_CreateHook(fg, (void*)&Hook_GetFovAngle, (void**)&o_GetFovAngle) == MH_OK &&
                   MH_EnableHook(fg) == MH_OK) {
            g_fovHookOk = 1;
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter hooked at %p (base+0x%llX) - cutscenes and "
                            "menus will render %.0f deg/eye like gameplay",
                      fg, (unsigned long long)kFovGetterRva, kCineTargetDeg);
        } else {
            sprintf_s(line, "[OLVR][CINEFOV] FOV getter hook FAILED at %p", fg);
        }
        OLProxyLog(line);
    }

    // [NVLIGHT] USpotLightComponent::SetRotation - lets the camcorder/NV light follow the head.
    // Prologue-verified like every other hook here, so a different build declines instead of
    // patching a wrong address.
    {
        void* sr = (void*)(g_base + kSpotSetRotRva);
        unsigned char have[sizeof(kSpotSetRotPrologue)] = { 0 };
        if (!SafeReadBytes((char*)sr, have, sizeof(have)) ||
            memcmp(have, kSpotSetRotPrologue, sizeof(have)) != 0) {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation prologue MISMATCH at %p - not "
                            "hooking (the NV light will not follow the head)", sr);
        } else if (MH_CreateHook(sr, (void*)&Hook_SpotSetRotation, (void**)&o_SpotSetRot) == MH_OK &&
                   MH_EnableHook(sr) == MH_OK) {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation hooked at %p (base+0x%llX) - the "
                            "camcorder/NV light will follow your head", sr, (unsigned long long)kSpotSetRotRva);
        } else {
            sprintf_s(line, "[OLVR][NVLIGHT] SpotLight SetRotation hook FAILED at %p", sr);
        }
        OLProxyLog(line);
    }

    // SFR hooks Draw itself so it can run the whole render twice.
    if (g_sfrMode) {
        if (!g_drawRva) {
            OLProxyLog("[OLVR][SFR] Mode=sfr but no DrawRva -> SFR DISABLED");
            g_sfrMode = 0;
        } else {
            void* dt = (void*)(g_base + g_drawRva);
            if (MH_CreateHook(dt, (void*)&Hook_Draw, (void**)&o_Draw) == MH_OK && MH_EnableHook(dt) == MH_OK) {
                sprintf_s(line, "[OLVR][SFR] Draw hooked at %p (base+0x%llx) - two full primary passes per frame",
                          dt, (unsigned long long)g_drawRva);
            } else {
                sprintf_s(line, "[OLVR][SFR] Draw hook FAILED at %p -> SFR off", dt);
                g_sfrMode = 0;
            }
            OLProxyLog(line);
        }
    }

    if (g_creatorRva) o_StateCreator = (StateCreator_t)(g_base + g_creatorRva);
    if (g_sharedRva) {
        void* gs = (void*)(g_base + g_sharedRva);
        if (MH_CreateHook(gs, (void*)&Hook_GetSharedState, (void**)&o_GetSharedState) == MH_OK &&
            MH_EnableHook(gs) == MH_OK) {
            sprintf_s(line, "[OLVR][S3] shared view-state getter hooked at base+0x%llx", (unsigned long long)g_sharedRva);
        } else {
            sprintf_s(line, "[OLVR][S3] shared view-state getter hook FAILED");
        }
        OLProxyLog(line);
    }

    // The Outlast-custom occlusion tracker (see Hook_OcclUpdate above). A second RVA gets its
    // own prologue gate - same reasoning as CalcSceneView's.
    if (g_occlRva) {
        void* ot = (void*)(g_base + g_occlRva);
        bool ok = true;
        if (g_occlWantLen > 0) {
            unsigned char got[32] = { 0 };
            ok = SafeReadBytes(ot, got, (size_t)g_occlWantLen) &&
                 memcmp(got, g_occlWantBytes, (size_t)g_occlWantLen) == 0;
        }
        if (!ok) {
            OLProxyLog("[OLVR][OCCL] SKIPPED: bytes at base+OcclRva do not match OcclPrologue (wrong build?)");
        } else if (MH_CreateHook(ot, (void*)&Hook_OcclUpdate, (void**)&o_OcclUpdate) == MH_OK &&
                   MH_EnableHook(ot) == MH_OK) {
            sprintf_s(line, "[OLVR][OCCL] UpdateAndGetCoverage hooked at base+0x%llx, force-visible %s", (unsigned long long)g_occlRva, g_occlForce ? "ON" : "off");
            OLProxyLog(line);
        } else {
            OLProxyLog("[OLVR][OCCL] hook FAILED");
        }
    }

    // Precomputed visibility (see Hook_PvsGet above). Same per-RVA prologue gate.
    if (g_pvsRva) {
        void* pt = (void*)(g_base + g_pvsRva);
        bool ok = true;
        if (g_pvsWantLen > 0) {
            unsigned char got[32] = { 0 };
            ok = SafeReadBytes(pt, got, (size_t)g_pvsWantLen) &&
                 memcmp(got, g_pvsWantBytes, (size_t)g_pvsWantLen) == 0;
        }
        if (!ok) {
            OLProxyLog("[OLVR][PVS] SKIPPED: bytes at base+PvsRva do not match PvsPrologue (wrong build?)");
        } else if (MH_CreateHook(pt, (void*)&Hook_PvsGet, (void**)&o_PvsGet) == MH_OK &&
                   MH_EnableHook(pt) == MH_OK) {
            sprintf_s(line, "[OLVR][PVS] GetPrecomputedVisibilityData hooked at base+0x%llx, bypass %s", (unsigned long long)g_pvsRva, g_occlForce ? "ON" : "off");
            OLProxyLog(line);
        } else {
            OLProxyLog("[OLVR][PVS] hook FAILED");
        }
    }

    // AllocateViewState: resolved, not hooked. It just needs to be callable once.
    if (g_avsRva) {
        o_AllocViewState = (AllocViewState_t)(g_base + g_avsRva);
        sprintf_s(line, "[OLVR][S3] AllocateViewState at base+0x%llx", (unsigned long long)g_avsRva);
    } else {
        sprintf_s(line, "[OLVR][S3] no AllocViewStateRva -> both eyes share one view state (expect "
                        "flicker + per-eye colour drift)");
    }
    OLProxyLog(line);
}

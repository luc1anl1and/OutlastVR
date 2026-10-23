#pragma once
// Outlast FlatXR: D3D9 -> (CPU readback) -> D3D11 -> OpenXR.
struct IDirect3DDevice9;

void OLXR_OnPresent(IDirect3DDevice9* dev);   // capture backbuffer + submit to the headset
void OLXR_Shutdown();
void OLXR_Recenter();                          // zero the head-tracking baseline
void OLXR_ToggleProjection();                  // quad <-> projection layer

// Put the picture back to untouched. Reachable from a hotkey because a brightness setting can
// leave you unable to READ the control that would undo it.
extern "C" void OLXR_ResetGamma();

extern "C" void OLProxyLog(const char* line);  // implemented in d3d9_proxy.cpp

// Head pose relative to the recenter baseline. Written by ol_xr.cpp, read by the (future)
// camera bake in d3d9_proxy.cpp. Nothing consumes these yet - Stage 2 is flat.
extern volatile float g_olYawRad;
extern volatile float g_olPitchRad;
extern volatile float g_olRollRad;
extern volatile float g_olHeadX;               // meters, +right
extern volatile float g_olHeadY;               // meters, +up
extern volatile float g_olHeadZ;               // meters, +back
extern volatile long  g_olPoseValid;

// Declared FOV for the projection layer. 0 = use the fallback default until the camera lane
// gives the game's REAL rendered FOV. Never invent a number here once that exists -
// declared FOV must match what was rendered.
extern volatile float g_olDeclFovXDeg;
extern volatile float g_olDeclFovYDeg;
extern volatile long  g_olUseProjection;       // 1 = projection layer, 0 = head-locked quad

// ---- [XRINPUT] motion controls ----------------------------------------------------------------
// The VR controllers, read by the submit worker and published once per frame. d3d9_proxy.cpp
// turns this into the XInput state the game polls (step 1 proved it reads a fabricated pad).
// Sticks are -1..1, triggers/grips 0..1. Hand poses are in the recenter frame, metres, and are
// only filled when the runtime says the pose is tracked.
struct OLXrPose { float qx, qy, qz, qw; float px, py, pz; };
struct OLXrPad {
    float    moveX, moveY;     // left stick  (+X right, +Y forward)
    float    lookX, lookY;     // right stick
    float    trigL, trigR;
    float    gripL, gripR;
    unsigned buttons;          // OLXR_BTN_*
    OLXrPose handPose[2];      // [0] left, [1] right
    unsigned handValid;        // bit 0 = left, bit 1 = right
    OLXrPose aimPose[2];       // [LOOKUSE] where each controller POINTS (aim pose), same frame
    unsigned aimValid;
    unsigned indexHands;       // active Valve Index interaction profile, bit per hand
    float fingerCurl[2][5];    // thumb, index, middle, ring, little: 0 open .. 1 closed
    unsigned fingerValid[2];   // bit per finger; invalid data retains the native animation
};
enum {
    OLXR_BTN_A = 1u << 0, OLXR_BTN_B = 1u << 1, OLXR_BTN_X = 1u << 2, OLXR_BTN_Y = 1u << 3,
    OLXR_BTN_LSTICK = 1u << 4, OLXR_BTN_RSTICK = 1u << 5, OLXR_BTN_MENU = 1u << 6,
};
extern "C" int OLXR_MotionControlsReady();
extern "C" int OLXR_GetPad(OLXrPad* out);      // 1 = out filled with a coherent one-frame snapshot
extern "C" int OLXR_UsesIndex(int hand);      // cached profile; never calls OpenXR on game thread
extern "C" long OLXR_RecenterGeneration();     // increments after each completed XR recenter

// [HANDSYNC] Both hands and the RAW (unclamped) head position, located at the SAME predicted
// display time as the head pose the camera renders this frame, and frozen/promoted with it at
// publish. OLXR_GetPad's handPose is sampled one display period earlier and is not frozen, so
// the arms drift relative to the view by up to a frame. Recenter frame, metres.
struct OLXrFrameHands {
    OLXrPose hand[2];          // [0] left, [1] right (grip pose)
    unsigned valid;            // bit 0 = left, bit 1 = right
    float    headRaw[3];       // head centre before the body clamp: +X right, +Y up, +Z back
};
extern "C" int OLXR_GetFrameHands(OLXrFrameHands* out);   // 1 = filled

// [INTERACT] (Test37) The head pose the camera renders this frame (recenter frame, latched with the
// hands), a short controller buzz, and the developer markers the headset draws.
extern "C" int  OLXR_GetFrameHead(OLXrPose* out);              // 1 = filled
extern "C" void OLXR_HapticPulse(int hand, float amp, int ms);  // hand 0 left, 1 right
// Rings (developer markers), soft glows and how-to labels (Test38). A label is a panel with an arrow
// at its bottom centre: put it a little above the spot it points at.
enum { OLMK_GREEN = 0, OLMK_RED, OLMK_YELLOW, OLMK_CYAN, OLMK_ORANGE, OLMK_WHITE,
       OLMK_GLOW, OLMK_GLOW_RED,
       OLMK_L_TAKE_CAM, OLMK_L_RAISE, OLMK_L_PULL_OLD, OLMK_L_DROP_OLD, OLMK_L_TAKE_NEW,
       OLMK_L_SLIDE_IN, OLMK_L_NO_SPARE, OLMK_L_GRAB_HANDLE, OLMK_L_LOCKED, OLMK_L_CHARGED,
       OLMK_L_CLIMB, OLMK_L_PULL_UP,                                  // (Test40) climbing
       OLMK_L_COVER_MOUTH,                                            // (Test43) [BREATH]
       OLMK_L_RECENTER,                                               // (Beta 1.1) [RECENTERHINT]
       OLMK_COUNT };
// pos: recenter frame, metres. size: width in metres. aspect: width / height (0 = square).
struct OLMarker { float pos[3]; float size; int color; float aspect; };
extern "C" void OLXR_SetMarkers(const OLMarker* m, int n);      // n <= 16; 0 clears
// [FLIPSCREEN] (Test43) The camcorder's flip-out screen, live: where the screen is (recenter frame,
// metres; q turns the quad's +Z toward whoever looks at it, +Y up the screen), its size, and the
// direction the lens points (recenter frame, unit). The compositor shows the part of the frame the
// lens looks at there, with the camcorder's night-vision look. on = 0 hides it. Game thread writes.
struct OLFlipScreen { float pos[3]; float q[4]; float w, h; float lens[3]; int on; int nv; };   // nv: night vision (else colour)
extern "C" void OLXR_SetFlipScreen(const OLFlipScreen* s);
// [ROOMWALK] (Test44) Game thread. Allow: Miles may walk with you this frame (lapses after 200 ms).
// Take: the room steps (m, recenter frame: x right, z back) of this frame's pose since the last take.
extern "C" void OLXR_RoomWalkAllow(int on);
extern "C" int  OLXR_RoomWalkTake(float out[2]);

// [HAPTICS] The game's own pad rumble (XInputSetState), 0..1 per motor. Game thread writes, the
// submit worker forwards it to the controllers.
extern "C" void OLXR_SetRumble(float lowFreq, float highFreq);
extern "C" int   OLXR_GetHaptics();       extern "C" void OLXR_SetHaptics(int on);
extern "C" float OLXR_GetHapticStrength(); extern "C" void OLXR_SetHapticStrength(float v);

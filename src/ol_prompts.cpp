// ol_prompts.cpp - [PROMPTS] (Test37) the game's button hints in Quest 3 words
// =============================================================================
// Every hint the game shows ("Press or hold {OLA_Use} to open", the tutorials, "Press {OLA_Use}
// to pick up %s") goes through one native, UOLUtils' token resolver at 0xD8BF90:
//     FString* Resolve(FString* out, const FString* in)
// It finds each {OLA_<action>} in a COPY of `in`, looks the action's key up in the player's
// bindings and writes "{X}" for a pad key (the HUD movie turns that into an Xbox button icon) or
// "(E)" for a keyboard key. It only reads `in` (disassembly: [in] and [in+8] are its only uses).
//
// The hook hands the original a rewritten input instead:
//  - whole hints that a hand gesture now does are replaced ("Grab the handle to open, or press
//    X"), each only while that gesture is switched on;
//  - every remaining {OLA_<action>} becomes the Quest 3 control the mod maps to it (d3d9_proxy
//    MapPadButtons: A/B/X/Y pass straight through, LB = left grip, RB = right grip, Back and the
//    D-pad = left grip + B/X/Y, Start = Menu), so no Xbox icon is left to draw.
// The rewritten text lives on this call's stack; the original copies it into its own FString, so
// the game never holds a pointer to it. Anything this does not recognise passes through untouched.
// =============================================================================
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cstdint>
#include "MinHook.h"
#include "ol_interact.h"
#include "ol_motion.h"
#include "ol_xr.h"

extern "C" void OLProxyLog(const char* line);

struct OLFString { wchar_t* data; int num; int max; };   // num counts the terminator
typedef OLFString* (__fastcall *Resolve_t)(OLFString* out, const OLFString* in);
static Resolve_t o_Resolve = nullptr;
static const uintptr_t kResolveRva = 0xD8BF90;
static const unsigned char kResolvePrologue[] = {
    0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
    0x48,0x8D,0x6C,0x24,0xD9,0x48,0x81,0xEC,0xF0,0x00,0x00,0x00 };

// ---- what each action is on the Quest 3 controllers -------------------------------------------------
struct ActionWord { const wchar_t* action; const wchar_t* quest; };
static const ActionWord kActions[] = {
    { L"Use", L"X" },                         { L"Reload", L"Y" },
    { L"Jump", L"A" },                        { L"CrouchToggle", L"B" },
    { L"Crouch", L"B" },                      { L"Run", L"the left grip" },
    { L"ToggleCamcorder", L"the right grip" },{ L"ToggleNightVision", L"the right stick" },
    { L"ZoomContinuousIn", L"right stick up" },  { L"ZoomContinuousOut", L"right stick down" },
    { L"ZoomImpulseIn", L"right stick up" },     { L"ZoomImpulseOut", L"right stick down" },
    { L"AnalogLeanLeft", L"the left trigger" },  { L"AnalogLeanRight", L"the right trigger" },
    { L"LeanLeft", L"the left trigger" },        { L"LeanRight", L"the right trigger" },
    { L"ShowTabMenu", L"left grip + B" },        { L"ShowRecordingMenu", L"left grip + X" },
    { L"ShowEvidenceMenu", L"left grip + Y" },   { L"ShowMenu", L"the Menu button" },
    { L"MoveForward", L"the left stick" },       { L"MoveBackward", L"the left stick" },
    { L"StrafeLeft", L"the left stick" },        { L"StrafeRight", L"the left stick" },
    { L"TurnLeft", L"the right stick" },         { L"TurnRight", L"the right stick" },
};

// ---- whole hints the hands now do (English text, OLGame.int) -----------------------------------------
enum Gate { G_ALWAYS, G_DOORS, G_RAISE, G_BATTERY, G_DUCK, G_HOLSTER, G_HOLSTER_BATTERY, G_CLIMB };
struct Sentence { Gate gate; const wchar_t* game; const wchar_t* quest; };
static const Sentence kSentences[] = {
    { G_DOORS, L"Press or hold {OLA_Use} to open", L"Grab the handle to open, or press {OLA_Use}" },
    { G_DOORS, L"Press {OLA_Use} to open",         L"Grab the handle to open, or press {OLA_Use}" },
    { G_DOORS, L"Press {OLA_Use} to close",        L"Grab the handle to close, or press {OLA_Use}" },
    { G_DOORS, L"To open a door quickly, press {OLA_Use}. To open a door slowly, hold down {OLA_Use} and move forward or backward.",
               L"To open a door, grab its handle and push or pull it. {OLA_Use} still opens it in one go." },
    // (Test38) the chest holster and the hands-on reload come first; without them, Test37's wording
    { G_HOLSTER, L"To raise your camcorder, press {OLA_ToggleCamcorder}",
                 L"Your camcorder is on your right hip: take it with your right hand and bring it to your eye" },
    { G_HOLSTER_BATTERY, L"Press {OLA_Reload} to reload batteries",
                 L"To reload, pull the old battery out of the camcorder, take a new one from your left hip and slide it in, or press {OLA_Reload}" },
    { G_HOLSTER_BATTERY, L"Batteries are used for the infrared light of your camcorder. You can reload by pressing {OLA_Reload}",
                 L"Batteries are used for the infrared light of your camcorder. To reload, pull the old one out of the camcorder with your left hand, take a new one from your left hip and slide it in, or press {OLA_Reload}" },
    { G_RAISE, L"To raise your camcorder, press {OLA_ToggleCamcorder}",
               L"To raise your camcorder, lift your right hand to your eye, or squeeze the right grip" },
    { G_BATTERY, L"Press {OLA_Reload} to reload batteries",
                 L"To reload, take a battery from your left hip to the camcorder, or press {OLA_Reload}" },
    { G_BATTERY, L"Batteries are used for the infrared light of your camcorder. You can reload by pressing {OLA_Reload}",
                 L"Batteries are used for the infrared light of your camcorder. To reload, take one from your left hip and bring it to your right hand, or press {OLA_Reload}" },
    { G_DUCK, L"Crouch or uncrouch with {OLA_CrouchToggle}", L"Duck down to crouch and stand up to stand, or press {OLA_CrouchToggle}" },
    // (Test40) climbing with your hands
    { G_CLIMB, L"To climb up, press {OLA_Jump}",
               L"To climb up, grab the edge with both hands and pull yourself up (or press {OLA_Jump})" },
    { G_CLIMB, L"To climb up, press {OLA_Jump}. To drop, press {OLA_CrouchToggle}",
               L"To climb up, hold on with both grips and pull down. To drop, let go with both hands (or press {OLA_CrouchToggle})" },
    { G_CLIMB, L"To jump up, press {OLA_Jump}",
               L"To jump up, reach up at the edge with both hands, squeeze and pull (or press {OLA_Jump})" },
    { G_CLIMB, L"To vault, press {OLA_Jump} while moving towards waist-high objects.",
               L"To vault, put both hands on a waist-high edge, squeeze and push down (or press {OLA_Jump} while moving towards it)" },
    { G_ALWAYS, L"With the camcorder active, you can zoom in and out with {OLA_ZoomContinuousIn} and {OLA_ZoomContinuousOut}",
                L"With the camcorder raised, push the right stick up to zoom in and down to zoom out" },
};
static bool GateOn(Gate g) {
    switch (g) {
    case G_DOORS:   return OLInteract_Get(0) != 0;
    case G_RAISE:   return OLInteract_Get(1) != 0;
    case G_BATTERY: return OLInteract_Get(2) != 0;
    case G_DUCK:    return OLMotion_GetPhysCrouch() != 0;
    case G_HOLSTER: return OLInteract_Get(6) != 0;
    case G_HOLSTER_BATTERY: return OLInteract_Get(6) != 0 && OLInteract_Get(2) != 0;
    case G_CLIMB:   return OLInteract_Get(8) != 0;
    default:        return true;
    }
}

// ---- the rewrite ---------------------------------------------------------------------------------------
static bool Put(wchar_t* out, size_t cap, size_t* n, const wchar_t* s, size_t len) {
    if (*n + len + 1 > cap) return false;
    memcpy(out + *n, s, len * sizeof(wchar_t)); *n += len; out[*n] = 0;
    return true;
}
// A control named right after a noun reads as a label ("Notebook (left grip + B)"); after a verb
// or a conjunction it is the object ("press X", "hold the left trigger or the right trigger").
static bool AfterVerb(const wchar_t* s, size_t at) {
    size_t e = at;
    while (e > 0 && s[e - 1] == L' ') --e;
    size_t b = e;
    while (b > 0 && iswalpha(s[b - 1])) --b;
    if (b == e) return true;   // start of the text, or after punctuation
    static const wchar_t* kWords[] = { L"press", L"pressing", L"hold", L"holding", L"with", L"down",
                                       L"and", L"or", L"use", L"squeeze", L"tap" };
    for (const wchar_t* w : kWords)
        if (wcslen(w) == e - b && _wcsnicmp(s + b, w, e - b) == 0) return true;
    return false;
}
// Returns true when the text changed (out then holds the new text).
static bool Rewrite(const wchar_t* s, size_t len, wchar_t* out, size_t cap) {
    bool changed = false;
    for (const Sentence& st : kSentences) {
        if (wcslen(st.game) == len && wcsncmp(s, st.game, len) == 0 && GateOn(st.gate)) {
            s = st.quest; len = wcslen(s); changed = true;
            break;
        }
    }
    size_t n = 0; out[0] = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] == L'{' && i + 5 <= len && wcsncmp(s + i, L"{OLA_", 5) == 0) {
            size_t j = i + 5;
            while (j < len && s[j] != L'}' && j - i < 48) ++j;
            if (j < len && s[j] == L'}') {
                const wchar_t* name = s + i + 5; const size_t nl = j - (i + 5);
                const wchar_t* quest = nullptr;
                for (const ActionWord& a : kActions)
                    if (wcslen(a.action) == nl && wcsncmp(name, a.action, nl) == 0) { quest = a.quest;
                        if (OLXR_UsesIndex(0)) {
                            if (!wcscmp(a.action, L"Use")) quest = L"left A";
                            else if (!wcscmp(a.action, L"Reload")) quest = L"left B";
                            else if (!wcscmp(a.action, L"ShowRecordingMenu")) quest = L"left grip + left A";
                            else if (!wcscmp(a.action, L"ShowEvidenceMenu")) quest = L"left grip + left B";
                            else if (!wcscmp(a.action, L"ShowMenu")) quest = L"the left trackpad";
                        }
                        if (OLXR_UsesIndex(1)) {
                            if (!wcscmp(a.quest, L"A")) quest = L"right A";
                            else if (!wcscmp(a.quest, L"B")) quest = L"right B";
                            else if (!wcscmp(a.action, L"ShowTabMenu")) quest = L"left grip + right B";
                        }
                        break;
                    }
                if (quest) {
                    const bool bare = AfterVerb(s, i);
                    if (!bare && !Put(out, cap, &n, L"(", 1)) return false;
                    if (!Put(out, cap, &n, quest, wcslen(quest))) return false;
                    if (!bare && !Put(out, cap, &n, L")", 1)) return false;
                    changed = true; i = j + 1;
                    continue;
                }
            }
        }
        if (!Put(out, cap, &n, s + i, 1)) return false;
        ++i;
    }
    return changed;
}

// The first few distinct rewrites go to the log, to check them against the screen.
static void LogOnce(const wchar_t* from, size_t len, const wchar_t* to) {
    static unsigned s_seen[48]; static int s_n = 0;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ (unsigned)from[i]) * 16777619u;
    for (int i = 0; i < s_n; ++i) if (s_seen[i] == h) return;
    if (s_n >= 48) return;
    s_seen[s_n++] = h;
    char a[200], b[200]; size_t k;
    for (k = 0; k < len && k < sizeof(a) - 1; ++k) a[k] = (from[k] >= 32 && from[k] < 127) ? (char)from[k] : '?';
    a[k] = 0;
    for (k = 0; to[k] && k < sizeof(b) - 1; ++k) b[k] = (to[k] >= 32 && to[k] < 127) ? (char)to[k] : '?';
    b[k] = 0;
    char line[460]; sprintf_s(line, "[OLVR][PROMPTS] \"%s\" -> \"%s\"", a, b); OLProxyLog(line);
}

static bool ReadIn(const OLFString* in, const wchar_t** s, int* num) {
    __try { *s = in->data; *num = in->num; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static OLFString* __fastcall Hook_Resolve(OLFString* out, const OLFString* in) {
    const wchar_t* s = nullptr; int num = 0;
    if (!OLInteract_Get(5) || !in || !ReadIn(in, &s, &num) || !s || num <= 1 || num > 1024)
        return o_Resolve(out, in);
    wchar_t buf[2048];
    if (!Rewrite(s, (size_t)(num - 1), buf, _countof(buf))) return o_Resolve(out, in);
    LogOnce(s, (size_t)(num - 1), buf);
    const int n = (int)wcslen(buf) + 1;
    OLFString mine = { buf, n, n };
    return o_Resolve(out, &mine);
}

// Called from the engine hook setup in ol_ue3.cpp (MinHook is initialised by then).
extern "C" void OLPrompts_Install(uintptr_t base) {
    void* fn = (void*)(base + kResolveRva);
    unsigned char have[sizeof(kResolvePrologue)] = {};
    bool same = false;
    __try { memcpy(have, fn, sizeof(have)); same = memcmp(have, kResolvePrologue, sizeof(have)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
    char line[200];
    if (!same) {
        sprintf_s(line, "[OLVR][PROMPTS] hint resolver prologue MISMATCH at %p - the game's own button icons stay", fn);
    } else if (MH_CreateHook(fn, (void*)&Hook_Resolve, (void**)&o_Resolve) == MH_OK && MH_EnableHook(fn) == MH_OK) {
        sprintf_s(line, "[OLVR][PROMPTS] hint resolver hooked at base+0x%llX: hints name the active controller buttons (%s)",
                  (unsigned long long)kResolveRva, OLInteract_Get(5) ? "on" : "off");
    } else {
        sprintf_s(line, "[OLVR][PROMPTS] hint resolver hook FAILED at %p", fn);
    }
    OLProxyLog(line);
}

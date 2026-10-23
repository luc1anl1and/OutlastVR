#include <cassert>
#include <limits>
#include <cstdio>
#include "../src/ol_index_input.h"
#include "../src/ol_xr_types.h"
#include "../src/ol_prompts.cpp"

static unsigned testIndexHands = 0;
extern "C" int OLXR_UsesIndex(int hand) { return (testIndexHands & (1u << hand)) != 0; }
extern "C" int OLInteract_Get(int) { return 1; }
extern "C" int OLMotion_GetPhysCrouch() { return 1; }
extern "C" void OLProxyLog(const char*) {}
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_OK; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_OK; }

static void Hint(const wchar_t* in, const wchar_t* expected) {
    wchar_t out[2048];
    assert(Rewrite(in, wcslen(in), out, _countof(out)));
    assert(wcscmp(out, expected) == 0);
}

int main() {
    static_assert(sizeof(OLXR::XrHandTrackerCreateInfoEXT) == 24, "OpenXR x64 ABI");
    static_assert(sizeof(OLXR::XrHandJointsLocateInfoEXT) == 32, "OpenXR x64 ABI");
    static_assert(sizeof(OLXR::XrHandJointLocationEXT) == 40, "OpenXR x64 ABI");
    static_assert(sizeof(OLXR::XrHandJointLocationsEXT) == 32, "OpenXR x64 ABI");
    bool held = false;
    assert(OLIndexGrab(0.5f, held, .8f, .55f) == 0);
    assert(OLIndexGrab(0.8f, held, .8f, .55f) == 1);
    for (int i = 0; i < 20; ++i) {
        assert(OLIndexGrab(.79f, held, .8f, .55f) == 1);
        assert(OLIndexGrab(.56f, held, .8f, .55f) == 1);
    }
    assert(OLIndexGrab(.55f, held, .8f, .55f) == 0);
    assert(OLIndexGrab(.7f, held, .8f, .55f) == 0);
    assert(OLIndexGrab(1, held, .8f, .55f) == 1);
    assert(OLIndexGrab(0, held, .8f, .55f) == 0); // inactive/disconnected action releases
    assert(OLIndexGrab(std::numeric_limits<float>::quiet_NaN(), held, .8f, .55f) == 0);
    const float a[] = {0,0,0}, b[] = {1,0,0}, c[] = {2,0,0}, d[] = {1,1,0};
    assert(fabsf(OLFingerBend(a,b,c)) < 1e-5f);
    assert(fabsf(OLFingerBend(a,b,d) - 1.5707963f) < 1e-5f);
    assert(OLFingerBend(a,a,c) < 0); // malformed joints do not animate
    const float ar[] = {4,2,8}, br[] = {4,3,8}, dr[] = {3,3,8};
    assert(fabsf(OLFingerBend(ar,br,dr) - OLFingerBend(a,b,d)) < 1e-5f);
    testIndexHands = 3;
    Hint(L"Press {OLA_Use} to open", L"Grab the handle to open, or press left A");
    Hint(L"Press {OLA_Reload}", L"Press left B");
    Hint(L"Press {OLA_Jump} or {OLA_CrouchToggle}", L"Press right A or right B");
    Hint(L"Press {OLA_ShowMenu}", L"Press the left trackpad");
    Hint(L"Press {OLA_ShowEvidenceMenu}", L"Press left grip + left B");
    Hint(L"Press {OLA_ShowRecordingMenu}", L"Press left grip + left A");
    Hint(L"Press {OLA_ShowTabMenu}", L"Press left grip + right B");
    testIndexHands = 0;
    Hint(L"Press {OLA_Use} to open", L"Grab the handle to open, or press X");
    Hint(L"Press {OLA_Reload}", L"Press Y");
    testIndexHands = 1;
    Hint(L"Press {OLA_Use} or {OLA_Jump}", L"Press left A or A");
    wchar_t out[4] = {};
    assert(!Rewrite(L"Press {OLA_Use}", 15, out, _countof(out)));
    assert(!Rewrite(L"Unknown {OLA_Unknown}", 21, out, _countof(out)));
    puts("Index grab, finger math, OpenXR ABI, Index/Touch/mixed-controller hints: passed.");
}

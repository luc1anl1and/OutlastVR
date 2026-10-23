#pragma once
#include <cmath>

// Index squeeze/value measures hand closure; force would require a deliberate squeeze.
inline float OLIndexGrab(float closure, bool& held, float closeAt, float openAt) {
    if (!std::isfinite(closure)) closure = 0.0f;
    if (held) { if (closure <= openAt) held = false; }
    else if (closure >= closeAt) held = true;
    return held ? 1.0f : 0.0f;
}

// Angle between successive finger segments, invariant under hand position and rotation.
inline float OLFingerBend(const float a[3], const float b[3], const float c[3]) {
    float dot = 0, aa = 0, bb = 0;
    for (int k = 0; k < 3; ++k) {
        const float u = b[k] - a[k], v = c[k] - b[k];
        dot += u * v; aa += u * u; bb += v * v;
    }
    if (aa < 1e-10f || bb < 1e-10f) return -1.0f;
    float cosine = dot / std::sqrt(aa * bb);
    if (!std::isfinite(cosine)) return -1.0f;
    if (cosine < -1) cosine = -1; if (cosine > 1) cosine = 1;
    return std::acos(cosine);
}

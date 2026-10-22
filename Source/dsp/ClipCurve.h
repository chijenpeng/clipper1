// The clipper's static transfer curve. No JUCE dependency; the processor, the editor's curve
// display and the tests all call this one function, so what is drawn is what is processed.
//
// The curve works on the level relative to the Threshold (u = 1 is the Threshold) and returns a
// level relative to the Out Ceiling (1 is the Ceiling). A quadratic soft knee of half-width w
// is centred on u = 1:
//
//     |u| <= 1 - w         y = |u|                           straight line, gain 1
//     1 - w < |u| < 1 + w  y = |u| - (|u| - (1 - w))^2 / (4w)   slope falls linearly from 1 to 0
//     |u| >= 1 + w         y = 1                             the ceiling
//
// w = Softness / 100 (0 = hard clip, 1 = the knee starts at zero and ends at twice the
// Threshold). The curve is odd-symmetric, continuous with a continuous first derivative for
// w > 0, monotonic, and never above 1. This is the same quadratic soft knee used by compressor
// gain computers (Giannoulis, Massberg, Reiss, JAES 2012), applied to the level instead of the
// gain. See docs/dsp.md.
#pragma once

#include <algorithm>
#include <cmath>

namespace clipper
{
constexpr float kMinGainDb = -30.0f;

template <typename T>
inline T dbToGain (T db) noexcept { return static_cast<T> (std::pow (static_cast<T> (10), db / static_cast<T> (20))); }

/** Softness in percent to the knee half-width w. */
template <typename T>
inline T softnessToKnee (T percent) noexcept { return std::min (std::max (percent / static_cast<T> (100), T (0)), T (1)); }

/** u: level relative to the Threshold (signed). w: knee half-width, 0..1. Returns -1..1. */
template <typename T>
inline T shape (T u, T w) noexcept
{
    const T a = std::abs (u);
    T y;
    if (w <= static_cast<T> (1.0e-6))
        y = std::min (a, T (1));
    else if (a <= T (1) - w)
        y = a;
    else if (a >= T (1) + w)
        y = T (1);
    else
    {
        const T d = a - (T (1) - w);
        y = std::min (a - d * d / (static_cast<T> (4) * w), T (1));
    }
    return std::copysign (y, u);
}
} // namespace clipper

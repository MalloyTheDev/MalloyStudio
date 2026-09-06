#pragma once

// Pure mixing arithmetic for the program bus.
//
// Kept out of AudioController so the pan law and the per-channel summing can be
// unit-tested without WASAPI devices: the bug this file exists to prevent was in
// the summing loop, not in the law.

#include <algorithm>
#include <cstdint>
#include <vector>

// Balance law for a stereo input. Pan attenuates the opposite side and never
// folds one channel into the other, so a source panned centre passes through
// untouched and keeps its stereo image.
//
// An equal-power law (cos/sin) would be the right choice for a mono source being
// placed in the field, but every input on this bus is already stereo: desktop
// audio and stereo microphones arrive with a left and a right that mean
// something, and summing them to mono to re-spread them throws that away.
inline void balanceGains(float pan, float& gainL, float& gainR) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    gainL = pan <= 0.0f ? 1.0f : 1.0f - pan;
    gainR = pan >= 0.0f ? 1.0f : 1.0f + pan;
}

// Adds one input's interleaved stereo samples into the accumulator: left into
// left, right into right. `nSamples` counts samples, not frames. Accumulating in
// int32 is what lets the caller clamp once at the end instead of wrapping here.
inline void mixStereoInto(std::vector<int32_t>& accum, const int16_t* src, int nSamples,
                          float gainL, float gainR) {
    if (!src) return;
    const int frames = nSamples / 2;
    for (int f = 0; f < frames; ++f) {
        const size_t fi = static_cast<size_t>(f) * 2;
        if (fi + 1 >= accum.size()) break;
        accum[fi]     += static_cast<int32_t>(static_cast<float>(src[fi])     * gainL);
        accum[fi + 1] += static_cast<int32_t>(static_cast<float>(src[fi + 1]) * gainR);
    }
}

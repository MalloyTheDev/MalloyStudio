#pragma once

// Pure mixing arithmetic for the program bus.
//
// Kept out of AudioController so the pan law and the per-channel summing can be
// unit-tested without WASAPI devices: the bug this file exists to prevent was in
// the summing loop, not in the law.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Byte-accurate PCM buffer for one input.
//
// The mixer needs exactly one tick of audio per tick. Device chunks do not
// arrive in tick-sized pieces, so a buffer that hands back whole chunks either
// throws away the tail of a large one or leaves the rest of the tick silent
// while more data sits in the queue. This keeps the leftovers instead, so the
// sample stream stays continuous across ticks.
//
// Reads consume from the front; when the buffer runs long (a stalled encoder,
// a device delivering faster than the mixer drains) the OLDEST bytes go first,
// because live audio wants to stay near the present rather than play a backlog.
class PcmFifo {
public:
    void push(const char* data, int bytes) {
        if (!data || bytes <= 0) return;
        m_buf.insert(m_buf.end(), data, data + bytes);
    }

    // Copies up to `bytes` into `out` and consumes them. Returns how many bytes
    // were actually available, which is less than `bytes` on an underrun.
    int take(char* out, int bytes) {
        if (!out || bytes <= 0) return 0;
        const int got = std::min<int>(bytes, available());
        if (got <= 0) return 0;
        std::memcpy(out, m_buf.data() + m_read, static_cast<size_t>(got));
        m_read += static_cast<size_t>(got);
        compact();
        return got;
    }

    // Drops the oldest bytes until at most `maxBytes` remain, bounding latency.
    void trimToLast(int maxBytes) {
        if (maxBytes < 0) maxBytes = 0;
        const int over = available() - maxBytes;
        if (over <= 0) return;
        m_read += static_cast<size_t>(over);
        compact();
    }

    int available() const { return static_cast<int>(m_buf.size() - m_read); }
    bool isEmpty() const { return available() == 0; }
    void clear() { m_buf.clear(); m_read = 0; }

private:
    // Reading advances an offset rather than moving the whole buffer; the front
    // is reclaimed once it dominates, which keeps steady-state reads cheap.
    void compact() {
        if (m_read == 0) return;
        if (m_read >= m_buf.size()) { m_buf.clear(); m_read = 0; return; }
        if (m_read >= m_buf.size() / 2) {
            m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_read));
            m_read = 0;
        }
    }

    std::vector<char> m_buf;
    std::size_t       m_read = 0;
};

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

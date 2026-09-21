#pragma once

// Conversion of captured device samples into the bus format: interleaved
// 16-bit stereo, with every channel the device delivers folded into the pair.
//
// Kept apart from WasapiCapture so the arithmetic can be tested without a
// device. The capture used to read channels 0 and 1 of each frame and step
// over the rest, so a 5.1 or 7.1 playback device lost its centre channel,
// which is where film and game dialogue is, along with its surrounds; and a
// device delivering packed 24-bit samples was recorded as silence while its
// meter looked alive.

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

// How the device stores one sample. Anything not named here is refused
// rather than guessed at: a guess that is wrong records noise or silence.
enum class SampleEncoding {
    Unsupported,
    UInt8,         // unsigned, 128 is silence, as WAV defines 8-bit PCM
    Int16,
    Int24Packed,   // three bytes, little-endian
    Int32,         // including 24 or 20 valid bits left-justified in 32
    Float32,
};

// What the format tag, or SubFormat for WAVE_FORMAT_EXTENSIBLE, says the
// samples are.
enum class SampleType { Pcm, Float, Other };

struct DeviceSampleFormat {
    SampleEncoding encoding = SampleEncoding::Unsupported;
    int     channels    = 0;
    quint32 channelMask = 0;   // dwChannelMask; 0 when the format carries none

    int bytesPerSample() const {
        switch (encoding) {
        case SampleEncoding::UInt8:       return 1;
        case SampleEncoding::Int16:       return 2;
        case SampleEncoding::Int24Packed: return 3;
        case SampleEncoding::Int32:       return 4;
        case SampleEncoding::Float32:     return 4;
        case SampleEncoding::Unsupported: break;
        }
        return 0;
    }
    int  bytesPerFrame() const { return bytesPerSample() * channels; }
    bool supported() const { return bytesPerSample() > 0 && channels > 0; }
};

// Reads a device format from its WAVEFORMATEX fields. `containerBits` is
// wBitsPerSample, the storage each sample occupies: WAVEFORMATEXTENSIBLE keeps
// the valid bits left-justified in that container, so a 24-in-32 format reads
// correctly as 32-bit and wValidBitsPerSample is not needed.
//
// `blockAlign` has to agree with the rest. It is the stride WASAPI uses for
// the packet buffer, so a format whose samples would not fit in it cannot be
// read without running off the end of each packet.
inline DeviceSampleFormat describeDeviceFormat(SampleType type, int containerBits, int blockAlign,
                                               int channels, quint32 channelMask) {
    DeviceSampleFormat f;
    f.channels = channels;
    f.channelMask = channelMask;
    if (type == SampleType::Float && containerBits == 32) {
        f.encoding = SampleEncoding::Float32;
    } else if (type == SampleType::Pcm) {
        switch (containerBits) {
        case 8:  f.encoding = SampleEncoding::UInt8; break;
        case 16: f.encoding = SampleEncoding::Int16; break;
        case 24: f.encoding = SampleEncoding::Int24Packed; break;
        case 32: f.encoding = SampleEncoding::Int32; break;
        default: break;
        }
    }
    if (channels <= 0 || f.bytesPerFrame() != blockAlign)
        f.encoding = SampleEncoding::Unsupported;
    return f;
}

// How much of each device channel goes into the left and the right output.
struct StereoGain {
    float left  = 0.0f;
    float right = 0.0f;
};

struct StereoLayout {
    // Only the first `count` channels contribute; any after them carry no
    // speaker position and are left out.
    std::array<StereoGain, 32> gains{};
    int count = 0;
};

// The downmix coefficients, indexed by speaker position as dwChannelMask
// numbers them (SPEAKER_FRONT_LEFT is bit 0). They follow ITU-R BS.775: the
// centre goes into both sides at -3 dB, and each surround into its own side at
// -3 dB. LFE is left out, as that recommendation leaves it out: it is
// supplementary bass meant for a subwoofer played 10 dB hot, and folding it
// into the pair muddies the mix and eats headroom. The positions BS.775 does not
// cover follow ffmpeg's rematrixing: the pair either side of centre goes to
// its own side at full level, and a single rear centre is a surround split
// across both sides, -3 dB again. Height channels go to their own side at
// -3 dB as surrounds do, and the centred ones split like the rear centre.
//
// The front pair keeps its level, so a 5.1 or 7.1 mix is as loud as a stereo
// one. The sum can pass full scale when every channel peaks at once; it is
// clipped there, as the bus clips any other overload, rather than every
// recording from a surround device being made several decibels quieter.
inline StereoLayout stereoLayoutFor(int channels, quint32 channelMask) {
    constexpr float k = 0.70710678f;   // -3 dB
    constexpr StereoGain kBySpeaker[] = {
        {1.0f, 0.0f},   // front left
        {0.0f, 1.0f},   // front right
        {k, k},         // front centre
        {0.0f, 0.0f},   // low frequency
        {k, 0.0f},      // back left
        {0.0f, k},      // back right
        {1.0f, 0.0f},   // front left of centre
        {0.0f, 1.0f},   // front right of centre
        {0.5f, 0.5f},   // back centre
        {k, 0.0f},      // side left
        {0.0f, k},      // side right
        {0.5f, 0.5f},   // top centre
        {k, 0.0f},      // top front left
        {0.5f, 0.5f},   // top front centre
        {0.0f, k},      // top front right
        {k, 0.0f},      // top back left
        {0.5f, 0.5f},   // top back centre
        {0.0f, k},      // top back right
    };
    constexpr int kPositions = int(sizeof(kBySpeaker) / sizeof(kBySpeaker[0]));
    constexpr quint32 kKnownPositions = (1u << kPositions) - 1u;

    StereoLayout layout;
    if (channels <= 0) return layout;

    // Mono is the one layout with a single channel to place, and it belongs
    // on both sides at full level, as it always has been. Taking its mask
    // literally would put a front-centre microphone 3 dB down.
    if (channels == 1) {
        layout.gains[0] = {1.0f, 1.0f};
        layout.count = 1;
        return layout;
    }

    // A device that assigns no speaker positions, as a multichannel audio
    // interface commonly does, has no layout to fold. Its first two channels
    // are taken as left and right, as they always were, rather than mixing
    // unrelated inputs together as if they were surround channels.
    if ((channelMask & kKnownPositions) == 0) {
        layout.gains[0] = {1.0f, 0.0f};
        layout.gains[1] = {0.0f, 1.0f};
        layout.count = 2;
        return layout;
    }

    // Channels appear in the order of the bits set in the mask.
    for (int bit = 0; bit < 32 && layout.count < channels
                      && layout.count < int(layout.gains.size()); ++bit) {
        if (!(channelMask & (1u << bit))) continue;
        layout.gains[size_t(layout.count)] = bit < kPositions ? kBySpeaker[bit] : StereoGain{};
        ++layout.count;
    }
    return layout;
}

// Loudest sample on each side of a converted packet, 0 to 1.
struct StereoPeaks {
    float left  = 0.0f;
    float right = 0.0f;
};

namespace stereo_downmix_detail {

inline float readSample(SampleEncoding encoding, const unsigned char* p) {
    switch (encoding) {
    case SampleEncoding::UInt8:
        return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
    case SampleEncoding::Int16: {
        qint16 v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<float>(v) / 32768.0f;
    }
    case SampleEncoding::Int24Packed: {
        // Built in the top three bytes of an int32, so the sign comes with it.
        const qint32 v = static_cast<qint32>((quint32(p[0]) << 8) | (quint32(p[1]) << 16)
                                             | (quint32(p[2]) << 24));
        return static_cast<float>(v) / 2147483648.0f;
    }
    case SampleEncoding::Int32: {
        qint32 v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<float>(v) / 2147483648.0f;
    }
    case SampleEncoding::Float32: {
        float v;
        std::memcpy(&v, p, sizeof v);
        // A NaN would survive every clamp below and reach the conversion to
        // integer, which is undefined for it.
        return std::isfinite(v) ? v : 0.0f;
    }
    case SampleEncoding::Unsupported:
        break;
    }
    return 0.0f;
}

inline float clampUnit(float v) {
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

inline qint16 toInt16(float v) {
    const long s = std::lrint(v * 32768.0f);
    return static_cast<qint16>(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
}

}  // namespace stereo_downmix_detail

// Converts `frames` frames of the device's samples at `src` into interleaved
// 16-bit stereo at `out`, which must hold 2 * frames samples. Nothing is
// written for a format that is not supported; the caller has to refuse such a
// device before it gets this far.
inline StereoPeaks downmixToStereo(const DeviceSampleFormat& format, const void* src, int frames,
                                   qint16* out) {
    using namespace stereo_downmix_detail;
    StereoPeaks peaks;
    if (!format.supported() || !src || !out || frames <= 0) return peaks;

    const StereoLayout layout = stereoLayoutFor(format.channels, format.channelMask);
    const size_t sampleBytes = size_t(format.bytesPerSample());
    const size_t frameBytes = size_t(format.bytesPerFrame());
    const auto* frame = static_cast<const unsigned char*>(src);

    for (int f = 0; f < frames; ++f, frame += frameBytes) {
        float l = 0.0f;
        float r = 0.0f;
        for (int c = 0; c < layout.count; ++c) {
            const StereoGain g = layout.gains[size_t(c)];
            if (g.left == 0.0f && g.right == 0.0f) continue;
            const float s = readSample(format.encoding, frame + size_t(c) * sampleBytes);
            l += g.left * s;
            r += g.right * s;
        }
        l = clampUnit(l);
        r = clampUnit(r);
        out[size_t(f) * 2]     = toInt16(l);
        out[size_t(f) * 2 + 1] = toInt16(r);
        peaks.left  = std::max(peaks.left, std::fabs(l));
        peaks.right = std::max(peaks.right, std::fabs(r));
    }
    return peaks;
}

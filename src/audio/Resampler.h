#pragma once

// Sample-rate conversion for captured audio.
//
// WASAPI hands us whatever rate the device is set to. The rest of the app is
// fixed at 48 kHz, so a 44.1 kHz microphone used to be forwarded as if it were
// 48 kHz: the recording played back fast and sharp, and drifted further out of
// sync the longer it ran.
//
// Interpolation is Catmull-Rom over a four-sample window, which is cheap enough
// for a capture thread and well behaved for the rates in play here. Downsampling
// runs a second-order lowpass first, because decimating without one folds
// everything above the new Nyquist back into the audible band. A polyphase
// windowed-sinc would be the next step if quality ever demands it; this is a
// large improvement over forwarding the samples untouched.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class StereoResampler {
public:
    StereoResampler() { reset(48000, 48000); }
    StereoResampler(int inRate, int outRate) { reset(inRate, outRate); }

    void reset(int inRate, int outRate) {
        m_out = outRate > 0 ? outRate : 48000;
        m_in  = inRate  > 0 ? inRate  : m_out;
        m_ratio = static_cast<double>(m_in) / static_cast<double>(m_out);
        m_pos = static_cast<double>(kHistory - 2);   // first interpolable position
        m_hist.assign(static_cast<size_t>(kHistory) * 2, 0.0f);
        m_filter[0] = Biquad{};
        m_filter[1] = Biquad{};
        m_downsampling = m_in > m_out;
        if (m_downsampling) designLowpass();
    }

    // False when the device already runs at the target rate, in which case
    // process() copies straight through and costs nothing.
    bool active() const { return m_in != m_out; }
    int inputRate() const { return m_in; }
    int outputRate() const { return m_out; }

    // Appends the converted stereo frames to `out`. Interleaved int16 in and
    // out. Fractional position and filter state carry across calls, so a
    // continuous input stream produces a continuous output stream.
    void process(const int16_t* in, int frames, std::vector<int16_t>& out) {
        if (!in || frames <= 0) return;
        if (!active()) {
            out.insert(out.end(), in, in + static_cast<size_t>(frames) * 2);
            return;
        }

        m_work.assign(m_hist.begin(), m_hist.end());
        m_work.reserve(m_work.size() + static_cast<size_t>(frames) * 2);
        for (int f = 0; f < frames; ++f) {
            float l = static_cast<float>(in[static_cast<size_t>(f) * 2]);
            float r = static_cast<float>(in[static_cast<size_t>(f) * 2 + 1]);
            if (m_downsampling) {
                l = m_filter[0].process(l);
                r = m_filter[1].process(r);
            }
            m_work.push_back(l);
            m_work.push_back(r);
        }

        const int total = static_cast<int>(m_work.size() / 2);
        while (true) {
            const int i = static_cast<int>(std::floor(m_pos));
            if (i < 1 || i + 2 > total - 1) break;
            const double t = m_pos - i;
            for (int c = 0; c < 2; ++c) {
                const float p0 = m_work[static_cast<size_t>(i - 1) * 2 + c];
                const float p1 = m_work[static_cast<size_t>(i)     * 2 + c];
                const float p2 = m_work[static_cast<size_t>(i + 1) * 2 + c];
                const float p3 = m_work[static_cast<size_t>(i + 2) * 2 + c];
                out.push_back(toPcm(catmullRom(p0, p1, p2, p3, t)));
            }
            m_pos += m_ratio;
        }

        // Keep the tail as history and rebase the read position onto it.
        const int consumed = total - kHistory;
        if (consumed > 0) {
            m_hist.assign(m_work.end() - static_cast<std::ptrdiff_t>(kHistory) * 2, m_work.end());
            m_pos -= consumed;
        } else {
            m_hist = m_work;   // fewer frames than the window: keep everything
        }
    }

private:
    static constexpr int kHistory = 3;   // frames carried between calls

    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        float process(float x) {
            const double in = x;
            const double y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = y;
            return static_cast<float>(y);
        }
    };

    // Second-order Butterworth at just under the output Nyquist, designed at the
    // input rate (RBJ cookbook lowpass).
    void designLowpass() {
        const double fs = static_cast<double>(m_in);
        const double fc = std::min(0.45 * static_cast<double>(m_out), 0.45 * fs);
        const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
        const double q = 0.70710678118654752440;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cosw = std::cos(w0);
        const double a0 = 1.0 + alpha;

        Biquad b;
        b.b0 = ((1.0 - cosw) / 2.0) / a0;
        b.b1 = (1.0 - cosw) / a0;
        b.b2 = b.b0;
        b.a1 = (-2.0 * cosw) / a0;
        b.a2 = (1.0 - alpha) / a0;
        m_filter[0] = b;
        m_filter[1] = b;
    }

    static double catmullRom(double p0, double p1, double p2, double p3, double t) {
        const double t2 = t * t;
        const double t3 = t2 * t;
        return 0.5 * ((2.0 * p1)
                      + (-p0 + p2) * t
                      + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2
                      + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    }

    static int16_t toPcm(double v) {
        return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
    }

    int    m_in = 48000;
    int    m_out = 48000;
    double m_ratio = 1.0;
    double m_pos = 1.0;
    bool   m_downsampling = false;
    Biquad m_filter[2];
    std::vector<float> m_hist;
    std::vector<float> m_work;
};

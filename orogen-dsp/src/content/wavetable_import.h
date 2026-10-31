// Offline WAV-to-wavetable preparation. No I/O or allocation in the audio callback.
// Packed frames preserve their phase; ordinary recordings use harmonic spectral
// snapshots and a bounded pitch estimator. The canonical frames are stored so
// a project does not depend on the original file after import.
#pragma once

#include "wavetable_bank.h"
#include <algorithm>
#include <complex>
#include <memory>
#include <string>
#include <vector>
#include <cmath>

namespace orogen {

inline constexpr int kUserWtFrameSamples = 2048;
inline constexpr int kUserWtMaxFrames = 256;
inline constexpr int kResynthFrames = 64;

struct UserWavetable
{
    std::string name;
    int frames = 0;
    // 2048 real samples per frame, without DC. This is the persistent source.
    std::vector<float> samples;
    Wavetable table;
};

inline bool validUserFrames (const std::vector<float>& samples, int frames) noexcept
{
    if (frames < 1 || frames > kUserWtMaxFrames
        || samples.size() != (size_t) frames * kUserWtFrameSamples) return false;
    for (float s : samples)
        if (! std::isfinite (s) || std::fabs (s) > 8.0f) return false;
    return true;
}

// Construct a single band-limited pyramid once, before publication to voices.
// One 256-frame table occupies about 25 MB, shared by all voices in one slot.
inline std::shared_ptr<const UserWavetable> makeUserWavetable (
    const std::vector<float>& canonicalFrames, int frames, std::string name)
{
    if (! validUserFrames (canonicalFrames, frames)) return {};
    auto result = std::make_shared<UserWavetable>();
    result->name = std::move (name);
    result->frames = frames;
    result->samples = canonicalFrames;
    auto& t = result->table;
    t.frames = frames;
    t.name = result->name;
    t.levels.resize (kWtLevels);
    t.lens.resize (kWtLevels);
    t.harms.resize (kWtLevels);
    for (int level = 0; level < kWtLevels; ++level)
    {
        const int h = levelMaxHarm (level);
        const int len = nextPow2 (std::max (8, h * 2 * kWtLenMult));
        t.lens[(size_t) level] = len;
        t.harms[(size_t) level] = h;
        t.levels[(size_t) level].resize ((size_t) frames * (size_t) len);
    }
    std::vector<std::complex<float>> source ((size_t) kUserWtFrameSamples);
    for (int frame = 0; frame < frames; ++frame)
    {
        for (int i = 0; i < kUserWtFrameSamples; ++i)
            source[(size_t) i] = { canonicalFrames[(size_t) frame * kUserWtFrameSamples + (size_t) i], 0.0f };
        Fft::forward (source);
        for (int level = 0; level < kWtLevels; ++level)
        {
            const int h = t.harms[(size_t) level], len = t.lens[(size_t) level];
            std::vector<std::complex<float>> freq ((size_t) len);
            // Input DFT is unnormalised; compensate for different IFFT sizes.
            const float scale = (float) len / (float) kUserWtFrameSamples;
            for (int k = 1; k <= h && k < kUserWtFrameSamples / 2; ++k)
            {
                // Same high-partial taper as the factory bank, used only at
                // the limit of the selected MIP; DC is intentionally removed.
                float window = 1.0f;
                const float edge = (float) h * 0.78f;
                if ((float) k > edge && h > 4)
                    window = 0.5f + 0.5f * std::cos ((float) kPi * ((float) k - edge) / ((float) h - edge));
                const auto v = source[(size_t) k] * (scale * window);
                freq[(size_t) k] = v;
                freq[(size_t) (len - k)] = std::conj (v);
            }
            Fft::inverse (freq);
            auto* dst = t.levels[(size_t) level].data() + (size_t) frame * (size_t) len;
            for (int i = 0; i < len; ++i) dst[i] = freq[(size_t) i].real();
        }
    }
    return result;
}

// A packed wavetable has an explicitly selected power-of-two cycle length.
// This is not an auto-detector for ordinary recordings.
inline bool unpackWavetableFrames (const std::vector<float>& mono, int cycleLength,
                                   std::vector<float>& out)
{
    if (cycleLength < 32 || cycleLength > 8192 || (cycleLength & (cycleLength - 1)) != 0
        || mono.empty() || mono.size() % (size_t) cycleLength != 0) return false;
    const size_t frames = mono.size() / (size_t) cycleLength;
    if (frames < 1 || frames > kUserWtMaxFrames) return false;
    for (float x : mono) if (! std::isfinite (x) || std::fabs (x) > 8.0f) return false;
    std::vector<float> candidate (frames * kUserWtFrameSamples);
    std::vector<std::complex<float>> src ((size_t) cycleLength);
    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (int i = 0; i < cycleLength; ++i) src[(size_t) i] = { mono[frame * (size_t) cycleLength + (size_t) i], 0.0f };
        Fft::forward (src);
        std::vector<std::complex<float>> dest ((size_t) kUserWtFrameSamples);
        const float scale = (float) kUserWtFrameSamples / (float) cycleLength;
        for (int k = 1; k < std::min (cycleLength / 2, kUserWtFrameSamples / 2); ++k)
        {
            dest[(size_t) k] = src[(size_t) k] * scale;
            dest[(size_t) (kUserWtFrameSamples - k)] = std::conj (dest[(size_t) k]);
        }
        Fft::inverse (dest);
        for (int i = 0; i < kUserWtFrameSamples; ++i)
            candidate[frame * kUserWtFrameSamples + (size_t) i] = dest[(size_t) i].real();
    }
    if (! validUserFrames (candidate, (int) frames)) return false;
    out.swap (candidate);
    return true;
}

// Pitch estimate for a voiced snapshot. Returns zero for weak/aperiodic input;
// in that case resynthesis uses a fixed frequency-bin mapping instead.
inline double estimateResynthPitch (const std::vector<float>& mono, size_t center, double sr)
{
    const int stride = std::max (1, (int) std::lround (sr / 12000.0));
    constexpr int n = 512;
    std::array<float, n> x {};
    const long long start = (long long) center - (long long) n * stride / 2;
    double mean = 0.0, energy = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        for (int j = 0; j < stride; ++j)
        {
            const auto pos = start + (long long) i * stride + j;
            if (pos >= 0 && pos < (long long) mono.size()) sum += mono[(size_t) pos];
        }
        x[(size_t) i] = (float) (sum / stride);
        mean += x[(size_t) i];
    }
    mean /= n;
    for (auto& v : x) { v -= (float) mean; energy += (double) v * v; }
    if (energy < 1.0e-8) return 0.0;
    const double reducedRate = sr / stride;
    const int lo = std::max (2, (int) (reducedRate / 1400.0));
    const int hi = std::min (n / 2, (int) (reducedRate / 45.0));
    std::array<double, n / 2 + 2> corr {};
    for (int lag = lo; lag <= hi; ++lag)
    {
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (int i = 0; i < n - lag; ++i)
        {
            const double a = x[(size_t) i], b = x[(size_t) (i + lag)];
            xy += a * b; xx += a * a; yy += b * b;
        }
        corr[(size_t) lag] = xy / std::sqrt (std::max (1.0e-30, xx * yy));
    }
    for (int lag = lo + 1; lag < hi; ++lag)
        if (corr[(size_t) lag] >= 0.78 && corr[(size_t) lag] >= corr[(size_t) lag - 1]
            && corr[(size_t) lag] >= corr[(size_t) lag + 1])
        {
            const double a = corr[(size_t) lag - 1], b = corr[(size_t) lag], c = corr[(size_t) lag + 1];
            const double denominator = a - 2.0 * b + c;
            const double offset = std::abs (denominator) > 1.0e-12 ? clamp (0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0;
            return reducedRate / (lag + offset);
        }
    return 0.0;
}

// Ordinary audio -> 64 evolving harmonic snapshots. Input may be longer than a
// wavetable; snapshot centres span the *whole* recording. The result is a
// playable wavetable, not a time-stretched reproduction or a sample player.
inline bool resynthesizeWavetable (const std::vector<float>& mono, double sr,
                                   std::vector<float>& out, int frameCount = kResynthFrames)
{
    if (mono.size() < 32 || ! std::isfinite (sr) || sr < 8000.0 || sr > 384000.0
        || frameCount < 1 || frameCount > kUserWtMaxFrames) return false;
    for (float x : mono) if (! std::isfinite (x) || std::fabs (x) > 8.0f) return false;
    constexpr int fftSize = 8192;
    std::vector<float> candidate ((size_t) frameCount * kUserWtFrameSamples);
    std::vector<std::complex<float>> spec ((size_t) fftSize);
    double windowSum = 0.0;
    for (int i = 0; i < fftSize; ++i) windowSum += 0.5 - 0.5 * std::cos (kTwoPi * i / (fftSize - 1));
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const double fraction = frameCount == 1 ? 0.5 : (double) frame / (frameCount - 1);
        const size_t margin = std::min ((size_t) fftSize / 2, mono.size() / 2);
        const size_t center = margin + (size_t) std::llround (fraction * (double) (mono.size() - 2 * margin));
        const long long start = (long long) center - fftSize / 2;
        for (int i = 0; i < fftSize; ++i)
        {
            const auto pos = start + i;
            const float s = pos >= 0 && pos < (long long) mono.size() ? mono[(size_t) pos] : 0.0f;
            const float w = (float) (0.5 - 0.5 * std::cos (kTwoPi * i / (fftSize - 1)));
            spec[(size_t) i] = { s * w, 0.0f };
        }
        Fft::forward (spec);
        double pitch = estimateResynthPitch (mono, center, sr);
        if (pitch <= 0.0) pitch = sr * 8.0 / fftSize; // full-band aperiodic fallback
        const auto at = [&spec] (double bin)
        {
            const int i = (int) bin;
            if (i < 1 || i + 1 >= fftSize / 2) return std::complex<float> (0.0f, 0.0f);
            const float f = (float) (bin - i);
            // A centred Hann window changes the phase by pi between adjacent
            // DFT bins. Rotate them to the window centre before interpolating;
            // otherwise an off-bin sine can cancel almost completely.
            const float sign = (i & 1) ? -1.0f : 1.0f;
            return sign * (spec[(size_t) i] * (1.0f - f) - spec[(size_t) i + 1] * f);
        };
        const double fundamentalBin = pitch * fftSize / sr;
        const auto first = at (fundamentalBin);
        const double basePhase = std::arg (first);
        std::vector<std::complex<float>> dest ((size_t) kUserWtFrameSamples);
        for (int k = 1; k < kUserWtFrameSamples / 2; ++k)
        {
            const double bin = fundamentalBin * k;
            if (bin >= fftSize / 2 - 1) break;
            const std::complex<float> align ((float) std::cos (-basePhase * k), (float) std::sin (-basePhase * k));
            const auto v = at (bin) * align * (float) ((double) kUserWtFrameSamples / windowSum);
            dest[(size_t) k] = v;
            dest[(size_t) (kUserWtFrameSamples - k)] = std::conj (v);
        }
        Fft::inverse (dest);
        for (int i = 0; i < kUserWtFrameSamples; ++i)
            candidate[(size_t) frame * kUserWtFrameSamples + (size_t) i] = dest[(size_t) i].real();
    }
    // A global, not per-frame, scaling only when necessary. Relative levels
    // and morph movement are preserved; this is not an output limiter.
    float peak = 0.0f;
    for (float x : candidate) peak = std::max (peak, std::fabs (x));
    if (peak > 1.0f) for (auto& x : candidate) x /= peak;
    if (! validUserFrames (candidate, frameCount)) return false;
    out.swap (candidate);
    return true;
}

} // namespace orogen

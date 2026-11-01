// Immutable per-slot sample for Granular. File decoding and this preparation
// are control/worker-thread operations; an audio voice only reads the result.
#pragma once

#include "sample_bank.h"
#include <memory>

namespace orogen {

// Bounded so embedding eight samples in a DAW project remains predictable.
// The frame cap also applies when a high sample rate reaches it before 30 s.
inline constexpr size_t kUserSampleMinFrames = 512;
inline constexpr size_t kUserSampleMaxFrames = 1440000;
inline constexpr double kUserSampleMaxSeconds = 30.0;
inline constexpr double kUserSampleMinRate = 8000.0;
inline constexpr double kUserSampleMaxRate = 192000.0;

struct UserSample
{
    OneShot shot;
    double sourceRate = 48000.0;
};

inline bool validUserSample (const std::vector<float>& samples, double sourceRate) noexcept
{
    if (! std::isfinite (sourceRate) || sourceRate < kUserSampleMinRate || sourceRate > kUserSampleMaxRate
        || samples.size() < kUserSampleMinFrames || samples.size() > kUserSampleMaxFrames
        || (double) samples.size() > sourceRate * kUserSampleMaxSeconds + 0.5)
        return false;
    for (const float s : samples)
        if (! std::isfinite (s) || std::fabs (s) > 8.0f) return false;
    return true;
}

inline std::shared_ptr<const UserSample> makeUserSample (
    std::vector<float> samples, double sourceRate, std::string name)
{
    if (! validUserSample (samples, sourceRate)) return {};
    auto result = std::make_shared<UserSample>();
    result->sourceRate = sourceRate;
    result->shot.name = std::move (name);
    result->shot.rootNote = 60; // overridden by the per-slot root-note parameter
    result->shot.levels.emplace_back (std::move (samples));
    buildMipLevels (result->shot, sourceRate);
    return result;
}

} // namespace orogen

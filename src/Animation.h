#pragma once

#include "Common.h"

namespace ld
{

/// Critically tunable damped spring integrated with a fixed sub-step so the
/// motion feels identical on 60 Hz, 120 Hz and 144 Hz displays.
struct SpringParams
{
    /// Oscillation frequency in Hz.
    float frequency = 4.2f;

    /// 1.0 = critically damped, < 1 overshoots.
    float damping = 0.72f;

    /// Position tolerance below which the spring is considered settled.
    float positionEpsilon = 0.0008f;

    /// Velocity tolerance (units per second).
    float velocityEpsilon = 0.06f;
};

class Spring
{
public:
    float value = 0.0f;
    float target = 0.0f;
    float velocity = 0.0f;

    /// Advances the spring by dt seconds. dt is split into sub steps so that
    /// large frame gaps (a stall, a slow machine) stay numerically stable.
    void Update(double dt, const SpringParams& params);

    /// Snaps to the target and clears velocity.
    void Reset(float newValue)
    {
        value = newValue;
        target = newValue;
        velocity = 0.0f;
    }

    bool Settled(const SpringParams& params) const
    {
        return std::fabs(value - target) < params.positionEpsilon
            && std::fabs(velocity) < params.velocityEpsilon;
    }

    void AddImpulse(float amount) { velocity += amount; }
};

/// Bounce profile used while an application is launching.
struct BounceParams
{
    SpringParams spring{2.6f, 0.34f, 0.02f, 0.35f};

    /// Upward kick applied each cycle, in pixels per second.
    float impulse = 380.0f;

    /// Seconds between impulses.
    float interval = 0.46f;

    /// Give up after this many seconds even if the process was never seen.
    float timeout = 12.0f;
};

} // namespace ld

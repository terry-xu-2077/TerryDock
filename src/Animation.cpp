#include "Animation.h"

namespace ld
{

void Spring::Update(double dt, const SpringParams& params)
{
    if (dt <= 0.0)
    {
        return;
    }

    // Clamp: a very long stall should not teleport the item.
    if (dt > 0.25)
    {
        dt = 0.25;
    }

    constexpr double kMaxStep = 1.0 / 240.0;

    int steps = static_cast<int>(std::ceil(dt / kMaxStep));
    if (steps < 1)
    {
        steps = 1;
    }

    if (steps > 64)
    {
        steps = 64;
    }

    const double h = dt / static_cast<double>(steps);
    const float omega = 2.0f * kPi * params.frequency;
    const float omegaSq = omega * omega;
    const float dampingTerm = 2.0f * params.damping * omega;

    for (int i = 0; i < steps; ++i)
    {
        const float acceleration =
            -omegaSq * (value - target) - dampingTerm * velocity;

        velocity += acceleration * static_cast<float>(h);
        value += velocity * static_cast<float>(h);
    }

    if (Settled(params))
    {
        value = target;
        velocity = 0.0f;
    }
}

} // namespace ld

// Фаза тонального источника с синусоидальным вибрато.
// frequency(t) = fundamental * (1 + depth * sin(2*pi*rate*t + phase)).
// Возвращает интеграл частоты в оборотах от t=0, а не frequency(t)*t:
// последний вариант добавляет растущую со временем ошибку высоты.
#pragma once

#include "../core/dsp.h"
#include <cmath>

namespace orogen {
inline double sinusoidalVibratoCycles (double fundamental, double time, double rate,
                                       double depth, double phase) noexcept
{
    const double omega = kTwoPi * rate;
    if (std::abs (omega) < 1.0e-12)
        return fundamental * time * (1.0 + depth * std::sin (phase));
    return fundamental * (time + (depth / omega)
                            * (std::cos (phase) - std::cos (omega * time + phase)));
}
} // namespace orogen

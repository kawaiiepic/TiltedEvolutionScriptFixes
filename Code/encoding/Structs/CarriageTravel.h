#pragma once

#include <chrono>

namespace CarriageTravel
{
inline constexpr auto kBoardingLifetime = std::chrono::seconds(60);
inline constexpr float kBoardingDistance = 512.f;
}

#pragma once
#include <cstdint>

namespace MC::PIL::PerformanceTimer {

// @brief	The current value of the OS's high-resolution, monotonic counter (Windows:
// @brief	QueryPerformanceCounter). Only the difference between two readings means anything.
std::int64_t Ticks();

// @brief	How many ticks the counter advances per second (Windows: QueryPerformanceFrequency).
// @brief	Fixed while the system runs.
std::int64_t TicksPerSecond();

}  // namespace MC::PIL::PerformanceTimer

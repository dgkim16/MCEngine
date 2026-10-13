#include "PIL/PerformanceTimer.h"
#include <windows.h>

namespace MC::PIL::PerformanceTimer {

std::int64_t Ticks()
{
	LARGE_INTEGER count;
	QueryPerformanceCounter(&count);
	return count.QuadPart;
}

std::int64_t TicksPerSecond()
{
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	return frequency.QuadPart;
}

}  // namespace MC::PIL::PerformanceTimer

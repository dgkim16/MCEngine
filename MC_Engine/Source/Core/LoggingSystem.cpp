#include "Core/LoggingSystem.h"
#include "PIL/DebugOutput.h"

namespace MC {

	void LoggingSystem::Write(Level, std::string_view message)
	{
		// The level is not printed yet, so messages read exactly like the OutputDebugString
		// calls they replace. It is kept for a later level filter or log file.
		PIL::DebugOutput(message);
	}

}  // namespace MC
#pragma once

namespace MC::PIL {

enum class PumpResult { Empty, Handled, Quit };

// @brief	Handles one queued OS event for this thread, delivering it to the window it is for.
// @brief	Empty: the queue was empty. Quit: the OS asked the application to quit, and exitCode
// @brief	gets the code it sent.
PumpResult PumpOneEvent(int& exitCode);

}  // namespace MC::PIL

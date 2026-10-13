#pragma once
#include <string_view>

namespace MC::PIL {

// @brief	Writes one line of UTF-8 text to the platform's debug output (Windows: the
// @brief	debugger's Output window). Appends the newline. Works before anything else in
// @brief	the engine exists, and depends on nothing in it.
void DebugOutput(std::string_view message);

}  // namespace MC::PIL

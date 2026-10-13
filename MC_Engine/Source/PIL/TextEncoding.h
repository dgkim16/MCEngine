#pragma once
#include <string>
#include <string_view>

namespace MC::PIL {

// @brief	Converts wide text from OS APIs (Windows: UTF-16, e.g. DXGI adapter names, the
// @brief	command line) to UTF-8, the encoding of all engine text and of LoggingSystem.
std::string ToUtf8(std::wstring_view text);

// @brief	Converts engine UTF-8 text to the OS's wide encoding (Windows: UTF-16), for OS
// @brief	APIs that take wide strings. Used inside the PIL's own implementation files.
std::wstring ToWide(std::string_view text);

}  // namespace MC::PIL

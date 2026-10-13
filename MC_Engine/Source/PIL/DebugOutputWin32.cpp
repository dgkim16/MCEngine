#include "PIL/DebugOutput.h"
#include "PIL/TextEncoding.h"
#include <windows.h>
#include <string>

namespace MC::PIL {

void DebugOutput(std::string_view message)
{
	// The engine's text is UTF-8 (/utf-8). OutputDebugStringA would read it in the ANSI code
	// page and garble anything non-ASCII, so convert to UTF-16 and use the W version.
	std::wstring line = ToWide(message);
	line += L'\n';
	OutputDebugStringW(line.c_str());
}

}  // namespace MC::PIL

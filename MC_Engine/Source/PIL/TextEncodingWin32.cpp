#include "PIL/TextEncoding.h"
#include <windows.h>

namespace MC::PIL {

std::string ToUtf8(std::wstring_view text)
{
	// A wstring_view is not null-terminated, so the length is passed explicitly. The first
	// call only measures; the second converts into a string of exactly that size.
	const int size = static_cast<int>(text.size());
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), size, utf8.data(), length, nullptr, nullptr);
	return utf8;
}

std::wstring ToWide(std::string_view text)
{
	// Same two-call pattern as ToUtf8, in the other direction.
	const int size = static_cast<int>(text.size());
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
	std::wstring wide(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), length);
	return wide;
}

}  // namespace MC::PIL

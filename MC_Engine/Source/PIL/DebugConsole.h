#pragma once

namespace MC::PIL {

// @brief	Opens a console window for std::cout/std::cerr/std::cin, titles it and gives it the
// @brief	application's icon. instance is the native application handle (Windows: the
// @brief	HINSTANCE) and iconId the icon resource; both come from the App layer.
void CreateDebugConsole(void* instance, int iconId);

}  // namespace MC::PIL

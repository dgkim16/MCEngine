#pragma once
#include <filesystem>
#include <optional>
#include <string_view>

namespace MC::PIL::FileDialog {

enum class Mode { Save, Open };

// @brief	Shows the OS file picker and returns the chosen path, or nullopt if the user
// @brief	cancelled or the dialog could not be created. Save mode appends filterPattern's
// @brief	extension when the user typed none, and creates defaultDir if it is missing.
// @brief	filterLabel and filterPattern are UTF-8, e.g. "Scene JSON" and "*.json".
// @brief	ownerWindow is the native window the dialog belongs to (Windows: the HWND), or
// @brief	nullptr for none.
std::optional<std::filesystem::path> Show(
	Mode mode,
	const std::filesystem::path& defaultDir,
	std::string_view filterLabel,
	std::string_view filterPattern,
	void* ownerWindow);

}  // namespace MC::PIL::FileDialog

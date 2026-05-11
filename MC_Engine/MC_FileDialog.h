#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <Windows.h>

namespace MCFileDialog {
    enum class Mode { Save, Open };

    // Returns the picked path, or nullopt if the user cancelled or the dialog
    // failed to construct. Save mode appends filterPattern's extension if the
    // user didn't type one and creates defaultDir if missing.
    std::optional<std::filesystem::path> Show(
        Mode                          mode,
        const std::filesystem::path& defaultDir,
        const std::wstring& filterLabel,    // e.g. L"Scene JSON"
        const std::wstring& filterPattern,  // e.g. L"*.json"
        HWND                          owner);
}
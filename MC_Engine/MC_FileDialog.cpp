#include "MC_FileDialog.h"
#include <ShObjIdl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

std::optional<std::filesystem::path> MCFileDialog::Show(
    Mode                          mode,
    const std::filesystem::path& defaultDir,
    const std::wstring& filterLabel,
    const std::wstring& filterPattern,
    HWND                          owner)
{
    // ImGui_ImplWin32_Init already called OleInitialize on this thread.
    ComPtr<IFileDialog> dialog;
    HRESULT hr = CoCreateInstance(
        mode == Mode::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
        nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return std::nullopt;

    COMDLG_FILTERSPEC filter[] = { { filterLabel.c_str(), filterPattern.c_str() } };
    dialog->SetFileTypes(1, filter);
    dialog->SetFileTypeIndex(1);

    std::error_code ec;
    auto absDefault = std::filesystem::absolute(defaultDir, ec);
    if (mode == Mode::Save) std::filesystem::create_directories(absDefault, ec);
    if (!ec) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
            absDefault.wstring().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    if (mode == Mode::Save) {
        // "*.json" -> "json"
        std::wstring ext = filterPattern;
        if (auto pos = ext.find(L'.'); pos != std::wstring::npos)
            ext = ext.substr(pos + 1);
        dialog->SetDefaultExtension(ext.c_str());
    }

    if (FAILED(dialog->Show(owner))) return std::nullopt;   // HRESULT_FROM_WIN32(ERROR_CANCELLED) on cancel

    ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(&result))) return std::nullopt;

    PWSTR raw = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw)
        return std::nullopt;
    std::filesystem::path picked = raw;
    CoTaskMemFree(raw);
    return picked;
}
#include "PIL/FileDialog.h"
#include "PIL/TextEncoding.h"
#include <windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
// Initializes COM on this thread for one dialog. Nothing else in the engine does, and the
// shell dialogs need it. Each successful CoInitializeEx, S_FALSE included, needs its own
// CoUninitialize. RPC_E_CHANGED_MODE means the thread is already in a multithreaded
// apartment: COM is usable, but that initialization is not ours to undo.
struct ScopedComInit {
	HRESULT hr;
	ScopedComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
	~ScopedComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
	ScopedComInit(const ScopedComInit&) = delete;
	ScopedComInit& operator=(const ScopedComInit&) = delete;
};
}

namespace MC::PIL::FileDialog {

std::optional<std::filesystem::path> Show(
	Mode mode,
	const std::filesystem::path& defaultDir,
	std::string_view filterLabel,
	std::string_view filterPattern,
	void* ownerWindow)
{
	// Declared before every ComPtr below, so it is destroyed after them: all COM objects
	// are released before CoUninitialize.
	const ScopedComInit com;

	ComPtr<IFileDialog> dialog;
	HRESULT hr = CoCreateInstance(
		mode == Mode::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
		nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
	if (FAILED(hr)) return std::nullopt;

	const std::wstring label = ToWide(filterLabel);
	const std::wstring pattern = ToWide(filterPattern);
	COMDLG_FILTERSPEC filter[] = { { label.c_str(), pattern.c_str() } };
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
		std::wstring ext = pattern;
		if (auto pos = ext.find(L'.'); pos != std::wstring::npos)
			ext = ext.substr(pos + 1);
		dialog->SetDefaultExtension(ext.c_str());
	}

	if (FAILED(dialog->Show(static_cast<HWND>(ownerWindow)))) return std::nullopt;   // HRESULT_FROM_WIN32(ERROR_CANCELLED) on cancel

	ComPtr<IShellItem> result;
	if (FAILED(dialog->GetResult(&result))) return std::nullopt;

	PWSTR raw = nullptr;
	if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw)
		return std::nullopt;
	std::filesystem::path picked = raw;
	CoTaskMemFree(raw);
	return picked;
}

}  // namespace MC::PIL::FileDialog

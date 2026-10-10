// Debug-only verification harness (bridge B1): capture sweep, headless test
// runner and D3D12 message log. Driven by the command line:
//
//   --capture-sweep <dir> --tier quick|full [--poison <pass|*>]
//       Walks a fixed (scene, configuration) table, Ch7 first. Per entry it writes
//       <dir>\<scene>_<config>.bmp (the image the Viewport panel shows), plus
//       .fg.txt (FrameGraph dump and aliasing plan) and .timers.txt, then exits
//       with the number of failed entries (0 on success). --poison installs a
//       FrameGraph pre-pass hook that, before every pass whose TypeName() is
//       <pass> (every pass for '*'), records a 1x1 viewport and scissor, no render
//       targets, stencil ref 0xFF, the debug-line and grass-cull root signatures,
//       the debug-line PSO, a point-list topology and null vertex and index
//       buffers. It never touches the descriptor heap. A self-contained pass
//       renders the same. A pass that relies on state an earlier pass set renders
//       differently, or records invalid commands: one that sets a PSO but no root
//       signature draws under the poisoned root signature. The debug layer reports
//       that while the list records, and executing such a list can hang the GPU.
//       So a poisoned run never submits a frame whose recording logged a D3D12
//       ERROR or CORRUPTION: the sweep stops there, and that entry and every
//       later one count as failed. An unknown pass name exits with code 2.
//   --run-tests <file>
//       Runs the Tests window suites once at the top of Update, writes one
//       PASS/FAIL line per test to <file>, exits with the number of failures.
//   --verify-log <file>
//       Logs every D3D12 WARNING/ERROR/CORRUPTION with break-on-severity off, and
//       at exit the ReportLiveObjects output under "== LIVE OBJECTS ==". The file
//       is also the process's stderr, so it keeps the text of a failed assert.
//   --shadow-casters <n>
//       RegisterLights adds n - 1 shadow-casting directional lights after the
//       scene light (T13a, two casters). The shaders light with gLightData[0]
//       only, so the extra casters add shadow passes and leave the image alone.
//   --fill-materials <extra>
//       On the first frame, registers materials in the engine's material manager
//       until its free list is full, then <extra> more (T13b on the live
//       MaterialCB). The first one past capacity throws out of Update, as Create >
//       Material would. The fillers use defaultTex, no item uses them, and they
//       are never saved.
//   --open-scene <path>   (with --capture-sweep <dir>)
//       On the first frame, File > Open with <path> in place of the Open dialog;
//       its error dialog, if it shows one, waits for a click. The sweep then
//       captures NoScene_default (the viewport with no active scene, after a
//       failed load) and Ch7_default (the switch back) instead of its tier.
//   --viewport-modes <list>   (with --capture-sweep <dir>)
//       Captures Ch7 with blur on once per Viewport resolution mode in <list>,
//       in order, instead of its tier, as Ch7_blur_<n>_<mode>.bmp. <list> is
//       comma-separated ViewportResMode names: Free, Ratio16x9, Ratio1x1, HD,
//       FullHD, K4. An unknown name exits with code 2. The L10 repro (transient
//       heap growth) is Free,K4,HD,K4.
//
// Without a flag the harness only serves the Tests window's "Capture viewport" button.
#include "Legacy/MCEngine.h"

#if defined(_DEBUG)
#include "Legacy/MCScene.h"
#include "Legacy/MC_AssetTests.h"
#include "imgui.h"
#include <crtdbg.h>
#include <dxgidebug.h>
#include <io.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

enum class SweepConfig {
	Default, Msaa, Wireframe, Blur, SobelDefault, SobelGaussian, SobelDepth, FrozenMoved,
	GrassCpu, GrassCpuCells, GrassGpu,
};

const char* ConfigName(SweepConfig c) {
	switch (c) {
	case SweepConfig::Default:       return "default";
	case SweepConfig::Msaa:          return "msaa";
	case SweepConfig::Wireframe:     return "wireframe";
	case SweepConfig::Blur:          return "blur";
	case SweepConfig::SobelDefault:  return "sobel_default";
	case SweepConfig::SobelGaussian: return "sobel_gaussian";
	case SweepConfig::SobelDepth:    return "sobel_depth";
	case SweepConfig::FrozenMoved:   return "frozen_moved";
	case SweepConfig::GrassCpu:      return "grass_cpu";
	case SweepConfig::GrassCpuCells: return "grass_cpu_cells";
	case SweepConfig::GrassGpu:      return "grass_gpu";
	}
	return "unknown";
}

struct SweepEntry {
	std::string scene;
	SweepConfig config;
	int viewport = -1;	// --viewport-modes: a ViewportResMode, index into kViewportModeNames; -1 keeps HD
};

// MCEngine::ViewportResMode, in enum order.
constexpr const char* kViewportModeNames[] = { "Free", "Ratio16x9", "Ratio1x1", "HD", "FullHD", "K4" };

// The V-img tiers. Ch7 always comes first: its tree sprites draw from the global
// rand(), so the layout only repeats in a fresh process that loads Ch7 first.
std::vector<SweepEntry> BuildSweepTable(bool full) {
	using C = SweepConfig;
	if (!full)	// quick: 4 captures
		return { {"Ch7", C::Default}, {"Ch7", C::Msaa}, {"Ch7", C::SobelGaussian}, {"Grass", C::GrassGpu} };
	std::vector<SweepEntry> table;	// full: 5 scenes x 8 + Grass x 3 = 43 captures
	for (const char* scene : { "Ch7", "Grass", "Empty", "SoloVoid", "SoloBox" }) {
		for (C c : { C::Default, C::Msaa, C::Wireframe, C::Blur, C::SobelDefault, C::SobelGaussian, C::SobelDepth, C::FrozenMoved })
			table.push_back({ scene, c });
		if (std::string(scene) == "Grass")
			for (C c : { C::GrassCpu, C::GrassCpuCells, C::GrassGpu })
				table.push_back({ scene, c });
	}
	return table;
}

// --viewport-modes: Ch7 with blur on, once per mode in the comma-separated list.
// Empty when a name is unknown.
std::vector<SweepEntry> BuildViewportTable(const std::wstring& list) {
	std::vector<SweepEntry> table;
	std::wstringstream names(list);
	for (std::wstring name; std::getline(names, name, L',');) {
		const std::string narrow = fs::path(name).string();
		int mode = 0;
		while (mode < static_cast<int>(std::size(kViewportModeNames)) && narrow != kViewportModeNames[mode])
			++mode;
		if (mode == static_cast<int>(std::size(kViewportModeNames)))
			return {};
		table.push_back({ "Ch7", SweepConfig::Blur, mode });
	}
	return table;
}

// Writes to stderr, which --verify-log redirects to its file.
struct VerifyLog {
	bool open = false;
	std::atomic<bool> sawError{ false };	// a D3D12 ERROR or CORRUPTION arrived, logged or not; read by RejectPoisonedFrame
	std::mutex mutex;	// the D3D12 message callback may run on any thread
	void Line(const std::string& text) {
		std::lock_guard<std::mutex> lock(mutex);
		if (open) { std::fputs(text.c_str(), stderr); std::fputc('\n', stderr); std::fflush(stderr); }
	}
};

// D3D12_MESSAGE_SEVERITY and DXGI_INFO_QUEUE_MESSAGE_SEVERITY share these values.
const char* SeverityName(int severity) {
	switch (severity) {
	case 0:  return "CORRUPTION";
	case 1:  return "ERROR";
	case 2:  return "WARNING";
	case 3:  return "INFO";
	default: return "MESSAGE";
	}
}

std::string D3D12MessageLine(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char* description) {
	return std::string("D3D12 ") + SeverityName(severity) + " #" + std::to_string(static_cast<int>(id))
		+ ": " + (description ? description : "");
}

void CALLBACK OnD3D12Message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
	D3D12_MESSAGE_ID id, LPCSTR description, void* context) {
	if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
		static_cast<VerifyLog*>(context)->sawError = true;
	if (severity <= D3D12_MESSAGE_SEVERITY_WARNING)
		static_cast<VerifyLog*>(context)->Line(D3D12MessageLine(severity, id, description));
}

void DrainInfoQueue(ID3D12InfoQueue* queue, VerifyLog& log) {
	const UINT64 count = queue->GetNumStoredMessages();
	for (UINT64 i = 0; i < count; ++i) {
		SIZE_T size = 0;
		if (FAILED(queue->GetMessage(i, nullptr, &size))) continue;
		std::vector<char> buffer(size);
		auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
		if (FAILED(queue->GetMessage(i, message, &size))) continue;
		if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
			log.sawError = true;
		if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
			log.Line(D3D12MessageLine(message->Severity, message->ID, message->pDescription));
	}
	queue->ClearStoredMessages();
}

}  // namespace

struct MCEngine::DebugHarness {
	// Modes, from the command line.
	bool sweep = false;
	bool runTests = false;
	bool testsDone = false;
	fs::path sweepDir;
	fs::path testsFile;
	std::wstring poison;	// --poison <pass|*>, installed on the first frame of a sweep
	bool poisoning = false;	// the --poison hook is installed; RejectPoisonedFrame checks every frame
	int shadowCasters = 1;	// --shadow-casters, read by RegisterLights
	int fillMaterials = -1;	// --fill-materials <extra>, consumed on the first frame; -1 without the flag
	std::string openScene;	// --open-scene <path>, consumed on the first frame

	// Capture sweep.
	std::vector<SweepEntry> entries;
	size_t entry = 0;
	enum class Phase { Switch, Apply, Settle, Wait } phase = Phase::Switch;
	int settledFrames = 0;
	int failures = 0;

	// One viewport capture in flight: armed -> copied in Draw -> written after its fence.
	bool captureArmed = false;
	fs::path capturePath;
	ComPtr<ID3D12Resource> readback;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT64 captureFence = 0;

	// --verify-log.
	VerifyLog log;
	ComPtr<ID3D12InfoQueue1> callbackQueue;
	DWORD callbackCookie = 0;
	ComPtr<ID3D12InfoQueue> drainQueue;	// drained once per frame when the callback is unavailable
};

void MCEngine::InitDebugHarness() {
	mHarness = decltype(mHarness)(new DebugHarness(), +[](DebugHarness* p) { delete p; });
	DebugHarness& h = *mHarness;

	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring tier = L"quick";
	std::wstring viewportModes;
	fs::path verifyLog;
	for (int i = 1; argv && i + 1 < argc; ++i) {	// every flag takes one value
		const std::wstring flag = argv[i];
		if      (flag == L"--capture-sweep") { h.sweep = true; h.sweepDir = argv[++i]; }
		else if (flag == L"--tier")          tier = argv[++i];
		else if (flag == L"--poison")        h.poison = argv[++i];
		else if (flag == L"--run-tests")     { h.runTests = true; h.testsFile = argv[++i]; }
		else if (flag == L"--verify-log")    verifyLog = argv[++i];
		else if (flag == L"--shadow-casters") h.shadowCasters = _wtoi(argv[++i]);
		else if (flag == L"--fill-materials") h.fillMaterials = _wtoi(argv[++i]);
		else if (flag == L"--open-scene")    h.openScene = fs::path(argv[++i]).string();
		else if (flag == L"--viewport-modes") viewportModes = argv[++i];
	}
	LocalFree(argv);

	if (h.runTests)
		h.sweep = false;	// the tests switch scenes and re-enter Ch7; a sweep after them would not repeat
	if (h.sweep && tier != L"quick" && tier != L"full") {
		OutputDebugStringW((L"[harness] unknown --tier '" + tier + L"'\n").c_str());
		h.sweep = false;
		PostQuitMessage(2);
	}
	if (h.sweep) {
		h.entries = h.openScene.empty() ? BuildSweepTable(tier == L"full")
			: std::vector<SweepEntry>{ {"NoScene", SweepConfig::Default}, {"Ch7", SweepConfig::Default} };
		if (!viewportModes.empty()) {
			h.entries = BuildViewportTable(viewportModes);
			if (h.entries.empty()) {
				OutputDebugStringW((L"[harness] unknown mode in --viewport-modes '" + viewportModes + L"'\n").c_str());
				PostQuitMessage(2);
			}
		}
		std::error_code ec;
		fs::create_directories(h.sweepDir, ec);
		// The Viewport panel's fixed HD mode: captures do not depend on window size,
		// DPI or the docked layout.
		mViewportResMode = ViewportResMode::HD;
		mViewportZoom = 0.0f;
		mAutosaveOnSwitch = false;	// a sweep must never write scene files
	}
	if (IsHarnessDriving()) {
		// Unattended: no CRT dialog, and focus changes never pause it. A failed
		// assert() prints to stderr and exits with code 3; _ASSERTE and CRT error
		// reports print to stderr and continue.
		_set_error_mode(_OUT_TO_STDERR);	// assert() in a WinMain app shows a message box otherwise
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		mAppPaused = false;
	}

	// A poisoned sweep needs the messages too, with or without a log: RejectPoisonedFrame.
	if (verifyLog.empty() && (!h.sweep || h.poison.empty()))
		return;
	// The log becomes stderr: the CRT stream, and the Win32 handle, which assert()
	// writes to directly while it is a console and _CRTDBG_FILE_STDERR resolves to.
	FILE* stream = nullptr;
	if (!verifyLog.empty() && _wfreopen_s(&stream, verifyLog.c_str(), L"w", stderr) == 0) {
		setvbuf(stderr, nullptr, _IONBF, 0);
		SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
		h.log.open = true;
	}
	ComPtr<ID3D12InfoQueue> queue;
	if (FAILED(md3dDevice.As(&queue))) {
		h.log.Line("== NO D3D12 INFO QUEUE ==");
		return;
	}
	// These modes only: report instead of breaking, so the run can finish.
	queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
	queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
	queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
	DrainInfoQueue(queue.Get(), h.log);	// stored while the device was created
	ComPtr<ID3D12InfoQueue1> queue1;
	if (SUCCEEDED(queue.As(&queue1)) && SUCCEEDED(queue1->RegisterMessageCallback(
			&OnD3D12Message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, &h.log, &h.callbackCookie)))
		h.callbackQueue = queue1;
	else
		h.drainQueue = queue;
}

bool MCEngine::IsHarnessDriving() const {
	return mHarness && (mHarness->sweep || mHarness->runTests);
}

int MCEngine::HarnessShadowCasters() const {
	return mHarness ? mHarness->shadowCasters : 1;
}

void MCEngine::TickDebugHarness() {
	if (!mHarness) return;
	DebugHarness& h = *mHarness;
	if (IsHarnessDriving() && ImGui::GetIO().IniFilename) {
		// Load the user's layout but never write it back: a run that quits after a
		// frame would save a half-settled layout into the tracked imgui.ini.
		ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
		ImGui::GetIO().IniFilename = nullptr;
	}
	if (h.drainQueue)
		DrainInfoQueue(h.drainQueue.Get(), h.log);

	// A copied capture is written once the fence of the frame that copied it completes.
	if (h.readback && mFence->GetCompletedValue() >= h.captureFence) {
		const bool written = CaptureViewportToBmp();
		OutputDebugStringW((std::wstring(written ? L"[harness] wrote " : L"[harness] FAILED to write ")
			+ h.capturePath.wstring() + L"\n").c_str());
		if (!written) ++h.failures;
		h.readback.Reset();
	}

	if (h.sweep && !h.poison.empty()) {
		const std::string target = fs::path(h.poison).string();
		h.poison.clear();
		bool known = target == "*";
		for (const auto& p : mPasses)
			known = known || target == p->TypeName();
		if (!known) {	// a misspelled pass would never be poisoned, and its gate would pass
			OutputDebugStringA(("[harness] unknown pass in --poison '" + target + "'\n").c_str());
			h.sweep = false;
			PostQuitMessage(2);
			return;
		}
		mFrameGraph->SetDebugPrePassHook([this, target](ID3D12GraphicsCommandList* cmdList, const RenderPass& pass) {
			if (target != "*" && target != pass.TypeName())
				return;
			const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
			const D3D12_RECT scissor = { 0, 0, 1, 1 };
			cmdList->RSSetViewports(1, &viewport);
			cmdList->RSSetScissorRects(1, &scissor);
			cmdList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
			cmdList->OMSetStencilRef(0xFF);
			cmdList->SetGraphicsRootSignature(mDebugLineRootSig.Get());		// unsets every graphics root argument
			cmdList->SetComputeRootSignature(mGrassCullRootSignature.Get());	// unsets every compute root argument
			cmdList->SetPipelineState(GetPSO(mScene4xMsaaState ? "debug_line_MSAA" : "debug_line"));
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
			cmdList->IASetVertexBuffers(0, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, nullptr);
			cmdList->IASetIndexBuffer(nullptr);
		});
		h.log.sawError = false;
		h.poisoning = true;
	}

	if (h.fillMaterials >= 0) {
		const int extra = h.fillMaterials;
		h.fillMaterials = -1;
		const TextureHandle tex = HashAssetIdentity<AssetKind::Texture>("defaultTex");
		CBFreeList& slots = mMaterialManager.MatCBFreeList();
		int n = 0;
		while (slots.HighWater() < slots.Capacity())
			mMaterialManager.Register("fill_material_" + std::to_string(n++), MCMaterial{}, tex);
		for (int i = 0; i < extra; ++i)	// past capacity: throws "CBFreeList exhausted"
			mMaterialManager.Register("fill_material_" + std::to_string(n++), MCMaterial{}, tex);
	}

	if (!h.openScene.empty()) {
		const std::string picked = h.openScene;
		h.openScene.clear();
		FileMenuOpenScene(picked);	// a failed load shows its dialog and leaves no active scene
		return;
	}

	if (h.runTests) {
		if (h.testsDone) return;
		h.testsDone = true;
		std::ofstream out(h.testsFile, std::ios::out | std::ios::trunc);
		int failed = out.is_open() ? 0 : 1;
		auto report = [&out, &failed](const char* tag, bool passed, const std::string& reason) {
			if (passed) out << "PASS " << tag << '\n';
			else { out << "FAIL " << tag << ": " << reason << '\n'; ++failed; }
		};
		// The Tests window's four "Run all" buttons, then the CBFreeList tab, in tab order.
		MCAssetTests::SetResultSink(report);
		MCAssetTests::RunAll(*this);
		MCAssetTests::RunAllRedirects(*this);
		MCAssetTests::RunAllResilience(*this);
		MCAssetTests::RunAllOutlier(*this);
		MCAssetTests::RunCBFreeListLifo();
		MCAssetTests::RunMaterialCapacity(*this);
		MCAssetTests::RunBigSceneSwitch(*this);
		MCAssetTests::SetResultSink(nullptr);
		out.close();
		PostQuitMessage(failed);
		return;
	}

	if (!h.sweep || h.entry >= h.entries.size())
		return;
	auto nextEntry = [&h]() {
		h.phase = DebugHarness::Phase::Switch;
		if (++h.entry == h.entries.size())
			PostQuitMessage(h.failures);
	};
	const SweepEntry& e = h.entries[h.entry];
	MCScene* active = mSceneManager.GetActive();
	const bool onScene = active ? active->name == e.scene : e.scene == "NoScene";	// NoScene: --open-scene

	switch (h.phase) {
	case DebugHarness::Phase::Switch:
		h.phase = DebugHarness::Phase::Apply;
		if (!onScene) {
			mSceneManager.RequestSwitch(e.scene);	// consumed later in this Update, like the scene combo
			return;
		}
		[[fallthrough]];
	case DebugHarness::Phase::Apply: {
		if (!onScene) {	// the switch failed; the "[switch] FAIL" line is in the debug output
			++h.failures;
			nextEntry();
			return;
		}
		// Defaults first, then the configuration, through the fields the UI writes.
		if (mScene4xMsaaState) { mScene4xMsaaState = false; mSceneSizeDirty = true; }
		mIsWireframe = false;
		blurValues = CSB_blurValues{};
		blurDirty = true;
		mIsSobel = false;
		mSobelType = SobelType::Gaussain;
		if (mFreezeCamera) { mFreezeCamera = false; DirtyAllRenderItems(); }
		mShowFrustum = false;
		isOrtho = false;
		mOrthoT = 0.0f;
		// Boot pose (BuildCamera).
		mMainCamera->SetPosition(0.0f, 3.0f, 20.0f);
		mMainCamera->LookAt(mMainCamera->GetPosition3f(), { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
		mMainCamera->UpdateViewMatrix();
		mCameraDirty = true;

		using C = SweepConfig;
		switch (e.config) {
		case C::Default:   break;
		case C::Msaa:      mScene4xMsaaState = true; mSceneSizeDirty = true; break;
		case C::Wireframe: mIsWireframe = true; break;
		case C::Blur:      blurValues.enabled = true; blurValues.sigma = 2.0f; blurValues.blurIter = 5; break;	// smoke matrix M4
		case C::SobelDefault:  mSobelType = SobelType::Default; mIsSobel = true; break;
		case C::SobelGaussian: mSobelType = SobelType::Gaussain; mIsSobel = true; blurValues.enabled = true; break;	// the checkbox enables blur for Gaussian
		case C::SobelDepth:    mSobelType = SobelType::Depth; mIsSobel = true; break;
		case C::FrozenMoved: {
			// Freeze as the "Freeze Culling Frustum" checkbox does, show the frozen
			// frustum, then move by a fixed offset with the same orientation.
			const DirectX::XMMATRIX view = mMainCamera->GetView();
			DirectX::XMVECTOR det = XMMatrixDeterminant(view);
			mMainCamera->GetFrustrum().Transform(mFrozenWorldFrustum, XMMatrixInverse(&det, view));
			XMStoreFloat4x4(&mFrozenViewProjT, XMMatrixTranspose(XMMatrixMultiply(view, mMainCamera->GetProj())));
			mFreezeCamera = true;
			DirtyAllRenderItems();
			mShowFrustum = true;
			mMainCamera->SetPosition(0.0f, 13.0f, 40.0f);	// boot pose + (0, 10, 20)
			break;
		}
		case C::GrassCpu:
		case C::GrassCpuCells:
		case C::GrassGpu: {
			auto* grass = active->GetModule<MCGrassCullingModule>();
			if (!grass || !grass->mGrassRitem) {
				++h.failures;
				nextEntry();
				return;
			}
			grass->useGpuCulling = (e.config == C::GrassGpu);				// "GPU Culling (Compute Shader)"
			grass->mGrassRitem->useCellCulling = (e.config != C::GrassCpu);	// "Cell culling (voxel)"
			break;
		}
		}
		static_assert(std::size(kViewportModeNames) == static_cast<size_t>(ViewportResMode::K4) + 1,
			"kViewportModeNames needs one name per ViewportResMode, K4 last");
		if (e.viewport >= 0)	// the resolution combo; IMGUI_UPDATE resizes later in this Update
			mViewportResMode = static_cast<ViewportResMode>(e.viewport);
		h.settledFrames = 0;
		h.phase = DebugHarness::Phase::Settle;
		return;
	}
	case DebugHarness::Phase::Settle: {
		if (++h.settledFrames < gNumFrameResources + 3)
			return;
		std::string name = e.scene + "_" + ConfigName(e.config);
		if (e.viewport >= 0)	// a mode may repeat in the list
			name += "_" + std::to_string(h.entry + 1) + "_" + kViewportModeNames[e.viewport];
		RequestViewportCapture((h.sweepDir / (name + ".bmp")).wstring());
		h.phase = DebugHarness::Phase::Wait;
		return;
	}
	case DebugHarness::Phase::Wait:
		if (!h.captureArmed && !h.readback)
			nextEntry();
		return;
	}
}

void MCEngine::RequestViewportCapture(const std::wstring& bmpPath) {
	if (!mHarness || mHarness->captureArmed || mHarness->readback)
		return;	// one capture in flight
	DebugHarness& h = *mHarness;
	if (bmpPath.empty()) {
		SYSTEMTIME t = {};
		GetLocalTime(&t);
		wchar_t name[64];
		swprintf_s(name, L"viewport_%04u%02u%02u_%02u%02u%02u.bmp",
			t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		h.capturePath = fs::path(L"x64") / L"Captures" / L"manual" / name;
	}
	else {
		h.capturePath = bmpPath;
	}
	h.captureArmed = true;
}

// Called from Draw after the FrameGraph executed: copies the displayed image into
// a readback buffer and returns it to PIXEL_SHADER_RESOURCE, all through BarrierManager.
void MCEngine::RecordViewportCapture() {
	if (!mHarness || !mHarness->captureArmed) return;
	DebugHarness& h = *mHarness;
	h.captureArmed = false;

	// The texture the Viewport panel shows (MC_imgui.cpp, ImGui::Image in IMGUI_UPDATE).
	MCTextureResource& shown = mIsSobel ? mSobelOutput : blurValues.enabled ? mBlurred0 : mSceneColor;
	const D3D12_RESOURCE_DESC desc = shown.mResource->GetDesc();
	UINT64 bytes = 0;
	md3dDevice->GetCopyableFootprints(&desc, 0, 1, 0, &h.footprint, nullptr, nullptr, &bytes);
	const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
	const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
	if (FAILED(md3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&h.readback)))) {
		++h.failures;
		return;
	}
	h.readback->SetName(L"ViewportCaptureReadback");

	mBarrierManager.TransitionState(shown, D3D12_RESOURCE_STATE_COPY_SOURCE);
	mBarrierManager.FlushBarriers(mCommandList.Get());
	const CD3DX12_TEXTURE_COPY_LOCATION dst(h.readback.Get(), h.footprint);
	const CD3DX12_TEXTURE_COPY_LOCATION src(shown.mResource.Get(), 0);
	mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	mBarrierManager.TransitionState(shown, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.FlushBarriers(mCommandList.Get());
	h.captureFence = mCurrentFence + 1;	// Draw signals ++mCurrentFence for this frame

	if (!h.sweep) return;
	// Per configuration: the graph as compiled this frame, and the timer averages.
	fs::path fgPath = h.capturePath;
	std::ofstream fg(fgPath.replace_extension(".fg.txt"), std::ios::out | std::ios::trunc);
	auto sink = [&fg](const char* text) { fg << text; };
	mFrameGraph->DumpToOutputDebugString(sink);
	fg << "== ALIASING PLAN ==\n";
	mFrameGraph->DumpAliasingPlan(sink);

	constexpr int kWindow = gNumFrameResources;
	fs::path timersPath = h.capturePath;
	std::ofstream timers(timersPath.replace_extension(".timers.txt"), std::ios::out | std::ios::trunc);
	timers << "# average ms over the last " << kWindow << " samples; for information, never compared\n"
		<< std::fixed << std::setprecision(4);
	for (TimerID id = 0; id < static_cast<TimerID>(mGpuTimer.RegisteredCount()); ++id)
		timers << "GPU " << mGpuTimer.Name(id) << " " << mGpuTimer.GetAverageMs(id, kWindow) << '\n';
	for (CpuTimerID id = 0; id < static_cast<CpuTimerID>(mCpuTimer.RegisteredCount()); ++id)
		timers << "CPU " << mCpuTimer.Name(id) << " " << mCpuTimer.GetAverageMs(id, kWindow) << '\n';
}

// Called from Draw once the frame is recorded, before the list is closed. Under
// --poison, a pass that inherits the root signature draws under the poisoned one;
// the debug layer reports that while the list records (#201), and executing the
// list can hang the GPU (a TDR). Such a frame is closed and never submitted, and
// the sweep ends: this entry and every later one count as failed.
bool MCEngine::RejectPoisonedFrame() {
	if (!mHarness || !mHarness->poisoning) return false;
	DebugHarness& h = *mHarness;
	if (h.drainQueue)
		DrainInfoQueue(h.drainQueue.Get(), h.log);
	if (!h.log.sawError) return false;
	mCommandList->Close();	// may fail on the invalid calls; the list is never executed
	const int failed = h.failures + static_cast<int>(h.entries.size() - h.entry);
	const std::string why = "[harness] --poison: a D3D12 error while recording; the frame was not submitted, "
		+ std::to_string(failed) + " entries failed";
	OutputDebugStringA((why + "\n").c_str());
	h.log.Line(why);
	h.poisoning = false;
	h.sweep = false;
	PostQuitMessage(failed);
	return true;
}

// Writes the copied image as a 32-bit BI_RGB BMP (bottom-up rows, BGRA, alpha kept).
bool MCEngine::CaptureViewportToBmp() {
	DebugHarness& h = *mHarness;
	const D3D12_SUBRESOURCE_FOOTPRINT& fp = h.footprint.Footprint;
	if (fp.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
		return false;
	std::error_code ec;
	if (h.capturePath.has_parent_path())
		fs::create_directories(h.capturePath.parent_path(), ec);
	std::ofstream out(h.capturePath, std::ios::binary | std::ios::trunc);
	if (!out) return false;

	const DWORD imageBytes = fp.Width * fp.Height * 4;
	BITMAPFILEHEADER fileHeader = {};
	BITMAPINFOHEADER infoHeader = {};
	fileHeader.bfType = 0x4D42;	// "BM"
	fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
	infoHeader.biSize = sizeof(BITMAPINFOHEADER);
	infoHeader.biWidth = static_cast<LONG>(fp.Width);
	infoHeader.biHeight = static_cast<LONG>(fp.Height);	// positive: bottom-up
	infoHeader.biPlanes = 1;
	infoHeader.biBitCount = 32;
	infoHeader.biCompression = BI_RGB;
	infoHeader.biSizeImage = imageBytes;
	out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
	out.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));

	BYTE* mapped = nullptr;
	// Up to the buffer's end: its last row is Width * 4 bytes, not padded to RowPitch.
	const D3D12_RANGE readRange = { static_cast<SIZE_T>(h.footprint.Offset),
		static_cast<SIZE_T>(h.readback->GetDesc().Width) };
	if (FAILED(h.readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
		return false;
	std::vector<BYTE> row(static_cast<size_t>(fp.Width) * 4);
	for (UINT y = fp.Height; y-- > 0;) {
		const BYTE* rgba = mapped + h.footprint.Offset + static_cast<UINT64>(y) * fp.RowPitch;
		for (UINT x = 0; x < fp.Width; ++x) {
			row[x * 4 + 0] = rgba[x * 4 + 2];
			row[x * 4 + 1] = rgba[x * 4 + 1];
			row[x * 4 + 2] = rgba[x * 4 + 0];
			row[x * 4 + 3] = rgba[x * 4 + 3];
		}
		out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
	}
	const D3D12_RANGE nothingWritten = { 0, 0 };
	h.readback->Unmap(0, &nothingWritten);
	return out.good();
}

// Engine destructor, just before the device is released.
void MCEngine::ReleaseDebugHarnessDevice() {
	if (!mHarness) return;
	DebugHarness& h = *mHarness;
	h.readback.Reset();
	if (h.callbackQueue) {
		h.callbackQueue->UnregisterMessageCallback(h.callbackCookie);
		h.callbackQueue.Reset();
	}
	if (h.drainQueue) {
		DrainInfoQueue(h.drainQueue.Get(), h.log);
		h.drainQueue.Reset();
	}
	if (h.log.open) {	// only what ReportLiveObjects adds is drained after it
		ComPtr<IDXGIInfoQueue> dxgiQueue;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiQueue))))
			dxgiQueue->ClearStoredMessages(DXGI_DEBUG_ALL);
	}
}

// Engine destructor, right after ReportLiveObjects.
void MCEngine::WriteDebugHarnessLiveObjects() {
	if (!mHarness || !mHarness->log.open) return;
	VerifyLog& log = mHarness->log;
	log.Line("== LIVE OBJECTS ==");
	ComPtr<IDXGIInfoQueue> queue;
	if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))))
		return;
	const UINT64 count = queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
	for (UINT64 i = 0; i < count; ++i) {
		SIZE_T size = 0;
		if (FAILED(queue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &size))) continue;
		std::vector<char> buffer(size);
		auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buffer.data());
		if (SUCCEEDED(queue->GetMessage(DXGI_DEBUG_ALL, i, message, &size)))
			log.Line(std::string(SeverityName(message->Severity)) + " #" + std::to_string(message->ID)
				+ ": " + (message->pDescription ? message->pDescription : ""));
	}
}

#endif // _DEBUG

// Ch7_Shapes.cpp
#include "Legacy/MCEngine.h"
#include "Renderer/D3D12/ShaderLib.h"
#include "Legacy/MCScene.h"
#include "Renderer/D3D12/UploadHelper.h"
#include "Renderer/D3D12/DescHeapManager.h"	// singleton - thus not saved as member variable inside MCEngine.h
#include "Legacy/MCLight.h"
#include <DirectXColors.h>
#include <WindowsX.h>
#include <dxgidebug.h>
#include <pix3.h>
#include "Core/AssetIdentity.h"
#include "Renderer/D3D12/ConstExpressionValues.h" // gNumFrameResources


/*
#include "FrameGraph/FrameGraph.h"
#include "FrameGraph/FrameGraphPasses.h"
#include "FrameGraph/LambdaPass.h"
*/
using namespace DirectX;

// extern const int gNumFrameResources;  // in d3dUtil.h

// Texture metadata now lives in Assets/Textures/*.mctex; loaded by
// mTextureManager.LoadDirectory in Initialize.

MCEngine::MCEngine(HINSTANCE hInstance)
	: D3DApp(hInstance),
	mMaterialManager(*this), mTextureManager(*this),
	mMeshSourceManager(*this), mSceneManager(*this), mMigrator()
{
	mMainWndCaption = L"MC Engine";
	BuildCamera();
	MCLightManager::Init();
}

MCEngine::~MCEngine()
{
	OutputDebugString(L"MC Engine DESTRUCTOR Start\n");
	if (md3dDevice != nullptr) {
		// A removed device makes the flush throw, and a throw out of a destructor
		// is std::terminate: the error dialog in WinMain would never show.
		try { FlushCommandQueue(); }
		catch (const std::exception& e) {
			OutputDebugStringA(("MC Engine DESTRUCTOR: flush failed: " + std::string(e.what()) + "\n").c_str());
		}
	}
	if (auto* active = mSceneManager.GetActive())
		active->Unload(*this);
	IMGUI_SHUTDOWN();

	for (auto& rs : mRootSignatures)
		rs.Reset();
	mComputeRootSignature.Reset();
	mGrassCullRootSignature.Reset();
	mDebugLineRootSig.Reset();
	mGrassCommandSignature.Reset();

	MCLightManager::Get().Shutdown();

	// DescHeapManager singleton owns a CPU-only staging descriptor heap for the Static tier.
	// Its instance lives in a static local inside Singleton::Init and is destroyed at program
	// exit — AFTER ReportLiveObjects below. Release the heaps now so they don't show up as leaks.
	// No heap: Initialize failed before BuildDescriptorHeaps, and Init never ran.
	if (mCbvSrvUavHeap != nullptr)
		DescHeapManager::Get().Shutdown();


	for (auto& pso : mPSOs)
		pso.second.Reset();
	mRtvHeap.Reset();
	mDsvHeap.Reset();
	mCbvSrvUavHeap.Reset();

	mSceneColor.mResource.Reset();
	mSceneColorMSAA.mResource.Reset();
	mSceneDepth.mResource.Reset();
	// mViewportColor.mResource.Reset();
	mDepthDebugColor.mResource.Reset();
	// mViewportNoAlpha.mResource.Reset();
	mBlurred0.mResource.Reset();
	// mBlurred1.mResource.Reset();
	mBlurUploadBuffer.reset();
	mSobelUploadBuffer.reset();
	for (int i = 0; i < (int)mDebugLineVB.size(); ++i) {
		if (mDebugLineVBMapped[i]) {
			mDebugLineVB[i]->Unmap(0, nullptr);
			mDebugLineVBMapped[i] = nullptr;
		}
		mDebugLineVB[i].Reset();
	}
	mSobelOutput.mResource.Reset();
	/*
	for (int i = 0; i < (int)mShadowMaps.size(); ++i) {
		mShadowMaps[i].mResource.Reset();
	}
	mShadowMaps.clear();
	*/
	mCommandList.Reset();
	mDirectCmdListAlloc.Reset();
	mFrameResources.clear();
	mGpuTimer.Reset();

	// Release manager-owned GPU resources before the device is destroyed.
	// Both managers hold ID3D12Resource ComPtrs (texture default heaps, geometry
	// VB/IB + upload heaps). Without explicit Clear() they destruct implicitly
	// AFTER this destructor body returns — i.e. after ReportLiveObjects below
	// has already counted their resources as leaked device refs.
	mMeshSourceManager.Clear();
	mTextureManager.Clear();
	if (auto* scene = mSceneManager.GetActive()) {
		scene->ResetSceneResources();
		// Null the back-pointer so ~MCScene does not call sceneAccess() on a
		// dying engine. The ObjCBIndex slots leaked here are process-exit
		// waste, not a runtime concern.
		scene->mEngine = nullptr;
	}
	// Destroy the owned scene now, while the device is still alive, so its
	// module ComPtrs release before ReportLiveObjects counts them as leaks.
	mSceneManager.ClearActive();

	mFrameGraph.reset();

	for (auto& bbuf : mSwapChainBuffer)
		bbuf.Reset();
	mDepthStencilBuffer.Reset();
	mFence.Reset();
	mCommandQueue.Reset();
	mSwapChain.Reset();
#if defined(_DEBUG)
	ReleaseDebugHarnessDevice();	// its info queue holds a device reference
#endif
	md3dDevice.Reset();

	OutputDebugString(L"======= START of LEAKED ID3D12Device dependent objects:\n");
	ComPtr<IDXGIDebug1> dxgiDebug;
	// use `if` instead of ThrowIfFailed
	// destructors are implicitly noexcept(true). Do not throw.
	// deeper rule : throwing from a destructor during stack unwinding
	//               calls std::terminate even if the dtor were marked noexcept(false).
	//               Destructors should not throw, period.
	if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)))) {
		dxgiDebug->ReportLiveObjects(
			DXGI_DEBUG_ALL,
			static_cast<DXGI_DEBUG_RLO_FLAGS>(
				DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
	}
#if defined(_DEBUG)
	WriteDebugHarnessLiveObjects();
#endif
	OutputDebugString(L"======= END of LEAKED ID3D12Device dependent objects\n");
	OutputDebugString(L"MC Engine DESTRUCTOR ENDING\n");
}

bool MCEngine::Initialize()
{
	std::cout << "start initialization\n";

	if (!D3DApp::Initialize())
		return false;
#if defined(_DEBUG)
	InitDebugHarness();	// verification flags; --verify-log attaches before any asset loads
#endif
	std::cout << "D3DAPP init Success\n"; OutputDebugString(L"D3DAPP init Success\n");
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
	if (HMODULE pix = PIXLoadLatestWinPixGpuCapturerLibrary())
		std::cout << "PIX GPU capturer loaded for attach support\n";
	else
		std::cout << "PIX not installed (Capture won't be available; engine runs normally)\n";

	mBarrierManager;
	mGpuTimer.Initialize(md3dDevice.Get(), mCommandQueue.Get());
	mMaterialManager.MatCBFreeList().SetCapacity(256); // 256 is a generous value for Phase1
	mMaterialManager.LoadDirectory("Assets/materials");

	BuildRootSignature();            std::cout << "BuildRootSignature() success\n" << std::endl; OutputDebugString(L"BuildRootSignature() success\n");
	BuildShadersAndInputLayout();    std::cout << "BuildShadersAndInputLayout() success\n" << std::endl; OutputDebugString(L"BuildShadersAndInputLayout() success\n");

	RegisterLights();
	// Fixed for the process, set before any load. BuildFrameResources sizes
	// ObjectCB from the high-water mark, so every allocated slot fits.
	constexpr std::uint32_t kBootObjCBCapacity = 4096;
	mObjCBFreeList.SetCapacity(kBootObjCBCapacity);

	// Catalog scene paths only — nothing loads yet (load-on-switch). The boot
	// scene loads through the same bracketed transition as every runtime switch,
	// after the setup below closes + flushes this command list.
	mSceneManager.LoadAssetRedirects("assets/asset_redirects.json");
	mSceneManager.SetScenePath("Ch7",      "assets/scenes/scene_ch7.json");
	mSceneManager.SetScenePath("Empty",    "assets/scenes/scene_empty.json");
	mSceneManager.SetScenePath("Grass",    "assets/scenes/scene_grass.json");
	mSceneManager.SetScenePath("SoloVoid", "assets/scenes/scene_solo_void.json");
	mSceneManager.SetScenePath("SoloBox",  "assets/scenes/scene_solo_box.json");

	BuildDescriptorHeaps();          std::cout << "Descriptor Heaps build success\n" << std::endl; OutputDebugString(L"Descriptor Heaps build success\n");
	// After BuildDescriptorHeaps (DescHeapManager::Init), before InitializeRenderPass (RegisterPass).
	mFrameGraph = std::make_unique<FrameGraph>(md3dDevice.Get(), DescHeapManager::Get(), mGpuTimer);

	// Step 12b (deferred half): texture LoadDirectory must run after
	// BuildDescriptorHeaps because MCTextureManager::LoadFromFile calls
	// DescHeapManager::Get().CreateSrv2d, which requires DescHeapManager::Init
	// to have been called inside BuildDescriptorHeaps.
	mTextureManager.LoadDirectory("Assets/Textures");

	BuildPSOs();                     std::cout << "PSO build success\n" << std::endl; OutputDebugString(L"PSO build success\n");
	BuildDebugLineResources();       std::cout << "Debug line resources built\n";
	// MCScene render targets must exist before compute-CB init (reads their bindless offsets).
	BuildSceneRenderTarget();
	BuildSceneRenderTargetDescriptors();
	MCLightManager::Get().CreateShadowMapResources(md3dDevice.Get(), mBarrierManager); // must be called AFTER registerLights()
	// BuildShadowMapResource();
	// BuildShadowMapDescriptors();
	MCLightManager::Get().SetupShadowMapDSVSRVs();
	BuildComputeShaderConstantBufferResources();
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	FlushCommandQueue();

	// Boot scene loads through the standard transition bracket — the same path
	// as every runtime switch (single construction point for scenes).
	SwitchSceneNow("Ch7");
	mSceneManager.ValidateRedirects();   // after first load; mesh targets warn, see ValidateRedirects.
	assert(mObjCBFreeList.HighWater() >= mSceneManager.GetActive()->allRitems.size()
	       && "free-list high-water below active scene's item count");
	assert(mObjCBCapacity >= mObjCBFreeList.HighWater()
	       && "per-frame ObjCB array smaller than free-list high-water; bump kObjCBHeadroom");

	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Start\n");
	IMGUI_INIT();
	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Done\n");

	// --- FrameGraph MVP test
	InitializeRenderPass();


	// Title bar reads the path lazily via mSceneManager.GetScenePath() in
	// RefreshWindowTitle below — no separate engine-side cache to keep in sync.
	RefreshWindowTitle();

	return true;
}

LRESULT MCEngine::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (IMGUI_WNDMSGHANDLER(hwnd, msg, wParam, lParam))
		return true;
	switch (msg)
	{
		// WM_ACTIVATE is sent when the window is activated or deactivated.
		// We pause the game when the window is deactivated and unpause it
		// when it becomes active.
	case WM_ACTIVATE:
#if defined(_DEBUG)
		if (IsHarnessDriving())	// unattended runs keep rendering when the window loses focus
			return 0;
#endif
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			mAppPaused = true;
			mTimer.Stop();
		}
		else
		{
			mAppPaused = false;
			mTimer.Start();
		}
		return 0;

		// WM_SIZE is sent when the user resizes the window.
	case WM_SIZE:
		// Save the new client area dimensions.
		mClientWidth = LOWORD(lParam);
		mClientHeight = HIWORD(lParam);
		if (md3dDevice)
		{
			if (wParam == SIZE_MINIMIZED)
			{
				mAppPaused = true;
				mMinimized = true;
				mMaximized = false;
			}
			else if (wParam == SIZE_MAXIMIZED)
			{
				mAppPaused = false;
				mMinimized = false;
				mMaximized = true;
				OnResize();
			}
			else if (wParam == SIZE_RESTORED)
			{

				// Restoring from minimized state?
				if (mMinimized)
				{
					mAppPaused = false;
					mMinimized = false;
					OnResize();
				}

				// Restoring from maximized state?
				else if (mMaximized)
				{
					mAppPaused = false;
					mMaximized = false;
					OnResize();
				}
				else if (mResizing)
				{
					// If user is dragging the resize bars, we do not resize
					// the buffers here because as the user continuously
					// drags the resize bars, a stream of WM_SIZE messages are
					// sent to the window, and it would be pointless (and slow)
					// to resize for each WM_SIZE message received from dragging
					// the resize bars.  So instead, we reset after the user is
					// done resizing the window and releases the resize bars, which
					// sends a WM_EXITSIZEMOVE message.
				}
				else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
				{
					OnResize();
				}
			}
		}
		return 0;

		// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
	case WM_ENTERSIZEMOVE:
		mAppPaused = true;
		mResizing = true;
		mTimer.Stop();
		return 0;

		// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
		// Here we reset everything based on the new window dimensions.
	case WM_EXITSIZEMOVE:
		mAppPaused = false;
		mResizing = false;
		mTimer.Start();
		OnResize();
		return 0;

		// WM_DESTROY is sent when the window is being destroyed.
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

		// The WM_MENUCHAR message is sent when a menu is active and the user presses
		// a key that does not correspond to any mnemonic or accelerator key.
	case WM_MENUCHAR:
		// Don't beep when we alt-enter.
		return MAKELRESULT(0, MNC_CLOSE);

		// Catch this message so to prevent the window from becoming too small.
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
		((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
		return 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN: {
		if (mSceneImageHovered)
			OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	}
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_MOUSEMOVE: {
		//ImGuiIO& io = ImGui::GetIO();
		if (mSceneImageHovered)
			OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	}
	case WM_KEYUP:
		if (wParam == VK_ESCAPE) // ESC
		{
			PostQuitMessage(0);
		}
		/*
		* recent version (2026) of dx12 does not allow
		* applying msaa onto render target directly on
		* the same resource as back buffer
		* fix: render onto different RT texture that has msaa enabled
		* then copy that RT texture onto RT of back buffer to present
		else if ((int)wParam == VK_F2)
			Set4xMsaaState(!m4xMsaaState);
			*/
		return 0;
	case WM_MOVE:
	case WM_DISPLAYCHANGE:
		UpdateCurrentMonitorRefreshRate(hwnd);
		return 0;
	case WM_MOUSEWHEEL: {
		int delta = GET_WHEEL_DELTA_WPARAM(wParam); // positive = scroll up, negative = scroll down
		// delta comes in multiples of WHEEL_DELTA (120)
		// so normalize it:
		float scrollAmount = (float)delta / WHEEL_DELTA; // +1.0 or -1.0 per notch

		if (mSceneImageHovered)
			OnMouseScroll(wParam,scrollAmount);
		return 0;
	}

	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void MCEngine::BeginSceneTransition() {
	FlushCommandQueue();
	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
}

void MCEngine::EndSceneTransition() {
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* lists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(lists), lists);
	FlushCommandQueue();
}

void MCEngine::ReloadActiveSceneNow(bool save) {
	BeginSceneTransition();

	std::string err;
	try { mSceneManager.Reload(save); }
	catch (const std::exception& e) { err = e.what(); }

	// Always End, even on Reload throw. Leaves cmdlist in the closed state Draw
	// expects to Reset against; partial work submitted to GPU is preferable to
	// leaving the cmdlist stuck open.
	EndSceneTransition();

	if (!err.empty()) throw std::runtime_error("ReloadActiveSceneNow: " + err);
}

// Immediate bracketed switch for call sites outside Update's pending-switch path
// (ImGui handlers, tests) — the cmdlist is closed there and Switch records GPU
// work. Same always-End contract as ReloadActiveSceneNow.
void MCEngine::SwitchSceneNow(const std::string& name) {
	BeginSceneTransition();

	std::string err;
	try { mSceneManager.Switch(name); }
	catch (const std::exception& e) { err = e.what(); }

	EndSceneTransition();

	if (!err.empty()) throw std::runtime_error("SwitchSceneNow: " + err);
}

void MCEngine::RebuildFrameResources()
{
	mFrameResources.clear();
	mCurrFrameResourceIndex = 0;
	BuildFrameResources();
	mCurrFrameResource = mFrameResources[0].get();
	mDepthDebugFramesDirty = gNumFrameResources;
}

void MCEngine::DirtyAllRenderItems() {
	auto* scene = mSceneManager.GetActive();
	if (!scene) return;
	for (auto& ri : scene->allRitems)
		ri->NumFramesDirty = gNumFrameResources;
	// static int dirtycounter = 0;
	// std::cout << "dirtied all frame resources to value 3, count : " << dirtycounter++ << std::endl;
}

void MCEngine::PreInitObjectCBs() {
	auto* scene = mSceneManager.GetActive();
	if (!scene) return;
	for (auto& fr : mFrameResources) {
		auto objCB = fr->ObjectCB.get();
		for (auto& e : scene->allRitems) {
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
			InstanceData rec;
			XMStoreFloat4x4(&rec.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&rec.TexTransform, XMMatrixTranspose(texTransform));
			rec.MaterialIndex = (uint32_t)e->Mat->MatCBIndex;
			objCB->CopyData(e->ObjCBIndex, rec);
		}
	}
}

// Returns a non-owning pointer; ownership lives in scene.allRitems after insertion.
// Inserts into scene.allRitems / scene.layers[i] / scene.nameToRitem (D8 Step 1d:
// scene state lives on the scene permanently; engine reads through mSceneManager.GetActive()).
// Resolves material against mMaterialManager and geometry against mMeshSourceManager — both
// via the content-hashed handles deserialized into r->materialHandle / r->meshHandle.
RenderItem* MCEngine::LoadRenderItemsFromJson(const nlohmann::json& j, MCScene& scene)
{
	auto r = std::make_unique<RenderItem>();
	j.get_to(*r);   // canonical ADL invocation — finds ::from_json(json, RenderItem&) in
						// the global namespace (see SceneSerialization.cpp). Calling
						// nlohmann::from_json(j, *r) directly does NOT work — that name is
						// not part of nlohmann's public API for user types. Use j.get_to
						// or *r = j.get<RenderItem>().

	// 0. Collapse any rename chain on the deserialized handles before consulting the
	//    managers. In-place mutation: downstream reads of r->materialHandle / r->meshHandle
	//    (Save, ImGui inspector, debug) see the canonical handle, not the stale on-disk one.
	r->materialHandle = mSceneManager.ResolveAssetRedirect(r->materialHandle);
	r->meshHandle     = mSceneManager.ResolveAssetRedirect(r->meshHandle);

	// 1. Resolve material by handle through mMaterialManager. Returns nullptr if
	//    the handle has no registered material (e.g., manager not yet populated
	//    via Step-12 cutover — JSON round-trip Load is non-functional until then).
	r->Mat = mMaterialManager.Get(r->materialHandle);
	if (!r->Mat) {
		throw std::runtime_error("LoadRenderItem: unknown material handle "
			+ std::to_string(r->materialHandle) + " for item '" + r->Name + "'");
	}

	// 2. Resolve geometry by handle through mMeshSourceManager. Same nullptr
	//    semantics as material; same Step-12 dependency.
	r->Geo = mMeshSourceManager.GetGeometry(r->meshHandle);
	if (!r->Geo) {
		throw std::runtime_error("LoadRenderItem: unknown mesh handle "
			+ std::to_string(r->meshHandle) + " for item '" + r->Name + "'");
	}

	// 3. Resolve submesh and copy DrawArgs. Under drop-merging (8A1+) every
	//    MCMeshGeometry has exactly one DrawArgs entry, keyed by Geo->Name.
	auto subIt = r->Geo->DrawArgs.find(r->Geo->Name);
	if (subIt == r->Geo->DrawArgs.end()) {
		throw std::runtime_error("LoadRenderItem: geometry '" + r->Geo->Name
			+ "' has no DrawArgs entry keyed by its own name (drop-merging invariant violated)");
	}
	r->IndexCount = subIt->second.IndexCount;
	r->StartIndexLocation = subIt->second.StartIndexLocation;
	r->BaseVertexLocation = subIt->second.BaseVertexLocation;
	r->Bounds = subIt->second.Bounds;

	// 4. PrimitiveType sanity — UNDEFINED (0) is a corrupt-file signal.
	if (r->PrimitiveType == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED) {
		throw std::runtime_error("LoadRenderItem: PrimitiveType is UNDEFINED for item '"
			+ r->Name + "'");
	}

	// 5. Mark dirty so UpdateObjectCBs recomposes World from SRT next frame
	//    and pushes to all frame-resource CBs.
	r->NumFramesDirty = gNumFrameResources;

	// 6. MCScene-side insertion. Atomicity: name
	//    uniqueness check first, then the ObjCBIndex slot, then scene.layers[i] for
	//    every set bit, then scene.allRitems takes ownership. If any step throws, the
	//    prior inserts must be rolled back manually — fragility documented in Step 3.
	auto [it, inserted] = scene.nameToRitem.try_emplace(r->Name, r.get());
	if (!inserted) {
		throw std::runtime_error("LoadRenderItem: duplicate name in scene '"
			+ scene.name + "': " + r->Name);
	}
	// Allocate ObjCBIndex from the free list after the name check, so a rejected
	// item never takes a slot. Throws if exhausted — see CBFreeList::Allocate.
	try { r->ObjCBIndex = mObjCBFreeList.Allocate(); }
	catch (...) { scene.nameToRitem.erase(it); throw; }
	for (uint32_t i = 0; i < static_cast<uint32_t>(RenderLayer::Count); ++i) {
		if (r->Layers & (1u << i)) {
			scene.layers[i].insert(r.get());
		}
	}
	RenderItem* raw = r.get();
	scene.allRitems.push_back(std::move(r));
	return raw;
}

void MCEngine::OnResize()
{
	D3DApp::OnResize(); // FlushCommandQueue gets called.
	OnSceneResize();
}

void MCEngine::OnSceneResize()
{
	// The window resized, so update the aspect ratio and recompute the projection matrix.
	// OutputDebugString(L"starting fovy\n");
	float fovy = mMainCamera == nullptr ? MathHelper::Pi * 0.25f : mMainCamera->GetFovY();
	mMainCamera->SetLens(fovy, sceneAspectRatio(), nearPlane, farPlane);
	// OutputDebugString(L"ending fovy\n");
	// XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, sceneAspectRatio(), nearPlane, farPlane);
	// XMStoreFloat4x4(&mProj, P);
	BuildSceneRenderTarget();
	if (mCbvSrvUavHeap != nullptr)
		BuildSceneRenderTargetDescriptors();

}

void MCEngine::Update(GameTimer& gt)
{
#if defined(_DEBUG)
	TickDebugHarness();	// safe point: command list closed, before this frame's switch and updates
#endif
	RefreshWindowTitle();

	if (reloadScene) {
		reloadScene = false;
		if (mSceneManager.GetActive()) {
			// D9 Step 5a — RequestReload's deferred path. Routes through
			// ReloadActiveSceneNow so the cmdlist is bracketed properly for
			// module OnLoad GPU work (e.g. grass instance buffer upload).
			try { ReloadActiveSceneNow(); OutputDebugStringA("[reload] active scene reloaded (deferred)\n"); }
			catch (const std::exception& e) {
				OutputDebugStringA(("[reload] FAIL (deferred): " + std::string(e.what()) + "\n").c_str());
			}
		}
	}

	// W4A D1 — autosave-on-switch: when toggled on, flush in-memory edits to the
	// active scene's JSON before consuming the pending switch. Gated by the
	// "Autosave on switch" checkbox in the MCScene Inspector.
	if (mSceneManager.HasPendingSwitch() && mAutosaveOnSwitch && mSceneManager.GetActive()) {
		try {
			mSceneManager.SaveActiveToRegisteredPath();
			OutputDebugStringA("[autosave-on-switch] saved active scene\n");
		}
		catch (const std::exception& e) {
			OutputDebugStringA(("[autosave-on-switch] FAIL: " + std::string(e.what()) + "\n").c_str());
		}
	}
	if (mSceneManager.HasPendingSwitch()) {
		BeginSceneTransition();
		// Switch now loads from disk — a bad file must not escape Update.
		try { mSceneManager.ConsumePendingSwitch(); }
		catch (const std::exception& e) {
			OutputDebugStringA(("[switch] FAIL: " + std::string(e.what()) + "\n").c_str());
		}
		EndSceneTransition();
	}
	float dt = gt.DeltaTime();
#if defined(_DEBUG)
	if (!IsHarnessDriving())	// key state is global; a sweep must not see keys typed into other windows
		OnKeyboardInput(gt);
#else
	OnKeyboardInput(gt);
#endif
	UpdateCamera(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Advance deferred-free counters so retired descriptors re-enter the free list
	// after NumFrameResources frames.
	DescHeapManager::Get().Update();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
	mGpuTimer.ReadbackForFrame(mCurrFrameResourceIndex);
	IMGUI_UPDATE();
	const float kTransitionSpeed = 4.0f; // units per second (ortho <-> persp lerp speed)
	if (isOrtho)
		mOrthoT = std::min(mOrthoT + dt * kTransitionSpeed, 1.0f);
	else
		mOrthoT = std::max(mOrthoT - dt * kTransitionSpeed, 0.0f);
	UpdateObjectCBs(gt);
	UpdateInstanceData(gt);
	UpdateMainPassCB(gt);
	UpdateMaterialCBs(gt);
	MCLightManager::Get().Update(*mCurrFrameResource);

	if (mSceneSizeDirty) {
		FlushCommandQueue();
		OnSceneResize();
		mSceneSizeDirty = false;
		mCameraDirty = true;
	}

	UpdateDepthDebugCB(gt);
}

void MCEngine::UpdateCamera(const GameTimer& gt)
{
	if (mCameraDirty) {
		DirtyAllRenderItems();
		mMainCamera->SetLens(mMainCamera->GetFovY(), sceneAspectRatio(), nearPlane, farPlane);
		mMainCamera->UpdateViewMatrix();
		cameraDirtyCount++;
		mCameraDirty = false;
	}
}

void MCEngine::UpdateObjectCBs(const GameTimer& gt) {
	auto currentObjectCB = mCurrFrameResource->ObjectCB.get();
	XMMATRIX view = mMainCamera->GetView();
	XMVECTOR detView = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&detView, view);

	// World-space culling frustum — computed once, handles non-uniform scale correctly
	// (BoundingFrustum::Transform only supports rigid-body transforms, not non-uniform scale)
	DirectX::BoundingFrustum liveFrustumWorld;
	mMainCamera->GetFrustrum().Transform(liveFrustumWorld, invView);
	const DirectX::BoundingFrustum& cullFrustum =
		mFreezeCamera ? mFrozenWorldFrustum : liveFrustumWorld;

	auto* scene = mSceneManager.GetActive();
	if (!scene) return;	// a failed load leaves no scene
	for (auto& e : scene->allRitems) {
		if (e->NumFramesDirty > 0) {
			XMStoreFloat4x4(&e->World,
				MathHelper::ComposeWorldMatrix(e->Position, e->Rotation, e->Scale));
		}
		// Frustum check: always runs every frame — it's a visibility decision, not a resource one
		XMMATRIX world = XMLoadFloat4x4(&e->World);
		// Transform local AABB to world space so the Contains test handles non-uniform scale
		DirectX::BoundingBox worldBounds;
		e->Bounds.Transform(worldBounds, world);
		auto result = cullFrustum.Contains(worldBounds);
		e->insideFrustrum = !(result == DirectX::DISJOINT);

		// CB write: only when resource data has changed, decrement only when actually written
		if (e->NumFramesDirty > 0 ) {
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
			InstanceData objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			objConstants.MaterialIndex = e->Mat->MatCBIndex;
			currentObjectCB->CopyData(e->ObjCBIndex, objConstants);
			e->NumFramesDirty--;
		}
	}

	// Merged world bounds of the Opaque layer (what the shadow pass draws and most of what receives shadows),
	// so the shadow fit can be clipped to where geometry exists
	// this recomputes sceneBounds every frame. At world of current scale, no problem. When 10k items in world, will scale badly.
	DirectX::BoundingBox sceneBounds;
	bool hasBounds = false;
	for (auto* e : scene->layers[(int)RenderLayer::Opaque]) { // must be changed to refer to Shadow render layer later
		DirectX::BoundingBox worldBounds;
		e->Bounds.Transform(worldBounds, XMLoadFloat4x4(&e->World));
		if (!hasBounds) { sceneBounds = worldBounds; hasBounds = true; }
		else DirectX::BoundingBox::CreateMerged(sceneBounds, sceneBounds, worldBounds);
	}
	if (hasBounds)
		MCLightManager::Get().SetSceneBounds(sceneBounds);
}

void MCEngine::UpdateInstanceData(const GameTimer& gt) {
	// if (mRitemLayer[(int)RenderLayer::OpaqueInstanced].empty())
	//		return;
	auto _instStart = std::chrono::high_resolution_clock::now(); // TODO - replace with CPU_TIMER
	int bbInstCount = 0;
	int globalOffset = 0;
	auto currentInsanceBuffer = mCurrFrameResource->InstanceCB.get();
	// auto currentObjectCB = mCurrFrameResource->ObjectCB.get();
	XMMATRIX view = mMainCamera->GetView();
	XMVECTOR detView = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&detView, view);
	// World-space culling frustum — computed once to avoid per-instance invWorld multiply
	DirectX::BoundingFrustum liveFrustumWorld;
	mMainCamera->GetFrustrum().Transform(liveFrustumWorld, invView);
	const DirectX::BoundingFrustum& cullFrustum =
		mFreezeCamera ? mFrozenWorldFrustum : liveFrustumWorld;

	auto* scene = mSceneManager.GetActive();
	if (!scene) return;	// a failed load leaves no scene
	for (auto& e : scene->layers[(int)RenderLayer::OpaqueInstanced]) {
		int visibleCount = 0;
		e->InstanceBufferStartIndex = globalOffset;
		const auto& instanceData = e->Instances;
		if (e->useCellCulling && e->checkBounds && enableBoundsCheck && !e->InstanceCells.empty()) {
			// --- Cell-based path --- (cell bounds are already world-space)
			for (const auto& cell : e->InstanceCells) {
				bool cellVisible = !(cullFrustum.Contains(cell.WorldBounds) == DirectX::DISJOINT);
				bbInstCount++;
				if (!cellVisible) continue;
				for (int ci = cell.StartIndex; ci < cell.StartIndex + cell.Count; ci++) {
					XMMATRIX world = XMLoadFloat4x4(&instanceData[ci].World);
					XMMATRIX texTransform = XMLoadFloat4x4(&instanceData[ci].TexTransform);
					InstanceData instConstants;
					XMStoreFloat4x4(&instConstants.World, XMMatrixTranspose(world));
					XMStoreFloat4x4(&instConstants.TexTransform, XMMatrixTranspose(texTransform));
					instConstants.MaterialIndex = instanceData[ci].MaterialIndex; // not used for now, but no harm in update
					// currentInsanceBuffer->CopyData(visibleCount++, instConstants);
					currentInsanceBuffer->CopyData(globalOffset + visibleCount++, instConstants);
				}
			}
		}
		else {
			for (UINT i = 0; i < (UINT)instanceData.size(); ++i) {
				bool instanceInsideFrustrum = false;
				if (e->checkBounds && enableBoundsCheck) {
					XMMATRIX world = XMLoadFloat4x4(&instanceData[i].World);
					// Transform local AABB to world space for correct non-uniform scale handling
					DirectX::BoundingBox worldBounds;
					e->Bounds.Transform(worldBounds, world);
					auto result = cullFrustum.Contains(worldBounds);
					instanceInsideFrustrum = !(result == DirectX::DISJOINT);
					bbInstCount++;
				}
				// CB write: only when resource data has changed, decrement only when actually written
				if (instanceInsideFrustrum || !enableBoundsCheck || !e->checkBounds) {
					XMMATRIX world = XMLoadFloat4x4(&instanceData[i].World);
					XMMATRIX texTransform = XMLoadFloat4x4(&instanceData[i].TexTransform);
					InstanceData instConstants;
					XMStoreFloat4x4(&instConstants.World, XMMatrixTranspose(world));
					XMStoreFloat4x4(&instConstants.TexTransform, XMMatrixTranspose(texTransform));
					instConstants.MaterialIndex = instanceData[i].MaterialIndex;
					currentInsanceBuffer->CopyData(globalOffset + visibleCount++, instConstants);
				}
			}
		}
		e->InstanceCount = visibleCount;
		e->bbInstTestCount = bbInstCount;
		globalOffset += visibleCount;
	}

	//! CPU Culling Path
	auto grassInstBuf = mCurrFrameResource->GrassInstanceCB.get();
	auto* gc = scene->GetModule<MCGrassCullingModule>();
	if (!(gc && gc->useGpuCulling)) {
		int grassOffset = 0;

		for (auto& e : scene->layers[(int)RenderLayer::GrassInstanced]) {
			int visibleCount = 0;
			e->GrassInstanceBufferStartIndex = grassOffset;
			const auto& instanceData = e->GrassInstances;
			DirectX::BoundingFrustum worldFrustum;
			if (mFreezeCamera)
				worldFrustum = mFrozenWorldFrustum;
			else
				mMainCamera->GetFrustrum().Transform(worldFrustum, invView);
			// cell culling
			if (e->useCellCulling && e->checkBounds && enableBoundsCheck && !e->InstanceCells.empty()) {
				for (const auto& cell : e->InstanceCells) {
					bool cellVisible = !(worldFrustum.Contains(cell.WorldBounds) == DirectX::DISJOINT);
					bbInstCount++;
					if (!cellVisible) continue;
					for (int ci = cell.StartIndex; ci < cell.StartIndex + cell.Count; ci++) {
						//XMMATRIX world = XMLoadFloat4x4(&instanceData[ci].World);
						XMVECTOR pos = XMLoadFloat3(&instanceData[ci].grassPosition);
						GrassInstanceData instConstants;
						//XMStoreFloat4x4(&instConstants.World, XMMatrixTranspose(world));
						XMStoreFloat3(&instConstants.grassPosition, pos);
						instConstants.cosYaw = instanceData[ci].cosYaw;
						instConstants.sinYaw = instanceData[ci].sinYaw;
						instConstants.scale = instanceData[ci].scale;
						grassInstBuf->CopyData(grassOffset + visibleCount++, instConstants);
					}
				}
			}
			// individual blade culling
			else {
				for (UINT i = 0; i < (UINT)instanceData.size(); ++i) {
					bool instanceInsideFrustrum = false;
					if (e->checkBounds && enableBoundsCheck) {
						float3 grassPos = instanceData[i].grassPosition;
						XMFLOAT3 t = { grassPos.x, grassPos.y, grassPos.z };
						XMMATRIX world = XMMatrixTranslation(t.x, t.y, t.z);
						DirectX::BoundingBox worldBounds;
						e->Bounds.Transform(worldBounds, world);
						auto result = cullFrustum.Contains(worldBounds);
						instanceInsideFrustrum = !(result == DirectX::DISJOINT);
						bbInstCount++;
					}
					if (instanceInsideFrustrum || !enableBoundsCheck || !e->checkBounds) {
						//XMMATRIX world = XMLoadFloat4x4(&instanceData[i].World);
						XMVECTOR pos = XMLoadFloat3(&instanceData[i].grassPosition);
						GrassInstanceData instConstants;
						//XMStoreFloat4x4(&instConstants.World, XMMatrixTranspose(world));
						XMStoreFloat3(&instConstants.grassPosition, pos);
						// instConstants.MaterialIndex = instanceData[i].MaterialIndex;
						instConstants.cosYaw = instanceData[i].cosYaw;
						instConstants.sinYaw = instanceData[i].sinYaw;
						instConstants.scale = instanceData[i].scale;
						grassInstBuf->CopyData(grassOffset + visibleCount++, instConstants);
					}
				}
			}
			e->InstanceCount = visibleCount;
			e->bbInstTestCount = bbInstCount;
			grassOffset += visibleCount;
		}
	}
	auto _instEnd = std::chrono::high_resolution_clock::now();
	mInstancingCullingTime = static_cast<float>(std::chrono::duration<double, std::milli>(_instEnd - _instStart).count());
}

void MCEngine::UpdateMainPassCB(const GameTimer& gt) {
	XMMATRIX view = mMainCamera->GetView();
	auto& passCB = mCurrFrameResource->mCachedMainPassCB;
	float viewWidth = mMainCamera->GetFovY() * 2.0f;
	float viewHeight = viewWidth / sceneAspectRatio();
	XMMATRIX orthoProj = XMMatrixOrthographicLH(
		viewWidth,   // e.g. 20.0f  — world-space width visible
		viewHeight,  // e.g. 20.0f / AspectRatio()
		nearPlane,   // near
		farPlane     // far
	);
	float t = MathHelper::EaseOut(mOrthoT); // this is dependent on fps, so needs modification
	XMMATRIX proj = mMainCamera->GetProj();
	XMMATRIX blendedProj = MathHelper::LerpMatrix(proj, orthoProj, t);
	XMMATRIX viewProj = XMMatrixMultiply(view, blendedProj);
	auto viewDet = XMMatrixDeterminant(view);
	auto projDet = XMMatrixDeterminant(proj);
	XMMATRIX invView = XMMatrixInverse(&viewDet, view);
	XMMATRIX invProj = XMMatrixInverse(&projDet, proj);
	auto viewProjDet = XMMatrixDeterminant(viewProj);
	XMMATRIX invViewProj = XMMatrixInverse(&viewProjDet, viewProj);

	XMStoreFloat4x4(&passCB.gView, XMMatrixTranspose(view));
	XMStoreFloat4x4(&passCB.gInvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&passCB.gProj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&passCB.gInvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&passCB.gViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&passCB.gInvViewProj, XMMatrixTranspose(invViewProj));
	passCB.gEyePosW = mMainCamera->GetPosition3f();
	passCB.gRenderTargetSize = XMFLOAT2(mSceneViewWidth, mSceneViewHeight);
	passCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mSceneViewWidth, 1.0f / mSceneViewHeight);
	passCB.gNearZ = nearPlane;
	passCB.gFarZ = farPlane;
	passCB.gTotalTime = gt.TotalTime();
#if defined(_DEBUG)
	if (IsHarnessDriving())
		passCB.gTotalTime = 0.0f;	// grass wind follows wall-clock time; a sweep capture must repeat
#endif
	passCB.gDeltaTime = gt.DeltaTime();
	passCB.gAmbientLight = mAmbientLight;
	passCB.gFogColor = mFogColor;
	passCB.gFogStart = mFogStart;
	passCB.gFogRange = mFogRange;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, passCB);
}

void MCEngine::UpdateMaterialCBs(const GameTimer& gt) {
	auto currMatCB = mCurrFrameResource->MaterialCB.get();
	for (auto& [h, v] : mMaterialManager.All()) {
		MCMaterial* mat = v.get();
		if (mat->NumFramesDirty > 0) {
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);
			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			matConstants.MatTransform = mat->MatTransform;
			auto* tex = mTextureManager.Get(mat->textureHandle);
			assert(tex && tex->resource && "Material's textureHandle doesn't resolve to a loaded MCTexture");
			matConstants.srvIndex = (tex && tex->resource) ? tex->resource->SRVs[0].offset : 0;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));
			currMatCB->CopyData(mat->MatCBIndex, matConstants);
			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void MCEngine::UpdateDepthDebugCB(const GameTimer& gt) {
	if (mDepthDebugFramesDirty <= 0)
		return;
	mDebugDepthCB.FarZ = farPlane;
	mDebugDepthCB.NearZ = nearPlane;
	mDebugDepthCB.VisualMaxDepth = mDepthDebugMax;
	auto currDepthCB = mCurrFrameResource->DepthCB.get();
	currDepthCB->CopyData(0, mDebugDepthCB);
	--mDepthDebugFramesDirty;
}

void MCEngine::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	// cmdlistAllocator gets set to specified allocator when resetting a command list

	if (mIsWireframe)
	{
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mScene4xMsaaState ? mPSOs["opaque_wireframe_MSAA"].Get() : mPSOs["opaque_wireframe"].Get()));
	}
	else
	{
		ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mScene4xMsaaState ? mPSOs["opaque_MSAA"].Get() : mPSOs["opaque"].Get()));
	}

	// Begin measuring GPU deltatime
	mGpuTimer.BeginFrame(mCurrFrameResourceIndex);
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "EntireFrame"); // only BeginEvent.

	{
		GPU_SCOPED(mGpuTimer, mCommandList.Get(), "RenderGraph");
		mFrameGraph->Begin(mCurrFrameResourceIndex);
		mFrameGraph->Compile(mBarrierManager, mCpuTimer);
		{
			// Commit the transient descriptors Compile created, before any draw.
			CPU_SCOPED(mCpuTimer, "[FG::Compile][L] commit transient desc to shader visible");
			DescHeapManager::Get().CommitToShaderVisible(); // dirty[] flags scope copy to new DYNAMIC tier only; static is untouched here
		}
		mFrameGraph->Execute(mCommandList.Get(), mBarrierManager, mGpuTimer);
		mFrameGraph->End();
	}
#if defined(_DEBUG)
	RecordViewportCapture();	// after the graph, before the UI composite
#endif

	// ==========================================================
	//! PASS 6: render Imgui to swap chain back buffer
	// ==========================================================
	mCurrBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
	auto transition_BB = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	mCommandList->ResourceBarrier(1, &transition_BB);
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);
	auto backRtv = CurrentBackBufferView();
	mCommandList->ClearRenderTargetView(backRtv, (float*)&mCurrFrameResource->mCachedMainPassCB.gFogColor, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	auto dsv = DepthStencilView();
	mCommandList->OMSetRenderTargets(1, &backRtv, true, &dsv);

	{
		GPU_SCOPED(mGpuTimer, mCommandList.Get(), "ImGui");
		IMGUI_RENDERDRAWDATA();
	}


	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT
	);
	mCommandList->ResourceBarrier(1, &transition);

	// End measuring GPU deltatime
	mGpuTimer.ResolveForFrame(mCommandList.Get());

	PIXEndEvent(mCommandList.Get());	// Frame

#if defined(_DEBUG)
	if (RejectPoisonedFrame())
		return;
#endif
	ThrowIfFailed(mCommandList->Close());
	// ==========================================================

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// swap the back and front buffers
	UINT presentFlags = mTearingSupported && !enableVsync ? DXGI_PRESENT_ALLOW_TEARING : 0;
	UINT syncInterval = mTearingSupported && !enableVsync ? 0 : 1;
	ThrowIfFailed(mSwapChain->Present(syncInterval, presentFlags)); // set SyncInterval to 1~4 to enable VSYNC (it gets enabled by default by dxgi tho, so need to use flags to disable it)
	// mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->Fence = ++mCurrentFence;						// Instead of flushing, advance the fence value to mark commands up to this fence point
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);		// then add instruction to cmd queue to set new fence point

}

void MCEngine::CreateRtvAndDsvDescriptorHeaps() {
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount+16;	// must match DescHeapManDesc.rtvHeapMaxCap
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc,
		IID_PPV_ARGS(mRtvHeap.GetAddressOf())
	));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 8; // must match DescHeapManDesc.dsvHeapMaxCap
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc,
		IID_PPV_ARGS(mDsvHeap.GetAddressOf())
	));
}

void MCEngine::BuildDescriptorHeaps()
{
	// Static-tier cap covers all SRV/UAV slots that don't churn per frame:
	// asset textures (loaded by mTextureManager.LoadDirectory after this fn
	// completes), render-target SRVs/UAVs (~12 slots from BuildSceneRenderTargetDescriptors),
	// plus headroom. Asset texture count isn't known here — flat cap covers it.
	mCsuTierStaticCap = kCsuTierStaticHeadroom;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc_unified = {};
	heapDesc_unified.NumDescriptors = kCsuReservedHead + mCsuTierStaticCap + kCsuTierDynamicCap;
	heapDesc_unified.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc_unified.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	heapDesc_unified.NodeMask = 0;

	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&heapDesc_unified,
		IID_PPV_ARGS(&mCbvSrvUavHeap)));

	MC_DESC_HEAP_MANAGER_DESC DescHeapManDesc = {};
	DescHeapManDesc.device = md3dDevice.Get();
	DescHeapManDesc.csuCombinedHeap = mCbvSrvUavHeap.Get();
	DescHeapManDesc.rtvHeap = mRtvHeap.Get();
	DescHeapManDesc.dsvHeap = mDsvHeap.Get();
	DescHeapManDesc.csuDescSize = mCbvSrvUavDescriptorSize;
	DescHeapManDesc.rtvDescSize = mRtvDescriptorSize;
	DescHeapManDesc.dsvDescSize = mDsvDescriptorSize;
	DescHeapManDesc.NumFrameResources = gNumFrameResources;
	// Sizing note: per resize cycle BuildSceneRenderTargetDescriptors allocates 2 RTVs + 1 DSV.
	// Deferred-free holds old slots for gNumFrameResources (=3) frames, so up to
	// gNumFrameResources in-flight resizes can coexist with the live set before freed slots rejoin the free list.
	// Headroom = (gNumFrameResources+1) cycles * perCycleAllocs covers init + that in-flight window.
	DescHeapManDesc.rtvHeapMaxCap = SwapChainBufferCount + 16;	// must match CreateRtvAndDsvDescriptorHeaps
	DescHeapManDesc.dsvHeapMaxCap = 8;
	DescHeapManDesc.rtvReservedHead = SwapChainBufferCount;		// D3DApp owns swap-chain RTVs at the head
	DescHeapManDesc.dsvReservedHead = 1;						// D3DApp owns offset 0 for the app-window depth buffer
	DescHeapManDesc.csuTierStaticCap = mCsuTierStaticCap;
	DescHeapManDesc.csuTierDynamicCap = kCsuTierDynamicCap;
	DescHeapManDesc.csuReservedHead = kCsuReservedHead;
	DescHeapManager::Init(DescHeapManDesc);

}

void MCEngine::BuildFrameResources()
{
	auto* scene = mSceneManager.GetActive();
	constexpr UINT kObjCBHeadroom = 64u;  // spare slots for runtime LoadRenderItemsFromJson
	// Items keep the slot they were allocated, anywhere below the high-water mark.
	// This runs after the load allocated, so the buffer covers every slot in use.
	const UINT itemCount = scene ? (UINT)scene->allRitems.size() : 0u;	// no scene after a failed load
	UINT objCount = std::max(mObjCBFreeList.HighWater(), itemCount) + kObjCBHeadroom;
	mObjCBCapacity = objCount;
	// Every slot the material free list can hand out, so materials created after
	// this build (Create > Material) still fit.
	UINT matCount = mMaterialManager.MatCBFreeList().Capacity();
	UINT totalInstances = 0;
	UINT totalGrassInstances = 0;
	if (scene) {
		for (auto* ri : scene->layers[(int)RenderLayer::OpaqueInstanced])
			totalInstances += (UINT)ri->Instances.size();
		for (auto* ri : scene->layers[(int)RenderLayer::GrassInstanced])
			totalGrassInstances += (UINT)ri->GrassInstances.size();
	}

	// Slot 0 is the main pass; light i writes its shadow pass to slot i + 1
	// (MCLightManager::UpdateShadowMapPassCBs). Lights register before the first build.
	const UINT passCount = 1u + (UINT)MCLightManager::Get().LightCount();
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		// FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT depthCount);
		auto fr = std::make_unique<FrameResource>(md3dDevice.Get(),
			passCount, objCount, matCount, 1,
			std::max(1u, totalInstances), std::max(1u, totalGrassInstances));
		mFrameResources.push_back(std::move(fr));
	}
}

void MCEngine::BuildComputeShaderConstantBufferResources()
{

	// Frame-buffered: gNumFrameResources copies so an in-flight frame's CB isn't
	// overwritten by the CPU before its dispatch reads it. (Was capacity 1 — single
	// buffer raced across in-flight frames; see W6.D3 CB-race diagnosis.)
	mSobelUploadBuffer = std::make_unique<UploadBuffer<CSB_default>>(md3dDevice.Get(), gNumFrameResources, 1);
	mSobelUploadBuffer.get()->Resource()->SetName(L"mSobelUploadBuffer");

	// Frame-buffered: 2 CB slots (H, V) per frame => gNumFrameResources * 2 copies. Same CB-race fix.
	auto blurBuf = std::make_unique<UploadBuffer<CSB_blur>>(md3dDevice.Get(), gNumFrameResources * 2, 1);
	blurBuf.get()->Resource()->SetName(L"mBlurUploadBuffer");
	mBlurUploadBuffer = std::move(blurBuf);
	OutputDebugString(L"copied blurCB1\n");
}

void MCEngine::BuildSceneRenderTarget()
{
	// OutputDebugString(L"BuildSceneRenderTarget - Starting\n");
	// Before any CreateCommittedResource() runs, queue old descriptors for reuse and release the resource.
	// Guarded: D3DApp::Initialize() triggers an OnResize() before BuildDescriptorHeaps() has called DescHeapManager::Init().
	if (mCbvSrvUavHeap != nullptr) {
		auto& dhm = DescHeapManager::Get();
		dhm.QueueRemoval_Texture(mSceneColor);
		dhm.QueueRemoval_Texture(mSceneColorMSAA);
		dhm.QueueRemoval_Texture(mSceneDepth);
		dhm.QueueRemoval_Texture(mDepthDebugColor);
		dhm.QueueRemoval_Texture(mBlurred0);
		dhm.QueueRemoval_Texture(mSobelOutput);
		// GPU is idle here (caller flushed the command queue before resize). Drain the
		// deferred-free list immediately so BuildSceneRenderTargetDescriptors below
		// reuses the just-retired slots. Keeps hGpu.ptr values captured earlier this
		// frame (e.g. ImGui's ImTextureID) pointing at refreshed descriptors.
		dhm.FlushPending();
	}
	mSceneColor = {};
	mSceneColorMSAA = {};
	mSceneDepth = {};
	mDepthDebugColor = {};
	mBlurred0 = {};
	mSobelOutput = {};
	// ==============================================
	//! create texture resource for scene color
	// ==============================================
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = (UINT64)mSceneViewWidth;
	texDesc.Height = (UINT64)mSceneViewHeight;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = mSceneFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;



	D3D12_CLEAR_VALUE clearValue = {};
	// clearValue.Format = mBackBufferFormat;
	clearValue.Format = mSceneFormat;
	clearValue.Color[0] = mFogColor.x;// clearValue.Color[0] = Colors::LightSteelBlue[0];
	clearValue.Color[1] = mFogColor.y;
	clearValue.Color[2] = mFogColor.z;
	clearValue.Color[3] = mFogColor.w;

	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(mSceneColor.mResource.GetAddressOf())
	));
	mSceneColor.mResource.Get()->SetName(L"mSceneColor");
	mSceneColor.Name = "mSceneColor";
	mBarrierManager.ResetTrackedState(mSceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_RESOURCE_DESC texDescMSAA = texDesc;
	texDescMSAA.SampleDesc.Count = 4;
	texDescMSAA.SampleDesc.Quality = m4xMsaaQuality - 1;

	if (mScene4xMsaaState) {
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&texDescMSAA,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			&clearValue,
			IID_PPV_ARGS(mSceneColorMSAA.mResource.GetAddressOf())
		));
		mSceneColorMSAA.mResource.Get()->SetName(L"mSceneColor");
		mSceneColorMSAA.Name = "mSceneColorMSAA";
		mBarrierManager.ResetTrackedState(mSceneColorMSAA, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// ==============================================
	//! create resource for blurred output. (non-msaa always)
	// ==============================================
	D3D12_RESOURCE_DESC blurDesc = texDesc;
	blurDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&blurDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(mBlurred0.mResource.GetAddressOf())));
	mBlurred0.mResource.Get()->SetName(L"mBlurred0");
	mBlurred0.Name = "mBlurred0";
	mBarrierManager.ResetTrackedState(mBlurred0, D3D12_RESOURCE_STATE_GENERIC_READ);

	// ==============================================
	//! create resource for sobel-outline output. (non-msaa always)
	// ==============================================
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&blurDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(mSobelOutput.mResource.GetAddressOf())));
	mSobelOutput.mResource.Get()->SetName(L"mSobelOutput");
	mSobelOutput.Name = "mSobelOutput";
	mBarrierManager.ResetTrackedState(mSobelOutput, D3D12_RESOURCE_STATE_GENERIC_READ);

	// ==============================================
	//! create resource for scene depth
	// ==============================================
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = (UINT64)mSceneViewWidth;
	depthStencilDesc.Height = (UINT64)mSceneViewHeight;;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1; // only need 1 for D/SBV

	// 24 bits : reasonable. DXGI_FORMAT_R24G8_TYPELESS
	// 32 bits : overkill (this goes beyond float's precision)
	// 16 bits : not sufficient
	depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS; //mDepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = mScene4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = mScene4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&optClear,
		IID_PPV_ARGS(mSceneDepth.mResource.GetAddressOf())));
	mSceneDepth.mResource.Get()->SetName(L"mSceneDepth");
	mSceneDepth.Name = "mSceneDepth";
	mBarrierManager.ResetTrackedState(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	// ==============================================
	//! create texture resource for debug depth (normalized depth)
	// ==============================================
	D3D12_RESOURCE_DESC debugDesc = {};
	debugDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	debugDesc.Alignment = 0;
	debugDesc.Width = (UINT64)mSceneViewWidth;
	debugDesc.Height = (UINT64)mSceneViewHeight;
	debugDesc.DepthOrArraySize = 1;
	debugDesc.MipLevels = 1;
	debugDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	debugDesc.SampleDesc.Count = 1;
	debugDesc.SampleDesc.Quality = 0;
	debugDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	debugDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE debugClear = {};
	debugClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	debugClear.Color[0] = 0.0f;
	debugClear.Color[1] = 0.0f;
	debugClear.Color[2] = 0.0f;
	debugClear.Color[3] = 1.0f;

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&debugDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&debugClear,
		IID_PPV_ARGS(mDepthDebugColor.mResource.GetAddressOf())
	));
	mDepthDebugColor.mResource.Get()->SetName(L"mDepthDebugColor");
	mDepthDebugColor.Name = "mDepthDebugColor";
	mBarrierManager.ResetTrackedState(mDepthDebugColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	// DSV created lazily here if BuildSceneRenderTarget runs before BuildSceneRenderTargetDescriptors.
	// When it runs via OnSceneResize the DSV will be re-created in BuildSceneRenderTargetDescriptors.

	mSceneViewport.TopLeftX = 0.0f;
	mSceneViewport.TopLeftY = 0.0f;
	mSceneViewport.Width = mSceneViewWidth;
	mSceneViewport.Height = mSceneViewHeight;
	mSceneViewport.MinDepth = 0.0f;
	mSceneViewport.MaxDepth = 1.0f;

	mSceneScissorRect = { 0, 0, (long)mSceneViewWidth, (long)mSceneViewHeight };
}

void MCEngine::BuildSceneRenderTargetDescriptors()
{
	auto& dhm = DescHeapManager::Get();

	// DSV for scene depth (was previously in BuildSceneRenderTarget; route through manager).
	dhm.CreateDsv(mSceneDepth, D3D12_DSV_FLAG_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT, mScene4xMsaaState, 0);

	// MCScene Color: RTV + SRV
	dhm.CreateRtv2d(mSceneColor, mSceneFormat, false, 0);
	dhm.CreateSrv2d(mSceneColor, mSceneFormat, false, MC_VIEW_TIER_STATIC);

	if(IsMsaa())
		dhm.CreateRtv2d(mSceneColorMSAA, mSceneFormat, true, 0);

	// MCScene Depth: typeless SRV view of the D24 resource
	dhm.CreateSrv2d(mSceneDepth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, mScene4xMsaaState, MC_VIEW_TIER_STATIC);

	// Depth Debug: RTV + SRV (non-MSAA, R8G8B8A8)
	dhm.CreateRtv2d(mDepthDebugColor, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
	dhm.CreateSrv2d(mDepthDebugColor, DXGI_FORMAT_R8G8B8A8_UNORM, false, MC_VIEW_TIER_STATIC);

	// Blur ping-pong: SRVs then UAVs
	dhm.CreateSrv2d(mBlurred0, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mBlurred0, mSceneFormat, 0, MC_VIEW_TIER_STATIC);

	// Sobel: SRV + UAV
	dhm.CreateSrv2d(mSobelOutput, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mSobelOutput, mSceneFormat, 0, MC_VIEW_TIER_STATIC);

	blurDirty = true; // UpdateBlurCB next tick rewrites CSB_blur with fresh offsets
}

void MCEngine::BuildCamera()
{
	auto mainCamera = std::make_unique<Camera>();
	mMainCamera = mainCamera.get();
	mMainCamera->moveSpeed = 0.5f;
	mMainCamera->SetPosition(.0f, 3.0f, 20.0f);
	mMainCamera->LookAt(mMainCamera->GetPosition3f(), { .0f, .0f, .0f }, { .0f,1.0f,.0f });
	mAllCameras.push_back(std::move(mainCamera));
	mCameraDirty = true;
}

// non-opt, max amount of light reserve but only assign 1, so .size() is 1
void MCEngine::RegisterLights() {
	// mLights.reserve(kLightBufferCapacity);
	LightData dLight = {};
	dLight.mLightColor = { 1.0f, 1.0f, 1.0f };
	dLight.mLightPosition = { 0.0f,0.0f,0.0f };
	dLight.mLightIntensity = 3.0f;
	dLight.mLightDirection = { 1.0f, 1.0f, 1.0f };
	dLight.mLightType = (int)(LightType::Directional);
	dLight.mCastShadow = 1;
	dLight.depthBias = 0.5f;

	MCLight dirLight(dLight);
	dirLight.mIsDirty = gNumFrameResources;
	MCLightManager::Get().AddLight(std::move(dirLight));
#if defined(_DEBUG)
	// T13a: extra shadow casters for the two-caster check. Light 0 stays the one
	// the shaders light with.
	dLight.mLightDirection = { -1.0f, 1.0f, 1.0f };
	for (int i = 1; i < HarnessShadowCasters(); ++i) {
		MCLight extraCaster(dLight);
		extraCaster.mIsDirty = gNumFrameResources;
		MCLightManager::Get().AddLight(std::move(extraCaster));
	}
#endif
}

void MCEngine::ReloadShaders()
{
    FlushCommandQueue();
    try
    {
        ShaderLibDx::GetLib().Reset();
        BuildShadersAndInputLayout();
        mPSOs.clear();
        BuildPSOs();
        OutputDebugString(L"ReloadShaders: SUCCESS\n");
    }
    catch (DxException& e)
    {
        std::wstring msg = L"ReloadShaders FAILED: " + e.ToString();
        OutputDebugStringW(msg.c_str());
        MessageBoxW(mhMainWnd, msg.c_str(), L"Shader Reload Error", MB_OK | MB_ICONERROR);
    }
}

// public; called by RenderPass inheritors
void MCEngine::DrawLayer(ID3D12GraphicsCommandList* cmdList, RenderLayer layer, bool ignoreFrustum, std::string pixEventName) {
	auto* scene = mSceneManager.GetActive();
	if (!scene) return;
	const auto& items = scene->layers[(int)layer];
	if (items.empty()) return;
	DrawRenderItems(cmdList, items, ignoreFrustum, std::move(pixEventName));
}

// public; called by RenderPass inheritors
void MCEngine::DrawInstancedLayer(ID3D12GraphicsCommandList* cmdList, RenderLayer layer, bool useGrass, bool ignoreFrustum, std::string pixEventName) {
	auto* scene = mSceneManager.GetActive();
	if (!scene) return;
	const auto& items = scene->layers[(int)layer];
	if (items.empty()) return;
	DrawInstanceRenderItems(cmdList, items, useGrass, ignoreFrustum, std::move(pixEventName));
}

void MCEngine::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool ignoreFrustum, std::string pixEventName)
{
	// UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
	// auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	for (auto& ri : ritems)
	{
		if (ri->insideFrustrum || !enableBoundsCheck || !ri->checkBounds || ignoreFrustum) {
			auto vbv = ri->Geo->VertexBufferView();
			auto ibv = ri->Geo->IndexBufferView();
			cmdList->IASetVertexBuffers(0, 1, &vbv);
			cmdList->IASetIndexBuffer(&ibv);
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
			cmdList->SetGraphicsRoot32BitConstant(0, ri->ObjCBIndex, 0);
			cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}
}

//! CPU culling path of rendering Grass
void MCEngine::DrawInstanceRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool useGrass, bool ignoreFrustum, std::string pixEventName) {
	UINT strideBytes = useGrass ? sizeof(GrassInstanceData) : sizeof(InstanceData);
	auto instBuf = useGrass
		? mCurrFrameResource->GrassInstanceCB->Resource()
		: mCurrFrameResource->InstanceCB->Resource();
	for (auto& ri : ritems)
	{
		if (ri->InstanceCount == 0) continue;
		UINT startIdx = useGrass ? ri->GrassInstanceBufferStartIndex : ri->InstanceBufferStartIndex;
		UINT64 byteOffset = (UINT64)startIdx * strideBytes;
		cmdList->SetGraphicsRoot32BitConstant(0, ri->ObjCBIndex, 0);
		cmdList->SetGraphicsRootShaderResourceView(3, instBuf->GetGPUVirtualAddress() + byteOffset);
		auto vbv = ri->Geo->VertexBufferView();
		auto ibv = ri->Geo->IndexBufferView();
		cmdList->IASetVertexBuffers(0, 1, &vbv);
		cmdList->IASetIndexBuffer(&ibv);
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
		cmdList->DrawIndexedInstanced(ri->IndexCount, ri->InstanceCount, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
}

// phase 3; separate this from runtime, only in editor
void MCEngine::RefreshWindowTitle() {
	std::wstring title = mMainWndCaption;
	title += (GetGbvState() ? L"(GBV) (fps:" : L" (fps:") + mFpsStr + L" mspf: " + mMspfStr + L") ";

	// Read the active scene's registered path directly from MCSceneManager —
	// single source of truth. Previously this cached into MCEngine::mCurrentScenePath,
	// which desynced after Switch() and caused Save to write to the wrong file.
	std::filesystem::path activePath;
	if (auto* active = mSceneManager.GetActive())
		activePath = mSceneManager.GetScenePath(active->name);
	title += activePath.empty() ? L"(unsaved)" : activePath.filename().wstring();

	if (mSceneDirty) title += L"*";
	SetWindowText(mhMainWnd, title.c_str());
}

float MCEngine::sceneAspectRatio() {
	return mSceneViewWidth / mSceneViewHeight;
}

std::vector<float> MCEngine::CalcGaussWeights(float sigma)
{
	float twoSigma2 = 2.0f * sigma * sigma;
	int MaxBlurRadius = 15;
	// Estimate the blur radius based on sigma since sigma controls the "width" of the bell curve.
	int blurRadius = (int)ceil(2.0f * sigma);
	blurRadius = blurRadius <= MaxBlurRadius ? blurRadius : MaxBlurRadius;
	std::vector<float> weights;
	weights.resize(2 * blurRadius + 1);
	float weightSum = 0.0f;

	for (int i = -blurRadius; i <= blurRadius; ++i)
	{
		float x = (float)i;
		weights[i + blurRadius] = expf(-x * x / twoSigma2);
		weightSum += weights[i + blurRadius];
	}

	// Divide by the sum so all the weights add up to 1.0.
	for (int i = 0; i < weights.size(); ++i)
	{
		weights[i] /= weightSum;
	}
	return weights;
}

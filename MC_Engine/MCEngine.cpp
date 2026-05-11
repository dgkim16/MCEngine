// Ch7_Shapes.cpp
#include "MCEngine.h"
#include "ShaderLib.h"
#include "MCScene.h"
#include "UploadHelper.h"
#include "DescHeapManager.h"	// singleton - thus not saved as member variable inside MCEngine.h
#include <DirectXColors.h>
#include <WindowsX.h>
#include <dxgidebug.h>
#include <pix3.h>
#include "MCAssetIdentity.h"
#include "AuthorScenes.h"

using namespace DirectX;

extern const int gNumFrameResources;  // in d3dUtil.h

// Texture metadata now lives in Assets/Textures/*.mctex; loaded by
// mTextureManager.LoadDirectory in Initialize.

MCEngine::MCEngine(HINSTANCE hInstance)
	: D3DApp(hInstance), 
	mMaterialManager(*this), mTextureManager(*this), 
	mMeshSourceManager(*this), mSceneManager(*this), mMigrator()
{
	mMainWndCaption = L"MC Engine";
	BuildCamera();
}

MCEngine::~MCEngine()
{
	OutputDebugString(L"MC Engine DESTRUCTOR Start\n");
	if (md3dDevice != nullptr)
		FlushCommandQueue();
	if (auto* active = mSceneManager.GetActive())
		active->Deactivate(*this);
	IMGUI_SHUTDOWN();

	for (auto& rs : mRootSignatures)
		rs.Reset();
	mComputeRootSignature.Reset();
	mGrassCullRootSignature.Reset();
	mDebugLineRootSig.Reset();
	mGrassCommandSignature.Reset();

	// DescHeapManager singleton owns a CPU-only staging descriptor heap for the Static tier.
	// Its instance lives in a static local inside Singleton::Init and is destroyed at program
	// exit — AFTER ReportLiveObjects below. Release the heaps now so they don't show up as leaks.
	DescHeapManager::Get().Shutdown();

	for (auto& pso : mPSOs)
		pso.second.Reset();
	mRtvHeap.Reset();
	mDsvHeap.Reset();
	mCbvSrvUavHeap.Reset();
	mCbvHeap.Reset();

	mSceneColor.mResource.Reset();
	mSceneDepth.mResource.Reset();
	mViewportColor.mResource.Reset();
	mDepthDebugColor.mResource.Reset();
	mViewportNoAlpha.mResource.Reset();
	mBlurred0.mResource.Reset();
	mBlurred1.mResource.Reset();
	mForceAlphaUploadBuffer.reset();
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

	mCommandList.Reset();
	mDirectCmdListAlloc.Reset();
	mFrameResources.clear();

	// Release manager-owned GPU resources before the device is destroyed.
	// Both managers hold ID3D12Resource ComPtrs (texture default heaps, geometry
	// VB/IB + upload heaps). Without explicit Clear() they destruct implicitly
	// AFTER this destructor body returns — i.e. after ReportLiveObjects below
	// has already counted their resources as leaked device refs.
	mMeshSourceManager.Clear();
	mTextureManager.Clear();
	for (auto& [name, scene] : mSceneManager.All()) {
		scene->ResetSceneResources();
		// Null the back-pointer so ~MCScene (firing later, during member teardown)
		// does not call sceneAccess() on a dying engine. The ObjCBIndex slots
		// leaked here are process-exit waste, not a runtime concern.
		scene->mEngine = nullptr;
	}
	mSceneManager.ClearActive();
	// (mSceneManager owns its mScenes map; it's destroyed when MCEngine is destroyed.
	// We don't need to clear it manually here, but ResetSceneResources runs first to
	// release MCScene-owned GPU resources while the device is still alive.)

	for (auto& bbuf : mSwapChainBuffer)
		bbuf.Reset();
	mDepthStencilBuffer.Reset();
	mFence.Reset();
	mCommandQueue.Reset();
	mSwapChain.Reset();
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
	OutputDebugString(L"======= END of LEAKED ID3D12Device dependent objects\n");
	OutputDebugString(L"MC Engine DESTRUCTOR ENDING\n");
}

bool MCEngine::Initialize()
{
	std::cout << "start initialization\n";
	if (!D3DApp::Initialize())
		return false;
	std::cout << "D3DAPP init Success\n"; OutputDebugString(L"D3DAPP init Success\n");

#ifdef MC_AUTHOR_SCENES_ONCE
	// Step 8A4 one-shot: read v1-shape Assets/scenes/scene_*.json files,
	// transform each into the v2 eight-key shape (drop-merging), and write
	// them back. Process exits immediately after — re-run without the
	// -DMC_AUTHOR_SCENES_ONCE flag for normal boot.
	AuthorScenes::EmitAll();
	std::exit(0);
#endif
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mBarrierManager;

	// Material manager populated before scene::BuildRenderItems (which calls
	// engine.Materials().Get(handle)). Materials don't touch the descriptor
	// heap during load — just JSON parsing + CB-index assignment.
	// Texture LoadDirectory is deferred to after BuildDescriptorHeaps because
	// MCTextureManager::LoadFromFile calls DescHeapManager::Get().CreateSrv2d,
	// which requires DescHeapManager::Init.
	// MatCBFreeList needs non-zero capacity before LoadDirectory's Allocate;
	// 256 is a generous Phase-1 ceiling.
	mMaterialManager.MatCBFreeList().SetCapacity(256);
	mMaterialManager.LoadDirectory("Assets/materials");

	BuildRootSignature();            std::cout << "BuildRootSignature() success\n" << std::endl; OutputDebugString(L"BuildRootSignature() success\n");
	BuildShadersAndInputLayout();    std::cout << "BuildShadersAndInputLayout() success\n" << std::endl; OutputDebugString(L"BuildShadersAndInputLayout() success\n");

	RegisterLights();

	// mObjCBFreeList must be sized BEFORE LoadAll: under v2 each LoadRenderItemsFromJson
	// allocates an ObjCBIndex slot, and Allocate throws when capacity is 0. v1's
	// Scene_X::BuildRenderItems used a local objCBIndex++ counter, so the free list
	// only needed sizing later (after BuildFrameResources at line ~168). 8A5 pulled
	// the Allocate forward into LoadAll, hence the early sizing here. The downstream
	// BuildFrameResources still recomputes mObjCBCapacity from the active scene's
	// item count + 64 headroom, and the SetCapacity below adjusts to match — that
	// adjustment is safe as long as the headroom keeps it ≥ free-list high-water.
	constexpr std::uint32_t kBootObjCBCapacity = 256;
	mObjCBFreeList.SetCapacity(kBootObjCBCapacity);

	// Register all scenes via the manager (geometry upload uses the already-open command list)
	mSceneManager.LoadAssetRedirects("assets/asset_redirects.json");
	mSceneManager.LoadAll({
		{"Ch7",      "assets/scenes/scene_ch7.json"},
		{"Empty",    "assets/scenes/scene_empty.json"},
		{"Grass",    "assets/scenes/scene_grass.json"},
		{"SoloVoid", "assets/scenes/scene_solo_void.json"},   // D9 Step 6a — empty-scene edge case
		{"SoloBox",  "assets/scenes/scene_solo_box.json"},    // D9 Step 6b — single-render-item edge case
		});
	mSceneManager.ValidateRedirects();   // D9 Step 2b — dangling-target check; runs after LoadAll so procedural meshes are present.
	mSceneManager.Switch("Ch7");

	BuildFrameResources();           std::cout << "BuildFrameResources() success\n" << std::endl; OutputDebugString(L"BuildFrameResources() success\n");
	mObjCBFreeList.SetCapacity(mObjCBCapacity);
	// v1 had: assert(HighWater == GetActive()->allRitems.size()) — one scene loaded at a time.
	// v2 LoadAll loads every scene upfront, so HighWater spans all scenes' items, not just the
	// active one. Keep the weaker invariant: every active-scene item must have a slot, and
	// the per-frame ObjCB array (sized at mObjCBCapacity = active+headroom) must cover the
	// global high-water — otherwise a render-item ObjCBIndex would index past the end.
	assert(mObjCBFreeList.HighWater() >= mSceneManager.GetActive()->allRitems.size()
	       && "free-list high-water below active scene's item count");
	assert(mObjCBCapacity >= mObjCBFreeList.HighWater()
	       && "per-frame ObjCB array smaller than free-list high-water; bump kObjCBHeadroom");
	BuildDescriptorHeaps();          std::cout << "Descriptor Heaps build success\n" << std::endl; OutputDebugString(L"Descriptor Heaps build success\n");

	// Step 12b (deferred half): texture LoadDirectory must run after
	// BuildDescriptorHeaps because MCTextureManager::LoadFromFile calls
	// DescHeapManager::Get().CreateSrv2d, which requires DescHeapManager::Init
	// to have been called inside BuildDescriptorHeaps.
	mTextureManager.LoadDirectory("Assets/Textures");

	// BuildConstantBufferViews();      std::cout << "CBVs build success\n" << std::endl; OutputDebugString(L"CBVs build success\n");
	BuildPSOs();                     std::cout << "PSO build success\n" << std::endl; OutputDebugString(L"PSO build success\n");
	BuildDebugLineResources();       std::cout << "Debug line resources built\n";
	// MCScene render targets must exist before compute-CB init (reads their bindless offsets).
	BuildSceneRenderTarget();
	BuildSceneRenderTargetDescriptors();
	BuildComputeShaderConstantBufferResources();
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	FlushCommandQueue();
	mSceneManager.GetActive()->Activate(*this);
	mSceneManager.GetActive()->RebindCachedPointers(*this);   // D-3: caller orders both.
	PreInitObjectCBs();

	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Start\n");
	// IMGUI_BuildImGuiDescriptorHeap();
	IMGUI_INIT();
	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Done\n");

	// Sync mCurrentScenePath to whatever LoadAll registered for the active
	// scene. Without this the title bar shows "(unsaved)" until the user opens
	// or saves something, even though Ch7 was loaded from a real file on disk.
	if (auto* active = mSceneManager.GetActive())
		mCurrentScenePath = mSceneManager.GetScenePath(active->name);
	RefreshWindowTitle();

	return true;
}

// this should not be called when in runtime (phase 3, separating runtime and editor)
void MCEngine::RefreshWindowTitle() {
	std::wstring title = mMainWndCaption;
	title += L" (fps:" + mFpsStr + L" mspf: " + mMspfStr + L") ";
	title += mCurrentScenePath.empty() ? L"(unsaved)" : mCurrentScenePath.filename().wstring();
	
	if (mSceneDirty) title += L"*";
	SetWindowText(mhMainWnd, title.c_str());
}

// D9 Step 5a — open the cmdlist, run Reload (which calls module OnLoad for any
// re-instantiated module — GPU work for grass), close + execute + flush so
// the next Draw can Reset cmdlist as usual. cmdlist is closed at ImGui-handler
// time (IMGUI_UPDATE runs from Update, before Draw resets the list).
void MCEngine::ReloadActiveSceneNow() {
	FlushCommandQueue();
	ThrowIfFailed(mDirectCmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	std::string err;
	try { mSceneManager.Reload(); }
	catch (const std::exception& e) { err = e.what(); }

	// Always close + execute, even on Reload throw. Leaves cmdlist in the
	// closed state Draw expects to Reset against; partial work submitted to
	// GPU is preferable to leaving the cmdlist stuck open.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* lists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(lists), lists);
	FlushCommandQueue();

	if (!err.empty()) throw std::runtime_error("ReloadActiveSceneNow: " + err);
}

void MCEngine::Update(GameTimer& gt)
{
	RefreshWindowTitle();
	if (_requestRoundTripTest) {
		_requestRoundTripTest = false;
		mSceneManager.TestRoundTripActiveScene();
	}

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
	mSceneManager.ConsumePendingSwitch();
	float dt = gt.DeltaTime();
	OnKeyboardInput(gt);
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
	ReadBackGpuTimer(dt);
	IMGUI_UPDATE();
	TickProfiler(dt);
	const float kTransitionSpeed = 4.0f; // units per second
	if (isOrtho)
		mOrthoT = min(mOrthoT + dt * kTransitionSpeed, 1.0f);
	else
		mOrthoT = max(mOrthoT - dt * kTransitionSpeed, 0.0f);
	UpdateObjectCBs(gt);
	UpdateInstanceData(gt);
	UpdateMainPassCB(gt);
	UpdateMaterialCBs(gt);
	
	if (mSceneSizeDirty) {
		FlushCommandQueue();
		OnSceneResize();
		mSceneSizeDirty = false;
	}

	UpdateDepthDebugCB(gt);
	UpdateBlurCB(gt);
	UpdateSobelCB(gt);
}

int ranCounter = 0;
int runLimit = gNumFrameResources;
void MCEngine::UpdateCamera(const GameTimer& gt)
{		
	if (mCameraDirty) {
		// DirtyAllRenderItems(); // camera position changed, so need to do frustrum checking for all ritems
		// mNumCameraDirty = gNumFrameResources;
		DirtyAllRenderItems();
		mMainCamera->SetLens(mMainCamera->GetFovY(), sceneAspectRatio(), nearPlane, farPlane);
		mMainCamera->UpdateViewMatrix();
		cameraDirtyCount++;
		mCameraDirty = false;
		mFrustrumDirty = true;
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
	for (auto& e : scene->allRitems) {
		if (e->NumFramesDirty > 0) {
			XMStoreFloat4x4(&e->World,
				MathHelper::ComposeWorldMatrix(e->Position, e->Rotation, e->Scale));
		}
		// Frustum check: always runs every frame — it's a visibility decision, not a resource one
		// BUT if the camera wasn't dirty AND the object wasn't dirty, there is no need to check bounds
		if (e->checkBounds && (mFrustrumDirty || e->NumFramesDirty > 0)) {
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			// Transform local AABB to world space so the Contains test handles non-uniform scale
			DirectX::BoundingBox worldBounds;
			e->Bounds.Transform(worldBounds, world);
			auto result = cullFrustum.Contains(worldBounds);
			e->insideFrustrum = !(result == DirectX::DISJOINT);
		}
		// CB write: only when resource data has changed, decrement only when actually written
		if (e->NumFramesDirty > 0 && (e->insideFrustrum || !enableBoundsCheck || !e->checkBounds)) {
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
			PerObjectCB objConstants;
			XMStoreFloat4x4(&objConstants.gWorld, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.gTexTransform, XMMatrixTranspose(texTransform));
			objConstants.gMatIndex = e->Mat->MatCBIndex;
			currentObjectCB->CopyData(e->ObjCBIndex, objConstants);
			e->NumFramesDirty--;
		}
	}
}

void MCEngine::UpdateInstanceData(const GameTimer& gt) {
	// if (mRitemLayer[(int)RenderLayer::OpaqueInstanced].empty())
	//		return;
	auto _instStart = std::chrono::high_resolution_clock::now();
	int bbInstCount = 0;
	int globalOffset = 0;
	auto currentInsanceBuffer = mCurrFrameResource->InstanceCB.get();
	auto currentObjectCB = mCurrFrameResource->ObjectCB.get();
	XMMATRIX view = mMainCamera->GetView();
	XMVECTOR detView = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&detView, view);
	// World-space culling frustum — computed once to avoid per-instance invWorld multiply
	DirectX::BoundingFrustum liveFrustumWorld;
	mMainCamera->GetFrustrum().Transform(liveFrustumWorld, invView);
	const DirectX::BoundingFrustum& cullFrustum =
		mFreezeCamera ? mFrozenWorldFrustum : liveFrustumWorld;

	auto* scene = mSceneManager.GetActive();
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
					// instConstants.MaterialIndex = instanceData[ci].MaterialIndex;
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
					// instConstants.MaterialIndex = instanceData[i].MaterialIndex;
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
	auto* gc = Scenes().GetActive()->GetModule<MCGrassCullingModule>();
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
	
	float viewWidth = mMainCamera->GetFovY() * 2.0f;
	float viewHeight = viewWidth / sceneAspectRatio();
	XMMATRIX orthoProj = XMMatrixOrthographicLH(
		viewWidth,   // e.g. 20.0f  — world-space width visible
		viewHeight,  // e.g. 20.0f / AspectRatio()
		nearPlane,        // near
		farPlane      // far
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

	XMStoreFloat4x4(&mMainPassCB.gView, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.gInvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.gProj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.gInvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.gViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.gInvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.gEyePosW = mMainCamera->GetPosition3f();
	// mMainPassCB.gRenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight); mSceneViewWidth / mSceneViewHeight
	mMainPassCB.gRenderTargetSize = XMFLOAT2(mSceneViewWidth, mSceneViewHeight);
	mMainPassCB.gInvRenderTargetSize = XMFLOAT2(1.0f / mSceneViewWidth, 1.0f / mSceneViewHeight);
	mMainPassCB.gNearZ = nearPlane;
	mMainPassCB.gFarZ = farPlane;
	mMainPassCB.gTotalTime = gt.TotalTime();
	mMainPassCB.gDeltaTime = gt.DeltaTime();
	mMainPassCB.gAmbientLight = mAmbientLight;
	mMainPassCB.gFogColor = mFogColor;
	mMainPassCB.gFogStart = mFogStart;
	mMainPassCB.gFogRange = mFogRange;
	for (size_t i = 0; i < MAX_LIGHTS; i++)
		mMainPassCB.gLights[i] = mLights[i];

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
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

void MCEngine::UpdateBlurCB(const GameTimer& gt) {
	if (!blurDirty)
		return;
	CSB_blur blur = {};
	auto w = CalcGaussWeights(blurValues.sigma);
	blur.BlurRadius = (int)w.size() / 2;
	ZeroMemory(blur.WeightVec, sizeof(blur.WeightVec));
	CopyMemory(blur.WeightVec, w.data(), w.size() * sizeof(float));
	blur.InputIndex  = (INT)mBlurred0.SRVs[0].offset;
	blur.OutputIndex = (INT)mBlurred1.UAVs[0].offset;
	mBlurUploadBuffer.get()->CopyData(0, blur);
	blur.InputIndex  = (INT)mBlurred1.SRVs[0].offset;
	blur.OutputIndex = (INT)mBlurred0.UAVs[0].offset;
	mBlurUploadBuffer.get()->CopyData(1, blur);
	blurDirty = false;
}

void MCEngine::UpdateSobelCB(const GameTimer& gt) {
	CSB_default csbs;
	// INputIndex, OutputIndex, Widht, Height	
	// corresponds to...
	// gBlurIndex(to sobel), gOutputIndex, gSceneIndex (to mult by sobeled), PADDING (not used)
	switch (mSobelType) {
		case SobelType::Default:
			csbs.InputIndex = (INT)mViewportNoAlpha.SRVs[0].offset;
			break;
		case SobelType::Depth:
			csbs.InputIndex = (INT)mDepthDebugColor.SRVs[0].offset;
			break;
		case SobelType::Gaussain:
		default:
			csbs.InputIndex = (INT)mBlurred0.SRVs[0].offset;
			break;
	}
	if (blurValues.enabled)
		csbs.Width = (INT)mBlurred0.SRVs[0].offset;
	else
		csbs.Width = (INT)mViewportNoAlpha.SRVs[0].offset; // viewport with no alpha srv
	csbs.OutputIndex = (INT)mSobelOutput.UAVs[0].offset;
	
	mSobelUploadBuffer.get()->CopyData(0, csbs);
	mSobelUploadBuffer.get()->Resource()->SetName(L"mSobelUploadBuffer");
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

void MCEngine::BuildConstantBufferViews()
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
	UINT objCount = (UINT)mSceneManager.GetActive()->allRitems.size(); // one render item can be in multiple layers, so use this to get objCount
	std::cout << "[BuildConstantBufferViews] objCount = " << objCount << std::endl;
	// Need a CBV descriptor for each object for each frame resource.
	for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex) 
	{
		std::cout << "building frame at index : " << frameIndex << std::endl;
		auto objectCB = mFrameResources[frameIndex]->ObjectCB->Resource(); // get the actual ID3D12Resource inside UploadBuffer-wrapper
		for (UINT i = 0; i < objCount; i++) {
			std::cout << "building cbv for object index " << i << std::endl;
			D3D12_GPU_VIRTUAL_ADDRESS cbAddress = objectCB->GetGPUVirtualAddress();

			// Offset to the ith object constant buffer in the buffer.
			cbAddress += i * objCBByteSize;

			// Offset to the object cbv in the descriptor heap.
			int heapIndex = frameIndex * objCount + i;
			auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
			handle.Offset(heapIndex, mCbvSrvUavDescriptorSize);

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
			cbvDesc.BufferLocation = cbAddress;
			cbvDesc.SizeInBytes = objCBByteSize;

			md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
		}
	}
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
	//    The v1 SubmeshName authoring field is gone from JSON; populate
	//    r->SubmeshName here so legacy debug paths that read it still work.
	r->SubmeshName = r->Geo->Name;
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

	// 5. Allocate ObjCBIndex from the free list (Day 5 prerequisite).
	//    Throws if exhausted — see CBFreeList::Allocate.
	r->ObjCBIndex = mObjCBFreeList.Allocate();

	// 6. Mark dirty so UpdateObjectCBs recomposes World from SRT next frame
	//    and pushes to all frame-resource CBs.
	r->NumFramesDirty = gNumFrameResources;

	// 7. MCScene-side insertion. Mirrors MCScene::AddRenderItem in shape but lives here
	//    because it also routes through manager-resolved Mat/Geo. Atomicity: name
	//    uniqueness check first, then scene.layers[i] for every set bit, then
	//    scene.allRitems takes ownership. If any step throws, the prior inserts must
	//    be rolled back manually — fragility documented in Step 3.
	auto [it, inserted] = scene.nameToRitem.try_emplace(r->Name, r.get());
	if (!inserted) {
		throw std::runtime_error("LoadRenderItem: duplicate name in scene '"
			+ scene.name + "': " + r->Name);
	}
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
	D3DApp::OnResize();
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

void MCEngine::BuildSceneRenderTarget()
{
	// OutputDebugString(L"BuildSceneRenderTarget - Starting\n");
	// Before any CreateCommittedResource() runs, queue old descriptors for reuse and release the resource.
	// Guarded: D3DApp::Initialize() triggers an OnResize() before BuildDescriptorHeaps() has called DescHeapManager::Init().
	if (mCbvSrvUavHeap != nullptr) {
		auto& dhm = DescHeapManager::Get();
		dhm.QueueRemoval_Texture(mSceneColor);
		dhm.QueueRemoval_Texture(mSceneDepth);
		dhm.QueueRemoval_Texture(mDepthDebugColor);
		dhm.QueueRemoval_Texture(mViewportColor);
		dhm.QueueRemoval_Texture(mViewportNoAlpha);
		dhm.QueueRemoval_Texture(mBlurred0);
		dhm.QueueRemoval_Texture(mBlurred1);
		dhm.QueueRemoval_Texture(mSobelOutput);
		// GPU is idle here (caller flushed the command queue before resize). Drain the
		// deferred-free list immediately so BuildSceneRenderTargetDescriptors below
		// reuses the just-retired slots. Keeps hGpu.ptr values captured earlier this
		// frame (e.g. ImGui's ImTextureID) pointing at refreshed descriptors.
		dhm.FlushPending();
	}
	mSceneColor = {};
	mSceneDepth = {};
	mDepthDebugColor = {};
	mViewportColor = {};
	mBlurred0 = {};
	mBlurred1 = {};
	mViewportNoAlpha = {};
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
	texDesc.SampleDesc.Count = mScene4xMsaaState ? 4 : 1;
	texDesc.SampleDesc.Quality = mScene4xMsaaState ? (m4xMsaaQuality - 1) : 0;
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
	mSceneColor.m_currState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	// ==============================================
	//! create resource for viewport (non-msaa always)
	// ==============================================
	D3D12_RESOURCE_DESC texDesc_nonMSAA = texDesc;
	texDesc_nonMSAA.SampleDesc.Count = 1;
	texDesc_nonMSAA.SampleDesc.Quality = 0;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc_nonMSAA,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(mViewportColor.mResource.GetAddressOf())
	));
	mViewportColor.mResource.Get()->SetName(L"mViewportColor");
	mViewportColor.Name = "mViewportColor";
	mViewportColor.m_currState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	D3D12_RESOURCE_DESC texDesc_nonMSAA_NoAlpha = texDesc_nonMSAA;
	texDesc_nonMSAA_NoAlpha.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc_nonMSAA_NoAlpha,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(mViewportNoAlpha.mResource.GetAddressOf())
	));
	mViewportNoAlpha.mResource.Get()->SetName(L"mViewportNoAlpha");
	mViewportNoAlpha.Name = "mViewportNoAlpha";
	mViewportNoAlpha.m_currState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	

	// ==============================================
	//! create resource for blurred output. (non-msaa always)
	// ==============================================
	D3D12_RESOURCE_DESC blurDesc = texDesc_nonMSAA_NoAlpha;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&blurDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(mBlurred0.mResource.GetAddressOf())));
	mBlurred0.mResource.Get()->SetName(L"mBlurred0");
	mBlurred0.Name = "mBlurred0";
	mBlurred0.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&blurDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(mBlurred1.mResource.GetAddressOf())));
	mBlurred1.mResource.Get()->SetName(L"mBlurred1");
	mBlurred1.Name = "mBlurred1";
	mBlurred1.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;

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
	mSobelOutput.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;

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
	mSceneDepth.m_currState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

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
	mDepthDebugColor.m_currState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

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
	dhm.CreateRtv2d(mSceneColor, mSceneFormat, mScene4xMsaaState, 0);
	dhm.CreateSrv2d(mSceneColor, mSceneFormat, mScene4xMsaaState, MC_VIEW_TIER_STATIC);

	// MCScene Depth: typeless SRV view of the D24 resource
	dhm.CreateSrv2d(mSceneDepth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, mScene4xMsaaState, MC_VIEW_TIER_STATIC);

	// Depth Debug: RTV + SRV (non-MSAA, R8G8B8A8)
	dhm.CreateRtv2d(mDepthDebugColor, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
	dhm.CreateSrv2d(mDepthDebugColor, DXGI_FORMAT_R8G8B8A8_UNORM, false, MC_VIEW_TIER_STATIC);

	// Viewport scene copy target (SRV only)
	dhm.CreateSrv2d(mViewportColor, mSceneFormat, false, MC_VIEW_TIER_STATIC);

	// Viewport NoAlpha: SRV + UAV
	dhm.CreateSrv2d(mViewportNoAlpha, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mViewportNoAlpha, mSceneFormat, 0, MC_VIEW_TIER_STATIC);

	// Blur ping-pong: SRVs then UAVs
	dhm.CreateSrv2d(mBlurred0, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateSrv2d(mBlurred1, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mBlurred0, mSceneFormat, 0, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mBlurred1, mSceneFormat, 0, MC_VIEW_TIER_STATIC);

	// Sobel: SRV + UAV
	dhm.CreateSrv2d(mSobelOutput, mSceneFormat, false, MC_VIEW_TIER_STATIC);
	dhm.CreateUav2d(mSobelOutput, mSceneFormat, 0, MC_VIEW_TIER_STATIC);

	// Post-process compute CBs cache bindless offsets of the MCTextureResources above.
	// Offsets shift after every resize (new allocations land at fresh slots), so
	// re-upload here. mSobelUploadBuffer is re-uploaded every frame in UpdateSobelCB.
	if (mForceAlphaUploadBuffer) {
		CSB_default forceAlphaCB = {};
		forceAlphaCB.InputIndex  = (INT)mViewportColor.SRVs[0].offset;
		forceAlphaCB.OutputIndex = (INT)mViewportNoAlpha.UAVs[0].offset;
		mForceAlphaUploadBuffer->CopyData(0, forceAlphaCB);
	}
	blurDirty = true; // UpdateBlurCB next tick rewrites CSB_blur with fresh offsets
}

float MCEngine::sceneAspectRatio() {
	return mSceneViewWidth / mSceneViewHeight;
}

void MCEngine::RegisterLights() {
	mLights.reserve(MAX_LIGHTS);
	LightData dLight = {};
	dLight.mLightColor = { 1.0f, 1.0f, 1.0f };
	dLight.mLightIntensity = 3.0f;
	dLight.mLightDirection = { 1.0f, 1.0f, 1.0f };
	dLight.mLightType = (int)(LightType::Directional);
	mLights.push_back(std::move(dLight));
}

// MCEngine::RegisterScene was deleted in D8 Step 1c — moved to MCSceneManager::RegisterScene.
// MCEngine::SwitchScene was deleted in D8 Step 1c — moved to MCSceneManager::Switch.
// MCEngine::GetActiveScene / GetScenes were deleted in D8 Step 1c — use engine.Scenes().GetActive() / .All().

// MCEngine::LoadSceneIntoOpenList was deleted in D8 Step 8e — MCScene has no Load() to wrap,
// and post-8c every scene is loaded via MCSceneManager::LoadFromJson during the LoadAll boot
// sequence (which runs inside the engine's already-open cmd list).

void MCEngine::RebuildFrameResources()
{
	mFrameResources.clear();
	mCurrFrameResourceIndex = 0;
	BuildFrameResources();
	mCurrFrameResource = mFrameResources[0].get();
	mDepthDebugFramesDirty = gNumFrameResources;
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

void MCEngine::BuildFrameResources()
{
	auto* scene = mSceneManager.GetActive();
	constexpr UINT kObjCBHeadroom = 64u;  // spare slots for runtime LoadRenderItemsFromJson
	UINT objCount = max(1u, (UINT)scene->allRitems.size() + kObjCBHeadroom);
	mObjCBCapacity = objCount;
	constexpr UINT kMatCBHeadroom = 32u;
	UINT matCount = max(1u, (UINT)mMaterialManager.All().size() + kMatCBHeadroom);
	UINT totalInstances = 0;
	for (auto* ri : scene->layers[(int)RenderLayer::OpaqueInstanced])
		totalInstances += (UINT)ri->Instances.size();
	UINT totalGrassInstances = 0;
	for (auto* ri : scene->layers[(int)RenderLayer::GrassInstanced])
		totalGrassInstances += (UINT)ri->GrassInstances.size();

	for (int i = 0; i < gNumFrameResources; ++i)
	{
		// FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT depthCount);
		auto fr = std::make_unique<FrameResource>(md3dDevice.Get(),
			1, objCount, matCount, 1,
			max(1u, totalInstances), max(1u, totalGrassInstances));
		mFrameResources.push_back(std::move(fr));
	}
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

void MCEngine::BuildComputeShaderConstantBufferResources()
{
	mForceAlphaUploadBuffer = std::make_unique<UploadBuffer<CSB_default>>(md3dDevice.Get(), 1, 1);
	CSB_default forceAlphaCB = {};
	forceAlphaCB.InputIndex  = (INT)mViewportColor.SRVs[0].offset;
	forceAlphaCB.OutputIndex = (INT)mViewportNoAlpha.UAVs[0].offset;
	mForceAlphaUploadBuffer->CopyData(0, forceAlphaCB);
	mForceAlphaUploadBuffer->Resource()->SetName(L"mForceAlphaUploadBuffer");

	mSobelUploadBuffer = std::make_unique<UploadBuffer<CSB_default>>(md3dDevice.Get(), 1, 1);
	CSB_default csbs;
	csbs.InputIndex  = (INT)mBlurred0.SRVs[0].offset;
	csbs.OutputIndex = (INT)mSobelOutput.UAVs[0].offset;
	csbs.Width       = (INT)mViewportNoAlpha.SRVs[0].offset;
	csbs.Height      = (INT)mDepthDebugColor.SRVs[0].offset;
	mSobelUploadBuffer.get()->CopyData(0, csbs);
	mSobelUploadBuffer.get()->Resource()->SetName(L"mSobelUploadBuffer");
	
	auto blurBuf = std::make_unique<UploadBuffer<CSB_blur>>(md3dDevice.Get(), 2, 1);
	CSB_blur blurCB0;
	auto gauss_weights = CalcGaussWeights(0.1f);
	CopyMemory(blurCB0.WeightVec, gauss_weights.data(), gauss_weights.size() * sizeof(float));
	blurCB0.BlurRadius = (int)gauss_weights.size() / 2;
	blurCB0.InputIndex  = (INT)mBlurred0.SRVs[0].offset;
	blurCB0.OutputIndex = (INT)mBlurred1.UAVs[0].offset;
	blurBuf.get()->CopyData(0, blurCB0);
	OutputDebugString(L"copied blurCB0\n");
	CSB_blur blurCB1;
	CopyMemory(blurCB1.WeightVec, gauss_weights.data(), gauss_weights.size() * sizeof(float));
	blurCB1.BlurRadius = (int)gauss_weights.size() / 2;
	blurCB1.InputIndex  = (INT)mBlurred1.SRVs[0].offset;
	blurCB1.OutputIndex = (INT)mBlurred0.UAVs[0].offset;
	blurBuf.get()->CopyData(1, blurCB1);
	blurBuf.get()->Resource()->SetName(L"mBlurUploadBuffer");
	mBlurUploadBuffer = std::move(blurBuf);
	OutputDebugString(L"copied blurCB1\n");
}

void MCEngine::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, std::string pixEventName)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
	// UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
	// UINT objCount = mAllRitems.size();
	// auto pix_C = PIX_COLOR(1, 0, 0); // how to set color for pixEvent
	PIXBeginEvent(cmdList, PIX_COLOR(0,1,0), pixEventName.c_str());
	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	// For each render item... (non instances)
	for (auto& ri : ritems)
	{
		if (ri->insideFrustrum || !enableBoundsCheck || !ri->checkBounds) {
			auto vbv = ri->Geo->VertexBufferView();
			auto ibv = ri->Geo->IndexBufferView();
			cmdList->IASetVertexBuffers(0, 1, &vbv);
			cmdList->IASetIndexBuffer(&ibv);
			cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
			D3D12_GPU_VIRTUAL_ADDRESS handle = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
			cmdList->SetGraphicsRootConstantBufferView(0, handle);
			cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
		}
	}
	PIXEndEvent(cmdList);
}

//! CPU culling path of rendering Grass
void MCEngine::DrawInstanceRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool useGrass, std::string pixEventName) {
	PIXBeginEvent(cmdList, PIX_COLOR(0, 1, 0), pixEventName.c_str());
	UINT strideBytes = useGrass ? sizeof(GrassInstanceData) : sizeof(InstanceData);
	auto instBuf = useGrass
		? mCurrFrameResource->GrassInstanceCB->Resource()
		: mCurrFrameResource->InstanceCB->Resource();
	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
	for (auto& ri : ritems)
	{
		if (ri->InstanceCount == 0) continue;
		UINT startIdx = useGrass ? ri->GrassInstanceBufferStartIndex : ri->InstanceBufferStartIndex;
		UINT64 byteOffset = (UINT64)startIdx * strideBytes;
		D3D12_GPU_VIRTUAL_ADDRESS handle = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
		cmdList->SetGraphicsRootConstantBufferView(0, handle);
		mCommandList->SetGraphicsRootShaderResourceView(3, instBuf->GetGPUVirtualAddress() + byteOffset);
		auto vbv = ri->Geo->VertexBufferView();
		auto ibv = ri->Geo->IndexBufferView();
		cmdList->IASetVertexBuffers(0, 1, &vbv);
		cmdList->IASetIndexBuffer(&ibv);
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
		
		cmdList->DrawIndexedInstanced(ri->IndexCount, ri->InstanceCount, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
	}
	PIXEndEvent(cmdList);
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
			PerObjectCB obj;
			XMStoreFloat4x4(&obj.gWorld, XMMatrixTranspose(world));
			XMStoreFloat4x4(&obj.gTexTransform, XMMatrixTranspose(texTransform));
			obj.gMatIndex = e->Mat->MatCBIndex;
			objCB->CopyData(e->ObjCBIndex, obj);
		}
	}
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
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "Frame");
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
	
	// ==========================================================
	//! PASS 1: render scene to offscreen texture
	// ==========================================================
	mCommandList->RSSetViewports(1, &mSceneViewport);
	mCommandList->RSSetScissorRects(1, &mSceneScissorRect);

	// Indicate a state transition on the resource usage.
	mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
	mBarrierManager.TransitionState(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	mBarrierManager.FlushBarriers(mCommandList.Get());

	// Clear the back buffer and depth buffer.
	auto sceneRtv = mSceneColor.RTVs[0].hCpu;
	auto sceneDsv = mSceneDepth.DSVs[0].hCpu;
	mCommandList->ClearRenderTargetView(sceneRtv, (float*)&mFogColor, 0, nullptr);
	mCommandList->ClearDepthStencilView(sceneDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	mCommandList->OMSetRenderTargets(1, &sceneRtv, true, &sceneDsv);

	mSceneManager.GetActive()->Draw(*this);
	// GrassCullDispatch();
	ForwardPass(gt);
	TessellationExample(gt);

	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
	// ==========================================================
	//! PASS 2: render depth to debug depth texture
	// ==========================================================
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "Depth normalization pass");

	// mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mSceneDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mDepthDebugColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
	mBarrierManager.FlushBarriers(mCommandList.Get());
	
	mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["depthDebug_MSAA"].Get() : mPSOs["depthDebug"].Get());
	auto depthDebugRtv = mDepthDebugColor.RTVs[0].hCpu;
	mCommandList->OMSetRenderTargets(1, &depthDebugRtv, TRUE, nullptr);
	mCommandList->ClearRenderTargetView(depthDebugRtv, Colors::Black, 0, nullptr);

	// ID3D12DescriptorHeap* heaps[] = { mCbvSrvUavHeap.Get() };
	// mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);	// allows access to cbv/srv/uav via ResourceDescriptorHeap[index] in hlsl

	mCommandList->SetGraphicsRootSignature(mRootSignatures[1].Get());

	mCommandList->SetGraphicsRootDescriptorTable(0, mSceneDepth.SRVs[0].hGpu); // raw scene depth SRV
	mCommandList->SetGraphicsRootConstantBufferView(1, mCurrFrameResource->DepthCB.get()->Resource()->GetGPUVirtualAddress());

	mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	mCommandList->DrawInstanced(3, 1, 0, 0);

	// mBarrierManager.TransitionState(mDepthDebugColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	// mBarrierManager.TransitionState(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	// mBarrierManager.FlushBarriers(mCommandList.Get());

	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
	PIXEndEvent(mCommandList.Get());
	// ==========================================================
	//! PASS 3 : MSAA resolve; copy mSceneColor to mViewportColor (via ResolveSubresource)
	// ==========================================================
	if (mScene4xMsaaState) {
		PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "MSAA resolve pass");

		mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		mBarrierManager.TransitionState(mViewportColor, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		mBarrierManager.FlushBarriers(mCommandList.Get());
		
		mCommandList->ResolveSubresource(mViewportColor.mResource.Get(), 0, mSceneColor.mResource.Get(), 0, mSceneFormat);

		// mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// mBarrierManager.TransitionState(mViewportColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// mBarrierManager.FlushBarriers(mCommandList.Get());
		
		PIXEndEvent(mCommandList.Get());
	}
	else {
		PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "Copy scene texture to viewport texture pass");

		mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
		mBarrierManager.TransitionState(mViewportColor, D3D12_RESOURCE_STATE_COPY_DEST);
		mBarrierManager.FlushBarriers(mCommandList.Get());

		mCommandList->CopyResource(mViewportColor.mResource.Get(),mSceneColor.mResource.Get());

		// mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// BarrierManager.TransitionState(mViewportColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// mBarrierManager.FlushBarriers(mCommandList.Get());
		PIXEndEvent(mCommandList.Get());
	}
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
	// ==========================================================
	//! PASS 4: (COMPUTE) Copy RGB values from mViewColor to mViewportNoAlpha, but with Alpha = 1.0f
	// ==========================================================
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR(255,0,0), "PSO to forceAlphaOne");
	mCommandList->SetPipelineState(mPSOs["forceAlphaOne"].Get());
	mCommandList->SetComputeRootSignature(mComputeRootSignature.Get());
	mCommandList->SetComputeRootConstantBufferView(0, mForceAlphaUploadBuffer->Resource()->GetGPUVirtualAddress());
	PIXEndEvent(mCommandList.Get());
	UINT numGroupsX = (UINT)ceilf(mSceneViewWidth / 16.0f);
	UINT numGroupsY = (UINT)ceilf(mSceneViewHeight / 16.0f);
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "compute : force alpha pass");
	mBarrierManager.TransitionState(mViewportColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	mBarrierManager.FlushBarriers(mCommandList.Get());

	mCommandList->Dispatch(numGroupsX, numGroupsY, 1);
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
	PIXEndEvent(mCommandList.Get());
	// ==========================================================
	//! PASS 5: (COMPUTE) Blur what's on viewport
	// ==========================================================
	// copy to blur0 >> loop blur between blur0 and blur1 >> copy back into viewport
	if (blurValues.enabled || (mSobelType == SobelType::Gaussain && mIsSobel)) {
		PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "compute : blur pass");
		mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_COPY_SOURCE);
		mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_COPY_DEST);
		mBarrierManager.FlushBarriers(mCommandList.Get());

		mCommandList->CopyResource(mBlurred0.mResource.Get(), mViewportNoAlpha.mResource.Get());

		auto blurCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(CSB_blur));
		D3D12_GPU_VIRTUAL_ADDRESS blurCBaddresss0 = mBlurUploadBuffer->Resource()->GetGPUVirtualAddress();
		D3D12_GPU_VIRTUAL_ADDRESS blurCBaddresss1 = mBlurUploadBuffer->Resource()->GetGPUVirtualAddress() + blurCBByteSize;
		UINT blurGroupsX = (UINT)ceilf(mSceneViewWidth / 256.0f);
		UINT blurGroupsY = (UINT)ceilf(mSceneViewHeight / 256.0f);
		for (int i = 0; i < blurValues.blurIter; i++) {
			// HORIZONTAL BLUR
			mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			mBarrierManager.TransitionState(mBlurred1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mBarrierManager.FlushBarriers(mCommandList.Get());
			mCommandList->SetPipelineState(mPSOs["horzBlur"].Get());
			mCommandList->SetComputeRootConstantBufferView(0, blurCBaddresss0);
			mCommandList->Dispatch(blurGroupsX, static_cast<UINT>(mSceneViewHeight), 1);

			// VERTICAL BLUR
			mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mBarrierManager.TransitionState(mBlurred1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			mBarrierManager.FlushBarriers(mCommandList.Get());
			mCommandList->SetPipelineState(mPSOs["vertBlur"].Get());
			mCommandList->SetComputeRootConstantBufferView(0, blurCBaddresss1);
			mCommandList->Dispatch(static_cast<UINT>(mSceneViewWidth), blurGroupsY, 1);
		}
		PIXEndEvent(mCommandList.Get());
	}
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
	// ==========================================================
	//! PASS 5.5: (COMPUTE) use sobel on blurred output and multiply with scene before blur 
	if (mIsSobel) {
		PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "compute : sobel outline");
		UINT sobelGroupsX = (UINT)ceilf(mSceneViewWidth / 8.0f);
		UINT sobelGroupsY = (UINT)ceilf(mSceneViewHeight / 8.0f);

		switch (mSobelType) {
		case SobelType::Default:
			mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			break;
		case SobelType::Depth:
			mBarrierManager.TransitionState(mDepthDebugColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			break;
		case SobelType::Gaussain:
		default:
			mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			break;
		}
		mBarrierManager.TransitionState(mSobelOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		mBarrierManager.FlushBarriers(mCommandList.Get());

		mCommandList->SetPipelineState(mPSOs["sobel"].Get());
		D3D12_GPU_VIRTUAL_ADDRESS sobelCBaddress = mSobelUploadBuffer->Resource()->GetGPUVirtualAddress();
		mCommandList->SetComputeRootConstantBufferView(0, sobelCBaddress);
		mCommandList->Dispatch(sobelGroupsX, sobelGroupsY, 1);
		/*
		mBarrierManager.TransitionState(mSobelOutput, D3D12_RESOURCE_STATE_GENERIC_READ);
		mBarrierManager.FlushBarriers(mCommandList.Get());
		*/
		PIXEndEvent(mCommandList.Get());
	}
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 8);
	mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 9);
	mCommandList->ResolveQueryData(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,FrameResource::GpuTimerCount, mCurrFrameResource->GpuTimestampReadback.Get(), 0);
	// ==========================================================
	//! PASS 6: render Imgui to swap chain back buffer
	// ==========================================================
	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "set back buffer as render target");
	auto transition_BB = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	mCommandList->ResourceBarrier(1, &transition_BB);
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);
	auto backRtv = CurrentBackBufferView();
	mCommandList->ClearRenderTargetView(backRtv, (float*)&mMainPassCB.gFogColor, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	auto dsv = DepthStencilView();
	mCommandList->OMSetRenderTargets(1, &backRtv, true, &dsv);
	//mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
	PIXEndEvent(mCommandList.Get());

	PIXBeginEvent(mCommandList.Get(), PIX_COLOR_DEFAULT, "imgui pass");
	IMGUI_RENDERDRAWDATA();
	PIXEndEvent(mCommandList.Get());

	//mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 8);
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
		CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT
	);
	mCommandList->ResourceBarrier(1, &transition);

	// End measuring GPU deltatime
	//mCommandList->EndQuery(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 9);
	//mCommandList->ResolveQueryData(mCurrFrameResource->GpuTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,FrameResource::GpuTimerCount, mCurrFrameResource->GpuTimestampReadback.Get(), 0);
	PIXEndEvent(mCommandList.Get());	// Frame 
	ThrowIfFailed(mCommandList->Close());
	// ==========================================================
	
	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	
	// swap the back and front buffers
	UINT presentFlags = mTearingSupported && !enableVsync ? DXGI_PRESENT_ALLOW_TEARING : 0;
	ThrowIfFailed(mSwapChain->Present(0, presentFlags)); // set SyncInterval to 1~4 to enable VSYNC (it gets enabled by default by dxgi tho, so need to use flags to disable it)
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	mCurrFrameResource->Fence = ++mCurrentFence;						// Instead of flushing, advance the fence value to mark commands up to this fence point
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);		// then add instruction to cmd queue to set new fence point
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

// Ch7_Shapes.cpp
#include "MCEngine.h"
#include "ShaderLib.h"
#include "Scene.h"
#include "Scene_Ch7.h"
#include "Scene_Empty.h"
#include "Scene_grass.h"
#include "UploadHelper.h"
#include "DescHeapManager.h"	// singleton - thus not saved as member variable inside MCEngine.h
#include <DirectXColors.h>
#include <WindowsX.h>
#include <dxgidebug.h>
#include <pix3.h>

#define DHM DescHeapManager

using namespace DirectX;

extern const int gNumFrameResources;  // in d3dUtil.h

// texture mappings are here. Material to texture mapping is in Scene.h (differ by scenes)
static const std::map<std::string, std::wstring> texturesToLoad = {
	{"defaultTex",       L"Assets/Textures/white1x1.dds"},
	{"woodCrateTex",  L"Assets/Textures/WoodCrate01.dds"},
	{"modelTex",      L"Assets/Textures/다람디.dds"},
	{"gridTex",       L"Assets/Textures/tile.dds"},
	{"waterTex",      L"Assets/Textures/water1.dds"},
	{"grassTex",      L"Assets/Textures/grass.dds"},
	{"bricksTex",     L"Assets/Textures/bricks3.dds"},
	{"wirefenceTex",  L"Assets/Textures/WireFence.dds"},
	{"iceTex",        L"Assets/Textures/ice.dds"},
	{"treeArrTex",    L"Assets/Textures/treeArray2.dds"},
	{"teapot_normal", L"Assets/Textures/teapot_normal.dds"}
};

MCEngine::MCEngine(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
	mMainWndCaption = L"MC Engine";
	BuildCamera();
}

MCEngine::~MCEngine()
{
	OutputDebugString(L"MC Engine DESTRUCTOR Start\n");
	if (md3dDevice != nullptr)
		FlushCommandQueue();
	if (mActiveScene)
		mActiveScene->Deactivate(*this);
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
	DHM::Get().Shutdown();

	for (auto& tex : mTextures)
		tex.second->mResource.Reset();
	for (auto& pso : mPSOs)
		pso.second.Reset();
	for (auto& geo : mGeometries) {
		auto& mg = geo.second;
		mg->DisposeUploaders();
		mg->DisposeResources();
	} 
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

	// Release GPU resources stored inside inactive scenes.
	// On scene switch, the outgoing scene's MeshGeometry objects (vertex/index buffers)
	// are moved back into mScenes[x]->geometries. mScenes is a member destroyed AFTER
	// the destructor body (i.e. after ReportLiveObjects), so we must dispose them here.
	for (auto& [name, scene] : mScenes) {
		for (auto& [geoName, geo] : scene->geometries) {
			geo->DisposeUploaders();
			geo->DisposeResources();
		}
		scene->geometries.clear();
		scene->ResetSceneResources();
	}
	mScenes.clear();

	for (auto& bbuf : mSwapChainBuffer)
		bbuf.Reset();
	mDepthStencilBuffer.Reset();
	mFence.Reset();
	mCommandQueue.Reset();
	mSwapChain.Reset();
	md3dDevice.Reset();

	OutputDebugString(L"======= START of LEAKED ID3D12Device dependent objects:\n");
	ComPtr<IDXGIDebug1> dxgiDebug;
	ThrowIfFailed(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug)));
	dxgiDebug->ReportLiveObjects(
		DXGI_DEBUG_ALL,
		static_cast<DXGI_DEBUG_RLO_FLAGS>(
			DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
	OutputDebugString(L"======= END of LEAKED ID3D12Device dependent objects\n");
	OutputDebugString(L"MC Engine DESTRUCTOR ENDING\n");
}

bool MCEngine::Initialize()
{
	std::cout << "start initialization\n";
	if (!D3DApp::Initialize())
		return false;
	std::cout << "D3DAPP init Success\n"; OutputDebugString(L"D3DAPP init Success\n");
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mBarrierManager;

	LoadTextures();                  std::cout << "LoadTextures() success\n" << std::endl; OutputDebugString(L"LoadTextures() success\n");
	BuildRootSignature();            std::cout << "BuildRootSignature() success\n" << std::endl; OutputDebugString(L"BuildRootSignature() success\n");
	BuildShadersAndInputLayout();    std::cout << "BuildShadersAndInputLayout() success\n" << std::endl; OutputDebugString(L"BuildShadersAndInputLayout() success\n");

	RegisterLights();
	// Register all scenes (geometry upload uses the already-open command list)
	RegisterScene(std::make_unique<Scene_Ch7>());
	RegisterScene(std::make_unique<Scene_Empty>());
	RegisterScene(std::make_unique<Scene_grass>());
	// Load the initial scene inline (command list stays open for subsequent work)
	{
		mActiveScene = mScenes["Ch7"].get();
		mActiveScene->Load(*this);
		mActiveScene->loaded = true;
		mGeometries          = std::move(mActiveScene->geometries);
		mMaterials           = std::move(mActiveScene->materials);
		mMaterialsIndexTracker = std::move(mActiveScene->materialIndexTracker);
		mAllRitems           = std::move(mActiveScene->allRitems);
		for (int i = 0; i < (int)RenderLayer::Count; i++)
			mRitemLayer[i] = std::move(mActiveScene->layers[i]);
		for (int i = 0; i < (int)mAllRitems.size(); i++)
			mAllRitems[i]->ObjCBIndex = i;
		int mi = 0;
		for (auto& [k, v] : mMaterials) v->MatCBIndex = mi++;
		// Sync instance MaterialIndex after MatCBIndex reassignment (unordered_map order != build order)
		/*
		for (int layer : { (int)RenderLayer::OpaqueInstanced, (int)RenderLayer::GrassInstanced })
			for (auto* ri : mRitemLayer[layer])
				for (auto& inst : ri->Instances)
					inst.MaterialIndex = ri->Mat->MatCBIndex;
					*/
		total_objects = (int)mAllRitems.size();
		std::cout << "Scene 'Ch7' loaded: " << total_objects << " render items\n";
		OutputDebugString(L"Scene 'Ch7' loaded\n");
	}

	BuildFrameResources();           std::cout << "BuildFrameResources() success\n" << std::endl; OutputDebugString(L"BuildFrameResources() success\n");
	
	BuildDescriptorHeaps();          std::cout << "Descriptor Heaps build success\n" << std::endl; OutputDebugString(L"Descriptor Heaps build success\n");
	FixupMaterialDiffuseIndices();
	// BuildConstantBufferViews();      std::cout << "CBVs build success\n" << std::endl; OutputDebugString(L"CBVs build success\n");
	BuildPSOs();                     std::cout << "PSO build success\n" << std::endl; OutputDebugString(L"PSO build success\n");
	BuildDebugLineResources();       std::cout << "Debug line resources built\n";
	// Scene render targets must exist before compute-CB init (reads their bindless offsets).
	BuildSceneRenderTarget();
	BuildSceneRenderTargetDescriptors();
	BuildComputeShaderConstantBufferResources();
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	FlushCommandQueue();

	mActiveScene->Activate(*this);
	PreInitObjectCBs();

	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Start\n");
	// IMGUI_BuildImGuiDescriptorHeap();
	IMGUI_INIT();
	OutputDebugString(L"MCEngine-Initialize() - IMGUI init Done\n");
	return true;
}



void MCEngine::Update(GameTimer& gt)
{
	if (reloadScene) {
		reloadScene = false;
		if (mActiveScene) {
			std::string name = mActiveScene->name;
			mActiveScene->loaded = false;
			mActiveScene = nullptr;
			SwitchScene(name);
		}
	}

	if (!mPendingScene.empty()) {
		SwitchScene(mPendingScene);
		mPendingScene.clear();
	}
	float dt = gt.DeltaTime();
	OnKeyboardInput(gt);
	UpdateCamera(gt);

	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Advance deferred-free counters so retired descriptors re-enter the free list
	// after NumFrameResources frames.
	DHM::Get().Update();

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
	
	if (mSceneSizeDirty) {
		FlushCommandQueue();
		mScene4xMsaaState = mScene4xMsaaStateImGuiRequest;
		OnSceneResize();
		mSceneSizeDirty = false;
	}

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

	for (auto& e : mAllRitems) {
		// Frustum check: always runs every frame — it's a visibility decision, not a resource one
		// BUT if the camera wasn't dirty AND the object wasn't dirty, there is no need to check bounds
		if (e->Mat->renderLevel == (int)RenderLayer::OpaqueInstanced || e->Mat->renderLevel == (int)RenderLayer::GrassInstanced) {
			// continue;
		}
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

	for (auto& e : mRitemLayer[(int)RenderLayer::OpaqueInstanced]) {
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
	if (!(mGrassScene && mGrassScene->useGpuCulling)) {
		int grassOffset = 0;
		
		for (auto& e : mRitemLayer[(int)RenderLayer::GrassInstanced]) {
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
	mInstancingCullingTime = std::chrono::duration<double, std::milli>(_instEnd - _instStart).count();
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
	for (auto& e : mMaterials) {
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0) {
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);
			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			matConstants.MatTransform = mat->MatTransform;
			matConstants.srvIndex = mat->DiffuseSrvHeapIndex;
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
	if (mSobelCBFramesDirty <= 0) return;
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
		csbs.Width = (INT)mBlurred0.SRVs[0].offset;	// width = gSceneIndex
	else
		csbs.Width = (INT)mViewportNoAlpha.SRVs[0].offset; // viewport with no alpha srv
	csbs.OutputIndex = (INT)mSobelOutput.UAVs[0].offset;
	
	mCurrFrameResource->SobelCB.get()->CopyData(0, csbs);
	--mSobelCBFramesDirty;
}

void MCEngine::UpdateSobelState() {
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
	if (blurValues.enabled) // sobel without blur, but blur itself is enabled
		mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	else
		mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mSobelOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void MCEngine::BuildDescriptorHeaps()
{
	mNumImportedTextureSrvs = (UINT)mTextures.size();
	mCsuTierStaticCap = mNumImportedTextureSrvs + kCsuTierStaticHeadroom;

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
	DHM::Init(DescHeapManDesc);

	auto hDescriptor_Unified = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart());

	// hDescriptor.Offset((INT)mTextureSrvOffset, mCbvSrvUavDescriptorSize);
	hDescriptor_Unified.Offset(0, mCbvSrvUavDescriptorSize);

	for (auto& e : mTextures) {
		auto& mtex = *e.second;
		DHM::Get().CreateSrv2d(mtex, mtex.mResource->GetDesc().Format);
	}
}

void MCEngine::BuildConstantBufferViews()
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
	UINT objCount = mAllRitems.size(); // one render item can be in multiple layers, so use this to get objCount
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

MCTexture* MCEngine::GetTexture(const std::string& name) const
{
	auto it = mTextures.find(name);
	return (it == mTextures.end()) ? nullptr : it->second.get();
}

void MCEngine::LoadTextures()
{
	for (auto& e : texturesToLoad) {
		auto tex = std::make_unique<MCTexture>();
		tex->Name = e.first;
		tex->Filename = e.second;
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
			md3dDevice.Get(),
			mCommandList.Get(),
			tex->Filename.c_str(),
			tex->mResource,
			tex->UploadHeap));
		std::wstring wname = AnsiToWString(tex->Name);
		tex->mResource->SetName(wname.c_str());
		tex->UploadHeap->SetName((L"UPLOAD_" + wname).c_str());
		std::cout << "loaded texture : " << e.first << std::endl;
		mTextures[tex->Name] = std::move(tex);
	}
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	FlushCommandQueue();
	mDirectCmdListAlloc->Reset();
	mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);

	for (auto& e : mTextures) // upload heap is no longer needed for these textures. batch reset.
		e.second->UploadHeap.Reset();

	int q = 0;
	for (auto& e : mTextures) {
		std::string name = e.first;
		mTexturesIndexStrTracker[q] = std::move(name);
		q++;
	}
	for (auto& e : mTexturesIndexStrTracker) {
		mTexturesStrIndexTracker[e.second] = e.first;
	}
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
	mSobelCBFramesDirty = gNumFrameResources; // with descriptors rebuit, .offset value inside csbs may change. So need to update.

}

void MCEngine::BuildSceneRenderTarget()
{
	// OutputDebugString(L"BuildSceneRenderTarget - Starting\n");
	// Before any CreateCommittedResource() runs, queue old descriptors for reuse and release the resource.
	// Guarded: D3DApp::Initialize() triggers an OnResize() before BuildDescriptorHeaps() has called DHM::Init().
	if (mCbvSrvUavHeap != nullptr) {
		auto& dhm = DHM::Get();
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
		// reuses the just-retired slots. NOTE: Their order is NOT guaranteed
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
	auto& dhm = DHM::Get();

	// DSV for scene depth (was previously in BuildSceneRenderTarget; route through manager).
	dhm.CreateDsv(mSceneDepth, D3D12_DSV_FLAG_NONE, DXGI_FORMAT_D24_UNORM_S8_UINT, mScene4xMsaaState, 0);

	// Scene Color: RTV + SRV
	dhm.CreateRtv2d(mSceneColor, mSceneFormat, mScene4xMsaaState, 0);
	dhm.CreateSrv2d(mSceneColor, mSceneFormat, mScene4xMsaaState, MC_VIEW_TIER_STATIC);

	// Scene Depth: typeless SRV view of the D24 resource
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

	// Post-process compute CBs cache bindless offsets of the MCTextures above.
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

void MCEngine::FixupMaterialDiffuseIndices()
{
	for (auto& [name, mat] : mMaterials) {
		if (!mat->textureName.empty() && mTextures.count(mat->textureName))
			mat->DiffuseSrvHeapIndex = mTextures[mat->textureName]->SRVs[0].offset;
	}
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

void MCEngine::RegisterScene(std::unique_ptr<Scene> scene)
{
	std::string key = scene->name;
	mScenes[key] = std::move(scene);
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

void MCEngine::GrassCullDispatch()
{
	if (!mGrassScene || !mGrassScene->useGpuCulling) return;
	auto* gs = mGrassScene;

	PIXBeginEvent(mCommandList.Get(), PIX_COLOR(0, 200, 100), "Grass Cull CS");

	// 1. Reset counter to 0 (counterReset is GENERIC_READ, counter is COPY_DEST)
	// note that actual counter is GPU-memory (default heap), so to update it
	// we need to do something like UpdateSubresource() with an upload heap CPU-memory
	// but that is a helper function that uses raw cpu-memory data (const void* wrapped in D3D12_SUBRESOURCE_DATA)
	// This is a lower level API that allows direct copying from one resource to another
	// see documentation for requirements of using CopyBufferRegion
	mCommandList->CopyBufferRegion(
		gs->mGrassCounterBuffer.mResource.Get(), 0,
		gs->mGrassCounterResetBuffer.mResource.Get(), 0,
		sizeof(UINT));

	// 2. Transition visible buffer and counter to UAV for CS writes
	mBarrierManager.TransitionState(gs->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	mBarrierManager.TransitionState(gs->mGrassCounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	mBarrierManager.FlushBarriers(mCommandList.Get());
	/*
	CD3DX12_RESOURCE_BARRIER toUAV[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassVisibleBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
	};
	mCommandList->ResourceBarrier(_countof(toUAV), toUAV);
	*/
	// 3. Update CullCB — extract world-space frustum planes on CPU, send to shader
	// mMainPassCB.gViewProj stores Transpose(M_vp), so its rows equal the columns of M_vp.
	// Gribb-Hartmann (row-vector DX convention): planes come from columns of M_vp.
	GrassCullCB cullCB = {};
	{
		// When frozen, use the snapshot captured at freeze time; otherwise use the live VP.
		XMMATRIX vpT = mFreezeCamera
			? XMLoadFloat4x4(&mFrozenViewProjT)
			: XMLoadFloat4x4(&mMainPassCB.gViewProj);
		XMVECTOR rawPlanes[6] = {
			vpT.r[3] + vpT.r[0],   // left
			vpT.r[3] - vpT.r[0],   // right
			vpT.r[3] + vpT.r[1],   // bottom
			vpT.r[3] - vpT.r[1],   // top
			vpT.r[2],               // near  (DX NDC z >= 0)
			vpT.r[3] - vpT.r[2],   // far
		};
		for (int i = 0; i < 6; i++)
			XMStoreFloat4(&cullCB.FrustumPlanes[i], XMPlaneNormalize(rawPlanes[i]));
	}
	cullCB.EyePosW            = mMainPassCB.gEyePosW;
	cullCB.DrawDistance       = 500.0f;
	cullCB.InstanceCount      = gs->mTotalGrassInstances;
	cullCB.GrassMaterialIndex = gs->mGrassMaterial->MatCBIndex; // always current, even after reassignment
	cullCB.SphereRadius       = gs->grassHeight;
	// gs->mGrassCullCB->CopyData(0, cullCB);
	gs->mGrassCullCB[mCurrFrameResourceIndex]->CopyData(0, cullCB);

	// 4. Dispatch culling CS (one thread per grass instance)
	mCommandList->SetComputeRootSignature(mGrassCullRootSignature.Get());
	mCommandList->SetPipelineState(mPSOs["grass_cull_cs"].Get());
	// mCommandList->SetComputeRootConstantBufferView(0, gs->mGrassCullCB->Resource()->GetGPUVirtualAddress());
	mCommandList->SetComputeRootConstantBufferView(0, gs->mGrassCullCB[mCurrFrameResourceIndex]->Resource()->GetGPUVirtualAddress());
	mCommandList->SetComputeRootShaderResourceView(1, gs->mGrassFullInstanceBuffer.mResource->GetGPUVirtualAddress());
	mCommandList->SetComputeRootUnorderedAccessView(2, gs->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
	mCommandList->SetComputeRootUnorderedAccessView(3, gs->mGrassCounterBuffer.mResource->GetGPUVirtualAddress());
	UINT groups = (gs->mTotalGrassInstances + 63) / 64;
	mCommandList->Dispatch(groups, 1, 1);

	/*
	// 5. UAV barriers: ensure CS writes are visible before reads
	// this stalls subsequent commands until writes to UAVs are complete
	CD3DX12_RESOURCE_BARRIER uavBarriers[] = {
		CD3DX12_RESOURCE_BARRIER::UAV(gs->mGrassVisibleBuffer.mResource.Get()),
		CD3DX12_RESOURCE_BARRIER::UAV(gs->mGrassCounterBuffer.mResource.Get()),
	};
	mCommandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
	*/
	mBarrierManager.InsertUAVBarrier(gs->mGrassVisibleBuffer);
	mBarrierManager.InsertUAVBarrier(gs->mGrassCounterBuffer);
	// mBarrierManager.FlushBarriers(mCommandList.Get());

	mBarrierManager.TransitionState(gs->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(gs->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	mBarrierManager.TransitionState(gs->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	mBarrierManager.FlushBarriers(mCommandList.Get());
	// 6. Transition: visible → SRV; counter → COPY_SOURCE; indirect args → COPY_DEST
	/*
	CD3DX12_RESOURCE_BARRIER postCS[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassVisibleBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassIndirectArgsBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_COPY_DEST),
	};
	mCommandList->ResourceBarrier(_countof(postCS), postCS);
	*/

	// 7. Copy visible count into indirect args InstanceCount field
	mCommandList->CopyBufferRegion(
		gs->mGrassIndirectArgsBuffer.mResource.Get(),
		offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
		gs->mGrassCounterBuffer.mResource.Get(), 0,
		sizeof(UINT));

	/*
	// 8. Transition indirect args → INDIRECT_ARGUMENT; counter → COPY_DEST for next frame
	CD3DX12_RESOURCE_BARRIER postCopy[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassIndirectArgsBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
		CD3DX12_RESOURCE_BARRIER::Transition(gs->mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST),
	};
	mCommandList->ResourceBarrier(_countof(postCopy), postCopy);
	*/
	mBarrierManager.TransitionState(gs->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	mBarrierManager.TransitionState(gs->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	mBarrierManager.FlushBarriers(mCommandList.Get());

	PIXEndEvent(mCommandList.Get());
}

void MCEngine::SwitchScene(const std::string& name)
{
	if (mActiveScene && mActiveScene->name == name) return;
	
	FlushCommandQueue();

	// --- Reset special render item pointers ---
	mModelRitem = mReflectedModelRitem = mShadowedModelRitem = mTessellatedRitem = nullptr;

	// --- Move engine data back into the current scene before leaving ---
	if (mActiveScene) {
		mActiveScene->Deactivate(*this);
		mActiveScene->allRitems = std::move(mAllRitems);
		for (int i = 0; i < (int)RenderLayer::Count; i++)
			mActiveScene->layers[i] = std::move(mRitemLayer[i]);
		mActiveScene->geometries        = std::move(mGeometries);
		mActiveScene->materials         = std::move(mMaterials);
		mActiveScene->materialIndexTracker = std::move(mMaterialsIndexTracker);
	} else {
		mAllRitems.clear();
		for (auto& layer : mRitemLayer) layer.clear();
		mGeometries.clear();
		mMaterials.clear();
		mMaterialsIndexTracker.clear();
	}

	// --- Switch to new scene ---
	mActiveScene = mScenes.at(name).get();
	if (!mActiveScene->loaded) {
		ThrowIfFailed(mDirectCmdListAlloc->Reset());
		ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
		mActiveScene->Load(*this);
		mActiveScene->loaded = true;
		ThrowIfFailed(mCommandList->Close());
		ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
		mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
		FlushCommandQueue();
	}

	// --- Move data into engine hot paths ---
	mGeometries        = std::move(mActiveScene->geometries);
	mMaterials         = std::move(mActiveScene->materials);
	mMaterialsIndexTracker = std::move(mActiveScene->materialIndexTracker);
	mAllRitems         = std::move(mActiveScene->allRitems);
	for (int i = 0; i < (int)RenderLayer::Count; i++)
		mRitemLayer[i] = std::move(mActiveScene->layers[i]);

	// --- Reassign CB indices ---
	for (int i = 0; i < (int)mAllRitems.size(); i++)
		mAllRitems[i]->ObjCBIndex = i;
	int mi = 0;
	for (auto& [k, v] : mMaterials) v->MatCBIndex = mi++;
	// Sync instance MaterialIndex after MatCBIndex reassignment (unordered_map order != build order)
	/*
	for (int layer : { (int)RenderLayer::OpaqueInstanced, (int)RenderLayer::GrassInstanced })
		for (auto* ri : mRitemLayer[layer])
			for (auto& inst : ri->Instances)
				inst.MaterialIndex = ri->Mat->MatCBIndex;
	*/
	total_objects = (int)mAllRitems.size();

	// --- Rebuild per-frame resources for new counts ---
	mFrameResources.clear();
	mCurrFrameResourceIndex = 0;
	BuildFrameResources();
	mCurrFrameResource = mFrameResources[0].get(); // must be set before returning to IMGUI_UPDATE

	// --- Rebuild descriptor heap and CBV descriptors ---
	// FixupMaterialDiffuseIndices();

	// --- Mark everything dirty ---
	DirtyAllRenderItems();
	for (auto& [k, v] : mMaterials)
		v->NumFramesDirty = gNumFrameResources;
	mDepthDebugFramesDirty = gNumFrameResources;

	// --- Scene-specific camera / lighting / fog ---
	mActiveScene->Activate(*this);
	PreInitObjectCBs();

	std::cout << "SwitchScene -> '" << name << "': " << total_objects << " render items\n";
	OutputDebugStringA(("SwitchScene -> '" + name + "'\n").c_str());
}

void MCEngine::BuildFrameResources()
{
	UINT objCount = max(1u, (UINT)mAllRitems.size());
	UINT matCount = max(1u, (UINT)mMaterials.size());
	UINT totalInstances = 0;
	for (auto* ri : mRitemLayer[(int)RenderLayer::OpaqueInstanced])
		totalInstances += (UINT)ri->Instances.size();
	UINT totalGrassInstances = 0;
	for (auto* ri : mRitemLayer[(int)RenderLayer::GrassInstanced])
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
	float MaxBlurRadius = 15;
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

	CSB_default csbs;
	csbs.InputIndex = (INT)mBlurred0.SRVs[0].offset;
	csbs.OutputIndex = (INT)mSobelOutput.UAVs[0].offset;
	csbs.Width = (INT)mViewportNoAlpha.SRVs[0].offset;
	csbs.Height = (INT)mDepthDebugColor.SRVs[0].offset;
	int i = 0;
	for (auto& fr : mFrameResources) {
		fr->SobelCB.get()->CopyData(0, csbs);
		std::wstring name =	std::format(L"mSobelCB_{}", i);
		fr->SobelCB.get()->Resource()->SetName(name.c_str());
	}
	
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
	
	for (auto& ri : mAllRitems)
		ri->NumFramesDirty = gNumFrameResources;
	// static int dirtycounter = 0;
	// std::cout << "dirtied all frame resources to value 3, count : " << dirtycounter++ << std::endl;
}

void MCEngine::PreInitObjectCBs() {
	for (auto& fr : mFrameResources) {
		auto objCB = fr->ObjectCB.get();
		for (auto& e : mAllRitems) {
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

	GrassCullDispatch();
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
			mCommandList->Dispatch(blurGroupsX, mSceneViewHeight, 1);

			// VERTICAL BLUR
			mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mBarrierManager.TransitionState(mBlurred1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			mBarrierManager.FlushBarriers(mCommandList.Get());
			mCommandList->SetPipelineState(mPSOs["vertBlur"].Get());
			mCommandList->SetComputeRootConstantBufferView(0, blurCBaddresss1);
			mCommandList->Dispatch(mSceneViewWidth, blurGroupsY, 1);
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

		UpdateSobelState();
		mBarrierManager.FlushBarriers(mCommandList.Get());

		mCommandList->SetPipelineState(mPSOs["sobel"].Get());
		D3D12_GPU_VIRTUAL_ADDRESS sobelCBaddress = mCurrFrameResource->SobelCB->Resource()->GetGPUVirtualAddress();
		mCommandList->SetComputeRootConstantBufferView(0, sobelCBaddress);
		mCommandList->Dispatch(sobelGroupsX, sobelGroupsY, 1);
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
	mCommandList->ResourceBarrier(1, &transition_BB); // TODO(barrier-tracker): Back Buffers are not wrapped by MCTexture, and I don't see a reason to do so. When splitting into runtime / editor, may be (phase 3)
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

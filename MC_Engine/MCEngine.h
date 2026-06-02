#pragma once
#define SHDAER_MAJOR 6
#include "Assets/Shaders/SharedTypes.h"
#include "d3dApp.h"
#include "FrameResource.h"
#include "Camera.h"
#include "RenderItem.h"
#include "MC_Types.h"
#include "BarrierManager.h" // not a singleton - saved as a member variable of MCEngine
#include "SceneSerialization.h"
#include "MCSceneManager.h"
#include "CBFreeList.h"
#include "JsonMigrator.h"
#include "MCMaterialManager.h"
#include "MCTextureManager.h"
#include "MCMeshSourceManager.h"
#include "MCGrassCullingModule.h"
#include "GpuTimer.h"
#include "CpuTimer.h"
#include "ConstExpressionValues.h" // gNumFrameResources
#include "FrameGraph/FrameGraph.h"
#include "FrameGraph/RenderPass.h"
#include <set>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#define NOMINMAX // required to use `std::min` when `Windows.h` is also included
using namespace DirectX;

// extern const int gNumFrameResources;

struct CSB_blurValues // blur related values changed via imgui — CPU-only, not GPU-facing
{
	bool enabled = false;
	float sigma = 0.5f;
	int blurIter = 5;
};

// RenderItem and RenderLayer are now in RenderItem.h
class MCScene;

class MCEngine : public D3DApp
{
public:
	MCEngine(HINSTANCE hInstance);
	MCEngine(const MCEngine& rhs) = delete;
	MCEngine& operator=(const MCEngine& rhs) = delete;
	~MCEngine();

	virtual bool Initialize()override;
	virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)override;

	void ReloadShaders();

	// --- Light management (will be moved to scene-specific on a later date) ---
	void RegisterLights();

	// --- MCScene management (public so ImGui and scenes can call SwitchScene) ---
	const JsonMigrator& Migrator() const { return mMigrator; }
	MCMaterialManager& Materials() { return mMaterialManager; }
	const MCMaterialManager& Materials() const { return mMaterialManager; }
	MCTextureManager& Textures() { return mTextureManager; }
	const MCTextureManager& Textures() const { return mTextureManager; }
	MCMeshSourceManager& Meshes() { return mMeshSourceManager; }
	const MCMeshSourceManager& Meshes() const { return mMeshSourceManager; }
	MCSceneManager& Scenes() { return mSceneManager; }
	const MCSceneManager& Scenes() const { return mSceneManager; }

	// --- FrameGraph (non-lambda RenderPass inheritance)
	FrameGraph& GetFrameGraph() { return *mFrameGraph; }
	const FrameGraph& GetFrameGraph() const { return *mFrameGraph; }
	MCTextureResource& SceneColor() { return mSceneColor; }
	MCTextureResource& SceneDepth() { return mSceneDepth; }
	bool IsWireFrame() const { return mIsWireframe; }
	bool IsMsaa() const { return mScene4xMsaaState; }
	void DrawLayer(ID3D12GraphicsCommandList* cmdList,RenderLayer layer, std::string pixEventName = "");
	void DrawInstancedLayer(ID3D12GraphicsCommandList* cmdList,RenderLayer layer, bool useGrass = false, std::string pixEventName = "");
	friend class DebugLinePass; // too many private variables needed. just set as friend.

	// D9 Step 5a — bracket the cmdlist around MCSceneManager::Reload so module
	// OnLoad calls (e.g. MCGrassCullingModule's GPU instance buffer upload) can
	// record commands. Safe to call from ImGui handlers (cmdlist closed at that
	// phase). Always returns the cmdlist to the closed state Draw expects, even
	// on exception.
	void ReloadActiveSceneNow(bool save = false);
	void RequestReload() { reloadScene = true; }
	void MarkSceneDirty() { mSceneDirty = true; }	// every module's OnImGui slider that mutates scene-serialized state must call this - Generalize for any future module's OnImGui.

	// --- Engine-side helpers exposed for MCSceneManager (D8 Step 1c) ---
	// Phase-2 ticket: replace the SceneAccess proxy + these helpers with
	// typed accessors. Until then, these are the manager's bridge into engine
	// internals it needs at scene-switch time.
	using D3DApp::FlushCommandQueue;            // protected on D3DApp; re-exposed for manager.
	void RebuildFrameResources();               // clears mFrameResources, rebuilds, sets mCurrFrameResource.
	void DirtyAllRenderItems();
	void PreInitObjectCBs();

	// --- Accessors used by MCSceneModule() inheriotrs ---
	ID3D12Device*              GetDevice()  const { return md3dDevice.Get(); }
	ID3D12GraphicsCommandList* GetCmdList() const { return mCommandList.Get(); }
	FrameResource*				GetCurrentFrameResource() const { return  mCurrFrameResource; }
	bool						IsCameraFrozen() const { return mFreezeCamera; }
	const PerPassCB&			GetMainPassCB() const { return	mMainPassCB; }
	const BoundingFrustum&		GetFrozenWorldFrustum() const { return mFrozenWorldFrustum; }
	const XMFLOAT4X4&			GetFrozenViewProjT() const { return mFrozenViewProjT; }
	BarrierManager&				GetBarrierManager() { return mBarrierManager; }
	const BarrierManager&		GetBarrierManager() const { return mBarrierManager; }
	int						    GetCurrFrameResourceIndex()  const { return mCurrFrameResourceIndex; }
	ID3D12RootSignature*    GetGrassCullRootSignature()  const { return mGrassCullRootSignature.Get(); }
	ID3D12PipelineState*    GetPSO(const std::string& name) const {
		auto it = mPSOs.find(name);
		return it == mPSOs.end() ? nullptr : it->second.Get();
	}

	// --- Special render item setters (called by concrete scenes during Load) ---
	// these should eventually be removed.
	void SetModelRitem(RenderItem* ri)          { mModelRitem = ri; }
	void SetReflectedModelRitem(RenderItem* ri) { mReflectedModelRitem = ri; }
	void SetShadowedModelRitem(RenderItem* ri)  { mShadowedModelRitem = ri; }	// nothing calls it currently
	void SetTessellatedRitem(RenderItem* ri)    { mTessellatedRitem = ri; }

	XMFLOAT4& GetFogColor() { return mFogColor; }

	Camera& GetMainCamera() { return *mMainCamera; }
	std::vector<float> GetScreenSize() { return { mSceneViewWidth, mSceneViewHeight }; }
	XMFLOAT2& GetSceneMousePos() { return mSceneMousePos; }

	// --- MCScene load from json ---
	// Returns a non-owning pointer to the loaded item; ownership lives in scene.allRitems
	// after the AddRenderItem call inside.
	RenderItem* LoadRenderItemsFromJson(const nlohmann::json& j, MCScene& scene);
	
protected:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;	// overridden to set dsvHeapDesc.NumDescriptors = 2 instead of 1.

private:
	virtual void OnResize()override;
	virtual void OnSceneResize();
	virtual void Update(GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	// custom draw Passes
	void InitializeRenderPass();

	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;
	void OnMouseScroll(WPARAM btnState, float delta);

	void OnKeyboardInput(const GameTimer& gt);
	void UpdateCamera(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateInstanceData(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateMaterialCBs(const GameTimer& gt);
	void UpdateDepthDebugCB(const GameTimer& gt);

	void BuildDescriptorHeaps();	// create descriptor heap for cbv
	void BuildConstantBufferViews(); // ch7 - replaced BuildConstantBuffer with BuildConstantBufferViews
	void BuildRootSignature();
	virtual void BuildShadersAndInputLayout();
	void BuildPSOs();
	void RegisterPSOVariants(const std::string& name,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base,bool withMSAA = true, bool withWireframe = true);

	// ch7 stuff - shapes
	void BuildFrameResources();
	void BuildComputeShaderConstantBufferResources(); // uses upload buffer, but aren't frame resources
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, std::string pixEventName = "null");
	void DrawInstanceRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool useGrass = false, std::string pixEventName = "null");

	// IMGUI
	void IMGUI_INIT();
	void IMGUI_UPDATE();
	void IMGUI_RENDERDRAWDATA();
	void IMGUI_UPDATE_DESCHEAP_VIEWER();
	void IMGUI_OUTLINER(); // MVP version of `Hierarchy` panel of Unity (no parent-child relationship. just lists all render items in scene)
	void IMGUI_INSPECTOR();
	void IMGUI_MENUBAR();
	void IMGUI_TEST();
	void IMGUI_FRAMEPROFILE();
	void IMGUI_FRAMEGRAPH();
	RenderItem* CurrentSelectedItem() const;
	LRESULT IMGUI_WNDMSGHANDLER(HWND& hwnd, UINT& msg, WPARAM& wParam, LPARAM& lParam);
	void IMGUI_SHUTDOWN();

	// editor (non-runtime)
	// these are greedy bad designs, coded on demand. must be refactored completely.
	void RefreshWindowTitle();
	bool FileMenuSave();
	bool FileMenuSaveAs();
	void FileMenuOpenScene();
	void FileMenuCreateScene();
	void FileMenuCreateMaterial();

	// rendering to texture
	void BuildSceneRenderTarget();
	void BuildSceneRenderTargetDescriptors();

	void BuildCamera();

	// Debug visualization
	void BuildDebugLineResources();
	UINT BuildDebugLineGeometry();

	float sceneAspectRatio();
	std::vector<float> CalcGaussWeights(float sigma);

private:
	BarrierManager mBarrierManager;
	// --- FrameGraph
	std::unique_ptr<FrameGraph> mFrameGraph;
	std::vector<std::unique_ptr<RenderPass>> mPasses;

	//! START- for rendering to Texture instead of directly to back buffer
	MCTextureResource mSceneColor, mSceneDepth, mDepthDebugColor;
	MCTextureResource mViewportNoAlpha; // , mViewportColor
	MCTextureResource mBlurred0; // mBlurred1;
	MCTextureResource mSobelOutput;

	DXGI_FORMAT mSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Upload buffers for the post-process compute shaders (resources proper
	// live on the MCTextureResource members above; descriptors on DescHeapManager).
	// All three are re-uploaded on resize because they cache bindless offsets
	// of post-process MCTextureResources, which shift when those textures are re-created.
	std::unique_ptr<UploadBuffer<CSB_default>> mForceAlphaUploadBuffer;
	std::unique_ptr<UploadBuffer<CSB_blur>> mBlurUploadBuffer;
	std::unique_ptr<UploadBuffer<CSB_default>> mSobelUploadBuffer;

	D3D12_VIEWPORT mSceneViewport = {};
	D3D12_RECT mSceneScissorRect = {};

	// Reserved leading range of mCbvSrvUavHeap exclusively for ImGui's SRV allocator
	// (see MC_imgui.cpp file-static free list). DescHeapManager's tiers start at
	// offset kCsuReservedHead, so these slots are off-limits to engine resources.
	// Was 2 (legacy single-descriptor mode + 1 reserved). Bumped to 16 to give the
	// dynamic font atlas slack during FontScaleMain rebuilds — the rebuild
	// allocates a NEW atlas slot before freeing the old, so capacity must
	// exceed the live texture count by at least 1.
	static constexpr int kCsuReservedHead = 16;
	static constexpr int kCsuTierDynamicCap = 64;     // FrameGraph transient SRV/UAVs: ~(live transients)*(SRV+UAV)*(NumFrameResources+1); 64 leaves migration headroom
	static constexpr int kCsuTierStaticHeadroom = 64; // covers asset textures + post-process SRVs/UAVs + headroom
	int mCsuTierStaticCap = 0;                        // = kCsuTierStaticHeadroom, set in BuildDescriptorHeaps

	UINT mImGuiSceneSrvIndex = 1; // 0 font, 1 scene Texture, 2 depth texture, 3 depth debug texture
	enum class ViewportResMode { Free, Ratio16x9, Ratio1x1, HD, FullHD, K4 };
	ViewportResMode mViewportResMode = ViewportResMode::Free;
	float mViewportZoom = 0.0f; // 0 = FIT, >0 = pixel-accurate factor (1.0 = 100%)

	float mSceneViewWidth = 800.0f;
	float mSceneViewHeight = 600.0f;
	bool mSceneSizeDirty = false; // just mSceneDirty
	// int mSceneSizeDirty = gNumFrameResources;

	bool mSceneImageSelected = false;
	bool mSceneImageHovered = false;
	XMFLOAT2 mSceneMousePos = { 0.0f, 0.0f };
	bool mScene4xMsaaState = false;
	//! END- for rendering to Texture instead of directly to back buffer

	CBFreeList mObjCBFreeList;
	uint32_t mObjCBCapacity; // set at BuildFrameResources. caches maxObjCount
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	std::vector<ComPtr<ID3D12RootSignature>> mRootSignatures;
	ComPtr<ID3D12RootSignature> mComputeRootSignature;
	ComPtr<ID3D12RootSignature> mGrassCullRootSignature;
	ComPtr<ID3D12CommandSignature> mGrassCommandSignature;
	ComPtr<ID3D12DescriptorHeap> mCbvSrvUavHeap = nullptr;		// unified descriptor heap. There should be no switching of descriptor heap within a draw pass
	ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;			// descriptors (aka views) are accessed via Descriptor Heap
	ComPtr<ID3D12DescriptorHeap> mSrvHeap = nullptr;			// for render target texture on IMGUI scene window
	// ComPtr<ID3D12DescriptorHeap> mDEPTH_SrvHeap = nullptr;	// instead of creating a separate descriptor heap for scene depth, I setup pre-existing descriptor heap to have 2 descriptors, not 1

	UINT mTextureSrvCount = 0;									// number of texture Srvs


	MCMaterialManager mMaterialManager;
	MCTextureManager mTextureManager;
	MCMeshSourceManager mMeshSourceManager;
	MCSceneManager mSceneManager;
	GpuTimer mGpuTimer;
	CpuTimer mCpuTimer;



	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> mInputLayout;

	RenderItem* mModelRitem = nullptr;
	RenderItem* mReflectedModelRitem = nullptr;
	RenderItem* mShadowedModelRitem = nullptr; // unused currently
	RenderItem* mTessellatedRitem = nullptr;

	// D8 Step 1d: render items live on MCSceneManager::GetActive()->allRitems / ->layers[i].
	// The engine no longer mirrors them. Read sites cache `auto* scene = mSceneManager.GetActive();`.

	PerPassCB mMainPassCB; // 0 for main, 1 for reflected
	DebugDepthConstants mDebugDepthCB;
	JsonMigrator mMigrator;
	bool mIsWireframe = false;

	std::vector<std::unique_ptr<Camera>> mAllCameras;
	Camera* mMainCamera = nullptr;
	std::vector<Camera*> mShadowmapCameras;
	bool mCameraDirty = true;
	bool mFrustrumDirty = false;
	int mNumCameraDirty = gNumFrameResources;
	POINT mLastMousePos;

	int total_objects = 0;	// updated by MCSceneManager::Switch via SceneAccess::TotalObjects (== active scene's allRitems.size())
	bool isOrtho = 0;
	float mOrthoT = 0.0f;

	XMFLOAT4 mAmbientLight = { .4f, .4f, .4f, 1.0f };
	DirectX::XMFLOAT4 mFogColor = { 1.0f,1.0f,1.0f,1.0f };
	float mFogStart = 30.0f;
	float mFogRange = 1000.0f;
	int mDepthDebugFramesDirty = gNumFrameResources;
	float mDepthDebugMax = 100.0f;

	bool blurDirty = true;
	CSB_blurValues blurValues;

	bool mIsSobel = false;
	enum SobelType : int {
		Default,
		Gaussain,
		Depth,
		Count
	} mSobelType = SobelType::Gaussain;

	bool enableBoundsCheck = true;
	int cameraDirtyCount = 0;

	// ---- Debug visualization ----
	bool mShowBoundingBoxes = false;
	bool mFreezeCamera      = false;  // freeze frustum for culling; camera still moves
	bool mShowFrustum       = false;  // draw frustum wireframe (frozen when mFreezeCamera)
	DirectX::BoundingFrustum mFrozenWorldFrustum;
	DirectX::XMFLOAT4X4     mFrozenViewProjT = {};  // Transpose(VP) snapshot for GPU grass culling

	static const UINT kMaxDebugLineVerts = 8192;
	std::vector<ComPtr<ID3D12Resource>> mDebugLineVB;
	std::vector<DirectX::XMFLOAT3*>    mDebugLineVBMapped;
	UINT mDebugBBoxVertStart  = 0;
	UINT mDebugBBoxVertCount  = 0;
	UINT mDebugFrustVertStart = 0;
	UINT mDebugFrustVertCount = 0;

	ComPtr<ID3D12RootSignature> mDebugLineRootSig;

	bool reloadScene = false;
	bool mSceneDirty = false;
	// (mCurrentScenePath removed — MCSceneManager::mScenePaths is the single
	// source of truth for the active scene's save path. Retrieve via
	// mSceneManager.GetScenePath(mSceneManager.GetActive()->name) at use sites.)


public:
	// --- MCScene management proxy (D8 D-2) ---
	// MCSceneManager calls sceneAccess() at scene-switch time. The struct's field set
	// bounds the manager's mutation surface to engine-internal frame state (CB free list,
	// frame-resource vector, total-objects counter); PSOs / heaps / signatures stay out
	// of reach. After D8 Step 1d, render items live on the scene, so AllRitems/RitemLayers
	// are no longer in the proxy. Phase-2 ticket: replace with typed accessors.
	struct SceneAccess {
		CBFreeList& ObjCBFreeList;
		std::vector<std::unique_ptr<FrameResource>>& FrameResources;
		int& TotalObjects;
	};
	SceneAccess sceneAccess() { return { mObjCBFreeList, mFrameResources, total_objects }; }
	friend struct SceneAccess;

	// ---- imgui W3 D10 -  Outliner state.
	int mSelectedItemIndex = -1; // -1 = nothing selected. Index into mSceneManager.GetActive()->allRitems

	// ---- imgui W4A D1 - Autosave-on-switch toggle.
	// When true, MCEngine::Update saves the active scene to its registered path
	// before consuming a pending switch. Default true to preserve the
	// "edits flush to disk on combo-switch" UX you currently rely on.
	bool mAutosaveOnSwitch = true;

private:

	// VSync
	bool enableVsync = false;

	float mInstancingCullingTime = 0.0f;

	std::vector<LightData> mLights;
	enum class LightType : int {Directional, Point, Spot, Count};

	// --- ImGui UI set values
	bool mShowDescHeapViewer = false;
	bool mShowFrameGraphViewer = false;
	bool mShowTestsWindow = false;

	// --- _requestRoundTripTest stays on engine; ImGui button at MC_imgui.cpp:502
	// sets the flag, MCEngine::Update routes the call through mSceneManager (D8 Step 2).
	bool _requestRoundTripTest = false;
};
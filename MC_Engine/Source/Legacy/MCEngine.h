#pragma once
// =============================================================================
// MCEngine.h — reorganized layout. Drop-in replacement for the old header:
// same class, same declarations, same initializers; only ordering and section
// banners changed. To adopt: rename old MCEngine.h away, rename this file to
// MCEngine.h, rebuild. Nothing in any .cpp needs to change.
//
// Layout rule: ONE public / ONE protected / ONE private block for functions,
// then ONE private block for all data. New members go into their banner
// section; a new section only for a new subsystem.
//
// Known smells kept for drop-in compatibility (fix as separate changes):
//  - MCGrassCullingModule.h include: the class is never named here; cpp files
//    using GetModule<MCGrassCullingModule> should include it themselves.
//
// DATA-ORDER CONSTRAINT (do not shuffle casually): member declaration order is
// construction order, reverse is destruction order. The asset managers must
// stay declared BEFORE mSceneManager so scenes destroy before the managers
// their Clear() path unregisters from.
// =============================================================================
#include "SharedTypes.h"
#include "Legacy/d3dApp.h"
#include "Renderer/D3D12/FrameResource.h"
#include "Legacy/Camera.h"
#include "Legacy/RenderItem.h"
#include "Renderer/D3D12/MC_Types.h"
#include "Renderer/D3D12/BarrierManager.h" // not a singleton - saved as a member variable of MCEngine
#include "Legacy/SceneSerialization.h"
#include "Legacy/MCSceneManager.h"
#include "Core/CBFreeList.h"
#include "Core/JsonMigrator.h"
#include "Legacy/MCMaterialManager.h"
#include "Legacy/MCTextureManager.h"
#include "Legacy/MCMeshSourceManager.h"
#include "Legacy/MCGrassCullingModule.h"
#include "Renderer/D3D12/GpuTimer.h"
#include "Core/CpuTimer.h"
#include "Renderer/D3D12/ConstExpressionValues.h" // gNumFrameResources
#include "Renderer/D3D12/FrameGraph/FrameGraph.h"
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include "Platform/PlatformSystem.h"
#include <set>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <array>
#include <chrono>

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
	// ======================= Lifecycle =======================
	MCEngine(void* hInstance, int iconId);
	MCEngine(const MCEngine& rhs) = delete;
	MCEngine& operator=(const MCEngine& rhs) = delete;
	~MCEngine();

	bool Initialize();
	int Run();
	// ======================= Subsystem accessors =======================
	const JsonMigrator& Migrator() const { return mMigrator; }
	MCMaterialManager& Materials() { return mMaterialManager; }
	const MCMaterialManager& Materials() const { return mMaterialManager; }
	MCTextureManager& Textures() { return mTextureManager; }
	const MCTextureManager& Textures() const { return mTextureManager; }
	MCMeshSourceManager& Meshes() { return mMeshSourceManager; }
	const MCMeshSourceManager& Meshes() const { return mMeshSourceManager; }
	MCSceneManager& Scenes() { return mSceneManager; }
	const MCSceneManager& Scenes() const { return mSceneManager; }
	BarrierManager& GetBarrierManager() { return mBarrierManager; }
	const BarrierManager& GetBarrierManager() const { return mBarrierManager; }

	// ======================= Scene transitions =======================
	// Command-list bracket for scene transitions (switch / reload). Between the
	// two calls the cmdlist is open and GPU-idle, so scene/module load hooks can
	// record uploads and barriers. End always closes + executes + flushes,
	// returning the cmdlist to the closed state Draw expects. Do not nest, and
	// do not call while the cmdlist is already open (e.g. inside Initialize's
	// setup bracket or inside Draw).
	void BeginSceneTransition();
	void EndSceneTransition();
	// D9 Step 5a — bracket the cmdlist around MCSceneManager::Reload so module
	// OnLoad calls (e.g. MCGrassCullingModule's GPU instance buffer upload) can
	// record commands. Safe to call from ImGui handlers (cmdlist closed at that
	// phase). Always returns the cmdlist to the closed state Draw expects, even
	// on exception.
	void ReloadActiveSceneNow(bool save = false);
	// Immediate bracketed switch — for call sites outside Update's pending-switch
	// path (ImGui handlers, tests). Rethrows Switch failures after closing the bracket.
	void SwitchSceneNow(const std::string& name);
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

	// --- MCScene load from json ---
	// Returns a non-owning pointer to the loaded item; ownership lives in scene.allRitems
	// after insertion.
	RenderItem* LoadRenderItemsFromJson(const nlohmann::json& j, MCScene& scene);

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

	// ======================= Frame/GPU accessors (modules + passes) =======================
	ID3D12Device*              GetDevice()  const { return md3dDevice.Get(); }
	ID3D12GraphicsCommandList* GetCmdList() const { return mCommandList.Get(); }
	const PerPassCB&			GetMainPassCB() const { return	mCurrFrameResource->mCachedMainPassCB; }
	bool						IsCameraFrozen() const { return mFreezeCamera; }
	const DirectX::XMFLOAT4X4&			GetFrozenViewProjT() const { return mFrozenViewProjT; }
	ID3D12PipelineState*    GetPSO(const std::string& name) const {
		auto it = mPSOs.find(name);
		return it == mPSOs.end() ? nullptr : it->second.Get();
	}
	MCTextureResource& ForwardPassWriteTarget() { return mScene4xMsaaState ? mSceneColorMSAA : mSceneColor; }
	MCTextureResource& SceneDepth() { return mSceneDepth; }
	bool IsWireFrame() const { return mIsWireframe; }
	bool IsMsaa() const { return mScene4xMsaaState; }
	void DrawLayer(ID3D12GraphicsCommandList* cmdList,RenderLayer layer, bool ignoreFrustum = false, std::string pixEventName = "");
	void DrawInstancedLayer(ID3D12GraphicsCommandList* cmdList,RenderLayer layer, bool useGrass = false, bool ignoreFrustum = false, std::string pixEventName = "");

	// ======================= Camera / view / environment =======================
	Camera& GetMainCamera() { return *mMainCamera; }
	std::vector<float> GetScreenSize() { return { mSceneViewWidth, mSceneViewHeight }; }
	DirectX::XMFLOAT2& GetSceneMousePos() { return mSceneMousePos; }
	DirectX::XMFLOAT4& GetFogColor() { return mFogColor; }

	// ======================= Misc engine ops =======================
	// --- Light management (will be moved to scene-specific on a later date) ---
	void RegisterLights();

	// ======================= Editor state (public data — deliberate, editor-only) =======================
	// ---- imgui W3 D10 -  Outliner state.
	int mSelectedItemIndex = -1; // -1 = nothing selected. Index into mSceneManager.GetActive()->allRitems

	// ---- imgui W4A D1 - Autosave-on-switch toggle.
	// When true, MCEngine::Update saves the active scene to its registered path
	// before consuming a pending switch. Default true to preserve the
	// "edits flush to disk on combo-switch" UX you currently rely on.
	bool mAutosaveOnSwitch = true;

private:
	// ======================= Frame loop =======================
	// @brief One frame: the seven phases below, in order. Called by D3DApp::Run.
	void RunFrame(float dt)override;
	// @brief takes resize request from platform, updates mClientWidth and Height, then calls OnResize();
	void ApplyWindowResize();
	// @brief Safe point: every GPU-affecting request runs here, with the command list closed
	// @brief [scene switch/reload, shader reload, viewport+MSAA rebuild, debug harness]
	void ApplyPendingRequests();
	// @brief Camera navigation and picking
	void UpdateInput(float dt);
	void UpdateInput(const MC::Input& input, float dt);
	// @brief Advance the frame ring, free old descriptors, wait for the slot's fence, read back GPU timers
	void BeginGpuFrame();
	// @brief ImGui: edits CPU data, writes *requested* settings, queues requests; never flushes
	void UpdateEditor();
	// @brief Ortho blend
	void UpdateLogic(float dt);
	// @brief Culling and constant-buffer uploads
	void PrepareRenderData();
	// @brief FrameGraph, UI composite, present, fence signal
	void RenderFrame();

	bool mShaderReloadRequest = false;	// set by the Reload Shaders button; consumed by ApplyPendingRequests
	void ReloadShaders();

	virtual void OnResize()override;
	virtual void OnSceneResize();

	void UpdateCamera();
	void UpdateObjectCBs();
	void UpdateInstanceData();
	void UpdateMainPassCB(const GameTimer& gt);
	void UpdateMaterialCBs();
	void UpdateDepthDebugCB();

	// ======================= Build (init-time) =======================
	void CreateRtvAndDsvDescriptorHeaps();	// the swap chain's RTVs and the window DSV at their heads, then the engine's
	void BuildDescriptorHeaps();	// create descriptor heap for cbv
	void BuildRootSignature();
	virtual void BuildShadersAndInputLayout();
	void BuildPSOs();
	void RegisterPSOVariants(const std::string& name,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base,bool withMSAA = true, bool withWireframe = true); // TODO : need to update this to create ShadowPass variant
	void BuildFrameResources();
	void BuildComputeShaderConstantBufferResources(); // uses upload buffer, but aren't frame resources
	// rendering to texture
	void BuildSceneRenderTarget();
	void BuildSceneRenderTargetDescriptors();
	void BuildCamera();
	// Debug visualization
	void BuildDebugLineResources();
	UINT BuildDebugLineGeometry();
	// custom draw Passes
	void InitializeRenderPass();

	// ======================= Draw helpers =======================
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool ignoreFrustum = false, std::string pixEventName = "null");
	void DrawInstanceRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool useGrass = false, bool ignoreFrustum = false, std::string pixEventName = "null");

	// ======================= ImGui =======================
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

	// ======================= Editor / file menu (non-runtime) =======================
	// these are greedy bad designs, coded on demand. must be refactored completely.
	void RefreshWindowTitle();
	bool FileMenuSave();
	bool FileMenuSaveAs();
	void FileMenuOpenScene(const std::string& path = {});	// empty: pick the file in the Open dialog
	void FileMenuCreateScene();
	void FileMenuCreateMaterial();

	// ======================= Small utils =======================
	float sceneAspectRatio();
	std::vector<float> CalcGaussWeights(float sigma);

#if defined(_DEBUG)
	// ======================= Debug verification harness (bridge B1, MC_CaptureSweep.cpp) =======================
	// Command line: --capture-sweep <dir> --tier quick|full [--poison <pass|*>],
	// --run-tests <file>, --verify-log <file>, --shadow-casters <n>,
	// --fill-materials <extra>, --open-scene <path>, --viewport-modes <list>. Inert when no flag is given;
	// the Tests window's "Capture viewport" button shares the capture path.
	struct DebugHarness;
	void InitDebugHarness();
	void TickDebugHarness();
	bool IsHarnessDriving() const;	// a sweep or a test run owns the engine
	int HarnessShadowCasters() const;	// --shadow-casters <n>, 1 without the flag
	void RequestViewportCapture(const std::wstring& bmpPath = L"");
	void RecordViewportCapture();
	bool RejectPoisonedFrame();	// true: the frame is closed and must not be submitted (--poison)
	bool CaptureViewportToBmp();
	void ReleaseDebugHarnessDevice();
	void WriteDebugHarnessLiveObjects();
#endif

private:
	// =============================== DATA ===============================

	// --- Core Systems ---
	MC::PlatformSystem mPlatform;

	// --- Core subsystems ---
	// Construction-order-sensitive: managers BEFORE mSceneManager (scenes must
	// destroy before the managers their Clear() path unregisters from).
	BarrierManager mBarrierManager;
	MCMaterialManager mMaterialManager;
	MCTextureManager mTextureManager;
	MCMeshSourceManager mMeshSourceManager;
	MCSceneManager mSceneManager;
	JsonMigrator mMigrator;
	GpuTimer mGpuTimer;
	CpuTimer mCpuTimer;

	// --- Frame resources / per-object CBs ---
	CBFreeList mObjCBFreeList;
	uint32_t mObjCBCapacity; // set at BuildFrameResources. caches maxObjCount
	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	// --- Frame-slot turnaround: time from a slot's Present until the slot is reused (after its fence wait) ---
	// Samples carry a timestamp so the UI can average over a time window (0.5 s), not a sample count.
	struct SlotTurnaround {
		static constexpr int kRingSize = 256;    // per slot; covers 0.5 s up to ~1500 fps with 3 slots
		std::array<float, kRingSize>  ms{};      // turnaround in ms
		std::array<float, kRingSize>  waitMs{};  // the fence-wait part of the turnaround, in ms (0 when the GPU was already done)
		std::array<double, kRingSize> atSec{};   // when the sample was recorded (seconds since mTurnaroundEpoch)
		int head = 0;                            // next write slot
		int filled = 0;                          // valid count (caps at kRingSize)
		std::chrono::steady_clock::time_point presentedAt{};
		bool presented = false;                  // set at Present, cleared when the slot is reused; skips frames that never presented
	};
	std::array<SlotTurnaround, gNumFrameResources> mSlotTurnaround;
	const std::chrono::steady_clock::time_point mTurnaroundEpoch = std::chrono::steady_clock::now();
	int total_objects = 0;	// updated by MCSceneManager::Switch via SceneAccess::TotalObjects (== active scene's allRitems.size())

	// --- Pipeline objects (root signatures / PSOs / input layouts) ---
	std::vector<ComPtr<ID3D12RootSignature>> mRootSignatures;
	ComPtr<ID3D12RootSignature> mComputeRootSignature;
	ComPtr<ID3D12RootSignature> mGrassCullRootSignature;
	ComPtr<ID3D12CommandSignature> mGrassCommandSignature;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;
	std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> mInputLayout;

	// --- Descriptor heaps ---
	ComPtr<ID3D12DescriptorHeap> mCbvSrvUavHeap = nullptr;		// unified descriptor heap. There should be no switching of descriptor heap within a draw pass
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

	// --- FrameGraph ---
	std::unique_ptr<FrameGraph> mFrameGraph;
	std::vector<std::unique_ptr<RenderPass>> mPasses;

	// --- Scene render targets + viewport (render-to-texture instead of back buffer) ---
	MCTextureResource mSceneColor, mSceneDepth, mDepthDebugColor, mSceneColorMSAA;
	MCTextureResource mBlurred0; // mBlurred1;
	MCTextureResource mSobelOutput;
	DXGI_FORMAT mSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	D3D12_VIEWPORT mSceneViewport = {};
	D3D12_RECT mSceneScissorRect = {};
	enum class ViewportResMode { Free, Ratio16x9, Ratio1x1, HD, FullHD, K4 };
	ViewportResMode mViewportResMode = ViewportResMode::Free;
	float mViewportZoom = 0.0f; // 0 = FIT, >0 = pixel-accurate factor (1.0 = 100%)
	// Applied scene view size and MSAA state: what the targets were built with, and the only
	// fields the passes and CB writers read. ApplyPendingRequests copies the requested
	// values below into them, with the GPU idle.
	float mSceneViewWidth = 800.0f;
	float mSceneViewHeight = 600.0f;
	bool mScene4xMsaaState = false;
	// Requested by the viewport panel, the MSAA checkbox and the capture harness.
	float mRequestedSceneViewWidth = 800.0f;
	float mRequestedSceneViewHeight = 600.0f;
	bool mRequestedMsaa = false;
	bool mSceneImageHovered = false;
	DirectX::XMFLOAT2 mSceneMousePos = { 0.0f, 0.0f };
	bool mIsWireframe = false;

	// --- Post-process (blur / sobel) ---
	// Upload buffers for the post-process compute shaders (resources proper
	// live on the MCTextureResource members above; descriptors on DescHeapManager).
	// Both are re-uploaded on resize because they cache bindless offsets
	// of post-process MCTextureResources, which shift when those textures are re-created.
	std::unique_ptr<UploadBuffer<CSB_blur>> mBlurUploadBuffer;
	std::unique_ptr<UploadBuffer<CSB_default>> mSobelUploadBuffer;
	bool blurDirty = true;
	CSB_blurValues blurValues;
	bool mIsSobel = false;
	enum SobelType : int {
		Default,
		Gaussain,
		Depth,
		Count
	} mSobelType = SobelType::Gaussain;

	// --- Camera ---
	std::vector<std::unique_ptr<Camera>> mAllCameras;
	Camera* mMainCamera = nullptr;
	bool mCameraDirty = true;
	bool isOrtho = 0;
	float mOrthoT = 0.0f;
	int cameraDirtyCount = 0;

	// --- Lighting / fog / depth-debug ---
	DirectX::XMFLOAT4 mAmbientLight = { .4f, .4f, .4f, 1.0f };
	DirectX::XMFLOAT4 mFogColor = { 1.0f,1.0f,1.0f,1.0f };
	float mFogStart = 30.0f;
	float mFogRange = 1000.0f;
	int mDepthDebugFramesDirty = gNumFrameResources;
	float mDepthDebugMax = 100.0f;
	DebugDepthConstants mDebugDepthCB;

	// --- Culling / debug visualization ---
	bool enableBoundsCheck = true;
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
	float mInstancingCullingTime = 0.0f;

	// --- Editor / UI flags ---
	bool reloadScene = false;
	bool mSceneDirty = false;
	// (mCurrentScenePath removed — MCSceneManager::mScenePaths is the single
	// source of truth for the active scene's save path. Retrieve via
	// mSceneManager.GetScenePath(mSceneManager.GetActive()->name) at use sites.)
	bool enableVsync = false;
	bool mShowDescHeapViewer = false;
	bool mShowFrameGraphViewer = false;
	bool mShowTestsWindow = false;

#if defined(_DEBUG)
	// --- Debug verification harness (bridge B1) ---
	std::unique_ptr<DebugHarness, void(*)(DebugHarness*)> mHarness{ nullptr, nullptr };
#endif
};

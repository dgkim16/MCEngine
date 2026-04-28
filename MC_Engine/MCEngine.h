#pragma once
#define SHDAER_MAJOR 6
#include "Assets/Shaders/SharedTypes.h"
#include "d3dApp.h"
#include "FrameResource.h"
#include "Camera.h"
#include "RenderItem.h"
#include "MC_Types.h"
#include "BarrierManager.h" // not a singleton - saved as a member variable of MCEngine

class Scene_grass; // forward declaration for GPU grass culling

#include <set>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

using namespace DirectX;

extern const int gNumFrameResources;

struct CSB_blurValues // blur related values changed via imgui — CPU-only, not GPU-facing
{
	bool enabled = false;
	float sigma = 0.5f;
	int blurIter = 5;
};

// RenderItem and RenderLayer are now in RenderItem.h
class Scene;

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

	// --- Scene management (public so ImGui and scenes can call SwitchScene) ---
	void RegisterScene(std::unique_ptr<Scene> scene);
	void RequestReload() { reloadScene = true; }
	void SwitchScene(const std::string& name);
	Scene* GetActiveScene() const { return mActiveScene; }
	const std::unordered_map<std::string, std::unique_ptr<Scene>>& GetScenes() const { return mScenes; }

	// --- Accessors used by Scene::Load() ---
	ID3D12Device*              GetDevice()  const { return md3dDevice.Get(); }
	ID3D12GraphicsCommandList* GetCmdList() const { return mCommandList.Get(); }
	BarrierManager& GetBarrierManager() { return mBarrierManager; }
	const BarrierManager& GetBarrierManager() const { return mBarrierManager; }
	MCTexture* GetTexture(const std::string& name) const;
	const std::unordered_map<int, std::string>& GetTextureIndexTracker() const { return mTexturesIndexStrTracker; }

	// --- Special render item setters (called by concrete scenes during Load) ---
	void SetModelRitem(RenderItem* ri)          { mModelRitem = ri; }
	void SetReflectedModelRitem(RenderItem* ri) { mReflectedModelRitem = ri; }
	void SetShadowedModelRitem(RenderItem* ri)  { mShadowedModelRitem = ri; }
	void SetTessellatedRitem(RenderItem* ri)    { mTessellatedRitem = ri; }

	XMFLOAT4& GetFogColor() { return mFogColor; }
	void SetGrassCullingScene(Scene_grass* s) { mGrassScene = s; }

	Camera& GetMainCamera() { return *mMainCamera; }
	std::vector<float> GetScreenSize() { return { mSceneViewWidth, mSceneViewHeight }; }
	XMFLOAT2& GetSceneMousePos() { return mSceneMousePos; }


	
protected:
	virtual void CreateRtvAndDsvDescriptorHeaps()override;	// overridden to set dsvHeapDesc.NumDescriptors = 2 instead of 1.

private:
	virtual void OnResize()override;
	virtual void OnSceneResize();
	virtual void Update(GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	// custom draw Passes
	virtual void ForwardPass(const GameTimer& gt);
	virtual void TessellationExample(const GameTimer& gt);
	// virtual void ComputePass(float gt); // after forward pass

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
	void UpdateBlurCB(const GameTimer& gt);
	void UpdateSobelCB(const GameTimer& gt);
	void UpdateSobelState();

	void BuildDescriptorHeaps();	// create descriptor heap for cbv
	void BuildConstantBufferViews(); // ch7 - replaced BuildConstantBuffer with BuildConstantBufferViews
	void BuildRootSignature();
	virtual void BuildShadersAndInputLayout();
	void BuildPSOs();
	void RegisterPSOVariants(const std::string& name,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& base,bool withMSAA = true, bool withWireframe = true);

	// ch7 stuff - shapes
	void BuildFrameResources();
	void BuildComputeShaderConstantBufferResources(); // uses upload buffer, but aren't frame resources
	void FixupMaterialDiffuseIndices(); // sets DiffuseSrvHeapIndex on all materials after heap is built
	void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, std::string pixEventName = "null");
	void DrawInstanceRenderItems(ID3D12GraphicsCommandList* cmdList, const std::set<RenderItem*>& ritems, bool useGrass = false, std::string pixEventName = "null");
	void DirtyAllRenderItems();
	void PreInitObjectCBs();

	// IMGUI
	void IMGUI_INIT();
	void IMGUI_UPDATE();
	void IMGUI_RENDERDRAWDATA();
	void IMGUI_UPDATE_DESCHEAP_VIEWER();
	LRESULT IMGUI_WNDMSGHANDLER(HWND& hwnd, UINT& msg, WPARAM& wParam, LPARAM& lParam);
	void IMGUI_SHUTDOWN();

	void ReadBackGpuTimer(float dt);
	void TickProfiler(float dt);
	void GrassCullDispatch();

	// ch 9 stuff - texture
	void LoadTextures();

	// rendering to texture
	void BuildSceneRenderTarget();
	void BuildSceneRenderTargetDescriptors();

	void BuildCamera();

	// Debug visualization
	void BuildDebugLineResources();
	UINT BuildDebugLineGeometry();

	float sceneAspectRatio();
	void PrintRenderItemInLayers();
	std::vector<float> CalcGaussWeights(float sigma);

private:
	BarrierManager mBarrierManager;

	//! START- for rendering to Texture instead of directly to back buffer
	MCTexture mSceneColor, mSceneDepth, mDepthDebugColor;
	MCTexture mViewportColor, mViewportNoAlpha;
	MCTexture mBlurred0, mBlurred1;
	MCTexture mSobelOutput;

	DXGI_FORMAT mSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Upload buffers for the post-process compute shaders (resources proper
	// live on the MCTexture members above; descriptors on DescHeapManager).
	// All three are re-uploaded on resize because they cache bindless offsets
	// of post-process MCTextures, which shift when those textures are re-created.
	std::unique_ptr<UploadBuffer<CSB_default>> mForceAlphaUploadBuffer;	// static-only, will break when toggled
	std::unique_ptr<UploadBuffer<CSB_blur>> mBlurUploadBuffer;
	

	D3D12_VIEWPORT mSceneViewport = {};
	D3D12_RECT mSceneScissorRect = {};

	UINT mNumImportedTextureSrvs = 0;

	static constexpr int kCsuReservedHead = 2;        // slot 0 ImGui font, slot 1 ImGui scene
	static constexpr int kCsuTierDynamicCap = 0;      // raise when first dynamic use lands
	static constexpr int kCsuTierStaticHeadroom = 64; // post-process SRVs/UAVs + future static allocs
	int mCsuTierStaticCap = 0;                        // imported tex count + headroom, set in BuildDescriptorHeaps

	UINT mImGuiSceneSrvIndex = 1; // 0 font, 1 scene Texture, 2 depth texture, 3 depth debug texture
	enum class ViewportResMode { Free, Ratio16x9, Ratio1x1, HD, FullHD, K4 };
	ViewportResMode mViewportResMode = ViewportResMode::Free;
	float mViewportZoom = 0.0f; // 0 = FIT, >0 = pixel-accurate factor (1.0 = 100%)

	float mSceneViewWidth = 800.0f;
	float mSceneViewHeight = 600.0f;
	bool mSceneSizeDirty = false; // just mSceneDirty

	bool mSceneImageSelected = false;
	bool mSceneImageHovered = false;
	XMFLOAT2 mSceneMousePos = { 0.0f, 0.0f };
	bool mScene4xMsaaState = false;
	bool mScene4xMsaaStateImGuiRequest = mScene4xMsaaState; // request by ImGui. Later update mScene4xMsaaState to this value just before OnSceneResize()
	//! END- for rendering to Texture instead of directly to back buffer

	std::vector<std::unique_ptr<FrameResource>> mFrameResources;
	FrameResource* mCurrFrameResource = nullptr;
	int mCurrFrameResourceIndex = 0;

	std::vector<ComPtr<ID3D12RootSignature>> mRootSignatures;
	ComPtr<ID3D12RootSignature> mComputeRootSignature;
	ComPtr<ID3D12RootSignature> mGrassCullRootSignature;
	ComPtr<ID3D12CommandSignature> mGrassCommandSignature;
	Scene_grass* mGrassScene = nullptr;
	ComPtr<ID3D12DescriptorHeap> mCbvSrvUavHeap = nullptr;		// unified descriptor heap. There should be no switching of descriptor heap within a draw pass
	ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;			// descriptors (aka views) are accessed via Descriptor Heap
	ComPtr<ID3D12DescriptorHeap> mSrvHeap = nullptr;			// for render target texture on IMGUI scene window
	// ComPtr<ID3D12DescriptorHeap> mDEPTH_SrvHeap = nullptr;	// instead of creating a separate descriptor heap for scene depth, I setup pre-existing descriptor heap to have 2 descriptors, not 1

	UINT mTextureSrvCount = 0;									// number of texture Srvs

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<MCTexture>> mTextures;

	std::unordered_map<int, std::string> mTexturesIndexStrTracker;			// need a better way of keeping track of resources.
	std::unordered_map<std::string, int> mTexturesStrIndexTracker;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<int, std::string> mMaterialsIndexTracker;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

	std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> mInputLayout;

	RenderItem* mModelRitem = nullptr;
	RenderItem* mReflectedModelRitem = nullptr;
	RenderItem* mShadowedModelRitem = nullptr;
	RenderItem* mTessellatedRitem = nullptr;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::set<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	PerPassCB mMainPassCB; // 0 for main, 1 for reflected
	DebugDepthConstants mDebugDepthCB;
	bool mIsWireframe = false;

	std::vector<std::unique_ptr<Camera>> mAllCameras;
	Camera* mMainCamera = nullptr;
	std::vector<Camera*> mShadowmapCameras;
	bool mCameraDirty = true;
	bool mFrustrumDirty = false;
	int mNumCameraDirty = gNumFrameResources;
	POINT mLastMousePos;

	int total_objects = 0;	// set at init stage by BuildRenderItems()	 == mAllRitems.size()
	bool isOrtho = 0;
	float mOrthoT = 0.0f;

	XMFLOAT4 mAmbientLight = { .4f, .4f, .4f, 1.0f };
	DirectX::XMFLOAT4 mFogColor = { 1.0f,1.0f,1.0f,1.0f };
	float mFogStart = 30.0f;
	float mFogRange = 1000.0f;
	int mDepthDebugFramesDirty = gNumFrameResources;
	float mDepthDebugMax = 100.0f;

	bool blurDirty = false;
	CSB_blurValues blurValues;

	bool mIsSobel = false;
	enum SobelType : int {
		Default,
		Gaussain,
		Depth,
		Count
	} mSobelType = SobelType::Gaussain;
	int mSobelCBFramesDirty = gNumFrameResources;

	struct FrameProfiler {
		bool        recording = false;
		float       timeAccum = 0.0f;
		static constexpr float kDuration = 5.0f;
		std::vector<double> cpuSamples;
		std::vector<double> gpuSamples;
		std::array<std::vector<double>, FrameResource::GpuTimerCount - 1> gpuStageSamples;
	} mProfiler;

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

	// --- Scene management ---
	std::unordered_map<std::string, std::unique_ptr<Scene>> mScenes;
	Scene* mActiveScene = nullptr;
	std::string mPendingScene;
	bool reloadScene = false;

	// VSync
	bool enableVsync = true;

	float mInstancingCullingTime = 0.0f;

	std::vector<LightData> mLights;
	enum class LightType : int {Directional, Point, Spot, Count};

	// --- ImGui UI set values
	bool mShowDescHeapViewer = true;

	// --- 
};
#include "MCEngine.h"
#include "MCScene.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "ImGuizmo.h"

#include "SceneSerialization.h"
#include "MC_AssetTests.h"
#include "MCMaterial.h"
#include <algorithm>

std::vector<const char*> fresnel_items;
std::vector<std::string> sobel_items;
std::vector<const char*> sobel_list;

// ----------------------------------------------------------------------------
// ImGui SRV allocator over the leading kCsuReservedHead slots of mCbvSrvUavHeap.
// Replaces the W3-era legacy single-descriptor path: ImGui 1.92's dynamic font
// atlas allocates a *new* texture before freeing the old one during a rebuild
// (e.g. on FontScaleMain change), and the legacy "1 simultaneous texture only"
// allocator asserts on the second alloc. Free-list grants any of the reserved
// slots; capacity must exceed live ImGui texture count by at least 1.
// ----------------------------------------------------------------------------
namespace {
	int sImGuiSrvDescSize = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE sImGuiCpuStart = {};
	D3D12_GPU_DESCRIPTOR_HANDLE sImGuiGpuStart = {};
	int sImGuiSrvCapacity = 0;
	std::vector<int> sImGuiSrvFreeList;

	void ImGui_AllocSrv(ImGui_ImplDX12_InitInfo*,
	                    D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
	                    D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
		IM_ASSERT(!sImGuiSrvFreeList.empty()
			&& "ImGui SRV slots exhausted; bump MCEngine::kCsuReservedHead");
		const int slot = sImGuiSrvFreeList.back();
		sImGuiSrvFreeList.pop_back();
		outCpu->ptr = sImGuiCpuStart.ptr + (SIZE_T)slot * sImGuiSrvDescSize;
		outGpu->ptr = sImGuiGpuStart.ptr + (UINT64)slot * sImGuiSrvDescSize;
	}

	void ImGui_FreeSrv(ImGui_ImplDX12_InitInfo*,
	                   D3D12_CPU_DESCRIPTOR_HANDLE cpu,
	                   D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
		const SIZE_T offsetBytes = cpu.ptr - sImGuiCpuStart.ptr;
		const int slot = (int)(offsetBytes / sImGuiSrvDescSize);
		IM_ASSERT(slot >= 0 && slot < sImGuiSrvCapacity
			&& "ImGui_FreeSrv: descriptor outside reserved range");
		sImGuiSrvFreeList.push_back(slot);
	}
}

// Style source: Hazel - https://github.com/TheCherno/Hazel/blob/master/Hazel/src/Hazel/ImGui/ImGuiLayer.cpp
void SetDarkThemeColors()
{
	auto& colors = ImGui::GetStyle().Colors;
	colors[ImGuiCol_WindowBg] = ImVec4{ 0.1f, 0.105f, 0.11f, 1.0f };

	// Headers
	colors[ImGuiCol_Header] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
	colors[ImGuiCol_HeaderHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
	colors[ImGuiCol_HeaderActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

	// Buttons
	colors[ImGuiCol_Button] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
	colors[ImGuiCol_ButtonHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
	colors[ImGuiCol_ButtonActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

	// Frame BG
	colors[ImGuiCol_FrameBg] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };
	colors[ImGuiCol_FrameBgHovered] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
	colors[ImGuiCol_FrameBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };

	// Tabs
	colors[ImGuiCol_Tab] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
	colors[ImGuiCol_TabHovered] = ImVec4{ 0.38f, 0.3805f, 0.381f, 1.0f };
	colors[ImGuiCol_TabActive] = ImVec4{ 0.28f, 0.2805f, 0.281f, 1.0f };
	colors[ImGuiCol_TabUnfocused] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
	colors[ImGuiCol_TabUnfocusedActive] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };

	// Title
	colors[ImGuiCol_TitleBg] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
	colors[ImGuiCol_TitleBgActive] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.1505f, 0.151f, 1.0f };
}

void MCEngine::IMGUI_INIT() {
	// Create ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	// Single-click on a Drag* widget enters text-edit mode (default is double-click).
	// Drag-to-modify still works: press-and-drag drags, press-and-release text-edits.
	io.ConfigDragClickToInputText = true;
	// Scale font load size by the main window's DPI so the font is the same
	// physical size on a high-DPI display as it would be at 100% scaling.
	// 96 DPI = 100% scaling; 144 = 150%; 192 = 200%. GetDpiForWindow is the
	// per-monitor-aware query; works because mhMainWnd already exists by here.
	const UINT dpi = GetDpiForWindow(mhMainWnd);
	const float dpiScale = (dpi > 0) ? dpi / 96.0f : 1.0f;
	io.Fonts->AddFontFromFileTTF(
		"Assets/Fonts/OpenSans-VariableFont_wdth,wght.ttf",
		16.0f * dpiScale);
	// Style (optional)
	ImGui::StyleColorsDark();
	SetDarkThemeColors();
	// Init Win32 backend — mhMainWnd is your HWND
	ImGui_ImplWin32_Init(mhMainWnd);

	ImGui_ImplDX12_InitInfo initInfo = {};
	initInfo.Device = md3dDevice.Get();
	initInfo.CommandQueue = mCommandQueue.Get(); // <-- critical in 1.92
	initInfo.NumFramesInFlight = gNumFrameResources;
	initInfo.RTVFormat = mBackBufferFormat;
	initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	initInfo.SrvDescriptorHeap = mCbvSrvUavHeap.Get();

	// Modern path: free-list allocator over slots 0..kCsuReservedHead-1 of
	// mCbvSrvUavHeap. Replaces the legacy single-descriptor fields, which only
	// allowed 1 live texture and asserted when ImGui's dynamic atlas tried
	// allocating a second slot mid-rebuild (e.g. FontScaleMain change).
	sImGuiSrvDescSize = mCbvSrvUavDescriptorSize;
	sImGuiCpuStart    = mCbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
	sImGuiGpuStart    = mCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
	sImGuiSrvCapacity = kCsuReservedHead;
	sImGuiSrvFreeList.clear();
	sImGuiSrvFreeList.reserve(kCsuReservedHead);
	for (int i = kCsuReservedHead - 1; i >= 0; --i)
		sImGuiSrvFreeList.push_back(i);

	initInfo.SrvDescriptorAllocFn = &ImGui_AllocSrv;
	initInfo.SrvDescriptorFreeFn  = &ImGui_FreeSrv;

	// Init DX12 backend
	ImGui_ImplDX12_Init(&initInfo);
	bool hasTextureFlag = (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0;
	OutputDebugStringA(hasTextureFlag
		? "ImGui: RendererHasTextures = TRUE (correct for 1.92)\n"
		: "ImGui: RendererHasTextures = FALSE (init failed!)\n");


	fresnel_items.reserve(FresnelR0_items.size());
	for (const auto& c : FresnelR0_items)
		fresnel_items.push_back(c);

	sobel_items.reserve((INT)SobelType::Count);
	sobel_list.reserve((INT)SobelType::Count);
	sobel_items.push_back("Default");
	sobel_items.push_back("Gaussain");
	sobel_items.push_back("Depth");
	for (const auto& s : sobel_items)
		sobel_list.push_back(s.c_str());
	OutputDebugString(L"END saving sobel items\n");
	// Verify
	IM_ASSERT((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures)
		&& "ImGui DX12 init failed - check Device and CommandQueue are valid");
}

void MCEngine::IMGUI_UPDATE() {
	// Start new ImGui frame
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplDX12_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGuiIO& io = ImGui::GetIO();
	ImGui::DockSpaceOverViewport(0, viewport, ImGuiDockNodeFlags_PassthruCentralNode & ImGuiDockNodeFlags_AutoHideTabBar);
	// Build your UI
	static ImVec2 viewportAvail;

	IMGUI_MENUBAR();

	ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoTitleBar); {
		// --- Row 1: Resolution mode ---
		static const char* resModeNames[] = {
			"Free", "16:9", "1:1", "HD (1280x720)", "Full HD (1920x1080)", "4K (3840x2160)"
		};
		int curMode = (int)mViewportResMode;
		float iwidth = ImGui::GetContentRegionAvail().x * 0.2f;
		ImGui::SetNextItemWidth(iwidth);
		if (ImGui::Combo("##resmode", &curMode, resModeNames, 6))
			mViewportResMode = (ViewportResMode)curMode;
		ImGui::SameLine();
		// --- Row 2: Zoom preset (combo, mirrors the resolution combo above) ---
		static const float zPresets[] = { 0.0f, 0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f };
		static const char* zLabels[]  = { "FIT", "25%", "50%", "75%", "100%", "125%", "150%", "175%", "200%" };
		int curZoomIdx = -1;
		for (int i = 0; i < IM_ARRAYSIZE(zPresets); ++i)
			if (mViewportZoom == zPresets[i]) { curZoomIdx = i; break; }
		ImGui::SetNextItemWidth(iwidth);
		if (ImGui::BeginCombo("##zoompreset", curZoomIdx >= 0 ? zLabels[curZoomIdx] : "Custom")) {
			for (int i = 0; i < IM_ARRAYSIZE(zPresets); ++i) {
				bool selected = (curZoomIdx == i);
				if (ImGui::Selectable(zLabels[i], selected)) mViewportZoom = zPresets[i];
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(iwidth);
		static float fitZoom = 1.0f;
		float sliderVal = (mViewportZoom == 0.0f) ? fitZoom : mViewportZoom;
		if (ImGui::SliderFloat("##zoom", &sliderVal, 0.1f, 2.0f, "%.2fx"))
			mViewportZoom = sliderVal;

		// --- Available space after both control rows ---
		ImVec2 avail = ImGui::GetContentRegionAvail();
		if (avail.x < 1.0f) avail.x = 1.0f;
		if (avail.y < 1.0f) avail.y = 1.0f;

		// --- Compute render target and FIT display size from resolution mode ---
		float renderW = avail.x, renderH = avail.y, displayW = avail.x, displayH = avail.y;
		switch (mViewportResMode) {
		case ViewportResMode::Free:
			renderW = avail.x; renderH = avail.y;
			displayW = avail.x; displayH = avail.y;
			break;
		case ViewportResMode::Ratio16x9:
			if (avail.x / avail.y <= 16.0f / 9.0f) { renderW = avail.x; renderH = avail.x * 9.0f / 16.0f; }
			else                                     { renderH = avail.y; renderW = avail.y * 16.0f / 9.0f; }
			displayW = renderW; displayH = renderH;
			break;
		case ViewportResMode::Ratio1x1:
			renderW = renderH = (std::min)(avail.x, avail.y);
			displayW = displayH = renderW;
			break;
		case ViewportResMode::HD:     renderW = 1280.0f; renderH =  720.0f; break;
		case ViewportResMode::FullHD: renderW = 1920.0f; renderH = 1080.0f; break;
		case ViewportResMode::K4:     renderW = 3840.0f; renderH = 2160.0f; break;
		default: break;
		}
		if (mViewportResMode >= ViewportResMode::HD) {
			float s = (std::min)(avail.x / renderW, avail.y / renderH);
			displayW = renderW * s; displayH = renderH * s;
		}
		renderW = (std::max)(renderW, 1.0f);
		renderH = (std::max)(renderH, 1.0f);

		// --- Update render target if dimensions changed ---
		if ((UINT)renderW != (UINT)mSceneViewWidth || (UINT)renderH != (UINT)mSceneViewHeight) {
			mSceneViewWidth  = renderW;
			mSceneViewHeight = renderH;
			mSceneSizeDirty  = true;
		}

		// --- Apply zoom to get final image size and UV window ---
		ImVec2 imageSize;
		ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
		float imgOffX = (std::max)(0.0f, (avail.x - displayW) * 0.5f);
		float imgOffY = (std::max)(0.0f, (avail.y - displayH) * 0.5f);
		

		if (mViewportZoom == 0.0f) {
			// FIT: full texture scaled to fill panel (displayW/H already computed)
			imageSize = ImVec2(displayW, displayH);
			fitZoom = (renderW > 0.0f) ? displayW / renderW : 1.0f;
		} else {
			float zoomedW = renderW * mViewportZoom;
			float zoomedH = renderH * mViewportZoom;
			if (zoomedW <= avail.x && zoomedH <= avail.y) {
				// Fits inside panel: exact zoomed size, centered
				imageSize = ImVec2(zoomedW, zoomedH);
				imgOffX   = (avail.x - zoomedW) * 0.5f;
				imgOffY   = (avail.y - zoomedH) * 0.5f;
				// uv stays (0,0)-(1,1)
			} else {
				// Overflows panel: fill panel, clip UV to visible center
				imageSize = ImVec2(avail.x, avail.y);
				float uvRangeX = avail.x / zoomedW;
				float uvRangeY = avail.y / zoomedH;
				uv0 = ImVec2(0.5f - uvRangeX * 0.5f, 0.5f - uvRangeY * 0.5f);
				uv1 = ImVec2(0.5f + uvRangeX * 0.5f, 0.5f + uvRangeY * 0.5f);
				imgOffX = 0.0f;
				imgOffY = 0.0f;
			}
		}

		ImVec2 cursor = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(cursor.x + imgOffX, cursor.y + imgOffY));
		ImTextureID tex = mIsSobel          ? (ImTextureID)mSobelOutput.SRVs[0].hGpu.ptr
		                : blurValues.enabled ? (ImTextureID)mBlurred0.SRVs[0].hGpu.ptr
		                :                      (ImTextureID)mViewportNoAlpha.SRVs[0].hGpu.ptr;

		mSceneImageSelected = ImGui::IsWindowFocused(ImGuiFocusedFlags_None);
		ImGui::Image(tex, imageSize, uv0, uv1);
		mSceneImageHovered = ImGui::IsItemHovered();
		if (mSceneImageHovered) {
			ImVec2 mousePos = ImGui::GetIO().MousePos;
			ImVec2 imgMin = ImGui::GetItemRectMin();
			float localX = mousePos.x - imgMin.x;          // 0..imageSize.x
			float localY = mousePos.y - imgMin.y;          // 0..imageSize.y
			float u = uv0.x + (localX / imageSize.x) * (uv1.x - uv0.x);
			float v = uv0.y + (localY / imageSize.y) * (uv1.y - uv0.y);
			mSceneMousePos.x = u * renderW;                // pixels in render target
			mSceneMousePos.y = v * renderH;
		};
		viewportAvail = avail;

		// ----------- IMGUIZMO
		if (auto* item = CurrentSelectedItem()) {
			const ImVec2 imgMin = ImGui::GetItemRectMin();
			const ImVec2 imgSize = ImGui::GetItemRectSize();

			ImGuizmo::SetDrawlist(); // draw into THIS window
			ImGuizmo::SetRect(imgMin.x, imgMin.y, imgSize.x, imgSize.y);

			// DirectX wants row-major in memory; ImGuizmo wants column - major.
			// mMainCamera stores as col-major for convenience
			// (UpdateObjectCBs at MCEngine.cpp:395 also transposes when uploading to HLSL.)
			XMFLOAT4X4 viewM, projM, worldM;
			XMStoreFloat4x4(&viewM, mMainCamera->GetView());
			XMStoreFloat4x4(&projM, mMainCamera->GetProj());
			worldM = item->World;

			ImGuizmo::Manipulate(
				reinterpret_cast<float*>(&viewM),
				reinterpret_cast<float*>(&projM),
				ImGuizmo::TRANSLATE,
				ImGuizmo::LOCAL,
				reinterpret_cast<float*>(&worldM));

			// Step 4 (next): if (ImGuizmo::IsUsing()) decompose worldT back into
			// item->Position/Rotation/Scale, MarkSceneDirty(), bump NumFramesDirty.

			if (ImGuizmo::IsUsing()) {
				float t[3], r[3], s[3];
				ImGuizmo::DecomposeMatrixToComponents(
					reinterpret_cast<float*>(&worldM), t, r, s);
				// TRANSLATE-only mode: write back Position ONLY.
				item->Position = { t[0], t[1], t[2] };
				item->NumFramesDirty = gNumFrameResources;
				MarkSceneDirty();
				// No explicit world rebuild: UpdateObjectCBs (MCEngine.cpp:350-353)
				// recomposes World from P/R/S whenever NumFramesDirty > 0, same path
				// as inspector edits.
			}
		}

	}ImGui::End();

	ImGui::Begin("Buffer View"); {
		ImVec2 avail = ImGui::GetContentRegionAvail();
		float dim = (std::min)(150.0f, (std::min)(avail.x, avail.y)) - 20.0f;
		ImGui::BeginGroup(); {
			ImGui::Image((ImTextureID)mSceneDepth.SRVs[0].hGpu.ptr, ImVec2(dim, dim));
			ImGui::Text("DSV view");
		}ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup(); {
			ImGui::Image((ImTextureID)mDepthDebugColor.SRVs[0].hGpu.ptr, ImVec2(dim, dim));
			ImGui::Text("DSV normalized");
		}ImGui::EndGroup();
	}ImGui::End();

	ImGui::Begin("Global Inspector"); {
		ImGui::Text("Cursor: %.0f, %.0f", mSceneMousePos.x, mSceneMousePos.y);
		if (ImGui::Button("Reload Shaders"))
			ReloadShaders();
		ImGui::SameLine();
		// D9 Step 5a — full-scene reload from disk. Save-before-drop preserves
		// any unsaved ImGui edits to the active scene's module params. Routed
		// through ReloadActiveSceneNow so the cmdlist is open when grass
		// module's OnLoad records GPU upload work.
		if (ImGui::Button("Reload Active Scene")) {
			try { ReloadActiveSceneNow(); OutputDebugStringA("[reload] active scene reloaded\n"); }
			catch (const std::exception& e) {
				OutputDebugStringA(("[reload] FAIL: " + std::string(e.what()) + "\n").c_str());
			}
		}
		ImGui::Text("Camera Dirty Count: %d", cameraDirtyCount); ImGui::SameLine();
		if (ImGui::Button("reset count"))
			cameraDirtyCount = 0;
		if (ImGui::Checkbox("Enable BoundingBox", &enableBoundsCheck))
			if (!enableBoundsCheck)
				DirtyAllRenderItems();
		if (ImGui::Checkbox("MCScene MSAA", &mScene4xMsaaState)) // slightly unsynced, but gets synced naturally by next round of frames (thus trivial)
			mSceneSizeDirty = true;
		ImGui::Text("CPU Frame: %.3f ms", mTimer.DeltaTime() * 1000.0f);
		ImGui::Text("GPU Frame: %.3f ms", (float)mCurrFrameResource->totalGpuFrameMs);
		ImGui::Text("Instancing culling time: %.3f ms", (float)mInstancingCullingTime);
		static float time = mTimer.TotalTime();
		static float percentage = (float)mInstancingCullingTime / (mTimer.DeltaTime() * 10.0f);
		if (mTimer.TotalTime() - time > 1.0f) {
			percentage = (float)mInstancingCullingTime / (mTimer.DeltaTime() * 10.0f);
			time = mTimer.TotalTime();
		}
		ImGui::Text("Instancing culling ratio : %.1f %%", percentage);
		if (mProfiler.recording) {
			float remaining = FrameProfiler::kDuration - mProfiler.timeAccum;
			ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Recording... %.1fs left", remaining);
		}
		else {
			
			if (ImGui::Button("Record 5s frame times"))
			{
				mProfiler.recording = true;
				mProfiler.timeAccum = 0.0f;
				mProfiler.cpuSamples.clear();
				mProfiler.gpuSamples.clear();
				for (auto& v : mProfiler.gpuStageSamples)
					v.clear();
			}
		}

		ImGui::Text("ImGui scene Mouse: %d", mSceneImageHovered);
		ImGui::Text("MCScene ratio: %.0f : %.0f", mSceneViewWidth, mSceneViewHeight);
		ImGui::Text("Viewport ratio: %.0f : %.0f", viewportAvail.x, viewportAvail.y);
		ImGui::Text("MCScene dirty: %d", mSceneSizeDirty);
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
		ImGui::Checkbox("press 1 for Wireframe", &mIsWireframe);
		static float fps_target_float = (float)targetFPS;
		ImGui::Checkbox("Vsync", &enableVsync);
		ImGui::Checkbox("Cap FPS", &capFPS);
		if(capFPS) {
			ImGui::SameLine();
			if (ImGui::SliderFloat("FPS target", &fps_target_float, 30.0f, (float)maxFPS))
				targetFPS = (double)fps_target_float;
		}
		if (ImGui::CollapsingHeader("Ambient light color")) {
			ImGui::ColorPicker4("Ambient Color", &mAmbientLight.x);
		}
		if (ImGui::CollapsingHeader("Fog settings")) {
			if (ImGui::Button("reset fogstart")) mFogStart = 30.0f; ImGui::SameLine();
			ImGui::SliderFloat("Fog Start", &mFogStart, 0.01f, 150.0f);
			if (ImGui::Button("reset fogrange")) mFogRange = 1000.0f; ImGui::SameLine();
			ImGui::SliderFloat("Fog Range", &mFogRange,0.1f, 5000.0f);
			ImGui::ColorPicker4("Fog Color", &mFogColor.x);
			// if (ImGui::ColorPicker4("Fog Color", &mFogColor.x))
			//	mSceneSizeDirty = true;
		}

		if (ImGui::CollapsingHeader("Visualization")) {
			ImGui::Checkbox("Descriptor Heap Manager", &mShowDescHeapViewer);
			ImGui::Separator();
			ImGui::Checkbox("Show Bounding Boxes", &mShowBoundingBoxes);
			
		}
		if (ImGui::CollapsingHeader("Camera Settings")) {
			ImGui::Checkbox("OrthoCamera", &isOrtho);
			ImGui::SliderFloat("camera speed", &mMainCamera->moveSpeed, .01f, 1.0f);
			bool chngNear = ImGui::SliderFloat("Near", &nearPlane, 0.01f, farPlane-0.01f);
			ImGui::SameLine();
			if (ImGui::Button("near 0.1")) {
				nearPlane = 0.1f;
				chngNear = true;
			}
			bool chngFar = ImGui::SliderFloat("Far", &farPlane, nearPlane+0.01f, 10000.0f);
			ImGui::SameLine();
			if (ImGui::Button("far 1000")) {
				farPlane = 1000.0f;
				chngFar = true;
			}
			if (chngNear || chngFar) {
				mDepthDebugFramesDirty = gNumFrameResources;
				mCameraDirty = true;
			}
			ImGui::Separator();
			bool prevFreeze = mFreezeCamera;
			ImGui::Checkbox("Freeze Culling Frustum", &mFreezeCamera);
			if (mFreezeCamera && !prevFreeze) {
				// Capture current world-space frustum and view-proj matrix for GPU culling
				XMMATRIX view = mMainCamera->GetView();
				XMVECTOR det = XMMatrixDeterminant(view);
				XMMATRIX invV = XMMatrixInverse(&det, view);
				mMainCamera->GetFrustrum().Transform(mFrozenWorldFrustum, invV);
				XMMATRIX proj = mMainCamera->GetProj();
				XMMATRIX viewProj = XMMatrixMultiply(view, proj);
				XMStoreFloat4x4(&mFrozenViewProjT, XMMatrixTranspose(viewProj));
				DirtyAllRenderItems();
				mFrustrumDirty = true;
			}
			else if (!mFreezeCamera && prevFreeze) {
				DirtyAllRenderItems();
				mFrustrumDirty = true;
			}
			if (mFreezeCamera)
				ImGui::TextDisabled("  Frustum frozen - camera moves freely");

			ImGui::Checkbox("Show Frustum Wireframe", &mShowFrustum);
			if (mShowFrustum) {
				ImGui::SameLine();
				ImGui::TextDisabled(mFreezeCamera ? "(frozen)" : "(live)");
			}
		}

		if (ImGui::CollapsingHeader("Outline Settings")) {
			if (ImGui::Checkbox("Sobel outlines", &mIsSobel) && mSobelType == SobelType::Gaussain)
				blurValues.enabled = mIsSobel;
			int cursobel = (int)mSobelType;
			if (ImGui::Combo("Sobel target", &cursobel, sobel_list.data(), (int)sobel_list.size()))
				mSobelType = (SobelType)cursobel;
		}
		if (ImGui::CollapsingHeader("Blur Settings")) {
			ImGui::Checkbox("Blur toggle", &blurValues.enabled);
			bool blurSigChng = ImGui::SliderFloat("sigma", &blurValues.sigma, 0.1f, 10.0f);
			bool blurItrChng = ImGui::SliderInt("blur iter", &blurValues.blurIter, 1, 10);
			if (blurSigChng || blurItrChng)
				blurDirty = true;
		}
		if (ImGui::CollapsingHeader("Direcitonal Light")) {
			float inputfloat_width = ImGui::GetContentRegionAvail().x * 0.1f;
			ImGui::SetNextItemWidth(inputfloat_width);
			bool xchng = ImGui::InputFloat("x", &mLights[0].mLightDirection.x);
			ImGui::SameLine();
			xchng = ImGui::SliderFloat("dir light x slider", &mLights[0].mLightDirection.x,-10.0f, 10.0f);

			ImGui::SetNextItemWidth(inputfloat_width);
			bool ychng = ImGui::InputFloat("y", &mLights[0].mLightDirection.y);
			ImGui::SameLine();
			ychng = ImGui::SliderFloat("dir light y slider", &mLights[0].mLightDirection.y, -10.0f, 10.0f);

			ImGui::SetNextItemWidth(inputfloat_width);
			bool zchng = ImGui::InputFloat("z", &mLights[0].mLightDirection.z);
			ImGui::SameLine();
			zchng = ImGui::SliderFloat("dir light z slider", &mLights[0].mLightDirection.z, -10.0f, 10.0f);

			if (ImGui::Button("Reset light Pos"))
				mLights[0].mLightDirection = { 1.0f,1.0f,1.0f };
			if (xchng || ychng || zchng) {
				// DirtyAllRenderItems(); // main pass gets updated regardless, so no need to dirty
			}
		}
	} ImGui::End();
	ImGui::Begin("Frame Profiles"); {
		ImGui::Text("GPU Frame (scene color): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[0]);
		ImGui::Text("GPU Frame (depth debug): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[1]);
		ImGui::Text("GPU Frame (msaa resolve): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[2]);
		ImGui::Text("GPU Frame (force alpha): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[3]);
		ImGui::Text("GPU Frame (blurs): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[4]);
		ImGui::Text("GPU Frame (sobel): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[5]);
		ImGui::Text("GPU Frame (switch RT): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[6]);
		ImGui::Text("GPU Frame (imgui): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[7]);
		ImGui::Text("GPU Frame (present): %.3f ms", (float)mCurrFrameResource->GpuFrameMs[8]);
	}ImGui::End();

	ImGui::Begin("MCScene Inspector"); 
	{
		// --- MCScene switcher ---
		{
			MCScene* active = mSceneManager.GetActive();
			std::string current = active ? active->name : "(none)";
			if (ImGui::BeginCombo("MCScene", current.c_str())) {
				for (auto& [sceneName, scenePtr] : mSceneManager.All()) {
					bool selected = (sceneName == current);
					if (ImGui::Selectable(sceneName.c_str(), selected))
						mSceneManager.RequestSwitch(sceneName);
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		if (ImGui::Button("Reload Scene")) {
			try { ReloadActiveSceneNow(); OutputDebugStringA("[reload] active scene reloaded\n"); }
			catch (const std::exception& e) {
				OutputDebugStringA(("[reload] FAIL: " + std::string(e.what()) + "\n").c_str());
			}
		}
		ImGui::SameLine();
		ImGui::Checkbox("Autosave on switch", &mAutosaveOnSwitch);

		// Runtime font-scale control. Multiplies the rasterized atlas at draw
		// time (ImGui 1.92 style.FontScaleMain). DPI-scaled load size is the
		// 1.0x baseline; this slider stacks on top. Glyphs at non-1.0 are
		// bilinear-filtered — slightly soft, fine for moderate ranges.
		ImGui::SetNextItemWidth(160.0f);
		ImGui::SliderFloat("Font scale", &ImGui::GetStyle().FontScaleMain,
		                   0.5f, 2.0f, "%.2fx");
		ImGui::Separator();
		if (auto* active = mSceneManager.GetActive())
			active->OnImGui(*this);
	}
	ImGui::End();
	IMGUI_OUTLINER();
	IMGUI_INSPECTOR();
	IMGUI_UPDATE_DESCHEAP_VIEWER();
	IMGUI_TEST();

	ImGui::Render(); // finalizes draw data, does NOT submit to GPU yet
}


void MCEngine::IMGUI_RENDERDRAWDATA() {
	// for all resources that will be used in IMGUI::Image, change state to D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE 
	// mBarrierManager.TransitionState(mSceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mSobelOutput, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mDepthDebugColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	mBarrierManager.FlushBarriers(mCommandList.Get());

	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT MCEngine::IMGUI_WNDMSGHANDLER(HWND& hwnd, UINT& msg, WPARAM& wParam, LPARAM& lParam) {
	return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
}

void MCEngine::IMGUI_SHUTDOWN()
{
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void MCEngine::IMGUI_TEST()
{
	if (!mShowTestsWindow) return;
	ImGui::Begin("IMGUI_TESTS");
	if (ImGui::BeginTabBar("IMGUI_TESTS_TabBar")) {

		if (ImGui::BeginTabItem("JSON round-trip")) {
			if (ImGui::Button("TestRoundTripFirstItem")) {
				_requestRoundTripTest = true;
			}
			ImGui::EndTabItem();
		}

		// D9 Step 1 — asset-manager verification suite. Six tests log
		// `[<tag>] PASS\n` / `[<tag>] FAIL — <reason>\n` to OutputDebugString.
		// Tests are CPU-only (no GPU command list); inline invocation, no flag.
		if (ImGui::BeginTabItem("Asset (D9 Step 1)")) {
			if (ImGui::Button("1a Cross-process determinism"))    MCAssetTests::RunDeterminism(*this);
			if (ImGui::Button("1b Cross-kind hash collision-free")) MCAssetTests::RunCrossKindCollisionFree();
			if (ImGui::Button("1c Hash-mismatch reject"))         MCAssetTests::RunHashMismatchReject(*this);
			if (ImGui::Button("1d JsonMigrator v0 reject"))       MCAssetTests::RunMigratorV0Reject(*this);
			if (ImGui::Button("1e JsonMigrator v999 reject"))     MCAssetTests::RunMigratorV999Reject(*this);
			if (ImGui::Button("1f Cross-asset name-different-kind")) MCAssetTests::RunCrossAssetSameName(*this);
			ImGui::Separator();
			if (ImGui::Button("Run all"))                         MCAssetTests::RunAll(*this);
			ImGui::EndTabItem();
		}

		// D9 Step 2 — asset_redirects.json edge cases. Four tests using temp
		// fixture files; cleanup after each. Local MCSceneManager keeps the
		// live state untouched.
		if (ImGui::BeginTabItem("Redirect (D9 Step 2)")) {
			if (ImGui::Button("2a Cycle detection"))               MCAssetTests::RunCycleDetection(*this);
			if (ImGui::Button("2b Dangling target"))               MCAssetTests::RunDanglingTarget(*this);
			if (ImGui::Button("2c Multi-redirect from same source")) MCAssetTests::RunMultiRedirect(*this);
			if (ImGui::Button("2d Depth-3 chain"))                 MCAssetTests::RunDepth3Chain(*this);
			ImGui::Separator();
			if (ImGui::Button("Run all (Step 2)"))                 MCAssetTests::RunAllRedirects(*this);
			ImGui::EndTabItem();
		}

		// D9 Step 4 — resilience suite. Six tests against malformed inputs.
		// All must surface clear errors, not crash.
		if (ImGui::BeginTabItem("Resilience (D9 Step 4)")) {
			if (ImGui::Button("4a Field deletion"))                MCAssetTests::RunFieldDeletion();
			if (ImGui::Button("4b Bad handle reference"))          MCAssetTests::RunBadHandleReference(*this);
			if (ImGui::Button("4c Re-save equivalence"))           MCAssetTests::RunResaveEquivalence(*this);
			if (ImGui::Button("4d version field absent"))          MCAssetTests::RunVersionAbsent(*this);
			if (ImGui::Button("4e Bad asset (.mcmat) version"))    MCAssetTests::RunBadAssetVersion(*this);
			if (ImGui::Button("4f Same name across scenes"))       MCAssetTests::RunSameNameAcrossScenes(*this);
			ImGui::Separator();
			if (ImGui::Button("Run all (Step 4)"))                 MCAssetTests::RunAllResilience(*this);
			ImGui::EndTabItem();
		}

		// D9 Step 6 — outlier-mode edge cases. Switch to fixture scenes and
		// programmatic checks against MCSceneManager::Switch behavior.
		if (ImGui::BeginTabItem("Outlier (D9 Step 6)")) {
			if (ImGui::Button("6a Empty scene (SoloVoid)"))         MCAssetTests::RunSoloVoid(*this);
			if (ImGui::Button("6b One-renderitem scene (SoloBox)")) MCAssetTests::RunSoloBox(*this);
			if (ImGui::Button("6c Switch nonexistent"))             MCAssetTests::RunSwitchNonexistent(*this);
			if (ImGui::Button("6d Switch to active (no-op)"))       MCAssetTests::RunSwitchSelf(*this);
			ImGui::Separator();
			if (ImGui::Button("Run all (Step 6)"))                  MCAssetTests::RunAllOutlier(*this);
			ImGui::EndTabItem();
		}

		// D9 Step 5c — per-asset Reload. One row per registered material.
		// Mutate-in-place; preserves cached MCMaterial* pointers, so render items
		// don't re-link. roughness/diffuseAlbedo edits apply via NumFramesDirty
		// without a scene reload.
		if (ImGui::BeginTabItem("Material reload (D9 Step 5c)")) {
			std::vector<std::pair<MaterialHandle, std::string>> rows;
			rows.reserve(mMaterialManager.All().size());
			for (const auto& [h, mptr] : mMaterialManager.All()) {
				rows.emplace_back(h, mptr ? mptr->Name : std::string{ "(null)" });
			}
			std::sort(rows.begin(), rows.end(),
				[](const auto& a, const auto& b) { return a.second < b.second; });
			for (const auto& [h, name] : rows) {
				ImGui::PushID(static_cast<int>(h));
				ImGui::Text("%s", name.c_str()); ImGui::SameLine();
				if (ImGui::Button("Reload")) {
					try { mMaterialManager.Reload(h); OutputDebugStringA(("[matreload] " + name + "\n").c_str()); }
					catch (const std::exception& e) {
						OutputDebugStringA(("[matreload] FAIL " + name + ": " + e.what() + "\n").c_str());
					}
				}
				ImGui::PopID();
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
	ImGui::End();
}

/*
// HOW TO create tabs
if (ImGui::BeginTabBar("Main Tab")) {
	if (ImGui::BeginTabItem("Window A"))
	{
		// Content that used to be in your first ImGui::Begin()/End()
		ImGui::Text("This is tab A");
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem("Window B"))
	{
		// Content that used to be in your second ImGui::Begin()/End()
		ImGui::Text("This is tab B");
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}
		*/

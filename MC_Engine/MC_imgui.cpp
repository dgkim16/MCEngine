#include "MCEngine.h"
#include "Scene.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

std::vector<std::string> r_itemNames;
std::vector<const char*> r_itemslist;
int selected_ri = 0;
std::vector<const char*> texture_itemslist;
std::vector<const char*> material_itemslist;
std::vector<const char*> fresnel_items;
std::vector<std::string> sobel_items;
std::vector<const char*> sobel_list;

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
	io.Fonts->AddFontFromFileTTF("Assets/Fonts/OpenSans-VariableFont_wdth,wght.ttf", 16.0f);
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

	// point to slot 0 of your SRV heap
	initInfo.LegacySingleSrvCpuDescriptor =
		mCbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
	initInfo.LegacySingleSrvGpuDescriptor =
		mCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();

	// Init DX12 backend
	ImGui_ImplDX12_Init(&initInfo);
	bool hasTextureFlag = (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0;
	OutputDebugStringA(hasTextureFlag
		? "ImGui: RendererHasTextures = TRUE (correct for 1.92)\n"
		: "ImGui: RendererHasTextures = FALSE (init failed!)\n");

	r_itemNames.clear();
	r_itemslist.clear();
	r_itemNames.reserve((int)total_objects);
	r_itemslist.reserve((int)total_objects);
	for (auto& ri : mAllRitems)
		r_itemNames.push_back(ri->Name + std::to_string(ri->ObjCBIndex));
	for (auto& name : r_itemNames)
	{
		std::cout << "render item name : " << name << std::endl;
		r_itemslist.push_back(name.c_str());
	}

	fresnel_items.reserve(FresnelR0_items.size());
	for (const auto& c : FresnelR0_items)
		fresnel_items.push_back(c);

	material_itemslist.clear();
	material_itemslist.reserve((int)mMaterials.size());
	for (const auto& m : mMaterials) {
		material_itemslist.push_back(m.first.c_str());
	}
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
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGuiIO& io = ImGui::GetIO();
	ImGui::DockSpaceOverViewport(0, viewport, ImGuiDockNodeFlags_PassthruCentralNode & ImGuiDockNodeFlags_AutoHideTabBar);
	// Build your UI
	static ImVec2 viewportAvail;
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

	ImGui::Begin("Inspector"); {
		ImGui::Text("Cursor: %.0f, %.0f", mSceneMousePos.x, mSceneMousePos.y);
		if (ImGui::Button("Reload Shaders"))
			ReloadShaders();
		ImGui::Text("Camera Dirty Count: %d", cameraDirtyCount); ImGui::SameLine();
		if (ImGui::Button("reset count"))
			cameraDirtyCount = 0;
		if (ImGui::Checkbox("Enable BoundingBox", &enableBoundsCheck))
			if (!enableBoundsCheck)
				DirtyAllRenderItems();
		if (ImGui::Checkbox("Scene MSAA", &mScene4xMsaaState)) // slightly unsynced, but gets synced naturally by next round of frames (thus trivial)
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
		ImGui::Text("Scene ratio: %.0f : %.0f", mSceneViewWidth, mSceneViewHeight);
		ImGui::Text("Viewport ratio: %.0f : %.0f", viewportAvail.x, viewportAvail.y);
		ImGui::Text("Scene dirty: %d", mSceneSizeDirty);
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
			if (ImGui::ColorPicker4("Fog Color", &mFogColor.x))
				mSceneSizeDirty = true;
		}

		if (ImGui::CollapsingHeader("Visualization")) {
			ImGui::Checkbox("Descriptor Heap Manager", &mShowDescHeapViewer);
			ImGui::Separator();
			ImGui::Checkbox("Show Bounding Boxes", &mShowBoundingBoxes);
			ImGui::Separator();
			bool prevFreeze = mFreezeCamera;
			ImGui::Checkbox("Freeze Culling Frustum", &mFreezeCamera);
			if (mFreezeCamera && !prevFreeze) {
				// Capture current world-space frustum and view-proj matrix for GPU culling
				XMMATRIX view = mMainCamera->GetView();
				XMVECTOR det  = XMMatrixDeterminant(view);
				XMMATRIX invV = XMMatrixInverse(&det, view);
				mMainCamera->GetFrustrum().Transform(mFrozenWorldFrustum, invV);
				XMMATRIX proj     = mMainCamera->GetProj();
				XMMATRIX viewProj = XMMatrixMultiply(view, proj);
				XMStoreFloat4x4(&mFrozenViewProjT, XMMatrixTranspose(viewProj));
				DirtyAllRenderItems();
				mFrustrumDirty = true;
			} else if (!mFreezeCamera && prevFreeze) {
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
		}
		/*
		ImGui::Combo("Render Item", &(selected_ri), r_itemslist.data(), (int)r_itemslist.size());
		{
			std::string c_msg = "OBJ_";
			c_msg.append(r_itemslist[selected_ri]);
			
			if (ImGui::CollapsingHeader(c_msg.c_str())) {
				ImGui::Text("Selected Material");
				int& selected = mAllRitems[selected_ri]->Mat->MatCBIndex;
				bool chngMat = ImGui::Combo("Material", &(selected), material_itemslist.data(), (int)material_itemslist.size());
				if (chngMat) {
					mRitemLayer[mAllRitems[selected_ri]->Mat->renderLevel].erase(mAllRitems[selected_ri].get());
					mAllRitems[selected_ri]->Mat = mMaterials[material_itemslist[selected]].get();
					mRitemLayer[mAllRitems[selected_ri]->Mat->renderLevel].insert(mAllRitems[selected_ri].get());
				}
				ImGui::Separator();
				ImGui::Text("Material Properties");
				Material& mat = *mAllRitems[selected_ri]->Mat;
				// bool chngTexture = ImGui::Combo("Texture", &(mat.DiffuseSrvHeapIndex), texture_itemslist.data(), (int)texture_itemslist.size());
				bool chngShiny = ImGui::SliderFloat(("Shininess " + c_msg).c_str(), &(mat.Roughness), 0.001f, 0.99f);
				if (ImGui::Combo("Fresnel", &(mat.FresnelIndex), fresnel_items.data(), (int)fresnel_items.size())) {
					mat.FresnelR0 = FresnelR0_Values[mat.FresnelIndex];
					chngShiny = true;
				}
				bool chngColor = ImGui::ColorPicker3(("Diffuse Color " + c_msg).c_str(), &(mat.DiffuseAlbedo.x));
				bool chngAlpha = ImGui::SliderFloat(("Diffuse Alpha " + c_msg).c_str(), &(mat.DiffuseAlbedo.w), 0.01f, 0.99f);
				if (chngTexture || chngShiny || chngColor || chngAlpha)
					mat.NumFramesDirty = gNumFrameResources;
			}
		}
		*/
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

	ImGui::Begin("Scene Inspector"); 
	{
		// --- Scene switcher ---
		{
			std::string current = mActiveScene ? mActiveScene->name : "(none)";
			if (ImGui::BeginCombo("Scene", current.c_str())) {
				for (auto& [sceneName, scenePtr] : mScenes) {
					bool selected = (sceneName == current);
					if (ImGui::Selectable(sceneName.c_str(), selected))
						mPendingScene = sceneName; // set up the flag
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
		ImGui::Separator();
		mActiveScene->Scene_IMGUI(*this);

	}
	ImGui::End();
	IMGUI_UPDATE_DESCHEAP_VIEWER();


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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MCEngine.h"
#include "MCScene.h"
#include "MathHelper.h"
#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

// layer is only changed by this script. 
// If other wants to change renderitem's layer,
// SetLayers() will have to move to MCEngine or MCScene
static void SetLayers(MCEngine& engine, MCScene* scene,
    RenderItem* item, uint32_t newLayers) {
    if (!scene || !item) return;
    if (item->Layers == newLayers) return;

    // Diff-update via XOR: touch only the bits that flipped, not all 11.
    // Each toggle costs exactly one set op (insert OR erase), regardless of
    // how many bits the bitmask currently has set.
    const uint32_t changed = item->Layers ^ newLayers;
    for (uint32_t b = 0; b < static_cast<uint32_t>(RenderLayer::Count); ++b) {
        const uint32_t bit = 1u << b;
        if (!(changed & bit)) continue;
        if (newLayers & bit) scene->layers[b].insert(item);
        else                 scene->layers[b].erase(item);
    }

    item->Layers = newLayers;
    item->NumFramesDirty = gNumFrameResources;
    engine.MarkSceneDirty();
}


void MCEngine::IMGUI_INSPECTOR() {
    ImGui::Begin("Inspector");
    static bool sInspectorOpen = true;
    if (!sInspectorOpen) {
        ImGui::End();
        return;
    }
    auto* item = CurrentSelectedItem();
    if (!item) {
        ImGui::TextUnformatted("Nothing selected.");
        ImGui::End();
        return;
    }

    // Inspector-local Euler cache (display only). Reset whenever selection changes,
    // so the user sees a fresh quat-derived Euler instead of stale drift.
    static int    sLastSelectedIdx = -1;
    static XMFLOAT3 sEulerDeg = { 0, 0, 0 };
    if (mSelectedItemIndex != sLastSelectedIdx) {
        sLastSelectedIdx = mSelectedItemIndex;
        sEulerDeg = MathHelper::QuatToEulerDegrees(item->Rotation);  // helper; gimbal-lock at ±90° pitch is acceptable.
    }

    bool changed = false;
    ImGui::Text("Transform");
    changed |= ImGui::DragFloat3("Position", &item->Position.x, 0.1f);
    if (ImGui::DragFloat3("Rotation (deg)", &sEulerDeg.x, 0.1f)) {
        item->Rotation = MathHelper::EulerDegreesToQuat(sEulerDeg);  // boundary conversion
        changed = true;
    }
    changed |= ImGui::DragFloat3("Scale", &item->Scale.x, 0.01f);

    // Scale min-clamp.
    item->Scale.x = std::max(item->Scale.x, 0.001f);
    item->Scale.y = std::max(item->Scale.y, 0.001f);
    item->Scale.z = std::max(item->Scale.z, 0.001f);

    // Name
    ImGui::Text("Name: %s", item->Name.empty() ? "<unnamed>" : item->Name.c_str());
    ImGui::Separator();

    // Mesh source display name
    const char* meshName = item->Geo ? item->Geo->Name.c_str() : "<missing>";
    ImGui::Text("Mesh: %s", meshName);
    ImGui::Separator();

    // Material display name
    const char* matName = item->Mat ? item->Mat->Name.c_str() : "<missing>";
    ImGui::Text("Material: %s", matName);
    if (ImGui::BeginCombo("Material", matName)) {
        for (auto& [handle, matPtr] : Materials().All()) {
            bool isSelected = (item->materialHandle == handle);
            if (ImGui::Selectable(matPtr->Name.c_str(), isSelected)) {
                item->Mat = matPtr.get();
                item->materialHandle = handle;
                item->NumFramesDirty = gNumFrameResources;
                MarkSceneDirty();
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();
    
    // RenderLayer
    auto* scene = mSceneManager.GetActive();
    // fixed RenderLayer. if RenderLayer can be added/deleted, that requires recompiling of app
    constexpr uint32_t kAll =
        (1u << static_cast<uint32_t>(RenderLayer::Count)) - 1u;
    if (ImGui::BeginCombo("Layers", LayerMaskSummary(item->Layers))) {
        if (ImGui::Selectable("Nothing", item->Layers == 0,
            ImGuiSelectableFlags_DontClosePopups)) {
            SetLayers(*this, scene, item, 0);
        }
        if (ImGui::Selectable("Everything", item->Layers == kAll,
            ImGuiSelectableFlags_DontClosePopups)) {
            SetLayers(*this, scene, item, kAll);
        }
        ImGui::Separator();

        // Per-layer rows. Checkbox doesn't close the popup by default in ImGui,
        // so multiple toggles per popup-open session work naturally.
        for (uint32_t i = 0; i < static_cast<uint32_t>(RenderLayer::Count); ++i) {
            const uint32_t bit = 1u << i;
            bool on = (item->Layers & bit) != 0;
            if (ImGui::Checkbox(RenderLayerName(static_cast<RenderLayer>(i)), &on)) {
                const uint32_t newMask = on ? (item->Layers | bit)
                    : (item->Layers & ~bit);
                SetLayers(*this, scene, item, newMask);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    // World matrix in collapsing header. 
    // Row major is how DirectXMath works in memory - translation lives in m[3][0..2]
    // this is tranposed to column major before sending to cbuffer (hlsl)
    if (ImGui::CollapsingHeader("World matrix")) {
        for (int row = 0; row < 4; ++row) {
            ImGui::Text("%.3f %.3f %.3f %.3f",
                item->World.m[row][0], item->World.m[row][1],
                item->World.m[row][2], item->World.m[row][3]);
        }
    }
    ImGui::Separator();
    if (changed) {
        item->NumFramesDirty = gNumFrameResources;
        MarkSceneDirty();
    }

    ImGui::End();
}
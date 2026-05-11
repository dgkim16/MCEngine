#include "MCEngine.h"
#include "MCScene.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

RenderItem* MCEngine::CurrentSelectedItem() const {
    auto* scene = mSceneManager.GetActive();
    if (!scene) return nullptr;
    auto& allRitems = scene->allRitems;
    if (mSelectedItemIndex < 0 ||
        mSelectedItemIndex >= static_cast<int>(allRitems.size())) {
        return nullptr;
    }
    return allRitems[mSelectedItemIndex].get();
}


/* 
Features proposed for implementation
1. Filter by layer (extend into filter by component)
2. Search by name
3. ... and all other featuers of Unity's hierarchy
*/

void MCEngine::IMGUI_OUTLINER() {
    ImGui::Begin("MVP Hierarchy");
	static bool mOutlinerOpen = true;
	ImGui::Checkbox("Open Outliner", &mOutlinerOpen);
    if (!mOutlinerOpen) {
        ImGui::End();
        return;
    }
    auto* scene = mSceneManager.GetActive();
    if (!scene) {
        ImGui::End();
        return;
    }

    auto& allRitems = scene->allRitems;

    for (int i = 0; i < static_cast<int>(allRitems.size()); ++i) {
        auto* ritem = allRitems[i].get();

        ImGuiSelectableFlags selFlags = ImGuiSelectableFlags_SpanAllColumns;
        bool isSelected = (i == mSelectedItemIndex);

        // Item label — fall back to "item[N]" if Name is empty.
        std::string slabel = ritem->Name.empty()
            ? "item[" + std::to_string(i) + "]"
            : ritem->Name;
        const char* label = slabel.c_str();

        if (ImGui::Selectable(label, isSelected, selFlags)) {
            mSelectedItemIndex = i;
        }
    }

    ImGui::End();
}
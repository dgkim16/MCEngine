#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"
#include "Renderer/D3D12/FrameGraph/FrameGraph.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

namespace {

// Pass x resource matrix, after Unity's Render Graph Viewer. Columns are passes (live ones
// in execution order, then culled ones), rows are resources in first-use order. A cell is
// green when the pass reads the resource, red when it writes it, split along the diagonal
// (red upper-left, green lower-right) when it does both, and grey while the resource is
// alive between its first and last use in a live pass.
void DrawResourceUsageTable(const FrameGraph::FgPanelData& snap) {
    constexpr uint8_t kRead = 1, kWrite = 2;
    const ImU32 readCol  = IM_COL32(181, 204,  80, 255);
    const ImU32 writeCol = IM_COL32(236, 104,  80, 255);
    const ImU32 aliveCol = IM_COL32(110, 110, 110, 255);

    std::vector<const FrameGraph::FgPanelRow*> passes;
    for (const auto& row : snap.rows) if (!row.culled) passes.push_back(&row);
    std::sort(passes.begin(), passes.end(),
        [](const auto* a, const auto* b) { return a->execIdx < b->execIdx; });
    const size_t liveCount = passes.size();
    for (const auto& row : snap.rows) if (row.culled) passes.push_back(&row);

    std::vector<std::string> labels;
    labels.reserve(passes.size());
    for (const auto* p : passes)
        labels.push_back(p->culled ? std::string(p->name) + " (culled)" : std::string(p->name));

    // access[resource][pass column] = kRead | kWrite
    std::vector<std::string> resNames;
    std::unordered_map<std::string, size_t> resIdx;
    std::vector<std::vector<uint8_t>> access;
    for (size_t p = 0; p < passes.size(); ++p) {
        for (const auto& u : passes[p]->usages) {
            const std::string name = (u.resource && *u.resource) ? u.resource : "<unnamed>";
            const auto [it, inserted] = resIdx.try_emplace(name, resNames.size());
            if (inserted) {
                resNames.push_back(name);
                access.emplace_back(passes.size(), uint8_t(0));
            }
            access[it->second][p] |= u.isWrite ? kWrite : kRead;
        }
    }
    if (passes.empty() || resNames.empty()) {
        ImGui::TextUnformatted("No compiled passes.");
        return;
    }

    // Square cells: column width and row content height are both one frame height.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cell = ImGui::GetFrameHeight();
    // Full content height, so the window scrolls instead of the table; ScrollX still needs an
    // explicit outer height. Angled row height mirrors ImGui::TableAngledHeadersRowEx: the
    // angle is measured from vertical, and label width includes the swapped cell padding.
    float maxLabelW = 0.f;
    for (const auto& l : labels) maxLabelW = (std::max)(maxLabelW, ImGui::CalcTextSize(l.c_str()).x);
    maxLabelW += style.CellPadding.y * 2.f;
    const float angle = style.TableAngledHeadersAngle; // radians
    const float angledH = maxLabelW * std::fabs(std::cos(angle))
        + (ImGui::GetFontSize() + style.CellPadding.x * 2.f) * std::fabs(std::sin(angle));
    const float headerH = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.f;
    const float rowH = cell + style.CellPadding.y * 2.f;
    const float tableH = std::ceil(angledH) + headerH + rowH * resNames.size()
        + style.ScrollbarSize + style.CellPadding.y * 2.f + 4.f; // + outer borders

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX
        | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter
        | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_HighlightHoveredColumn;
    if (!ImGui::BeginTable("fg_resource_usage", static_cast<int>(passes.size()) + 1, flags, ImVec2(0.f, tableH)))
        return;

    ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide);
    for (const auto& l : labels)
        ImGui::TableSetupColumn(l.c_str(), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_AngledHeader, cell);
    ImGui::TableSetupScrollFreeze(1, 0); // keep the resource-name column while scrolling passes
    ImGui::TableAngledHeadersRow();      // must be the first row
    ImGui::TableHeadersRow();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t r = 0; r < resNames.size(); ++r) {
        size_t first = SIZE_MAX, last = 0; // lifetime over live columns
        for (size_t p = 0; p < liveCount; ++p)
            if (access[r][p]) { first = (std::min)(first, p); last = p; }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(resNames[r].c_str());

        for (size_t p = 0; p < passes.size(); ++p) {
            ImGui::TableSetColumnIndex(static_cast<int>(p) + 1);
            const uint8_t acc = access[r][p];
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const ImVec2 b(a.x + cell, a.y + cell);
            const auto col = [&](ImU32 c) { return passes[p]->culled ? ((c & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 80)) : c; };

            if (acc == (kRead | kWrite)) {
                dl->AddTriangleFilled(a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), col(writeCol));
                dl->AddTriangleFilled(ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), col(readCol));
            } else if (acc == kWrite) {
                dl->AddRectFilled(a, b, col(writeCol));
            } else if (acc == kRead) {
                dl->AddRectFilled(a, b, col(readCol));
            } else if (first != SIZE_MAX && p > first && p < last) {
                dl->AddRectFilled(a, b, aliveCol);
            }

            ImGui::Dummy(ImVec2(cell, cell));
            if (acc && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s: %s", labels[p].c_str(), resNames[r].c_str(),
                    acc == (kRead | kWrite) ? "read + write" : (acc == kWrite ? "write" : "read"));
        }
    }
    ImGui::EndTable();
}

} // namespace


void MCEngine::IMGUI_FRAMEGRAPH() {
    if (!mShowFrameGraphViewer) return;
    FrameGraph::FgPanelData snap = mFrameGraph->BuildPanelSnapshot(mGpuTimer);
    snap.compileMs = mCpuTimer.GetAverageMs("FrameGraph::Compile");

    ImGui::Begin("Frame Graph"); {
        const double toMB = 1.0 / (1024.0 * 1024.0);
        const double aliased = snap.bytesAliased * toMB;
        const double noAlias = snap.bytesNoAlias * toMB;
        const double saved = noAlias - aliased;
        const double pct = (noAlias > 0.0) ? (saved / noAlias) * 100.0 : 0.0;

        ImGui::Text("Transient memory: %.2f MB", aliased);
        ImGui::Text("  (without aliasing: %.2f MB; saved %.2f MB / %.0f%%)", noAlias, saved, pct);
        ImGui::Text("Passes: %u live, %u culled", snap.liveCount, snap.culledCount);
        if (snap.compileMs < 0.f)
            ImGui::TextUnformatted("FrameGraph::Compile: n/a (Step 3.1 pending)");
        else
            ImGui::Text("FrameGraph::Compile: %.3f ms / frame (avg 60)", snap.compileMs);

        ImGui::Separator();

        if (ImGui::BeginTabBar("fg_tabs")) {
            if (ImGui::BeginTabItem("Resource usage")) {
                DrawResourceUsageTable(snap);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Pass list")) {
                // Same table flags as the Frame Profiles panel (MC_imgui.cpp:590).
                if (ImGui::BeginTable("fg_passes", 6,
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Pass");
                    ImGui::TableSetupColumn("R");
                    ImGui::TableSetupColumn("W");
                    ImGui::TableSetupColumn("Alias over");
                    ImGui::TableSetupColumn("Culled");
                    ImGui::TableSetupColumn("ms");
                    ImGui::TableHeadersRow();

                    for (const auto& row : snap.rows) {
                        ImGui::TableNextRow();
                        const bool dim = row.culled;
                        if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(row.name);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", row.reads);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u", row.writes);
                        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(row.aliasOver ? row.aliasOver : "");
                        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(row.culled ? "yes" : "");
                        ImGui::TableSetColumnIndex(5);
                        if (row.gpuMs < 0.f) ImGui::TextUnformatted("-");   // see font note
                        else                 ImGui::Text("%.3f", row.gpuMs);

                        if (dim) ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("FrameGraph::Compile specifics")) {
            if (ImGui::BeginTable("fg_compile_specfics", 2,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("compile_stage");
                ImGui::TableSetupColumn("ms");
                ImGui::TableHeadersRow();

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [A] Setup");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][A] Setup"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [B] Compute Live");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs(" [B] Compute Live"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [C] DependencyGraph");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][C] DependencyGraph"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [D] Kahns' algorithm");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][D] Kahns' algorithm"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [E] Compute Transient lifetime");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][E] Compute Transient lifetime"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [F] Transient Heap size determined & created");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][F] Transient Heap size determined & created"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [G] SolveAliasing-bin assignment");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][G] SolveAliasing-bin assignment"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [H] perPassAliasBarrier setup");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][H] perPassAliasBarrier setup"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [I] CreatePlacedResource-Allocation");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][I] CreatePlacedResource-Allocation"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [J] StateTransitionList at pass entry");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][J] StateTransitionList at pass entry"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [K] Materialize mCompiled");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][K] Materialize mCompiled"));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(" [L] commit transient desc to shader visible");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", mCpuTimer.GetAverageMs("[FG::Compile][L] commit transient desc to shader visible"));

                ImGui::EndTable();
            }
        }
    } ImGui::End();

}
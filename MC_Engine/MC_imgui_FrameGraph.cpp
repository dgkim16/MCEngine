#include "MCEngine.h"
#include "MCScene.h"
#include "FrameGraph/FrameGraph.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"


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
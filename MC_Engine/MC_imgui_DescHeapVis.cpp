#include "MCEngine.h"
#include "Scene.h"
#include "DescHeapManager.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

namespace {
	enum class SlotStatus { Free, Allocated, Pending, Reserved };

	ImU32 ColorForStatus(SlotStatus s) {
		switch (s) {
		case SlotStatus::Free:      return IM_COL32(70, 70, 70, 120);
		case SlotStatus::Allocated: return IM_COL32(40, 120, 60, 200);
		case SlotStatus::Pending:   return IM_COL32(180, 150, 40, 200);
		case SlotStatus::Reserved:  return IM_COL32(100, 60, 140, 200);
		}
		return 0;
	}
	const char* StatusText(SlotStatus s) {
		switch (s) {
		case SlotStatus::Free:      return "free";
		case SlotStatus::Allocated: return "alloc";
		case SlotStatus::Pending:   return "pending";
		case SlotStatus::Reserved:  return "reserved";
		}
		return "?";
	}
	SlotStatus StatusOf(const DHInfo& info, int localOff) {
		if (localOff < info.reservedHead) return SlotStatus::Reserved;
		for (auto& e : info.fpl) if (e.idx == localOff) return SlotStatus::Pending;
		if (info.Offset_Set.count(localOff)) return SlotStatus::Allocated;
		return SlotStatus::Free;
	}
	void DrawSummary(const DHInfo& info) {
		const int alloc = (int)info.Offset_Set.size();
		const int pending = (int)info.fpl.size();
		const int reserved = info.reservedHead;
		const int free = info.MaxCapacity - alloc - pending - reserved;
		ImGui::Text("cap=%d  used=%d  free=%d  pending=%d  reserved=%d  lastOff=%d",
			info.MaxCapacity, alloc, free, pending, reserved, info.lastOffset);
	}
	void DrawTableHeader() {
		ImGui::TableSetupColumn("Local", ImGuiTableColumnFlags_WidthFixed, 50);
		ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthFixed, 60);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
	}
	void DrawSlotRow(int localOff, int globalOff, SlotStatus s, const DHSlotInfo& si) {
		ImGui::TableNextRow();
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ColorForStatus(s));
		ImGui::TableNextColumn(); ImGui::Text("%d", localOff);
		ImGui::TableNextColumn(); ImGui::Text("%d", globalOff);
		ImGui::TableNextColumn(); ImGui::TextUnformatted(StatusText(s));
		ImGui::TableNextColumn(); ImGui::TextUnformatted(si.viewKind);
		ImGui::TableNextColumn(); ImGui::TextUnformatted(si.resourceName.c_str());
	}
	void DrawGridCell(int localOff, int globalOff, SlotStatus s, const DHSlotInfo& si) {
		const ImVec4 col = ImGui::ColorConvertU32ToFloat4(ColorForStatus(s));
		char id[32]; std::snprintf(id, sizeof(id), "##dh_%d_%d", globalOff, localOff);
		ImGui::ColorButton(id, col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(14, 14));
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::Text("local=%d  global=%d", localOff, globalOff);
			ImGui::Text("status=%s", StatusText(s));
			ImGui::Text("view=%s", si.viewKind);
			ImGui::Text("res=%s", si.resourceName.c_str());
			ImGui::EndTooltip();
		}
	}
	void DrawGrid(int count, int baseGlobal, const DHInfo& info) {
		const float avail = ImGui::GetContentRegionAvail().x;
		const int per = (int)(std::max)(1.0f, avail / 18.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
		for (int i = 0; i < count; ++i) {
			if (i > 0 && (i % per) != 0) ImGui::SameLine();
			DrawGridCell(i, baseGlobal + i, StatusOf(info, i), info.slotInfo[i]);
		}
		ImGui::PopStyleVar();
	}
	void DrawTable(int count, int baseGlobal, const DHInfo& info, ImGuiTextFilter& filter, const char* id) {
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
		// Fill the tab content area so the table grows/shrinks with the window.
		const ImVec2 outer = ImGui::GetContentRegionAvail();
		if (ImGui::BeginTable(id, 5, flags, outer)) {
			DrawTableHeader();
			for (int i = 0; i < count; ++i) {
				const auto& si = info.slotInfo[i];
				if (!filter.PassFilter(si.resourceName.c_str())) continue;
				DrawSlotRow(i, baseGlobal + i, StatusOf(info, i), si);
			}
			ImGui::EndTable();
		}
	}
} // namespace

static void DrawTier(MC_VIEW_TIER t, int viewMode, ImGuiTextFilter& filter) {
	const auto& csu = DescHeapManager::Get().GetCSU();
	const DHInfo& info = csu.tiers[(int)t];
	const int baseOff = csu.baseOffset[(int)t];
	DrawSummary(info);
	ImGui::Separator();
	if (info.MaxCapacity <= 0) { ImGui::TextDisabled("(tier capacity = 0)"); return; }
	if (viewMode == 0) DrawTable(info.MaxCapacity, baseOff, info, filter, "tier_slots");
	else               DrawGrid(info.MaxCapacity, baseOff, info);
}

static void DrawRtvDsv(bool isRtv, int viewMode, ImGuiTextFilter& filter) {
	const DHInfo& info = isRtv ? DescHeapManager::Get().GetRtv() : DescHeapManager::Get().GetDsv();
	DrawSummary(info);
	ImGui::Separator();
	if (info.MaxCapacity <= 0) { ImGui::TextDisabled("(heap capacity = 0)"); return; }
	if (viewMode == 0) DrawTable(info.MaxCapacity, /*baseGlobal*/0, info, filter, isRtv ? "rtv_slots" : "dsv_slots");
	else               DrawGrid(info.MaxCapacity, /*baseGlobal*/0, info);
}

static void DrawCombined(int viewMode, ImGuiTextFilter& filter) {
	const auto& csu = DescHeapManager::Get().GetCSU();
	const int total = (int)csu.combined->GetDesc().NumDescriptors;
	const int reservedHead = csu.reservedHead;

	int alloc = 0, pending = 0;
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t) {
		alloc += (int)csu.tiers[t].Offset_Set.size();
		pending += (int)csu.tiers[t].fpl.size();
	}
	const int freeCount = total - alloc - pending - reservedHead;
	ImGui::Text("cap=%d  used=%d  free=%d  pending=%d  reserved=%d",
		total, alloc, freeCount, pending, reservedHead);
	ImGui::Separator();

	// Resolve a global combined-heap offset to (status, slotInfo*, region name).
	static const DHSlotInfo kReserved{ "ImGui (reserved)", "-", DXGI_FORMAT_UNKNOWN };
	static const DHSlotInfo kEmpty{};
	auto resolve = [&](int globalOff, SlotStatus& outSt, const DHSlotInfo*& outSi, const char*& outRegion) {
		if (globalOff < reservedHead) {
			outSt = SlotStatus::Reserved; outSi = &kReserved; outRegion = "Reserved"; return;
		}
		for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t) {
			const int base = csu.baseOffset[t];
			const int cap = csu.tiers[t].MaxCapacity;
			if (globalOff >= base && globalOff < base + cap) {
				const int localOff = globalOff - base;
				outSt = StatusOf(csu.tiers[t], localOff);
				outSi = &csu.tiers[t].slotInfo[localOff];
				outRegion = (t == MC_VIEW_TIER_STATIC) ? "Static" : "Dynamic";
				return;
			}
		}
		outSt = SlotStatus::Free; outSi = &kEmpty; outRegion = "-";
		};

	if (viewMode == 0) {
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
		const ImVec2 outer = ImGui::GetContentRegionAvail();
		if (ImGui::BeginTable("combined_slots", 5, flags, outer)) {
			ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 80);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			for (int g = 0; g < total; ++g) {
				SlotStatus st; const DHSlotInfo* si; const char* region;
				resolve(g, st, si, region);
				if (!filter.PassFilter(si->resourceName.c_str())) continue;
				ImGui::TableNextRow();
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ColorForStatus(st));
				ImGui::TableNextColumn(); ImGui::Text("%d", g);
				ImGui::TableNextColumn(); ImGui::TextUnformatted(region);
				ImGui::TableNextColumn(); ImGui::TextUnformatted(StatusText(st));
				ImGui::TableNextColumn(); ImGui::TextUnformatted(si->viewKind);
				ImGui::TableNextColumn(); ImGui::TextUnformatted(si->resourceName.c_str());
			}
			ImGui::EndTable();
		}
	}
	else {
		const float avail = ImGui::GetContentRegionAvail().x;
		const int per = (int)(std::max)(1.0f, avail / 18.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
		for (int g = 0; g < total; ++g) {
			SlotStatus st; const DHSlotInfo* si; const char* region;
			resolve(g, st, si, region);
			if (g > 0 && (g % per) != 0) ImGui::SameLine();
			const ImVec4 col = ImGui::ColorConvertU32ToFloat4(ColorForStatus(st));
			char id[32]; std::snprintf(id, sizeof(id), "##cb_%d", g);
			ImGui::ColorButton(id, col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(14, 14));
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text("global=%d  region=%s", g, region);
				ImGui::Text("status=%s", StatusText(st));
				ImGui::Text("view=%s", si->viewKind);
				ImGui::Text("res=%s", si->resourceName.c_str());
				ImGui::EndTooltip();
			}
		}
		ImGui::PopStyleVar();
	}
}

void MCEngine::IMGUI_UPDATE_DESCHEAP_VIEWER() {
	if (!mShowDescHeapViewer) return;
	ImGui::Begin("Descriptor Heap Viewer", &mShowDescHeapViewer);
	static int viewMode = 0; // 0=Table, 1=Grid
	ImGui::RadioButton("Table", &viewMode, 0); ImGui::SameLine();
	ImGui::RadioButton("Grid", &viewMode, 1);
	static ImGuiTextFilter filter;
	ImGui::SameLine();
	filter.Draw("##filter", 200);

	if (ImGui::BeginTabBar("Heaps")) {
		if (ImGui::BeginTabItem("Combined (ShaderVisible)")) { DrawCombined(viewMode, filter); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("CSU Static")) { DrawTier(MC_VIEW_TIER_STATIC, viewMode, filter); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("CSU Dynamic")) { DrawTier(MC_VIEW_TIER_DYNAMIC, viewMode, filter); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("RTV")) { DrawRtvDsv(/*isRtv*/true, viewMode, filter); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("DSV")) { DrawRtvDsv(/*isRtv*/false, viewMode, filter); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
	}
	ImGui::End();
}
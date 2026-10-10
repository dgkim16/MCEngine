#pragma once
#include <functional>
#include <string>

// D9 Step 1 — asset-manager verification suite.
// Six self-contained tests + RunAll runner. Each test logs
// `[<tag>] PASS\n` or `[<tag>] FAIL — <reason>\n` to OutputDebugString.
// Wired to ImGui buttons in MC_imgui.cpp.
class MCEngine;

namespace MCAssetTests {
	void RunDeterminism(MCEngine& engine);              // 1a — two managers, same name → same handle
	void RunCrossKindCollisionFree();                   // 1b — same name across AssetKinds → distinct hashes
	void RunHashMismatchReject(MCEngine& engine);       // 1c — corrupt .mcmat handle field → LoadFromFile throws
	void RunMigratorV0Reject(MCEngine& engine);         // 1d — scene_v0.json → throws "no converter from version 0"
	void RunMigratorV999Reject(MCEngine& engine);       // 1e — scene_v999.json → throws "file version 999 > ..."
	void RunCrossAssetSameName(MCEngine& engine);       // 1f — cross-kind hashes differ + live-manager round-trip
	void RunAll(MCEngine& engine);                      // 1g — runs 1a-1f sequentially

	// D9 Step 2 — asset_redirects.json edge cases.
	void RunCycleDetection(MCEngine& engine);           // 2a — A→B→A → LoadAssetRedirects throws "cycle"
	void RunDanglingTarget(MCEngine& engine);           // 2b — to-handle not registered → ValidateRedirects throws "dangling"
	void RunMultiRedirect(MCEngine& engine);            // 2c — duplicate `from` → LoadAssetRedirects throws "ambiguous"
	void RunDepth3Chain(MCEngine& engine);              // 2d — A→B→C→D, ResolveAssetRedirect(A) == D
	void RunRecordCycleReject(MCEngine& engine);        // 2f — A→B loaded; Record(B→A) and Record(C→C) throw "cycle", nothing changes
	void RunRecordWritePath(MCEngine& engine);          // 2g — Record appends to the file LoadAssetRedirects read, not assets/asset_redirects.json
	void RunAllRedirects(MCEngine& engine);             // 2e — runs 2a-2d, 2f and 2g sequentially

	// D9 Step 4 — resilience suite. Malformed inputs surface clear errors, not crashes.
	void RunFieldDeletion();                            // 4a — RenderItem JSON missing "Position" → from_json throws
	void RunBadHandleReference(MCEngine& engine);       // 4b — RenderItem.materialHandle bogus → LoadRenderItemsFromJson throws
	void RunResaveEquivalence(MCEngine& engine);        // 4c — Save(Ch7) → parse both → nlohmann json deep-equal
	void RunVersionAbsent(MCEngine& engine);            // 4d — scene file w/o "version" → JsonMigrator throws
	void RunBadAssetVersion(MCEngine& engine);          // 4e — .mcmat w/ version=999 → LoadFromFile throws via Migrator
	void RunSameNameAcrossScenes(MCEngine& engine);     // 4f — two scenes both with renderItem "box" load fine; nameToRitem is per-scene
	void RunAllResilience(MCEngine& engine);            // 4g — runs 4a-4f sequentially

	// D9 Step 6 — outlier-mode edge cases.
	void RunSoloVoid(MCEngine& engine);                 // 6a — empty scene loads + switches without crash
	void RunSoloBox(MCEngine& engine);                  // 6b — single-render-item scene loads + round-trips
	void RunSwitchNonexistent(MCEngine& engine);        // 6c — Switch unknown name → throws with name in message
	void RunSwitchSelf(MCEngine& engine);               // 6d — Switch to active scene → no-op early return
	void RunUnknownModule(MCEngine& engine);            // L9 — scene_unknown_module.json fails to load → no active scene; Ch7 loads after
	void RunAllOutlier(MCEngine& engine);               // 6e — runs 6a-6d and L9 sequentially

	// CBFreeList slot recycling.
	void RunCBFreeListLifo();                           // a released slot is handed out again before the high-water mark grows
	void RunMaterialCapacity(MCEngine& engine);         // T13b — every material slot fits MaterialCB; the one past capacity throws
	void RunBigSceneSwitch(MCEngine& engine);           // T13c — scene_big.json (200 items) <-> SoloBox x20: distinct ObjCB slots inside ObjectCB

#if defined(_DEBUG)
	// Headless test runner hook (--run-tests): while set, every PASS/FAIL is also
	// reported here. The Tests window leaves it unset.
	void SetResultSink(std::function<void(const char* tag, bool passed, const std::string& reason)> sink);
#endif
}

#include "Legacy/MC_AssetTests.h"
#include "Core/LoggingSystem.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCMaterialManager.h"
#include "Legacy/MCMaterial.h"
#include "Core/AssetIdentity.h"
#include "Core/JsonMigrator.h"
#include "Core/CBFreeList.h"
#include "Legacy/MCSceneManager.h"
#include "Legacy/MCScene.h"
#include "Legacy/RenderItem.h"
#include "Legacy/SceneSerialization.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

#if defined(_DEBUG)
std::function<void(const char*, bool, const std::string&)> gResultSink;   // MCAssetTests::SetResultSink
#endif

void Pass(const char* tag) {
	MC::LoggingSystem::Info("[{}] PASS", tag);
#if defined(_DEBUG)
	if (gResultSink) gResultSink(tag, true, std::string());
#endif
}
void Fail(const char* tag, const std::string& reason) {
	MC::LoggingSystem::Error("[{}] FAIL — {}", tag, reason);
#if defined(_DEBUG)
	if (gResultSink) gResultSink(tag, false, reason);
#endif
}

}  // namespace

namespace MCAssetTests {

#if defined(_DEBUG)
void SetResultSink(std::function<void(const char* tag, bool passed, const std::string& reason)> sink) {
	gResultSink = std::move(sink);
}
#endif

// 1a — Cross-process determinism. Resurrected from D7 Step 7f's commented-out
// ImGui block in MC_imgui.cpp (kept as regression-test source). Two manager
// instances, same input, must produce equal handles. XXH64 is pure → result
// is identical on every machine, every process, every refactor.
void RunDeterminism(MCEngine& engine) {
	const char* tag = "1a determinism";
	try {
		MCMaterialManager mgrA(engine), mgrB(engine);
		mgrA.MatCBFreeList().SetCapacity(8);
		mgrB.MatCBFreeList().SetCapacity(8);
		MCMaterial params;
		params.DiffuseAlbedo = { 1, 1, 1, 1 };
		params.Roughness = 0.2f;
		auto h1 = mgrA.Register("woodCrate_test", params, 0);
		auto h2 = mgrB.Register("woodCrate_test", params, 0);
		if (h1 != h2) {
			Fail(tag, "h1=" + std::to_string(h1) + " h2=" + std::to_string(h2));
			return;
		}
		Pass(tag);
	} catch (const std::exception& e) {
		Fail(tag, std::string("exception: ") + e.what());
	}
}

// 1b — Asset-kind tag prefix structurally prevents cross-kind collisions even
// when display names match. HashAssetIdentity feeds a 4-byte AssetKind tag
// before the name → three distinct XXH64 inputs → three distinct outputs.
void RunCrossKindCollisionFree() {
	const char* tag = "1b crossKind";
	auto m = HashAssetIdentity<AssetKind::Material>("woodCrate");
	auto g = HashAssetIdentity<AssetKind::MeshSource>("woodCrate");
	auto t = HashAssetIdentity<AssetKind::Texture>("woodCrate");
	if (m == g || g == t || t == m) {
		Fail(tag, "collision: m=" + std::to_string(m) +
			" g=" + std::to_string(g) +
			" t=" + std::to_string(t));
		return;
	}
	Pass(tag);
}

// 1c — Hash-mismatch reject. The load-bearing assert: if .mcmat's `handle`
// field disagrees with HashAssetIdentity(displayName), LoadFromFile throws.
// Without this gate, a corrupted file silently registers under the wrong
// handle and downstream Get(realHandle) returns nullptr forever.
void RunHashMismatchReject(MCEngine& engine) {
	const char* tag = "1c hashMismatch";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/bad_hash.mcmat";

	// Hardcoded JSON: version=1 (passes Migrator), kind=MCMaterial, but
	// `handle` is bogus relative to displayName "test_bad_hash". Identity
	// matTransform spelled out because MCMaterial::matTransform requires all 16 fields.
	const char* bad = R"({
  "version": 1,
  "kind": "MCMaterial",
  "handle": "999999999999999",
  "displayName": "test_bad_hash",
  "params": {
    "diffuseAlbedo": {"x":1.0,"y":1.0,"z":1.0,"w":1.0},
    "fresnelR0":     {"x":0.05,"y":0.05,"z":0.05},
    "roughness":     0.2,
    "matTransform":  {"m00":1.0,"m01":0.0,"m02":0.0,"m03":0.0,
                      "m10":0.0,"m11":1.0,"m12":0.0,"m13":0.0,
                      "m20":0.0,"m21":0.0,"m22":1.0,"m23":0.0,
                      "m30":0.0,"m31":0.0,"m32":0.0,"m33":1.0}
  }
})";
	{ std::ofstream(tmp) << bad; }

	bool threw = false;
	std::string msg;
	try {
		engine.Materials().LoadFromFile(tmp.string());
	} catch (const std::exception& e) {
		threw = true;
		msg = e.what();
	}

	std::error_code ec; fs::remove(tmp, ec);   // best-effort cleanup

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("hash mismatch") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 1d — JsonMigrator v0 reject. Test fixture committed at assets/test/scene_v0.json.
void RunMigratorV0Reject(MCEngine& engine) {
	const char* tag = "1d migrator v0";
	bool threw = false;
	std::string msg;
	try {
		engine.Migrator().LoadAndMigrate("Assets/test/scene_v0.json");
	} catch (const std::exception& e) {
		threw = true;
		msg = e.what();
	}
	if (!threw) { Fail(tag, "expected throw"); return; }
	if (msg.find("no converter from version 0") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 1e — JsonMigrator v999 reject. Test fixture at assets/test/scene_v999.json.
// Actual error message is "file version 999 > engine version 1. Upgrade the engine."
// (JsonMigrator.cpp:27-29) — match on the version-comparison substring.
void RunMigratorV999Reject(MCEngine& engine) {
	const char* tag = "1e migrator v999";
	bool threw = false;
	std::string msg;
	try {
		engine.Migrator().LoadAndMigrate("Assets/test/scene_v999.json");
	} catch (const std::exception& e) {
		threw = true;
		msg = e.what();
	}
	if (!threw) { Fail(tag, "expected throw"); return; }
	if (msg.find("file version 999") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 1f — Cross-asset name-different-kind. Two proofs:
// (1) Hash level: same name across three AssetKinds → three distinct handles.
// (2) Manager round-trip: HashAssetIdentity<Material>("woodCrate") used as
//     a key into the live engine.Materials() returns the woodCrate material
//     whose Name field round-trips back to "woodCrate". Proves the manager
//     stores by-hash and that authoring name → handle → asset is invertible.
void RunCrossAssetSameName(MCEngine& engine) {
	const char* tag = "1f xAssetName";

	auto m = HashAssetIdentity<AssetKind::Material>("woodCrate");
	auto g = HashAssetIdentity<AssetKind::MeshSource>("woodCrate");
	auto t = HashAssetIdentity<AssetKind::Texture>("woodCrate");
	if (m == g || g == t || t == m) {
		Fail(tag, "cross-kind hash collision");
		return;
	}

	auto* mat = engine.Materials().Get(m);
	if (!mat) {
		Fail(tag, "Materials().Get(hash('woodCrate')) returned nullptr — "
			"is woodCrate.mcmat in Assets/materials/?");
		return;
	}
	if (mat->Name != "woodCrate") {
		Fail(tag, "round-trip name mismatch: hash('woodCrate') resolved to material '" + mat->Name + "'");
		return;
	}
	Pass(tag);
}

void RunAll(MCEngine& engine) {
	MC::LoggingSystem::Info("=== Asset verification suite (D9 Step 1) ===");
	RunDeterminism(engine);
	RunCrossKindCollisionFree();
	RunHashMismatchReject(engine);
	RunMigratorV0Reject(engine);
	RunMigratorV999Reject(engine);
	RunCrossAssetSameName(engine);
	MC::LoggingSystem::Info("=== End ===");
}

// =====================================================================
// D9 Step 2 — asset_redirects.json edge cases
// =====================================================================
//
// Pattern: each test (a) writes a temp redirects file under assets/test/,
// (b) constructs a local MCSceneManager(engine), (c) calls LoadAssetRedirects
// (and ValidateRedirects for 2b, RecordAssetRedirect for 2f and 2g) on the
// temp path, (d) asserts the expected throw, (e) cleans up. The local manager
// keeps the live engine.Scenes() state untouched.
namespace {

void WriteRedirects(const std::filesystem::path& p, const nlohmann::json& redirects) {
	nlohmann::json doc{ {"redirects", redirects} };
	std::ofstream(p) << doc.dump(2);
}

}  // namespace

// 2a — Cycle detection. A→B→A. LoadAssetRedirects must throw "cycle" before
// any caller can invoke ResolveAssetRedirect (which would infinite-loop).
void RunCycleDetection(MCEngine& engine) {
	const char* tag = "2a cycle";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/redirects_cycle.json";
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","200"},{"reason","test"},{"date","2026-05-09"}},
		{{"kind","material"},{"from","200"},{"to","100"},{"reason","test"},{"date","2026-05-09"}},
	}));

	MCSceneManager mgr(engine);
	bool threw = false;
	std::string msg;
	try { mgr.LoadAssetRedirects(tmp.string()); }
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("cycle") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 2b — Dangling target. `to` handle not registered in the kind's manager.
// LoadAssetRedirects passes (no cycle, no dup); ValidateRedirects throws.
void RunDanglingTarget(MCEngine& engine) {
	const char* tag = "2b dangling";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/redirects_dangling.json";
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","99999999999"},{"reason","test"},{"date","2026-05-09"}},
	}));

	MCSceneManager mgr(engine);
	bool threw = false;
	std::string msg;
	try {
		mgr.LoadAssetRedirects(tmp.string());
		mgr.ValidateRedirects();   // queries the live engine.Materials() — handle 99999999999 absent
	}
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("dangling") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 2c — Multi-redirect-from-same-source. Two entries with `from=100`. Second
// silently overwrote first under operator[]; try_emplace + throw catches it.
void RunMultiRedirect(MCEngine& engine) {
	const char* tag = "2c multi";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/redirects_multi.json";
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","200"},{"reason","test"},{"date","2026-05-09"}},
		{{"kind","material"},{"from","100"},{"to","300"},{"reason","test"},{"date","2026-05-09"}},
	}));

	MCSceneManager mgr(engine);
	bool threw = false;
	std::string msg;
	try { mgr.LoadAssetRedirects(tmp.string()); }
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("ambiguous") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 2d — Depth-3 chain. A→B, B→C, C→D. ResolveAssetRedirect(A) == D. Pure
// resolver test; no manager queries (handles bogus). Skip ValidateRedirects.
void RunDepth3Chain(MCEngine& engine) {
	const char* tag = "2d depth3";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/redirects_chain3.json";
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","200"},{"reason","test"},{"date","2026-05-09"}},
		{{"kind","material"},{"from","200"},{"to","300"},{"reason","test"},{"date","2026-05-09"}},
		{{"kind","material"},{"from","300"},{"to","400"},{"reason","test"},{"date","2026-05-09"}},
	}));

	MCSceneManager mgr(engine);
	std::uint64_t resolved = 0;
	std::string msg;
	bool threw = false;
	try {
		mgr.LoadAssetRedirects(tmp.string());
		resolved = mgr.ResolveAssetRedirect(100);
	}
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (threw) { Fail(tag, std::string("unexpected throw: ") + msg); return; }
	if (resolved != 400) {
		Fail(tag, "ResolveAssetRedirect(100) = " + std::to_string(resolved) + ", expected 400");
		return;
	}
	Pass(tag);
}

// 2f — Record rejects a cycle (L13). With A→B loaded, recording B→A or the
// self-redirect C→C throws "cycle" before anything changes: the table still
// resolves A to B, and the redirect file still holds its one entry.
void RunRecordCycleReject(MCEngine& engine) {
	const char* tag = "2f recordCycle";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/redirects_record_cycle.json";
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","200"},{"reason","test"},{"date","2026-09-29"}},
	}));

	MCSceneManager mgr(engine);
	auto recordError = [&mgr](std::uint64_t from, std::uint64_t to) {
		try { mgr.RecordAssetRedirect("material", from, to, "test"); }
		catch (const std::exception& e) { return std::string(e.what()); }
		return std::string();
	};
	std::string reason;
	try {
		mgr.LoadAssetRedirects(tmp.string());
		const std::string back = recordError(200, 100);
		const std::string self = recordError(300, 300);
		if (back.find("cycle") == std::string::npos)
			reason = "Record(200 -> 100): " + (back.empty() ? std::string("no throw") : "wrong message: " + back);
		else if (self.find("cycle") == std::string::npos)
			reason = "Record(300 -> 300): " + (self.empty() ? std::string("no throw") : "wrong message: " + self);
		else {
			nlohmann::json doc;
			std::ifstream(tmp) >> doc;
			if (mgr.ResolveAssetRedirect(100) != 200 || mgr.ResolveAssetRedirect(200) != 200 ||
				mgr.ResolveAssetRedirect(300) != 300)
				reason = "a rejected record changed the table";
			else if (doc["redirects"].size() != 1)
				reason = "a rejected record changed the file: " + doc.dump();
		}
	}
	catch (const std::exception& e) { reason = std::string("unexpected throw: ") + e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!reason.empty()) { Fail(tag, reason); return; }
	Pass(tag);
}

// 2g — Record writes the file LoadAssetRedirects read (L13), through <file>.tmp,
// and leaves assets/asset_redirects.json alone. If a regression writes the live
// file, the test puts its old content back before it fails.
void RunRecordWritePath(MCEngine& engine) {
	const char* tag = "2g recordPath";
	namespace fs = std::filesystem;
	const fs::path live = "Assets/asset_redirects.json";
	fs::path tmp = "Assets/test/redirects_record_path.json";
	fs::path tmpTmp = tmp; tmpTmp += ".tmp";
	auto readAll = [](const fs::path& p) {
		std::ifstream in(p, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	};
	std::error_code ec;
	const bool liveExisted = fs::exists(live, ec);
	const std::string liveBefore = readAll(live);
	WriteRedirects(tmp, nlohmann::json::array({
		{{"kind","material"},{"from","100"},{"to","200"},{"reason","test"},{"date","2026-09-29"}},
	}));

	MCSceneManager mgr(engine);
	std::string reason;
	try {
		mgr.LoadAssetRedirects(tmp.string());
		mgr.RecordAssetRedirect("material", 200, 300, "test");
		nlohmann::json doc;
		std::ifstream(tmp) >> doc;
		const auto& r = doc["redirects"];
		if (r.size() != 2 || r[1].value("from", "") != "200" || r[1].value("to", "") != "300")
			reason = "the loaded file does not end with the new entry: " + doc.dump();
		else if (mgr.ResolveAssetRedirect(100) != 300)
			reason = "ResolveAssetRedirect(100) = " + std::to_string(mgr.ResolveAssetRedirect(100)) + ", expected 300";
		else if (fs::exists(tmpTmp, ec))
			reason = tmpTmp.string() + " was left behind";
	}
	catch (const std::exception& e) { reason = std::string("unexpected throw: ") + e.what(); }

	if (fs::exists(live, ec) != liveExisted || readAll(live) != liveBefore) {
		if (liveExisted) std::ofstream(live, std::ios::binary) << liveBefore;
		else fs::remove(live, ec);
		reason = "Record wrote " + live.string() + " (restored)";
	}
	fs::remove(tmp, ec);
	fs::remove(tmpTmp, ec);

	if (!reason.empty()) { Fail(tag, reason); return; }
	Pass(tag);
}

void RunAllRedirects(MCEngine& engine) {
	MC::LoggingSystem::Info("=== Redirect verification suite (D9 Step 2) ===");
	RunCycleDetection(engine);
	RunDanglingTarget(engine);
	RunMultiRedirect(engine);
	RunDepth3Chain(engine);
	RunRecordCycleReject(engine);
	RunRecordWritePath(engine);
	MC::LoggingSystem::Info("=== End ===");
}

// =====================================================================
// D9 Step 4 — resilience suite
// =====================================================================
//
// Malformed inputs (missing fields, bogus handles, missing version, bad
// asset version) must surface clear errors, not crash. Each test uses a
// temp file under assets/test/ + cleanup; no live state polluted.
// ObjCBIndex slots from local-mgr scenes auto-release via ~MCScene →
// MCScene::Clear when the local mgr goes out of scope.

namespace {

// Identity 4x4 matrix as nlohmann::json object.
nlohmann::json IdentityMat4() {
	return {
		{"m00",1.0},{"m01",0.0},{"m02",0.0},{"m03",0.0},
		{"m10",0.0},{"m11",1.0},{"m12",0.0},{"m13",0.0},
		{"m20",0.0},{"m21",0.0},{"m22",1.0},{"m23",0.0},
		{"m30",0.0},{"m31",0.0},{"m32",0.0},{"m33",1.0},
	};
}

// Build a one-renderItem scene JSON. Uses woodCrate (10583501380713305711) +
// ch7_box_mesh (6727357290574742156) so the live managers' Get/Resolve succeed.
nlohmann::json BuildSimpleScene(const std::string& sceneName, const std::string& itemName) {
	using nlohmann::json;
	return {
		{"version", 1},
		{"sceneName", sceneName},
		{"materialHandles", json::array({"10583501380713305711"})},
		{"textureHandles",  json::array()},
		{"meshes", json::array({{
			{"kind","Procedural_Box"},{"name","ch7_box_mesh"},
			{"params", {{"d",1.5},{"h",0.5},{"subdivisions",3},{"w",1.5}}}
		}})},
		{"specialPointers", json::object()},
		{"modules", json::array()},
		{"renderItems", json::array({{
			{"Name", itemName},
			{"Position", {{"x",0.0},{"y",0.5},{"z",0.0}}},
			{"Rotation", {{"x",0.0},{"y",0.0},{"z",0.0},{"w",1.0}}},
			{"Scale",    {{"x",1.0},{"y",1.0},{"z",1.0}}},
			{"TexTransform", IdentityMat4()},
			{"PrimitiveType", 4},
			{"useCellCulling", false},
			{"checkBounds", true},
			{"layers", json::array({"Opaque"})},
			{"materialHandle", "10583501380713305711"},
			{"meshHandle",     "6727357290574742156"},
		}})},
	};
}

}  // namespace

// 4a — Field deletion. RenderItem JSON missing "Position" → from_json's
// j.at("Position") throws. Plan said submeshName but that field is no
// longer read (SceneSerialization.cpp:87). Position is unambiguously required.
void RunFieldDeletion() {
	const char* tag = "4a fieldDeletion";
	using nlohmann::json;
	json bad = {
		// Position deliberately missing.
		{"Rotation",     {{"x",0.0},{"y",0.0},{"z",0.0},{"w",1.0}}},
		{"Scale",        {{"x",1.0},{"y",1.0},{"z",1.0}}},
		{"TexTransform", IdentityMat4()},
		{"Name",         "test"},
		{"PrimitiveType", 4},
		{"useCellCulling", false},
		{"checkBounds",  true},
		{"layers",       json::array({"Opaque"})},
		{"materialHandle","10583501380713305711"},
		{"meshHandle",   "6727357290574742156"},
	};
	bool threw = false;
	std::string msg;
	try {
		RenderItem r;
		from_json(bad, r);   // ADL → ::from_json in global namespace
	} catch (const std::exception& e) { threw = true; msg = e.what(); }

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("Position") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 4b — Bad handle reference. RenderItem.materialHandle = bogus → after
// 0.5b's Resolve (identity for unknown handles) → mMaterialManager.Get
// returns nullptr → LoadRenderItemsFromJson throws "unknown material handle X".
void RunBadHandleReference(MCEngine& engine) {
	const char* tag = "4b badHandle";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/scene_bad_handle.json";

	auto scn = BuildSimpleScene("BadHandleTest", "bogus_box");
	scn["materialHandles"] = nlohmann::json::array({"99999999999"});  // bogus
	scn["renderItems"][0]["materialHandle"] = "99999999999";           // bogus
	std::ofstream(tmp) << scn.dump(2);

	// Construct the scene directly — the manager only holds one active scene now,
	// and this test must not touch the engine's live scene or frame resources.
	bool threw = false;
	std::string msg;
	try {
		MCScene scene("BadHandleTest");
		scene.LoadFromJson(engine, engine.Migrator().LoadAndMigrate(tmp.string()));
	}
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find("unknown material handle") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 4c — Re-save equivalence. Save the live Ch7 to a temp; parse source +
// resaved; compare nlohmann::json deep-equal (key order + whitespace
// independent). Field-set mismatch → load+save dropped or invented fields.
//
// On FAIL the temp file is kept at assets/test/scene_ch7_resaved.json so the
// diff can be inspected manually. On PASS the file is removed.
void RunResaveEquivalence(MCEngine& engine) {
	const char* tag = "4c resaveEquiv";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/scene_ch7_resaved.json";

	// Recursive diff: returns "" if equal-within-tolerance, else dotted/indexed
	// path to first differing leaf. Numbers compare with abs tolerance 1e-6 to
	// absorb float-vs-double round-trip noise (engine stores params as `float`;
	// authored doubles like 0.3 become 0.30000001192092896 after float→double
	// promotion on save). Plan's "field-set-identical" is interpreted as
	// "structure preserved + values equal within single-precision noise".
	std::function<std::string(const nlohmann::json&, const nlohmann::json&, const std::string&)> findDiff;
	findDiff = [&findDiff](const nlohmann::json& a, const nlohmann::json& b, const std::string& path) -> std::string {
		// Numeric tolerance — covers (int vs float) and (float vs float) noise.
		if (a.is_number() && b.is_number()) {
			double da = a.get<double>(), db = b.get<double>();
			if (std::abs(da - db) < 1e-6) return "";
			return path + " (" + a.dump() + " vs " + b.dump() + ")";
		}
		if (a.type() != b.type()) {
			return path + " (type " + std::string(a.type_name()) + " vs " + std::string(b.type_name()) + ")";
		}
		if (a.is_object()) {
			for (auto it = a.begin(); it != a.end(); ++it) {
				if (!b.contains(it.key())) return path + "." + it.key() + " (missing in resaved)";
				auto sub = findDiff(it.value(), b.at(it.key()), path + "." + it.key());
				if (!sub.empty()) return sub;
			}
			for (auto it = b.begin(); it != b.end(); ++it) {
				if (!a.contains(it.key())) return path + "." + it.key() + " (extra in resaved)";
			}
			return "";
		}
		if (a.is_array()) {
			if (a.size() != b.size()) {
				return path + " (size " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")";
			}
			for (size_t i = 0; i < a.size(); ++i) {
				auto sub = findDiff(a[i], b[i], path + "[" + std::to_string(i) + "]");
				if (!sub.empty()) return sub;
			}
			return "";
		}
		if (a != b) {
			return path + " (" + a.dump() + " vs " + b.dump() + ")";
		}
		return "";
	};

	bool ok = false;
	std::string msg;
	try {
		engine.Scenes().Save("Ch7", tmp.string());

		std::ifstream srcS("Assets/scenes/scene_ch7.json");
		std::ifstream rsvS(tmp);
		auto src = nlohmann::json::parse(srcS);
		auto rsv = nlohmann::json::parse(rsvS);
		// Use findDiff for both equality + diagnosis. Empty result = equivalent.
		auto diff = findDiff(src, rsv, "");
		ok = diff.empty();
		if (!ok) msg = diff;
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	if (ok) { std::error_code ec; fs::remove(tmp, ec); Pass(tag); return; }
	Fail(tag, (msg.empty() ? "field-set diff" : msg) + " (resaved file kept at " + tmp.string() + ")");
}

// 4d — Version field absent. JsonMigrator throws "missing or non-integer 'version' field".
void RunVersionAbsent(MCEngine& engine) {
	const char* tag = "4d versionAbsent";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/scene_no_version.json";

	auto scn = BuildSimpleScene("NoVersion", "x");
	scn.erase("version");
	std::ofstream(tmp) << scn.dump(2);

	bool threw = false;
	std::string msg;
	try { engine.Migrator().LoadAndMigrate(tmp); }
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw"); return; }
	if (msg.find("'version'") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 4e — Bad .mcmat version. JsonMigrator throws on v999 inside LoadFromFile,
// before any kind/handle check. Same code path as 1e applied to a .mcmat.
void RunBadAssetVersion(MCEngine& engine) {
	const char* tag = "4e badAssetVersion";
	namespace fs = std::filesystem;
	fs::path tmp = "Assets/test/bad_version.mcmat";

	const char* bad = R"({
  "version": 999,
  "kind": "MCMaterial",
  "handle": "0",
  "displayName": "x",
  "params": {
    "diffuseAlbedo": {"x":1.0,"y":1.0,"z":1.0,"w":1.0},
    "fresnelR0":     {"x":0.05,"y":0.05,"z":0.05},
    "roughness":     0.2,
    "matTransform":  {"m00":1.0,"m01":0.0,"m02":0.0,"m03":0.0,
                      "m10":0.0,"m11":1.0,"m12":0.0,"m13":0.0,
                      "m20":0.0,"m21":0.0,"m22":1.0,"m23":0.0,
                      "m30":0.0,"m31":0.0,"m32":0.0,"m33":1.0}
  }
})";
	{ std::ofstream(tmp) << bad; }

	bool threw = false;
	std::string msg;
	try { engine.Materials().LoadFromFile(tmp.string()); }
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	std::error_code ec; fs::remove(tmp, ec);

	if (!threw) { Fail(tag, "expected throw"); return; }
	if (msg.find("999") == std::string::npos) {
		Fail(tag, "wrong message: " + msg);
		return;
	}
	Pass(tag);
}

// 4f — Same render-item name across scenes. Two temp scene files (TestA,
// TestB), each with a renderItem named "box", loaded into two MCScene locals
// (the manager only holds one active scene under load-on-switch). Both
// nameToRitem maps own a "box" entry, pointing at distinct RenderItem*.
// Proves nameToRitem is per-scene, not global.
//
// ObjCBIndex slots are released back to mObjCBFreeList automatically when
// the locals go out of scope and ~MCScene → Clear fires. No leak.
void RunSameNameAcrossScenes(MCEngine& engine) {
	const char* tag = "4f sameNameXScenes";
	namespace fs = std::filesystem;
	fs::path tmpA = "Assets/test/scene_dup_a.json";
	fs::path tmpB = "Assets/test/scene_dup_b.json";

	{ std::ofstream(tmpA) << BuildSimpleScene("TestA", "box").dump(2); }
	{ std::ofstream(tmpB) << BuildSimpleScene("TestB", "box").dump(2); }

	bool ok = false;
	std::string msg;
	try {
		MCScene a("TestA"), b("TestB");
		a.LoadFromJson(engine, engine.Migrator().LoadAndMigrate(tmpA.string()));
		b.LoadFromJson(engine, engine.Migrator().LoadAndMigrate(tmpB.string()));
		if (!a.nameToRitem.count("box"))                 { msg = "TestA has no 'box'";   ok = false; }
		else if (!b.nameToRitem.count("box"))            { msg = "TestB has no 'box'";   ok = false; }
		else if (a.nameToRitem.at("box") == b.nameToRitem.at("box")) {
			msg = "TestA and TestB share the same RenderItem*"; ok = false;
		}
		else { ok = true; }
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	std::error_code ec;
	fs::remove(tmpA, ec);
	fs::remove(tmpB, ec);

	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

void RunAllResilience(MCEngine& engine) {
	MC::LoggingSystem::Info("=== Resilience suite (D9 Step 4) ===");
	RunFieldDeletion();
	RunBadHandleReference(engine);
	RunResaveEquivalence(engine);
	RunVersionAbsent(engine);
	RunBadAssetVersion(engine);
	RunSameNameAcrossScenes(engine);
	MC::LoggingSystem::Info("=== End ===");
}

// =====================================================================
// D9 Step 6 — outlier-mode edge cases
// =====================================================================
//
// 6a/6b switch the active scene to test fixtures (SoloVoid, SoloBox) and
// restore Ch7 at the end so subsequent tests start from a known state.
// 6c/6d are pure programmatic checks against MCSceneManager::Switch's
// error-naming and self-switch early-return behavior.

namespace {

// Restore the active scene to "Ch7" if a test changed it. Tolerates a throw —
// outer test reports its own FAIL. SwitchSceneNow brackets the cmdlist: under
// load-on-switch every Switch loads from disk and records GPU uploads, and the
// cmdlist is closed at ImGui-handler time.
void RestoreCh7(MCEngine& engine) {
	try { engine.SwitchSceneNow("Ch7"); }
	catch (const std::exception& e) {
		MC::LoggingSystem::Warning("[outlier] WARN restoring Ch7: {}", e.what());
	}
}

}  // namespace

// 6a — Empty scene with no render items. Boots, switches, viewport blank.
void RunSoloVoid(MCEngine& engine) {
	const char* tag = "6a soloVoid";
	bool ok = false;
	std::string msg;
	try {
		engine.SwitchSceneNow("SoloVoid");
		auto* a = engine.Scenes().GetActive();
		if (!a)                              msg = "no active scene after Switch";
		else if (a->name != "SoloVoid")      msg = "active='" + a->name + "', expected 'SoloVoid'";
		else if (!a->allRitems.empty())      msg = "allRitems should be empty, has " + std::to_string(a->allRitems.size());
		else                                  ok = true;
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	RestoreCh7(engine);
	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

// 6b — Scene with one render item. Switch + verify item count + round-trip.
void RunSoloBox(MCEngine& engine) {
	const char* tag = "6b soloBox";
	bool ok = false;
	std::string msg;
	try {
		engine.SwitchSceneNow("SoloBox");
		auto* a = engine.Scenes().GetActive();
		if (!a)                                msg = "no active scene after Switch";
		else if (a->name != "SoloBox")         msg = "active='" + a->name + "', expected 'SoloBox'";
		else if (a->allRitems.size() != 1)     msg = "allRitems.size()=" + std::to_string(a->allRitems.size()) + ", expected 1";
		else {
			// Round-trip via the production path: save shape-checks ToJson,
			// reload re-reads the registered file. Throws on JSON shape errors.
			// The probe goes to %TEMP%, never the working directory.
			engine.Scenes().SaveActive((std::filesystem::temp_directory_path() / "scene_test_full.json").string());
			engine.ReloadActiveSceneNow(false);
			ok = true;
		}
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	RestoreCh7(engine);
	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

// 6c — Switch to a scene that doesn't exist. Throw must name the missing scene.
void RunSwitchNonexistent(MCEngine& engine) {
	const char* tag = "6c switchNonexistent";
	const char* missing = "Nonexistent_xyz_6c";
	bool threw = false;
	std::string msg;
	try { engine.SwitchSceneNow(missing); }
	catch (const std::exception& e) { threw = true; msg = e.what(); }

	if (!threw) { Fail(tag, "expected throw, got none"); return; }
	if (msg.find(missing) == std::string::npos) {
		Fail(tag, "error message missing scene name '" + std::string(missing) + "': " + msg);
		return;
	}
	Pass(tag);
}

// 6d — Switch to the currently active scene. Early return; no double-activation.
// Snapshot the active pointer + name and verify both unchanged after Switch.
void RunSwitchSelf(MCEngine& engine) {
	const char* tag = "6d switchSelf";
	bool ok = false;
	std::string msg;
	try {
		// Pre-flight: ensure Ch7 is active.
		if (!engine.Scenes().GetActive() || engine.Scenes().GetActive()->name != "Ch7") {
			engine.SwitchSceneNow("Ch7");
		}
		auto* before = engine.Scenes().GetActive();
		const std::string beforeName = before ? before->name : std::string{};
		// Direct manager call is safe here: same-name Switch early-returns before
		// any disk load or GPU recording, so no bracket is needed — that early
		// return is exactly what this test verifies.
		engine.Scenes().Switch("Ch7");   // no-op
		auto* after = engine.Scenes().GetActive();
		if (after != before)                  msg = "active pointer changed (early-return path violated)";
		else if (!after || after->name != beforeName) msg = "name changed";
		else                                  ok = true;
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

// L9 step 1 — a scene that fails mid-load leaves no active scene, not a half-built
// one. The fixture's unknown module type throws after its mesh uploaded and its
// item took an ObjCB slot. A switch back to Ch7 must then work.
void RunUnknownModule(MCEngine& engine) {
	const char* tag = "L9 unknownModule";
	bool ok = false;
	std::string msg;
	try {
		engine.Scenes().SetScenePath("UnknownModule", "Assets/test/scene_unknown_module.json");
		std::string err;
		try { engine.SwitchSceneNow("UnknownModule"); }
		catch (const std::exception& e) { err = e.what(); }

		if (err.empty())                                               msg = "expected throw, got none";
		else if (err.find("unknown module type") == std::string::npos) msg = "wrong message: " + err;
		else if (engine.Scenes().GetActive())                          msg = "a scene is still active after the failed load";
		else {
			engine.SwitchSceneNow("Ch7");
			auto* a = engine.Scenes().GetActive();
			if (!a || a->name != "Ch7") msg = "switching back to Ch7 did not activate it";
			else                        ok = true;
		}
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	RestoreCh7(engine);
	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

void RunAllOutlier(MCEngine& engine) {
	MC::LoggingSystem::Info("=== Outlier suite (D9 Step 6) ===");
	RunSoloVoid(engine);
	RunSoloBox(engine);
	RunSwitchNonexistent(engine);
	RunSwitchSelf(engine);
	RunUnknownModule(engine);
	MC::LoggingSystem::Info("=== End ===");
}

// =====================================================================
// CBFreeList slot recycling
// =====================================================================

// Two slots, release slot 0: the next Allocate reuses 0 instead of growing,
// and the one after that grows to 2.
void RunCBFreeListLifo() {
	const char* tag = "CBFreeList LIFO";
	std::string msg;
	try {
		CBFreeList fl;
		fl.SetCapacity(10);
		const std::uint32_t first = fl.Allocate();
		const std::uint32_t second = fl.Allocate();
		if (first != 0 || second != 1) {
			msg = "first slots " + std::to_string(first) + ", " + std::to_string(second) + ", expected 0, 1";
		}
		else {
			fl.Release(0);
			const std::uint32_t recycled = fl.Allocate();
			const std::uint32_t grown = fl.Allocate();
			if (recycled != 0)            msg = "Allocate after Release(0) = " + std::to_string(recycled) + ", expected 0";
			else if (grown != 2)          msg = "next Allocate = " + std::to_string(grown) + ", expected 2";
			else if (fl.HighWater() != 3) msg = "HighWater = " + std::to_string(fl.HighWater()) + ", expected 3";
		}
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	if (!msg.empty()) { Fail(tag, msg); return; }
	Pass(tag);
}

// T13b, S2 form (L7). MaterialCB used to hold the materials loaded at the last
// scene build plus 32, while the material free list hands out slots up to its
// capacity, so the 33rd material created without a switch was written past the
// buffer. The materials here are created in memory by a local manager with the
// engine's free-list capacity (256) and are never saved. Every slot handed out
// must fit the MaterialCB of every frame resource, and the material past
// capacity (the 257th) must throw instead of reaching UpdateMaterialCBs.
void RunMaterialCapacity(MCEngine& engine) {
	const char* tag = "T13b materialCapacity";
	std::string msg;
	try {
		const std::uint32_t capacity = engine.Materials().MatCBFreeList().Capacity();
		const auto& frames = engine.sceneAccess().FrameResources;
		std::uint64_t cbElements = frames.empty() ? 0 : UINT64_MAX;
		for (const auto& fr : frames)
			cbElements = std::min<std::uint64_t>(cbElements,
				fr->MaterialCB->Resource()->GetDesc().Width / fr->MaterialCB->ElementByteSize());

		MCMaterialManager mgr(engine);
		mgr.MatCBFreeList().SetCapacity(capacity);
		MCMaterial params;
		for (std::uint32_t i = 0; i < capacity && msg.empty(); ++i) {
			const MCMaterial* mat = mgr.Get(mgr.Register("t13b_material_" + std::to_string(i), params, 0));
			if (mat->MatCBIndex < 0 || static_cast<std::uint64_t>(mat->MatCBIndex) >= cbElements)
				msg = "material " + std::to_string(i + 1) + " got slot " + std::to_string(mat->MatCBIndex)
					+ ", outside the " + std::to_string(cbElements) + "-element MaterialCB";
		}
		if (msg.empty()) {
			try {
				mgr.Register("t13b_material_" + std::to_string(capacity), params, 0);
				msg = "material " + std::to_string(capacity + 1) + " was created past the free-list capacity";
			} catch (const std::exception& e) {
				if (std::string(e.what()).find("CBFreeList exhausted") == std::string::npos)
					msg = "material " + std::to_string(capacity + 1) + " failed with: " + e.what();
			}
		}
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	if (!msg.empty()) { Fail(tag, msg); return; }
	Pass(tag);
}

// T13c (L8). The 200-item scene_big.json and the 1-item SoloBox, switched 20
// times each way. Before the fix every load renumbered ObjCBIndex over slots the
// free list still counted as free, and the object capacity stopped at the boot
// scene's items plus headroom, so the first load of Big threw "CBFreeList
// exhausted". Every load must put its items on distinct slots inside ObjectCB;
// a slot released twice trips the Debug assert in CBFreeList::Release.
void RunBigSceneSwitch(MCEngine& engine) {
	const char* tag = "T13c bigSceneSwitch";
	std::string msg;
	try {
		engine.Scenes().SetScenePath("Big", "Assets/test/scene_big.json");
		for (int step = 0; step < 40 && msg.empty(); ++step) {
			const std::string name = step % 2 == 0 ? "Big" : "SoloBox";
			const std::size_t expected = step % 2 == 0 ? 200 : 1;
			engine.SwitchSceneNow(name);

			const MCScene* a = engine.Scenes().GetActive();
			std::vector<UINT> slots;
			if (a) for (const auto& ri : a->allRitems) slots.push_back(ri->ObjCBIndex);
			std::sort(slots.begin(), slots.end());
			std::uint64_t cbElements = UINT64_MAX;
			for (const auto& fr : engine.sceneAccess().FrameResources)
				cbElements = std::min<std::uint64_t>(cbElements,
					fr->ObjectCB->Resource()->GetDesc().Width / fr->ObjectCB->ElementByteSize());

			const std::string at = "switch " + std::to_string(step + 1) + " to " + name + ": ";
			if (!a || a->name != name)
				msg = at + "not active";
			else if (slots.size() != expected)
				msg = at + std::to_string(slots.size()) + " items, expected " + std::to_string(expected);
			else if (std::adjacent_find(slots.begin(), slots.end()) != slots.end())
				msg = at + "two items share an ObjCB slot";
			else if (slots.back() >= cbElements)
				msg = at + "slot " + std::to_string(slots.back()) + " is outside the "
					+ std::to_string(cbElements) + "-element ObjectCB";
		}
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	RestoreCh7(engine);
	if (!msg.empty()) { Fail(tag, msg); return; }
	Pass(tag);
}

}  // namespace MCAssetTests

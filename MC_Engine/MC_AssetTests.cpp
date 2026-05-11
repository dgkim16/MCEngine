#include "MC_AssetTests.h"
#include "MCEngine.h"
#include "MCMaterialManager.h"
#include "MCMaterial.h"
#include "MCAssetIdentity.h"
#include "JsonMigrator.h"
#include "MCSceneManager.h"
#include "MCScene.h"
#include "RenderItem.h"
#include "SceneSerialization.h"
#include <nlohmann/json.hpp>
#include <Windows.h>          // OutputDebugStringA
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void Pass(const char* tag) {
	OutputDebugStringA(("[" + std::string(tag) + "] PASS\n").c_str());
}
void Fail(const char* tag, const std::string& reason) {
	OutputDebugStringA(("[" + std::string(tag) + "] FAIL — " + reason + "\n").c_str());
}

}  // namespace

namespace MCAssetTests {

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
	fs::path tmp = "assets/test/bad_hash.mcmat";

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
		engine.Migrator().LoadAndMigrate("assets/test/scene_v0.json");
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
		engine.Migrator().LoadAndMigrate("assets/test/scene_v999.json");
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
			"is woodCrate.mcmat in assets/materials/?");
		return;
	}
	if (mat->Name != "woodCrate") {
		Fail(tag, "round-trip name mismatch: hash('woodCrate') resolved to material '" + mat->Name + "'");
		return;
	}
	Pass(tag);
}

void RunAll(MCEngine& engine) {
	OutputDebugStringA("=== Asset verification suite (D9 Step 1) ===\n");
	RunDeterminism(engine);
	RunCrossKindCollisionFree();
	RunHashMismatchReject(engine);
	RunMigratorV0Reject(engine);
	RunMigratorV999Reject(engine);
	RunCrossAssetSameName(engine);
	OutputDebugStringA("=== End ===\n");
}

// =====================================================================
// D9 Step 2 — asset_redirects.json edge cases
// =====================================================================
//
// Pattern: each test (a) writes a temp redirects file under assets/test/,
// (b) constructs a local MCSceneManager(engine), (c) calls LoadAssetRedirects
// (and ValidateRedirects for 2b) on the temp path, (d) asserts the expected
// throw, (e) cleans up. The local manager keeps the live engine.Scenes()
// state untouched.
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
	fs::path tmp = "assets/test/redirects_cycle.json";
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
	fs::path tmp = "assets/test/redirects_dangling.json";
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
	fs::path tmp = "assets/test/redirects_multi.json";
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
	fs::path tmp = "assets/test/redirects_chain3.json";
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

void RunAllRedirects(MCEngine& engine) {
	OutputDebugStringA("=== Redirect verification suite (D9 Step 2) ===\n");
	RunCycleDetection(engine);
	RunDanglingTarget(engine);
	RunMultiRedirect(engine);
	RunDepth3Chain(engine);
	OutputDebugStringA("=== End ===\n");
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
	fs::path tmp = "assets/test/scene_bad_handle.json";

	auto scn = BuildSimpleScene("BadHandleTest", "bogus_box");
	scn["materialHandles"] = nlohmann::json::array({"99999999999"});  // bogus
	scn["renderItems"][0]["materialHandle"] = "99999999999";           // bogus
	std::ofstream(tmp) << scn.dump(2);

	MCSceneManager mgr(engine);
	bool threw = false;
	std::string msg;
	try { mgr.LoadFromJson("BadHandleTest", tmp.string()); }
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
	fs::path tmp = "assets/test/scene_ch7_resaved.json";

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

		std::ifstream srcS("assets/scenes/scene_ch7.json");
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
	fs::path tmp = "assets/test/scene_no_version.json";

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
	fs::path tmp = "assets/test/bad_version.mcmat";

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
// TestB), each with a renderItem named "box". One MCSceneManager loads
// both. Both nameToRitem maps own a "box" entry, pointing at distinct
// RenderItem*. Proves nameToRitem is per-scene, not global.
//
// ObjCBIndex slots are released back to mObjCBFreeList automatically when
// the local mgr goes out of scope and ~MCScene → Clear fires. No leak.
void RunSameNameAcrossScenes(MCEngine& engine) {
	const char* tag = "4f sameNameXScenes";
	namespace fs = std::filesystem;
	fs::path tmpA = "assets/test/scene_dup_a.json";
	fs::path tmpB = "assets/test/scene_dup_b.json";

	{ std::ofstream(tmpA) << BuildSimpleScene("TestA", "box").dump(2); }
	{ std::ofstream(tmpB) << BuildSimpleScene("TestB", "box").dump(2); }

	bool ok = false;
	std::string msg;
	try {
		MCSceneManager mgr(engine);
		mgr.LoadFromJson("TestA", tmpA.string());
		mgr.LoadFromJson("TestB", tmpB.string());
		auto* a = mgr.Get("TestA");
		auto* b = mgr.Get("TestB");
		if (!a || !b)                                    { msg = "scene not loaded";     ok = false; }
		else if (!a->nameToRitem.count("box"))           { msg = "TestA has no 'box'";   ok = false; }
		else if (!b->nameToRitem.count("box"))           { msg = "TestB has no 'box'";   ok = false; }
		else if (a->nameToRitem.at("box") == b->nameToRitem.at("box")) {
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
	OutputDebugStringA("=== Resilience suite (D9 Step 4) ===\n");
	RunFieldDeletion();
	RunBadHandleReference(engine);
	RunResaveEquivalence(engine);
	RunVersionAbsent(engine);
	RunBadAssetVersion(engine);
	RunSameNameAcrossScenes(engine);
	OutputDebugStringA("=== End ===\n");
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

// Restore the active scene to "Ch7" if a test changed it. Tolerates Switch
// throwing — outer test reports its own FAIL. ReloadActiveSceneNow is not
// needed because the fixture scenes are already loaded; Switch is enough.
void RestoreCh7(MCEngine& engine) {
	try { engine.Scenes().Switch("Ch7"); }
	catch (const std::exception& e) {
		OutputDebugStringA(("[outlier] WARN restoring Ch7: " + std::string(e.what()) + "\n").c_str());
	}
}

}  // namespace

// 6a — Empty scene with no render items. Boots, switches, viewport blank.
void RunSoloVoid(MCEngine& engine) {
	const char* tag = "6a soloVoid";
	bool ok = false;
	std::string msg;
	try {
		engine.Scenes().Switch("SoloVoid");
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
		engine.Scenes().Switch("SoloBox");
		auto* a = engine.Scenes().GetActive();
		if (!a)                                msg = "no active scene after Switch";
		else if (a->name != "SoloBox")         msg = "active='" + a->name + "', expected 'SoloBox'";
		else if (a->allRitems.size() != 1)     msg = "allRitems.size()=" + std::to_string(a->allRitems.size()) + ", expected 1";
		else {
			// Round-trip via the existing path. Throws on JSON shape errors.
			engine.Scenes().TestRoundTripActiveScene();
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
	try { engine.Scenes().Switch(missing); }
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
			engine.Scenes().Switch("Ch7");
		}
		auto* before = engine.Scenes().GetActive();
		const std::string beforeName = before ? before->name : std::string{};
		engine.Scenes().Switch("Ch7");   // no-op
		auto* after = engine.Scenes().GetActive();
		if (after != before)                  msg = "active pointer changed (early-return path violated)";
		else if (!after || after->name != beforeName) msg = "name changed";
		else                                  ok = true;
	} catch (const std::exception& e) { msg = std::string("exception: ") + e.what(); }

	if (!ok) { Fail(tag, msg); return; }
	Pass(tag);
}

void RunAllOutlier(MCEngine& engine) {
	OutputDebugStringA("=== Outlier suite (D9 Step 6) ===\n");
	RunSoloVoid(engine);
	RunSoloBox(engine);
	RunSwitchNonexistent(engine);
	RunSwitchSelf(engine);
	OutputDebugStringA("=== End ===\n");
}

}  // namespace MCAssetTests

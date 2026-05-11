#pragma once
#include "MCAssetIdentity.h"
#include "MCMeshSource.h"
#include "MCMeshGeometry.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class MCEngine;

class MCMeshSourceManager {
public:
	explicit MCMeshSourceManager(MCEngine& engine);

	// Register a source. The manager builds the GPU geometry from (kind, params)
	// — every kind under the v2 (drop-merging) model is data-driven. Caller MUST
	// pass source.geometry == nullptr; the manager populates it via BuildProcedural.
	// Engine's command list must be open at call time (CreateDefaultBuffer records
	// upload commands onto it).
	//
	// Idempotent on duplicate displayName with matching kind. Throws if the
	// displayName is already registered with a DIFFERENT kind (caller bug).
	MeshHandle Register(const std::string& displayName, MCMeshSource source);

	MCMeshSource*   GetSource(MeshHandle h);
	MCMeshGeometry* GetGeometry(MeshHandle h);  // shorthand for GetSource(h)->geometry.get()

	bool       IsLoaded(MeshHandle h) const;
	MeshHandle Find(const std::string& displayName) const;

	void   RegisterUsedBy(MeshHandle h, const std::string& sceneName);		// unord_map { (key=MeshHandle) : value=set{,...,sceneName} }
	void   UnregisterUsedBy(MeshHandle h, const std::string& sceneName);	// remove from unord_map, and erase key if no scene reference it.
	const std::set<std::string>& ScenesUsing(MeshHandle h) const;

	// Rename the asset to newDisplayName. Updates in-memory map, rekeys the
	// geometry's DrawArgs map (drop-merging invariant: single entry keyed by
	// Geo->Name), and records an asset redirect so existing scene JSONs
	// that still reference the old handle resolve to the new one on next load.
	// Throws std::runtime_error on (1) old handle not found, (2) new name already registered.
	MeshHandle Rename(MeshHandle oldHandle, const std::string& newDisplayName);

	const std::unordered_map<MeshHandle, MCMeshSource>& All() const { return mSources; }

	// Drop every MCMeshSource (and its owned MCMeshGeometry). Called from
	// MCEngine's destructor before ReportLiveObjects so VB/IB ComPtrs release
	// while the device is still alive and don't show as leaks.
	void Clear() { mSources.clear(); mUsedBy.clear(); }

private:
	std::unique_ptr<MCMeshGeometry> BuildProcedural(const std::string& displayName, const MCMeshSource& s);

	// Procedural_RandomSpritePoints needs a different vertex stride
	// (TreeSpriteVertex {Pos, Size}, not MCVertex), so it bypasses BuildProcedural's
	// MCVertex pipeline and assembles its own MCMeshGeometry.
	std::unique_ptr<MCMeshGeometry> BuildRandomSpritePoints(
		const std::string& displayName,
		const MCMeshSource::RandomSpritePointsParams& p);

	MCEngine& mEngine;
	std::unordered_map<MeshHandle, MCMeshSource>          mSources;
	std::unordered_map<MeshHandle, std::set<std::string>> mUsedBy;
	static const std::set<std::string> kEmptyScenes;
};

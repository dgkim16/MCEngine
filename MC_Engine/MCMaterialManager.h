#pragma once
#include "MCAssetIdentity.h"
#include "CBFreeList.h"
#include "MCMaterial.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class MCEngine;

class MCMaterialManager {
public:
	explicit MCMaterialManager(MCEngine& engine);
	MaterialHandle LoadFromFile(const std::string& path);
	void           LoadDirectory(const std::string& dirPath);

	void Save(MaterialHandle h, const std::string& path) const;
	void SaveAll(const std::string& dirPath) const; // Q for claude : why not use string_view?

	void Reload(MaterialHandle h);

	MCMaterial* Get(MaterialHandle h);
	bool IsLoaded(MaterialHandle h) const;
	MaterialHandle Find(const std::string& displayName) const; // Q for claude : why not use string_view?

	void   RegisterUsedBy(MaterialHandle h, const std::string& sceneName);
	void   UnregisterUsedBy(MaterialHandle h, const std::string& sceneName);
	const std::set<std::string>& ScenesUsing(MaterialHandle h) const;
	std::vector<MaterialHandle> UnusedMaterials() const;

	// Rename the asset to newDisplayName. Updates in-memory map + .mcmat file on disk
	// + records an asset redirect so existing scene JSONs that still reference the
	// old handle resolve to the new one on next load.
	// Throws std::runtime_error on (1) old handle not found, (2) new name already registered.
	MaterialHandle Rename(MaterialHandle oldHandle, const std::string& newDisplayName);

	// Accessor renamed from `CBFreeList()` to avoid shadowing the class name `CBFreeList`.
	CBFreeList& MatCBFreeList() { return mMatCBFreeList; }

	const std::unordered_map<MaterialHandle, std::unique_ptr<MCMaterial>>& All() const {
		return mMaterials;
	}
	MaterialHandle Register(const std::string& displayName, const MCMaterial& params, TextureHandle texH);


private:
	MCEngine& mEngine;
	std::unordered_map<MaterialHandle, std::unique_ptr<MCMaterial>> mMaterials;
	std::unordered_map<MaterialHandle, std::string>               mMaterialPaths;
	std::unordered_map<MaterialHandle, std::set<std::string>>     mUsedBy;
	CBFreeList mMatCBFreeList;

	static const std::set<std::string> kEmptyScenes;
};
#include "MCMaterialManager.h"
#include "MCEngine.h"
#include "MCSceneManager.h"   // RecordAssetRedirect
#include <filesystem>          // .mcmat file rename in Rename
#include <fstream>
#include <stdexcept>
#include <system_error>

const std::set<std::string> MCMaterialManager::kEmptyScenes;

MCMaterialManager::MCMaterialManager(MCEngine& engine) : mEngine(engine) {}

MaterialHandle MCMaterialManager::LoadFromFile(const std::string& path) {
    auto j = mEngine.Migrator().LoadAndMigrate(path);
    if (j.at("kind") != "MCMaterial") {
        throw std::runtime_error(path + ": expected kind 'MCMaterial'");
    }
    auto displayName = j.at("displayName").get<std::string>();
    auto storedHandle = std::stoull(j.at("handle").get<std::string>());
    auto computedHandle = HashAssetIdentity<AssetKind::Material>(displayName);
    if (storedHandle != computedHandle) {
        throw std::runtime_error(path + ": hash mismatch — stored " +
            std::to_string(storedHandle) + " vs computed " +
            std::to_string(computedHandle));
    }

    MCMaterial params;
    params.Name = displayName;
    auto& p = j.at("params");
    p.at("diffuseAlbedo").get_to(params.DiffuseAlbedo);
    p.at("fresnelR0").get_to(params.FresnelR0);
    p.at("roughness").get_to(params.Roughness);
    p.at("matTransform").get_to(params.MatTransform);

    TextureHandle texH = 0;
    if (j.contains("textureHandle")) {
        texH = std::stoull(j.at("textureHandle").get<std::string>());
    }

    auto h = Register(displayName, params, texH);
    mMaterialPaths[h] = path;
    return h;
}

void MCMaterialManager::LoadDirectory(const std::string& dirPath) {
    for (auto& entry : std::filesystem::directory_iterator(dirPath)) {
        if (entry.path().extension() == ".mcmat") {
            LoadFromFile(entry.path().string());
        }
    }
}

MaterialHandle MCMaterialManager::Register(const std::string& displayName,
    const MCMaterial& params,
    TextureHandle texH) {
    auto h = HashAssetIdentity<AssetKind::Material>(displayName);
    auto it = mMaterials.find(h);
    if (it != mMaterials.end()) {
        // Idempotent if same params; throw on conflict.
        // (For Phase 1 simplicity, accept the existing — Phase 2 can add a strict-compare check.)
        return h;
    }

    auto mat = std::make_unique<MCMaterial>(params);
    mat->Name = displayName;
    mat->textureHandle = texH;
    mat->MatCBIndex = static_cast<int>(mMatCBFreeList.Allocate());
    mat->NumFramesDirty = gNumFrameResources;

    mMaterials[h] = std::move(mat);
    return h;
}

void MCMaterialManager::Save(MaterialHandle h, const std::string& path) const {
    auto it = mMaterials.find(h);
    if (it == mMaterials.end()) {
        throw std::runtime_error("Save: material not loaded");
    }
    const auto& m = *it->second;

    nlohmann::json j;
    j["version"] = 1;
    j["kind"] = "MCMaterial";
    j["handle"] = std::to_string(h);
    j["displayName"] = m.Name;
    j["textureHandle"] = std::to_string(m.textureHandle);
    j["params"] = {
        {"diffuseAlbedo", m.DiffuseAlbedo},
        {"fresnelR0",     m.FresnelR0},
        {"roughness",     m.Roughness},
        {"matTransform",  m.MatTransform},
    };
    std::ofstream(path) << j.dump(2);
}

void MCMaterialManager::SaveAll(const std::string& dirPath) const {
    std::filesystem::create_directories(dirPath);
    for (const auto& [h, mat] : mMaterials) {
        Save(h, dirPath + "/" + mat->Name + ".mcmat");
    }
}

void MCMaterialManager::Reload(MaterialHandle h) {
    auto pIt = mMaterialPaths.find(h);
    if (pIt == mMaterialPaths.end()) {
        throw std::runtime_error("Reload: handle has no path on file");
    }
    auto* existing = Get(h);
    if (!existing) {
        throw std::runtime_error("Reload: handle not loaded");
    }
    auto savedMatCBIndex = existing->MatCBIndex;

    auto j = mEngine.Migrator().LoadAndMigrate(pIt->second);
    auto& p = j.at("params");
    p.at("diffuseAlbedo").get_to(existing->DiffuseAlbedo);
    p.at("fresnelR0").get_to(existing->FresnelR0);
    p.at("roughness").get_to(existing->Roughness);
    p.at("matTransform").get_to(existing->MatTransform);
    if (j.contains("textureHandle")) {
        existing->textureHandle = std::stoull(j.at("textureHandle").get<std::string>());
    }
    existing->MatCBIndex = savedMatCBIndex;     // preserve CB slot
    existing->NumFramesDirty = gNumFrameResources;  // mark all frame copies stale
}

MCMaterial* MCMaterialManager::Get(MaterialHandle h) {
    auto it = mMaterials.find(h);
    return it == mMaterials.end() ? nullptr : it->second.get();
}

bool MCMaterialManager::IsLoaded(MaterialHandle h) const {
    return mMaterials.count(h) > 0;
}

MaterialHandle MCMaterialManager::Find(const std::string& displayName) const {
    auto h = HashAssetIdentity<AssetKind::Material>(displayName);
    return mMaterials.count(h) ? h : 0;
}

void MCMaterialManager::RegisterUsedBy(MaterialHandle h, const std::string& sceneName) {
    mUsedBy[h].insert(sceneName);
}

void MCMaterialManager::UnregisterUsedBy(MaterialHandle h, const std::string& sceneName) {
    auto it = mUsedBy.find(h);
    if (it != mUsedBy.end()) it->second.erase(sceneName);
}

const std::set<std::string>& MCMaterialManager::ScenesUsing(MaterialHandle h) const {
    auto it = mUsedBy.find(h);
    return it == mUsedBy.end() ? kEmptyScenes : it->second;
}

std::vector<MaterialHandle> MCMaterialManager::UnusedMaterials() const {
    std::vector<MaterialHandle> out;
    for (const auto& [h, _] : mMaterials) {
        auto it = mUsedBy.find(h);
        if (it == mUsedBy.end() || it->second.empty()) out.push_back(h);
    }
    return out;
}

// D8 Step 9b — Rename a registered material.
// Mutations in order: in-memory map (insert new before erase old; exception safety),
// mUsedBy map, .mcmat file on disk + re-save (so file's internal name matches its
// filename), then RecordAssetRedirect so future scene loads can resolve old handle → new.
// No rollback if RecordAssetRedirect or filesystem rename throws — Phase 1 accepted
// partial-state hazard. Recovery: re-save scenes / re-emit asset_redirects.json by hand.
MaterialHandle MCMaterialManager::Rename(MaterialHandle oldHandle, const std::string& newDisplayName) {
    auto it = mMaterials.find(oldHandle);
    if (it == mMaterials.end())
        throw std::runtime_error("MCMaterialManager::Rename: handle not loaded");
    auto newHandle = HashAssetIdentity<AssetKind::Material>(newDisplayName);
    if (mMaterials.count(newHandle))
        throw std::runtime_error("MCMaterialManager::Rename: target name '" + newDisplayName + "' already registered");

    auto mat = std::move(it->second);
    mat->Name = newDisplayName;
    mMaterials.emplace(newHandle, std::move(mat));   // insert before erase — exception safety
    mMaterials.erase(oldHandle);

    if (auto uIt = mUsedBy.find(oldHandle); uIt != mUsedBy.end()) {
        mUsedBy[newHandle] = std::move(uIt->second);
        mUsedBy.erase(uIt);
    }

    // Rename .mcmat file on disk + re-save so the file's internal displayName/handle
    // fields match its filename. Best-effort: filesystem failure logs but does not
    // roll back. Mirror of MCTextureManager::Rename's pattern (Step 9d).
    if (auto pIt = mMaterialPaths.find(oldHandle); pIt != mMaterialPaths.end()) {
        std::filesystem::path oldPath = pIt->second;
        std::filesystem::path newPath = oldPath.parent_path() / (newDisplayName + ".mcmat");
        std::error_code ec;
        std::filesystem::rename(oldPath, newPath, ec);
        mMaterialPaths.erase(pIt);
        mMaterialPaths[newHandle] = newPath.string();
        if (ec) OutputDebugStringA(("MCMaterialManager::Rename: filesystem rename failed: " + ec.message() + "\n").c_str());
        Save(newHandle, mMaterialPaths[newHandle]);
    }

    mEngine.Scenes().RecordAssetRedirect("material", oldHandle, newHandle, "Rename via API");
    return newHandle;
}
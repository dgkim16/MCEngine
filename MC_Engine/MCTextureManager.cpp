#include "MCTextureManager.h"
#include "MCEngine.h"
#include "MCSceneManager.h"   // RecordAssetRedirect (Step 9d)
#include "DescHeapManager.h"
#include "Common/d3dUtil.h"   // AnsiToWString, ThrowIfFailed
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

const std::set<std::string> MCTextureManager::kEmptyScenes;

MCTextureManager::MCTextureManager(MCEngine& engine) : mEngine(engine) {}

// LoadFromFile reads the .mctex JSON, materializes both the asset record and
// its GPU resource. The engine's command list must be open when this is
// called; the caller (typically LoadDirectory + engine init) batches the
// command-list close/execute/flush after all assets are loaded so a single
// fence wait covers the whole batch. Until that flush completes, every
// MCTextureResource::UploadHeap created here must stay alive.
TextureHandle MCTextureManager::LoadFromFile(const std::string& path) {
    auto j = mEngine.Migrator().LoadAndMigrate(path);
    if (j.at("kind") != "MCTexture") {
        throw std::runtime_error(path + ": expected kind 'MCTexture'");
    }
    auto displayName    = j.at("displayName").get<std::string>();
    auto storedHandle   = std::stoull(j.at("handle").get<std::string>());
    auto computedHandle = HashAssetIdentity<AssetKind::Texture>(displayName);
    if (storedHandle != computedHandle) {
        throw std::runtime_error(path + ": hash mismatch — stored " +
            std::to_string(storedHandle) + " vs computed " +
            std::to_string(computedHandle));
    }

    // Idempotent: if the same handle is already loaded, return it. Phase 2 can
    // strict-compare assetPath/params and throw on conflict.
    if (auto it = mTextures.find(computedHandle); it != mTextures.end()) {
        return computedHandle;
    }

    MCTexture asset;
    asset.Name      = displayName;
    asset.assetPath = j.at("assetPath").get<std::string>();
    if (j.contains("params")) {
        const auto& p = j.at("params");
        if (p.contains("sRGB"))         asset.sRGB         = p.at("sRGB").get<bool>();
        if (p.contains("generateMips")) asset.generateMips = p.at("generateMips").get<bool>();
        if (p.contains("addressU"))     asset.addressU     = p.at("addressU").get<std::string>();
        if (p.contains("addressV"))     asset.addressV     = p.at("addressV").get<std::string>();
    }

    // Realize the asset on the GPU. The MCTextureResource is built locally,
    // wired through the descriptor manager, then handed to the asset.
    auto res = std::make_unique<MCTextureResource>();
    res->Name     = displayName;
    res->Filename = AnsiToWString(asset.assetPath);

    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(
        mEngine.GetDevice(),
        mEngine.GetCmdList(),
        res->Filename.c_str(),
        res->mResource,
        res->UploadHeap));

    res->mResource->SetName(res->Filename.c_str());
    res->UploadHeap->SetName((L"UPLOAD_" + res->Filename).c_str());

    // SRV in the static tier; consumers read asset.resource->SRVs[0].offset
    // as the bindless slot.
    DescHeapManager::Get().CreateSrv2d(*res, res->mResource->GetDesc().Format);

    asset.resource = std::move(res);
    mTextures[computedHandle]     = std::move(asset);
    mTexturePaths[computedHandle] = path;
    return computedHandle;
}

void MCTextureManager::LoadDirectory(const std::string& dirPath) {
    if (!std::filesystem::exists(dirPath)) return;
    for (auto& entry : std::filesystem::directory_iterator(dirPath)) {
        if (entry.path().extension() == ".mctex") {
            LoadFromFile(entry.path().string());
        }
    }
    // Command-list close/execute/flush is the caller's responsibility — the
    // engine init path batches multiple subsystems into one execution.
}

void MCTextureManager::Save(TextureHandle h, const std::string& path) const {
    auto it = mTextures.find(h);
    if (it == mTextures.end()) {
        throw std::runtime_error("MCTextureManager::Save: texture not loaded (handle " +
            std::to_string(h) + ")");
    }
    const auto& tex = it->second;

    nlohmann::json j;
    j["version"]     = 1;
    j["kind"]        = "MCTexture";
    j["handle"]      = std::to_string(h);
    j["displayName"] = tex.Name;
    j["assetPath"]   = tex.assetPath;
    j["params"] = {
        {"sRGB",         tex.sRGB},
        {"generateMips", tex.generateMips},
        {"addressU",     tex.addressU},
        {"addressV",     tex.addressV},
    };
    std::ofstream(path) << j.dump(2);
}

void MCTextureManager::SaveAll(const std::string& dirPath) const {
    std::filesystem::create_directories(dirPath);
    for (const auto& [h, tex] : mTextures) {
        Save(h, dirPath + "/" + tex.Name + ".mctex");
    }
}

// Reload re-reads the .mctex file and updates sampler/mip params in place.
// AssetPath changes are rejected: re-uploading the DDS would require freeing
// the existing GPU resource through the deferred-removal path, which is
// Phase 2 work. To swap a DDS source today, unload + reload manually.
void MCTextureManager::Reload(TextureHandle h) {
    auto pathIt = mTexturePaths.find(h);
    if (pathIt == mTexturePaths.end()) {
        throw std::runtime_error("MCTextureManager::Reload: handle has no path on file");
    }
    auto* asset = Get(h);
    if (!asset) {
        throw std::runtime_error("MCTextureManager::Reload: handle not loaded");
    }

    auto j = mEngine.Migrator().LoadAndMigrate(pathIt->second);
    auto newAssetPath = j.at("assetPath").get<std::string>();
    if (newAssetPath != asset->assetPath) {
        throw std::runtime_error(
            "MCTextureManager::Reload: assetPath changed (" + asset->assetPath +
            " -> " + newAssetPath + "). Phase 1 reload only refreshes sampler/mip "
            "params; manually unload + reload to swap the DDS source.");
    }
    if (j.contains("params")) {
        const auto& p = j.at("params");
        if (p.contains("sRGB"))         asset->sRGB         = p.at("sRGB").get<bool>();
        if (p.contains("generateMips")) asset->generateMips = p.at("generateMips").get<bool>();
        if (p.contains("addressU"))     asset->addressU     = p.at("addressU").get<std::string>();
        if (p.contains("addressV"))     asset->addressV     = p.at("addressV").get<std::string>();
    }
}

MCTexture* MCTextureManager::Get(TextureHandle h) {
    auto it = mTextures.find(h);
    return it == mTextures.end() ? nullptr : &it->second;
}

bool MCTextureManager::IsLoaded(TextureHandle h) const {
    return mTextures.count(h) > 0;
}

TextureHandle MCTextureManager::Find(const std::string& displayName) const {
    auto h = HashAssetIdentity<AssetKind::Texture>(displayName);
    return mTextures.count(h) ? h : 0;
}

void MCTextureManager::RegisterUsedBy(TextureHandle h, const std::string& sceneName) {
    mUsedBy[h].insert(sceneName);
}

void MCTextureManager::UnregisterUsedBy(TextureHandle h, const std::string& sceneName) {
    auto it = mUsedBy.find(h);
    if (it != mUsedBy.end()) it->second.erase(sceneName);
}

const std::set<std::string>& MCTextureManager::ScenesUsing(TextureHandle h) const {
    auto it = mUsedBy.find(h);
    return it == mUsedBy.end() ? kEmptyScenes : it->second;
}

// D8 Step 9d — Rename a registered texture.
// Renames the .mctex JSON on disk + re-saves so the file's internal name matches
// its filename. The underlying .dds (MCTexture::assetPath) is untouched — texture
// content didn't change. RecordAssetRedirect emits the redirect so existing scene
// JSONs that still reference the old handle resolve to the new one on next load.
// No rollback if filesystem rename or RecordAssetRedirect throws — Phase 1 accepted
// partial-state hazard.
TextureHandle MCTextureManager::Rename(TextureHandle oldHandle, const std::string& newDisplayName) {
    auto it = mTextures.find(oldHandle);
    if (it == mTextures.end())
        throw std::runtime_error("MCTextureManager::Rename: handle not loaded");
    auto newHandle = HashAssetIdentity<AssetKind::Texture>(newDisplayName);
    if (mTextures.count(newHandle))
        throw std::runtime_error("MCTextureManager::Rename: target name '" + newDisplayName + "' already registered");

    MCTexture tex = std::move(it->second);
    tex.Name = newDisplayName;
    // tex.assetPath (the .dds) is untouched — content didn't change.

    mTextures.emplace(newHandle, std::move(tex));   // insert before erase — exception safety, mirrors 9b/9c
    mTextures.erase(oldHandle);                     // erase by KEY: emplace above may have rehashed and invalidated `it`

    // Rename .mctex on disk; best-effort. Filesystem failure logs but does not roll back.
    if (auto pIt = mTexturePaths.find(oldHandle); pIt != mTexturePaths.end()) {
        std::filesystem::path oldPath = pIt->second;
        std::filesystem::path newPath = oldPath.parent_path() / (newDisplayName + ".mctex");
        std::error_code ec;
        std::filesystem::rename(oldPath, newPath, ec);
        mTexturePaths.erase(pIt);
        mTexturePaths[newHandle] = newPath.string();
        if (ec) OutputDebugStringA(("MCTextureManager::Rename: filesystem rename failed: " + ec.message() + "\n").c_str());
        // Re-save asset so its internal displayName/handle JSON fields are self-consistent.
        Save(newHandle, mTexturePaths[newHandle]);
    }
    if (auto uIt = mUsedBy.find(oldHandle); uIt != mUsedBy.end()) {
        mUsedBy[newHandle] = std::move(uIt->second);
        mUsedBy.erase(uIt);
    }

    mEngine.Scenes().RecordAssetRedirect("texture", oldHandle, newHandle, "Rename via API");
    return newHandle;
}

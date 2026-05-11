#pragma once
#include "MCAssetIdentity.h"
#include "MCTexture.h"           // asset struct (owns its MCTextureResource)
#include "DDSTextureLoader.h"
#include <set>
#include <string>
#include <unordered_map>

class MCEngine;

class MCTextureManager {
public:
    explicit MCTextureManager(MCEngine& engine);

    TextureHandle LoadFromFile(const std::string& path);
    void          LoadDirectory(const std::string& dirPath);

    void Save(TextureHandle h, const std::string& path) const;
    void SaveAll(const std::string& dirPath) const;

    void Reload(TextureHandle h);

    MCTexture*    Get(TextureHandle h);
    bool          IsLoaded(TextureHandle h) const;
    TextureHandle Find(const std::string& displayName) const;

    void   RegisterUsedBy(TextureHandle h, const std::string& sceneName);
    void   UnregisterUsedBy(TextureHandle h, const std::string& sceneName);
    const std::set<std::string>& ScenesUsing(TextureHandle h) const;

    // Rename the asset to newDisplayName. Updates in-memory map + .mctex file on disk
    // (the underlying .dds is untouched — content didn't change) + records an asset
    // redirect so existing scene JSONs that still reference the old handle resolve to
    // the new one on next load.
    // Throws std::runtime_error on (1) old handle not found, (2) new name already registered.
    TextureHandle Rename(TextureHandle oldHandle, const std::string& newDisplayName);

    const std::unordered_map<TextureHandle, MCTexture>& All() const { return mTextures; }

    // Drop every MCTexture (and its owned MCTextureResource). Called from
    // MCEngine's destructor before ReportLiveObjects so ID3D12Resource ComPtrs
    // release while the device is still alive and don't show as leaks.
    void Clear() { mTextures.clear(); mTexturePaths.clear(); mUsedBy.clear(); }

private:
    MCEngine& mEngine;
    std::unordered_map<TextureHandle, MCTexture>             mTextures;
    std::unordered_map<TextureHandle, std::string>           mTexturePaths;
    std::unordered_map<TextureHandle, std::set<std::string>> mUsedBy;
    static const std::set<std::string> kEmptyScenes;
};

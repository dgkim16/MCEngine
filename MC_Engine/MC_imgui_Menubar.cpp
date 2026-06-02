#include "MCEngine.h"
#include "MCScene.h"
#include "MC_FileDialog.h"
#include <fstream>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

void MCEngine::IMGUI_MENUBAR() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) { FileMenuOpenScene(); }
            if (ImGui::MenuItem("Save", "Ctrl+S")) { FileMenuSave(); }
            if (ImGui::MenuItem("Save As...")) { FileMenuSaveAs(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { PostMessage(mhMainWnd, WM_CLOSE, 0, 0); }
            ImGui::EndMenu();
        } 
        if (ImGui::BeginMenu("Create")) {
            if (ImGui::MenuItem("Scene")) { FileMenuCreateScene(); }
            if (ImGui::MenuItem("Material")) { FileMenuCreateMaterial(); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            if (ImGui::MenuItem("Descriptor Heap Viewer")) { mShowDescHeapViewer = !mShowDescHeapViewer; }
            if (ImGui::MenuItem("FrameGraph Viewer")) { mShowFrameGraphViewer = !mShowFrameGraphViewer; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Test")) {
            if (ImGui::MenuItem("Phase 1 Week 3 Tests")) { mShowTestsWindow = !mShowTestsWindow;  }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

enum class DirtyChoice { Save, Discard, Cancel };

DirtyChoice PromptDirtyChoice(HWND owner,
    const std::filesystem::path& currentPath) {
    const std::string filename = currentPath.empty()
        ? "(unsaved)" : currentPath.filename().string();
    const std::string msg =
        "Save changes to " + filename + " before opening another scene?";
    int r = MessageBoxA(owner, msg.c_str(), "Unsaved changes",
        MB_YESNOCANCEL | MB_ICONWARNING);
    return r == IDYES ? DirtyChoice::Save
        : r == IDNO ? DirtyChoice::Discard
        : DirtyChoice::Cancel;
}

std::optional<std::filesystem::path> ShowSaveSceneDialog(HWND owner) {
    return MCFileDialog::Show(MCFileDialog::Mode::Save,
        "Assets/scenes/", L"Scene JSON", L"*.json", owner);
}
std::optional<std::filesystem::path> ShowOpenSceneDialog(HWND owner) {
    return MCFileDialog::Show(MCFileDialog::Mode::Open,
        "Assets/scenes/", L"Scene JSON", L"*.json", owner);
}
std::optional<std::filesystem::path> ShowSaveMaterialDialog(HWND owner) {
    return MCFileDialog::Show(MCFileDialog::Mode::Save,
        "Assets/materials/", L"Material .mcmat", L"*.mcmat", owner);
}

bool MCEngine::FileMenuSave() {
    auto* scene = mSceneManager.GetActive();
    if (!scene) return false;
    // No path argument — the scene manager owns the active scene's path via
    // mScenePaths. If no path is registered for this scene yet, fall back to
    // Save As so the user picks one. This is the single-source-of-truth fix
    // for the stale-path bug: any cached copy of the path on MCEngine would
    // desync the moment the active scene switched.
    if (mSceneManager.GetScenePath(scene->name).empty()) return FileMenuSaveAs();
    mSceneManager.SaveActiveToRegisteredPath();
    mSceneDirty = false;
    RefreshWindowTitle();
    return true;
}

bool MCEngine::FileMenuSaveAs() {
    auto picked = ShowSaveSceneDialog(mhMainWnd);
    if (!picked) return false;        // user cancelled the dialog
    auto* scene = mSceneManager.GetActive();
    if (!scene) return false;

    // SetScenePath BEFORE SaveActive so the assert inside MCSceneManager::Save
    // sees a matching registered path. Otherwise the assert fires on the first
    // SaveAs of a never-registered scene.
    mSceneManager.SetScenePath(scene->name, picked->string());
    mSceneManager.SaveActiveToRegisteredPath();
    mSceneDirty = false;
    RefreshWindowTitle();
    return true;
}

void MCEngine::FileMenuOpenScene() {
    auto picked = ShowOpenSceneDialog(mhMainWnd);
    if (!picked) return;

    // 1. Validate without registering. Migrator + sceneName extract; throws on
    //    schema fail / migration fail / missing sceneName. Done before the dirty
    //    prompt so an invalid pick can't burn the user's unsaved work.
    nlohmann::json j;
    std::string    name;
    try {
        j = mMigrator.LoadAndMigrate(*picked);
        name = j.at("sceneName").get<std::string>();
    }
    catch (const std::exception& e) {
        MessageBoxA(mhMainWnd, e.what(), "Open Scene failed", MB_OK | MB_ICONERROR);
        return;
    }

    // 2. Dirty prompt. Save can recurse into Save As, which the user may cancel.
    if (mSceneDirty) {
        auto* active = mSceneManager.GetActive();
        std::filesystem::path activePath;
        if (active) activePath = mSceneManager.GetScenePath(active->name);
        switch (PromptDirtyChoice(mhMainWnd, activePath)) {        // returns Save | Discard | Cancel
        case DirtyChoice::Cancel:                 return;
        case DirtyChoice::Save:    if (!FileMenuSave()) return;  // includes Save-As-cancel
            break;
        case DirtyChoice::Discard:                break;         // fall through to load
        }
    }

    // 3. Collision dance — if ANY existing entry in mScenes already uses this
    //    name (active OR just preloaded by boot's LoadAll), evict it cleanly
    //    BEFORE LoadFromJson runs. Two scenes sharing the same name look like
    //    one user to MCMeshSourceManager::mUsedBy (set semantics), so the new
    //    scene's RegisterUsedBy and the old scene's UnregisterUsedBy cancel
    //    out — the mesh source drops while the new scene is still referencing
    //    it. Symptom: AV at MCMeshGeometry::VertexBufferView() with
    //    VertexBufferGPU.ptr_ == 0xFFFFFFFFFFFFFFFF on the first frame after
    //    Switch. Mirrors MCSceneManager::Reload at .cpp:227-232.
    if (mSceneManager.Get(name)) {
        if (auto* active = mSceneManager.GetActive(); active && active->name == name) {
            active->Deactivate(*this);
            mSceneManager.ClearActive();
        }
        mSceneManager.EraseScene(name);
    }
    // 4. Commit. Bracket the load+switch with cmdlist Reset/Close/Execute/Flush
    //    because module OnLoad for grass etc. records GPU upload commands via
    //    CreateDefaultBuffer (d3dUtil.cpp:63). At ImGui-handler time the cmdlist
    //    is closed (Draw resets it next frame). Mirrors ReloadActiveSceneNow at
    //    MCEngine.cpp:238-255.
    FlushCommandQueue();
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    std::string err;
    try {
        mSceneManager.LoadFromJson(name, picked->string());
        mSceneManager.Switch(name);
    }
    catch (const std::exception& e) {
        err = e.what();
    }

    // Always close + execute, even on throw — leaves cmdlist in the closed
    // state Draw expects to Reset against. Partial GPU work is preferable
    // to leaving the list stuck open.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* lists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(lists), lists);
    FlushCommandQueue();

    if (!err.empty()) {
        MessageBoxA(mhMainWnd, err.c_str(), "Open Scene failed (load)", MB_OK | MB_ICONERROR);
        return;        // engine left in whatever state the partial load reached;
        // mActive may be null. User picks another file or restarts.
    }

    mSelectedItemIndex = -1;          // Switch already does this; explicit for clarity.
    // No mCurrentScenePath assignment — MCSceneManager::LoadFromJson registers
    // the path in mScenePaths (single source of truth). The title bar reads
    // from there via mSceneManager.GetScenePath(active->name).
    mSceneDirty = false;
    RefreshWindowTitle();   // update already calls this every frame; explicit for clarity
}

void MCEngine::FileMenuCreateScene() {
    auto picked = ShowSaveSceneDialog(mhMainWnd);
    if (!picked) return;

    const std::string name = picked->stem().string();   // sceneName = filename stem
    nlohmann::json j;
    j["version"] = 1;
    j["sceneName"] = name;
    j["materialHandles"] = nlohmann::json::array();
    j["textureHandles"] = nlohmann::json::array();
    j["meshes"] = nlohmann::json::array();
    j["renderItems"] = nlohmann::json::array();
    j["modules"] = nlohmann::json::array();
    j["specialPointers"] = nlohmann::json::object();

    std::ofstream(*picked) << j.dump(2);
    // engine state unchanged: mActive, mSceneDirty, mScenePaths all untouched.
    // To use the new scene, the user does Open Scene afterwards.
}

void MCEngine::FileMenuCreateMaterial() {
    auto picked = ShowSaveMaterialDialog(mhMainWnd);
    if (!picked) return;
    const std::string name = picked->stem().string();

    // textureHandle = 0 is rejected by UpdateMaterialCBs (MCEngine.cpp:595 asserts
    // every material resolves to a loaded MCTexture). Point new materials at the
    // ship-with-engine `defaultTex` (loaded by Textures().LoadDirectory at boot
    // from Assets/Textures/defaultTex.mctex). Same texture default.mcmat uses.
    const TextureHandle defaultTexH = HashAssetIdentity<AssetKind::Texture>("defaultTex");
    if (!mTextureManager.IsLoaded(defaultTexH)) {
        MessageBoxA(mhMainWnd,
            "defaultTex is not loaded — Assets/Textures/defaultTex.mctex missing or LoadDirectory failed.",
            "Create Material failed", MB_OK | MB_ICONERROR);
        return;
    }

    // Register first — allocates the CB slot, hashes the handle, inserts into
    // mMaterialManager's in-memory map. Defaults: white diffuse, .05 fresnel,
    // .25 roughness, identity matTransform (MCMaterial.h:20-24).
    MCMaterial defaults;
    const MaterialHandle h = mMaterialManager.Register(name, defaults, defaultTexH);

    // Save writes the on-disk .mcmat from the in-memory state — same body as
    // MCMaterialManager::Save at .cpp:74-94.
    mMaterialManager.Save(h, picked->string());

    // Inspector's material combo (Step 6) sees the new material on its next
    // frame via engine.Materials().All(). No active-scene mutation.
}

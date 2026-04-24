#include "ShaderLib.h"
#include "ShaderMacros.h"
#include "d3dUtil.h"
#include <algorithm>

using namespace DirectX;

bool ShaderLib::IsInitialized()const
{
    return mIsInitialized;
}

void ShaderLib::Init() {
	mShaders["standardVS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/color.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/color.hlsl", defines, "PS", "ps_5_1");
	mShaders["transparentPS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/color.hlsl", transparentDefines, "PS", "ps_5_1");
	mShaders["alphaTestedPS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/color.hlsl", alphaTestDefines, "PS", "ps_5_1");
	mShaders["depthDebugVS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/depthDebug.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["depthDebugPS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/depthDebug.hlsl", nullptr, "PS", "ps_5_1");
	mShaders["depthDebugMSAAPS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/depthDebug.hlsl", depthDebugDefines, "PS", "ps_5_1");

	mShaders["treesVS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/TreeBillboard.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["treesGS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/TreeBillboard.hlsl", nullptr, "GS", "gs_5_1");
	mShaders["treesPS"] = d3dUtil::CompileShaderFX(L"Assets/Shaders/TreeBillboard.hlsl", alphaTestDefines, "PS", "ps_5_1");

	mIsInitialized = true;
}

bool ShaderLib::AddShader(const std::string& name, Microsoft::WRL::ComPtr<ID3DBlob> shader) {
	if (mShaders.find(name) == mShaders.end()) {
		mShaders[name] = shader;
		return true;
	}
	return false;
}

ID3DBlob* ShaderLib::operator[](const std::string& name) {
	if (mShaders.find(name) != mShaders.end())
		return mShaders[name].Get();
	return nullptr;
}
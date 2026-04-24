#include "ShaderLib.h"
#include "d3dUtil.h"
#include <algorithm>

using namespace DirectX;

bool ShaderLibDx::IsInitialized()const
{
    return mIsInitialized;
}

void ShaderLibDx::Init()
{
#pragma once
#if defined(DEBUG) || defined(_DEBUG)  
#define COMMA_DEBUG_ARGS ,DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS
#else
#define COMMA_DEBUG_ARGS
#endif
    std::vector<LPCWSTR> standardVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6", L"-D", L"FOG=1", L"-D", L"FORCE_OPAQUE_ALPHA=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> opaquePSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"FOG=1", L"-D", L"FORCE_OPAQUE_ALPHA=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> transparentPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"FOG=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> alphaTestedPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"FOG=1", L"-D", L"ALPHA_TEST=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> depthDebugVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> depthDebugPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> depthDebugMSAAPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"DEPTH_MSAA=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> treesVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> treesGSArgs = { L"-E", L"GS", L"-T", L"gs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> treesPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"FOG=1", L"-D", L"ALPHA_TEST=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> tessVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> HSArgs = { L"-E", L"HS", L"-T", L"hs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> DSArgs = { L"-E", L"DS", L"-T", L"ds_6_6" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> instancedVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6", L"-D", L"FOG=1", L"-D", L"FORCE_OPAQUE_ALPHA=1",L"-D", L"DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> instancedPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6", L"-D", L"FOG=1", L"-D", L"FORCE_OPAQUE_ALPHA=1",L"-D", L"DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> grassTessVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> grassTessHSArgs = { L"-E", L"HS", L"-T", L"hs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> grassTessDSArgs = { L"-E", L"DS", L"-T", L"ds_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> grassTessPSArgs = { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };

    std::vector<LPCWSTR> instancedTessVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6", L"-D", L"DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> instancedTessHSArgs = { L"-E", L"HS", L"-T", L"hs_6_6", L"-D", L"DRAW_INSTANCED=1" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> instancedTessDSArgs = { L"-E", L"DS", L"-T", L"ds_6_6", L"-D", L"DRAW_INSTANCED=1" COMMA_DEBUG_ARGS }; // PS same as instancedPS

    std::vector<LPCWSTR> csForceAlphaArgs = std::vector<LPCWSTR>{ L"-E", L"CSMain", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> csHorzBlurArgs = std::vector<LPCWSTR>{ L"-E", L"HorizontalBlurCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> csVertBlurArgs = std::vector<LPCWSTR>{ L"-E", L"VerticalBlurCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> csSobel = std::vector<LPCWSTR>{ L"-E", L"CSMain", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };

    mShaders["standardVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", standardVSArgs);
    mShaders["opaquePS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", opaquePSArgs);
    mShaders["transparentPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", transparentPSArgs);
    mShaders["alphaTestedPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", alphaTestedPSArgs);

    mShaders["depthDebugVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\depthDebug.hlsl", depthDebugVSArgs);
    mShaders["depthDebugPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\depthDebug.hlsl", depthDebugPSArgs);
    mShaders["depthDebugMSAAPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\depthDebug.hlsl", depthDebugMSAAPSArgs);

    mShaders["treesVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\TreeBillboard.hlsl", treesVSArgs);
    mShaders["treesGS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\TreeBillboard.hlsl", treesGSArgs);
    mShaders["treesPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\TreeBillboard.hlsl", treesPSArgs);

    mShaders["forceAlphaCS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\ForceAlpha.hlsl", csForceAlphaArgs);

    mShaders["horzBlurCS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\blursCS.hlsl", csHorzBlurArgs);
    mShaders["vertBlurCS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\blursCS.hlsl", csVertBlurArgs);

    mShaders["sobelCS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\SobelCS.hlsl", csForceAlphaArgs);

    OutputDebugString(L"attempting to create shaders for tess..");
    mShaders["tessVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", tessVSArgs);
    mShaders["tessHS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", HSArgs);
    mShaders["tessDS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", DSArgs);
    OutputDebugString(L"success to create shaders for tess..");

    mShaders["grassVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Grass.hlsl", grassTessVSArgs);
    mShaders["grassHS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Grass.hlsl", grassTessHSArgs);
    mShaders["grassDS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Grass.hlsl", grassTessDSArgs);
    mShaders["grassPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Grass.hlsl", grassTessPSArgs);

    mShaders["instancedTessVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", instancedTessVSArgs);
    mShaders["instancedTessHS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", instancedTessHSArgs);
    mShaders["instancedTessDS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\Tessellation.hlsl", instancedTessDSArgs);

    mShaders["instancedVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", instancedVSArgs);
    mShaders["instancedPS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\color.hlsl", instancedPSArgs);

    std::vector<LPCWSTR> grassCullCSArgs = { L"-E", L"CullCS", L"-T", L"cs_6_6" COMMA_DEBUG_ARGS };
    mShaders["grassCullCS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\GrassCull.hlsl", grassCullCSArgs);

    std::vector<LPCWSTR> debugLineVSArgs = { L"-E", L"VS", L"-T", L"vs_6_6" COMMA_DEBUG_ARGS };
    std::vector<LPCWSTR> debugLinePSArgs = { L"-E", L"PS", L"-T", L"ps_6_6" COMMA_DEBUG_ARGS };
    mShaders["debugLineVS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\debugLine.hlsl", debugLineVSArgs);
    mShaders["debugLinePS"] = d3dUtil::CompileShaderDX(L"Assets\\Shaders\\debugLine.hlsl", debugLinePSArgs);

    mIsInitialized = true;
}

bool ShaderLibDx::AddShader(const std::string& name, Microsoft::WRL::ComPtr<IDxcBlob> shader)
{
    if (mShaders.find(name) == mShaders.end())
    {
        mShaders[name] = shader;
        return true;
    }

    return false;
}

IDxcBlob* ShaderLibDx::operator[](const std::string& name)
{
    if (mShaders.find(name) != mShaders.end())
    {
        return mShaders[name].Get();
    }

    return nullptr;
}
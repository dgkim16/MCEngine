#pragma once

#include <windows.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <vector>
#include <map>
#include <comdef.h>
#include <string>
#include <minwindef.h>
#include <memory>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <cassert>
#include <iomanip>
#include <iostream>
#include "DDSTextureLoader.h" // ch.9 texturing
#include "dxc/inc/dxcapi.h"
#include "dxc/inc/d3d12shader.h"
#include "GeometryGenerator.h"

#include <MathHelper.h>
// note: `d3d12.h` is inside `d3dx12.h` via `#include`
// excluding d3dx12.h has no effect, so it is commented out for now.
// #include "d3dx12.h" 

// #include "ConstExpressionValues.h" // gNumFrameResources

class d3dUtil
{
public:
    static Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        const void* initData,
        UINT64 byteSize,
        Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer);

    static UINT CalcConstantBufferByteSize(UINT byteSize)
    {
        // Constant buffers must be a multiple of the minimum hardware
        // allocation size (usually 256 bytes).  So round up to nearest
        // multiple of 256.  We do this by adding 255 and then masking off
        // the lower 2 bytes which store all bits < 256.
        // Example: Suppose byteSize = 300.
        // (300 + 255) & ~255
        // 555 & ~255
        // 0x022B & ~0x00ff
        // 0x022B & 0xff00
        // 0x0200
        // 512
        return (byteSize + 255) & ~255;
    }
	static D3D12_SHADER_BYTECODE ByteCodeFromBlobXc(IDxcBlob* shader)
	{
		return { reinterpret_cast<BYTE*>(shader->GetBufferPointer()), shader->GetBufferSize() };
	}

	static D3D12_SHADER_BYTECODE ByteCodeFromBlob(ID3DBlob* shader)
	{
		return { reinterpret_cast<BYTE*>(shader->GetBufferPointer()), shader->GetBufferSize() };
	}


	static std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();
	static Microsoft::WRL::ComPtr<ID3DBlob> LoadBinary(const std::wstring& filename);
	static Microsoft::WRL::ComPtr<ID3DBlob> CompileShaderFX(
		const std::wstring& filename,
		const D3D_SHADER_MACRO* defines,
		const std::string& entrypoint,
		const std::string& target);
	static Microsoft::WRL::ComPtr<IDxcBlob> CompileShaderDX(
		const std::wstring& filename,
		std::vector<LPCWSTR>& compileArgs);
	static void WriteBinaryToFile(IDxcBlob* blob, const std::wstring& filename);
};

// used by ThrowIfFailed
class DxException
{
public:
    DxException() = default;
    DxException(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber);

    std::wstring ToString()const;

    HRESULT ErrorCode = S_OK;
    std::wstring FunctionName;
    std::wstring Filename;
    int LineNumber = -1;
};

inline std::wstring AnsiToWString(const std::string& str)
{
    WCHAR buffer[512];
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
    return std::wstring(buffer);
}

#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                              \
{                                                                     \
    HRESULT hr__ = (x);                                               \
    std::wstring wfn = AnsiToWString(__FILE__);                       \
    if(FAILED(hr__)) { OutputDebugString(wfn.c_str()); throw DxException(hr__, L#x, wfn, __LINE__); } \
}
#endif

#ifndef ReleaseCom
#define ReleaseCom(x) { if(x){ x->Release(); x = 0; } }
#endif

// Vertex / SubmeshGeometry / MeshGeometry moved to MC_Engine/MCVertex.h and
// MC_Engine/MCMeshGeometry.h. Frank Luna's framework header now contains only
// framework-layer utilities.
// const int gNumFrameResources = 3;


static const std::array<const char*, 7> FresnelR0_items =
{
	"FresnelR0_Water",
	"FresnelR0_Glass",
	"FresnelR0_Plastic",
	"FresnelR0_Gold",
	"FresnelR0_Silver",
	"FresnelR0_Copper",
	"FresnelR0_Wood"
};

static const std::vector<DirectX::XMFLOAT3> FresnelR0_Values{
	{0.02f, 0.02f, 0.02f} ,
	{ 0.08f, 0.08f, 0.08f } ,
	{ 0.05f, 0.05f, 0.05f } ,
	{ 1.0f,  0.71f, 0.29f } ,
	{ 0.95f, 0.93f, 0.88f } ,
	{ 0.95f, 0.64f, 0.54f } ,
	{ 0.01f,0.01f ,0.01f }
};


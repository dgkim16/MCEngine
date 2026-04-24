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

struct Vertex
{
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT3 Normal;
	DirectX::XMFLOAT2 TexC;
};

// Defines a subrange of geometry in a MeshGeometry.  This is for when multiple
// geometries are stored in one vertex and index buffer.  It provides the offsets
// and data needed to draw a subset of geometry stores in the vertex and index 
// buffers so that we can implement the technique described by Figure 6.3.
struct SubmeshGeometry
{
	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	INT BaseVertexLocation = 0;

	// Bounding box of the geometry defined by this submesh. 
	// This is used in later chapters of the book.
	// std::unique_ptr<DirectX::BoundingBox> BoundsPtr;
	DirectX::BoundingBox Bounds;

	void CreateBounds(std::vector<GeometryGenerator::Vertex> vertices) {
		std::vector<DirectX::XMFLOAT3> positions;
		for (const auto& vertex : vertices) {
			positions.push_back(vertex.Position);
		}
		DirectX::BoundingBox::CreateFromPoints(Bounds, positions.size(), positions.data(), sizeof(DirectX::XMFLOAT3));
	}

	void CreateBounds(std::vector<Vertex> vertices) {
		std::vector<DirectX::XMFLOAT3> positions;
		for (const auto& vertex : vertices) {
			positions.push_back(vertex.Pos);
		}
		DirectX::BoundingBox::CreateFromPoints(Bounds, positions.size(), positions.data(), sizeof(DirectX::XMFLOAT3));
	}
};


struct MeshGeometry
{
	// Give it a name so we can look it up by name.
	std::string Name;

	// System memory copies.  Use Blobs because the vertex/index format can be generic.
	// It is up to the client to cast appropriately.  
	// ch7 does not use these
	Microsoft::WRL::ComPtr<ID3DBlob> VertexBufferCPU = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> IndexBufferCPU = nullptr;

	// don't use ID3DBlob for these two buffers 
	// because we need to use ID3D12Resource::GetGPUVirtualAddress()
	// to get Buffer location in GPU.
	Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferGPU = nullptr; 
	Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferGPU = nullptr;

	// we will use the upload buffer resources when calling d3dUtil::CreateDefaultBuffer()
	Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferUploader = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferUploader = nullptr;

	// Data about the buffers.
	UINT VertexByteStride = 0;
	UINT VertexBufferByteSize = 0;
	DXGI_FORMAT IndexFormat = DXGI_FORMAT_R16_UINT;
	UINT IndexBufferByteSize = 0;

	// A MeshGeometry may store multiple geometries in one vertex/index buffer.
	// Use this container to define the Submesh geometries so we can draw
	// the Submeshes individually.
	std::unordered_map<std::string, SubmeshGeometry> DrawArgs; // hash map (by nature, unsorted)

	D3D12_VERTEX_BUFFER_VIEW VertexBufferView()const // const function that creates and returns vbv
	{
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
		vbv.StrideInBytes = VertexByteStride;
		vbv.SizeInBytes = VertexBufferByteSize;

		return vbv;
	}

	D3D12_INDEX_BUFFER_VIEW IndexBufferView()const
	{
		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
		ibv.Format = IndexFormat;
		ibv.SizeInBytes = IndexBufferByteSize;

		return ibv;
	}

	// We can free this memory after we finish upload to the GPU.
	void DisposeUploaders()
	{
		VertexBufferUploader = nullptr;
		IndexBufferUploader = nullptr;
	}

	void DisposeResources()
	{
		VertexBufferCPU.Reset();
		IndexBufferCPU.Reset();
		VertexBufferGPU.Reset();
		IndexBufferGPU.Reset();
	}
};
const int gNumFrameResources = 3;


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

struct Material {
	std::string Name;
	std::string textureName;  // name key into mTextures; used by FixupMaterialDiffuseIndices()
	int MatCBIndex = -1;
	int DiffuseSrvHeapIndex = -1;
	int NumFramesDirty = gNumFrameResources;
	DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f,1.0f,1.0f,.5f };
	DirectX::XMFLOAT3 FresnelR0 = { 0.01f,0.01f ,0.01f };
	int FresnelIndex = 6;
	float Roughness = .25f;
	DirectX::XMFLOAT4X4 MatTransform = MathHelper::Identity4x4();
	int renderLevel = 0; // opaque, transparnet, etc
};



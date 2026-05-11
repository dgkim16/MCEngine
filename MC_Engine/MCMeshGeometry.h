#pragma once

#include "MCVertex.h"
#include "Common/GeometryGenerator.h"  // GeometryGenerator::Vertex (CreateBounds overload)

#include <windows.h>
#include <wrl.h>                        // Microsoft::WRL::ComPtr
#include <d3d12.h>                      // ID3D12Resource, D3D12_*_BUFFER_VIEW, DXGI_FORMAT
#include <d3dcommon.h>                  // ID3DBlob
#include <DirectXMath.h>
#include <DirectXCollision.h>           // BoundingBox

#include <string>
#include <vector>
#include <unordered_map>

// Subrange of an MCMeshGeometry's vertex/index buffer — describes how to draw
// one logical mesh that shares a backing buffer with others (Frank Luna's
// "Figure 6.3" pattern). The two CreateBounds overloads handle the two vertex
// formats the engine produces: GeometryGenerator::Vertex (procedural primitives)
// and MCVertex (imported / hand-built / scene-combined buffers).
struct MCSubmeshGeometry
{
	UINT IndexCount         = 0;
	UINT StartIndexLocation = 0;
	INT  BaseVertexLocation = 0;

	DirectX::BoundingBox Bounds;

	void CreateBounds(std::vector<GeometryGenerator::Vertex> vertices) {
		std::vector<DirectX::XMFLOAT3> positions;
		for (const auto& vertex : vertices) {
			positions.push_back(vertex.Position);
		}
		DirectX::BoundingBox::CreateFromPoints(Bounds, positions.size(), positions.data(), sizeof(DirectX::XMFLOAT3));
	}

	void CreateBounds(std::vector<MCVertex> vertices) {
		std::vector<DirectX::XMFLOAT3> positions;
		for (const auto& vertex : vertices) {
			positions.push_back(vertex.Pos);
		}
		DirectX::BoundingBox::CreateFromPoints(Bounds, positions.size(), positions.data(), sizeof(DirectX::XMFLOAT3));
	}
};

// GPU-resident mesh: one VB + one IB, plus a map of MCSubmeshGeometry ranges
// describing how to draw individual meshes that share the buffer. Owned by
// MCMeshSource (which owns it as a unique_ptr) for asset-loaded geometry; for
// engine-internal hand-built geometry (debug lines etc.) it can be owned
// directly by whoever creates it.
struct MCMeshGeometry
{
	std::string Name;

	// System-memory copies. ID3DBlob because vertex/index format is generic; the
	// client casts. Phase 1 (ch.7 onward) typically doesn't use these — uploads
	// go through CreateDefaultBuffer which holds the upload heap separately.
	Microsoft::WRL::ComPtr<ID3DBlob> VertexBufferCPU = nullptr;
	Microsoft::WRL::ComPtr<ID3DBlob> IndexBufferCPU  = nullptr;

	// GPU buffers. Direct ID3D12Resource (not blob) because we need
	// GetGPUVirtualAddress to feed VertexBufferView / IndexBufferView.
	Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferGPU = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferGPU  = nullptr;

	// Upload staging — alive until the GPU has copied (caller flushes). Then
	// callable code drops these via DisposeUploaders.
	Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferUploader = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferUploader  = nullptr;

	UINT        VertexByteStride     = 0;
	UINT        VertexBufferByteSize = 0;
	DXGI_FORMAT IndexFormat          = DXGI_FORMAT_R16_UINT;
	UINT        IndexBufferByteSize  = 0;

	// Submesh ranges keyed by submesh name (e.g., "box", "sphere"). RenderItems
	// reference this via SubmeshName today; Phase 2's per-primitive split moves
	// each submesh onto its own MCMeshGeometry and retires the indirection.
	std::unordered_map<std::string, MCSubmeshGeometry> DrawArgs;

	D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const {
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
		vbv.StrideInBytes  = VertexByteStride;
		vbv.SizeInBytes    = VertexBufferByteSize;
		return vbv;
	}

	D3D12_INDEX_BUFFER_VIEW IndexBufferView() const {
		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
		ibv.Format         = IndexFormat;
		ibv.SizeInBytes    = IndexBufferByteSize;
		return ibv;
	}

	void DisposeUploaders() {
		VertexBufferUploader = nullptr;
		IndexBufferUploader  = nullptr;
	}

	void DisposeResources() {
		VertexBufferCPU.Reset();
		IndexBufferCPU.Reset();
		VertexBufferGPU.Reset();
		IndexBufferGPU.Reset();
	}
};

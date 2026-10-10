#pragma once

#include <DirectXMath.h>

// Static-mesh vertex format. Used by ModelLoader (when reading OBJ vertex data
// into engine-side buffers) and by scenes that combine imported geometry with
// procedurally-generated geometry into a single VB. GPU-side this maps to the
// Pos/Normal/TexC input layout consumed by color.hlsl and lighting.hlsl.
//
// Distinct from GeometryGenerator::Vertex (which carries TangentU + Position
// instead of Pos and is the *output* type of GeometryGenerator::Create*).
struct MCVertex
{
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT3 Normal;
	DirectX::XMFLOAT2 TexC;
};

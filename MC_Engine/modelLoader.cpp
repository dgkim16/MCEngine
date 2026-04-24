#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _DEBUG
#pragma comment(lib, "assimp-vc143-mtd.lib")
#else
#pragma comment(lib, "assimp-vc143-mt.lib")
#endif

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "modelLoader.h"
#include <Windows.h>

using namespace DirectX;

bool ModelLoader::LoadObjToVertexIndexBuffers(
	const std::string& filename,
	std::vector<Vertex>& outVertices,
	std::vector<uint32_t>& outIndices)
{
	outVertices.clear();
	outIndices.clear();

	Assimp::Importer importer;

	const aiScene* scene = importer.ReadFile(
		filename,
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_GenSmoothNormals |
		aiProcess_FlipUVs |
		aiProcess_ImproveCacheLocality |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_FindInvalidData |
		aiProcess_ValidateDataStructure
	);

	if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE))
	{
		OutputDebugStringA(importer.GetErrorString());
		//std::cerr << importer.GetErrorString() << std::endl;
		return false;
	}

	for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
	{
		aiMesh* mesh = scene->mMeshes[meshIndex];

		const uint32_t baseVertex = static_cast<uint32_t>(outVertices.size());

		for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
		{
			Vertex v{};

			// Position
			v.Pos = XMFLOAT3(
				mesh->mVertices[i].x,
				mesh->mVertices[i].y,
				mesh->mVertices[i].z
			);

			// Normal
			if (mesh->HasNormals())
			{
				v.Normal = XMFLOAT3(
					mesh->mNormals[i].x,
					mesh->mNormals[i].y,
					mesh->mNormals[i].z
				);
			}
			else
			{
				v.Normal = XMFLOAT3(0.0f, 0.0f, 0.0f);
			}

			// Texture coordinates, OBJ usually uses channel 0
			if (mesh->HasTextureCoords(0))
			{
				v.TexC = XMFLOAT2(
					mesh->mTextureCoords[0][i].x,
					mesh->mTextureCoords[0][i].y
				);
			}
			else
			{
				v.TexC = XMFLOAT2(0.0f, 0.0f);
			}

			outVertices.push_back(v);
		}

		for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
		{
			const aiFace& face = mesh->mFaces[i];

			// Since aiProcess_Triangulate is enabled, every face should have 3 indices
			if (face.mNumIndices != 3)
				continue;

			outIndices.push_back(baseVertex + face.mIndices[0]);
			outIndices.push_back(baseVertex + face.mIndices[1]);
			outIndices.push_back(baseVertex + face.mIndices[2]);
		}
	}

	return true;
}
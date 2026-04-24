#pragma once

#include "d3dUtil.h"
#include <vector>
#include <string>
#include <cstdint>

class ModelLoader {
public:
    static bool LoadObjToVertexIndexBuffers(
        const std::string& filename,
        std::vector<Vertex>& outVertices,
        std::vector<uint32_t>& outIndices);
};
#pragma once

#include "MCVertex.h"
#include <vector>
#include <string>
#include <cstdint>

class ModelLoader {
public:
    static bool LoadObjToVertexIndexBuffers(
        const std::string& filename,
        std::vector<MCVertex>& outVertices,
        std::vector<uint32_t>& outIndices);
};
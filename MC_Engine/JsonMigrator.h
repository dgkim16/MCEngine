#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

class JsonMigrator {
public:
    static constexpr int kCurrentVersion = 1;

    nlohmann::json LoadAndMigrate(const std::filesystem::path& path) const;
};
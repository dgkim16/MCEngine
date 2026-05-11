#include "JsonMigrator.h"
#include <fstream>
#include <stdexcept>

nlohmann::json JsonMigrator::LoadAndMigrate(const std::filesystem::path& path) const {
    // On Windows, path::string() is a lossy UTF-8 narrowing of a wide path 
    // Passing path directly preserves Unicode filenames. cpp17 added filesystem::path overload
    // so we can just use in(path) here
	std::ifstream in(path); // ifstream's default behavior is to not throw on failure; it sets a fail bit
	if (!in) // therefore handles the failed case manually
		throw std::runtime_error("JsonMigrator: cannot open " + path.string());

	nlohmann::json j = nlohmann::json::parse(in); // the only nlohmann::json::parse call site allowed in the engine.

    if (!j.contains("version") || !j.at("version").is_number_integer()) {
        throw std::runtime_error(path.string() + ": missing or non-integer 'version' field");
    }
    const int v = j.at("version").get<int>();

    // Three-way dispatch on version
    
    // 1. happy path
    if (v == kCurrentVersion) return j; 

    // 2. File is from the future
    if (v > kCurrentVersion) {
        throw std::runtime_error(path.string() + ": file version " + std::to_string(v) +
            " > engine version " + std::to_string(kCurrentVersion) +
            ". Upgrade the engine.");
    }

    // 3. file is older than the engine and we have no migrator chain for it.
    throw std::runtime_error(path.string() + ": no converter from version " + std::to_string(v) +
        " to " + std::to_string(kCurrentVersion));
}
#pragma once
#include <filesystem>
#include <string>

namespace modloader {

struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    long long apiVersion = 0;
};

enum class ManifestStatus { missing, ok, invalid };

ManifestStatus readManifest(const std::filesystem::path& file, Manifest& out, std::string& error);

}  // namespace modloader

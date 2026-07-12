#include "embedded_resources.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace mj2x::cli {
namespace fs = std::filesystem;

namespace {

#include "mj2x_crt_resources.generated.hpp"

bool writeResource(const fs::path& base, const EmbeddedResource& resource)
{
    std::error_code ec;
    const auto      dest = base / resource.path;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        fprintf(
            stderr, "[mj2x] ERROR: cannot create resource directory: %s\n", ec.message().c_str());
        return false;
    }

    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        fprintf(
            stderr, "[mj2x] ERROR: cannot write embedded resource: %s\n", dest.string().c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(resource.data),
              static_cast<std::streamsize>(resource.size));
    return true;
}

}   // namespace

const EmbeddedResource* findEmbeddedResource(const char* path)
{
    if (path == nullptr) return nullptr;
    for (std::size_t index = 0; index < kResourceCount; ++index) {
        const auto& resource = kResources[index];
        if (std::string_view(resource.path) == path) return &resource;
    }
    return nullptr;
}

const EmbeddedResource* embeddedResources(std::size_t* outCount)
{
    if (outCount != nullptr) *outCount = kResourceCount;
    return kResources;
}

bool writeEmbeddedCrtTree(const std::filesystem::path& projectDir)
{
    for (const auto& resource : kResources) {
        if (std::string_view(resource.path).rfind("src/", 0) != 0) continue;
        const EmbeddedResource stripped{
            resource.path + std::string_view("src/").size(), resource.data, resource.size};
        if (!writeResource(projectDir / "src" / "mj2x-crt", stripped)) return false;
    }
    return true;
}

}   // namespace mj2x::cli

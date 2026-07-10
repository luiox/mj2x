#pragma once

#include <cstddef>
#include <filesystem>

namespace mj2x::cli {

struct EmbeddedResource
{
    const char*        path;
    const unsigned char* data;
    std::size_t       size;
};

const EmbeddedResource* findEmbeddedResource(const char* path);
const EmbeddedResource* embeddedResources(std::size_t* outCount);
bool writeEmbeddedCrtTree(const std::filesystem::path& projectDir);

}   // namespace mj2x::cli

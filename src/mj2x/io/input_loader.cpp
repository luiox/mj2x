#include "input_loader.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace mj2x {

using namespace jbytecode;

struct InputLoader::Impl {};

InputLoader::InputLoader()
    : impl_(std::make_unique<Impl>())
{}

InputLoader::~InputLoader() = default;

static std::vector<ca::u8> readFileBytes(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        throw std::runtime_error("Cannot open: " + path);
    auto size = ifs.tellg();
    std::vector<ca::u8> bytes(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

std::unique_ptr<CompilerIndex> InputLoader::load(const std::string& path) {
    auto bytes = readFileBytes(path);

    ClassReader reader(bytes.data(), bytes.size());
    auto classNode = std::make_unique<ClassNode>();
    reader.accept(classNode.get(), 0);

    auto index = std::make_unique<CompilerIndex>();

    CompilerClass cls;
    cls.classNode = std::move(classNode);
    cls.modified = false;

    for (auto* mn : cls.classNode->methods) {
        CompilerMethod cm;
        cm.methodNode = mn;
        cls.methods.push_back(std::move(cm));
    }
    for (auto* fn : cls.classNode->fields) {
        CompilerField cf;
        cf.fieldNode = fn;
        cls.fields.push_back(std::move(cf));
    }

    auto className = cls.classNode->name;
    auto numMethods = cls.classNode->methods.size();
    auto numFields = cls.classNode->fields.size();

    index->addClass(className, std::move(cls));

    printf("[InputLoader] Loaded: %s (%zu methods, %zu fields)\n",
           className.c_str(), numMethods, numFields);

    return index;
}

} // namespace mj2x

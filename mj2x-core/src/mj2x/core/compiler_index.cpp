#include "compiler_index.hpp"

namespace mj2x {

void CompilerIndex::addClass(const std::string& name, CompilerClass cls) {
    classes_[name] = std::move(cls);
}

CompilerClass* CompilerIndex::getClass(const std::string& name) {
    auto it = classes_.find(name);
    return it != classes_.end() ? &it->second : nullptr;
}

void CompilerIndex::addJarEntry(JarEntryRecord entry) {
    jarEntries_.push_back(std::move(entry));
}

void CompilerIndex::upsertJarResource(const std::string& name, std::vector<ca::u8> data) {
    for (auto& entry : jarEntries_) {
        if (entry.name == name) {
            entry.data = std::move(data);
            entry.isClass = false;
            entry.className.clear();
            return;
        }
    }

    JarEntryRecord entry;
    entry.name = name;
    entry.data = std::move(data);
    entry.isClass = false;
    jarEntries_.push_back(std::move(entry));
}

} // namespace mj2x


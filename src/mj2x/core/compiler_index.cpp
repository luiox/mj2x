#include "compiler_index.hpp"

namespace mj2x {

void CompilerIndex::addClass(const std::string& name, CompilerClass cls) {
    classes_[name] = std::move(cls);
}

CompilerClass* CompilerIndex::getClass(const std::string& name) {
    auto it = classes_.find(name);
    return it != classes_.end() ? &it->second : nullptr;
}

} // namespace mj2x

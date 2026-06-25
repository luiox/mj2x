#pragma once

#include <jbytecode/jbytecode.hpp>
#include <libca/str/utf8_string_arena.hpp>

#include <string>
#include <vector>
#include <unordered_map>

#include "mj2x/backend/backend_result.hpp"

namespace mj2x {

struct MethodID {
    std::string owner;
    std::string name;
    std::string desc;

    bool operator==(const MethodID& o) const {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};

struct FieldID {
    std::string owner;
    std::string name;
    std::string desc;

    bool operator==(const FieldID& o) const {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};

struct BackendResult;

struct CompilerMethod {
    jbytecode::tree::MethodNode* methodNode = nullptr;
    bool markedForCompile = false;
    bool compiled = false;
    BackendResult backendResult;
};

struct CompilerField {
    jbytecode::tree::FieldNode* fieldNode = nullptr;
};

struct CompilerClass {
    std::unique_ptr<jbytecode::tree::ClassNode> classNode;
    std::vector<CompilerMethod> methods;
    std::vector<CompilerField> fields;
    bool modified = false;
    bool isLibrary = false;
};

} // namespace mj2x

// -- hash specializations for MethodID / FieldID --
namespace std {
template<>
struct hash<mj2x::MethodID> {
    size_t operator()(const mj2x::MethodID& id) const {
        return hash<string>()(id.owner) ^
               (hash<string>()(id.name) << 1) ^
               (hash<string>()(id.desc) << 2);
    }
};
template<>
struct hash<mj2x::FieldID> {
    size_t operator()(const mj2x::FieldID& id) const {
        return hash<string>()(id.owner) ^
               (hash<string>()(id.name) << 1) ^
               (hash<string>()(id.desc) << 2);
    }
};
} // namespace std

namespace mj2x {

class CompilerIndex {
public:
    using ClassMap = std::unordered_map<std::string, CompilerClass>;

    void addClass(const std::string& name, CompilerClass cls);
    CompilerClass* getClass(const std::string& name);

    ClassMap& getClasses() { return classes_; }
    const ClassMap& getClasses() const { return classes_; }

    // Utf8StringArena ownership: ClassReader/ClassWriter 需要 arena&，且 arena 必须比
    // 它们活得更久。CompilerIndex 持有 arena，全编译期共享一个（ARCHITECTURE_REDESIGN §2.1）。
    ca::str::Utf8StringArena& arena() { return arena_; }
    const ca::str::Utf8StringArena& arena() const { return arena_; }

private:
    ClassMap classes_;
    ca::str::Utf8StringArena arena_;
};

} // namespace mj2x


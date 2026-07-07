#pragma once

#include <jbytecode/jbytecode.hpp>
#include <libca/str/utf8_string_arena.hpp>

#include <unordered_map>
#include <string>
#include <vector>

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

/**
 * @brief Original JAR entry tracked for whole-archive input/output.
 *
 * @details
 * mj2x compiles class entries through `CompilerClass` but must preserve the
 * rest of the archive for MVP JAR round trips.  Each entry records its original
 * path and raw, decompressed payload.  Class entries also remember the internal
 * JVM class name so `OutputWriter` can replace their bytes after backend
 * injection while keeping non-class resources unchanged.
 */
struct JarEntryRecord {
    std::string name;
    std::vector<ca::u8> data;
    bool isClass = false;
    std::string className;
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

    /**
     * @brief Add or replace a parsed class by internal JVM name.
     */
    void addClass(const std::string& name, CompilerClass cls);

    /**
     * @brief Return a mutable class record, or null when absent.
     */
    CompilerClass* getClass(const std::string& name);

    ClassMap& getClasses() { return classes_; }
    const ClassMap& getClasses() const { return classes_; }

    /**
     * @brief Mark this compilation as originating from a JAR archive.
     */
    void setJarInput(bool value) { jarInput_ = value; }

    /**
     * @brief Whether output should preserve whole-archive JAR entry shape.
     */
    bool isJarInput() const { return jarInput_; }

    /**
     * @brief Append an original JAR entry to the output-preservation list.
     */
    void addJarEntry(JarEntryRecord entry);

    /**
     * @brief Ordered JAR entries used by `OutputWriter`.
     */
    std::vector<JarEntryRecord>& jarEntries() { return jarEntries_; }
    const std::vector<JarEntryRecord>& jarEntries() const { return jarEntries_; }

    // Utf8StringArena ownership: ClassReader/ClassWriter 需要 arena&，且 arena 必须比
    // 它们活得更久。CompilerIndex 持有 arena，全编译期共享一个（ARCHITECTURE_REDESIGN §2.1）。
    ca::str::Utf8StringArena& arena() { return arena_; }
    const ca::str::Utf8StringArena& arena() const { return arena_; }

private:
    ClassMap classes_;
    std::vector<JarEntryRecord> jarEntries_;
    bool jarInput_ = false;
    ca::str::Utf8StringArena arena_;
};

} // namespace mj2x


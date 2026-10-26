#pragma once

#include <jbytecode/jbytecode.hpp>
#include <jbc/core/class_file.hpp>
#include <libca/str/utf8_string_arena.hpp>

#include <unordered_map>
#include <string>
#include <vector>

#include "mj2x/backend/backend_result.hpp"

namespace mj2x {

struct MethodID
{
    std::string owner;
    std::string name;
    std::string desc;

    bool operator==(const MethodID& o) const
    {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};

struct BackendResult;

struct CompilerMethod
{
    jbytecode::tree::MethodNode* methodNode       = nullptr;
    bool                         markedForCompile = false;
    bool                         compiled         = false;
    BackendResult                backendResult;
};

struct CompilerField
{
    jbytecode::tree::FieldNode* fieldNode = nullptr;
};

/// @brief Original JAR entry tracked for whole-archive input/output.
///
/// @details
/// mj2x compiles class entries through `CompilerClass` but must preserve the
/// rest of the archive for MVP JAR round trips.  Each entry records its original
/// path and raw, decompressed payload.  Class entries also remember the internal
/// JVM class name so `OutputWriter` can replace their bytes after backend
/// injection while keeping non-class resources unchanged.
struct JarEntryRecord
{
    std::string         name;
    std::vector<ca::u8> data;
    bool                isClass = false;
    std::string         className;
};

struct CompilerClass
{
    std::unique_ptr<jbytecode::tree::ClassNode> classNode;
    /// @brief libjbc DOM 视图（批 2 双表示过渡，libjbc 迁移批 3-6 逐批接管消费）。
    ///
    /// 由 syncTreeToDom 在 pipeline 阶段边界从当前 tree 序列化字节派生
    /// （唯一同步通道 = 真实编解码器字节往返，见 class_dom_sync.hpp）；
    /// 新鲜度契约：compile 入口与 postprocessor 入口时刻与 tree 一致。
    std::unique_ptr<jbc::core::ClassFile>       dom;
    std::vector<CompilerMethod>                 methods;
    std::vector<CompilerField>                  fields;
    bool                                        modified  = false;
    bool                                        isLibrary = false;
    /// @brief 被类级 glob 过滤器（--native-include/--native-exclude）剔除。
    ///
    /// 只影响"是否进入 native 编译集"：类仍留在 index 里（JAR 往返保留原始
    /// 条目、其他类可能引用它），但方法级选择与 <init>/<clinit> 搬运入口
    /// 一律跳过。置位者：NativeClassFilterPreprocessor。
    bool                                        filteredOut = false;
    /// @brief 是否在**混淆作用域**内（--obf-scope-include/exclude）。
    ///
    /// @details
    /// 与 filteredOut 正交：filteredOut 决定"进不进 native 编译集"，本标志决定
    /// "编译出来要不要施加 RegIR 变换"。作用域外的类仍编译成 native，只是跳过
    /// MBA / opaque / CFF / 常量隐藏 / 调用间接化——产物与未开混淆时一致。
    /// 默认 true（不给任何作用域模式时零行为变化）。
    /// 置位者：ObfuscationScopePreprocessor。
    bool                                        inObfuscationScope = true;
};

}   // namespace mj2x

// -- hash specialization for MethodID --
namespace std {
template<>
struct hash<mj2x::MethodID>
{
    size_t operator()(const mj2x::MethodID& id) const
    {
        return hash<string>()(id.owner) ^ (hash<string>()(id.name) << 1) ^
               (hash<string>()(id.desc) << 2);
    }
};
}   // namespace std

namespace mj2x {

class CompilerIndex
{
public:
    using ClassMap = std::unordered_map<std::string, CompilerClass>;

    /// @brief Add or replace a parsed class by internal JVM name.
    void addClass(const std::string& name, CompilerClass cls);

    /// @brief Return a mutable class record, or null when absent.
    CompilerClass* getClass(const std::string& name);

    ClassMap&       getClasses() { return classes_; }
    const ClassMap& getClasses() const { return classes_; }

    /// @brief Mark this compilation as originating from a JAR archive.
    void setJarInput(bool value) { jarInput_ = value; }

    /// @brief Whether output should preserve whole-archive JAR entry shape.
    bool isJarInput() const { return jarInput_; }

    /// @brief Append an original JAR entry to the output-preservation list.
    void addJarEntry(JarEntryRecord entry);

    /// @brief Append or replace a generated JAR resource entry.
    void upsertJarResource(const std::string& name, std::vector<ca::u8> data);

    /// @brief Ordered JAR entries used by `OutputWriter`.
    std::vector<JarEntryRecord>&       jarEntries() { return jarEntries_; }
    const std::vector<JarEntryRecord>& jarEntries() const { return jarEntries_; }

    // Utf8StringArena ownership: ClassReader/ClassWriter 需要 arena&，且 arena 必须比
    // 它们活得更久。CompilerIndex 持有 arena，全编译期共享一个。
    ca::str::Utf8StringArena&       arena() { return arena_; }
    const ca::str::Utf8StringArena& arena() const { return arena_; }

private:
    ClassMap                    classes_;
    std::vector<JarEntryRecord> jarEntries_;
    bool                        jarInput_ = false;
    ca::str::Utf8StringArena    arena_;
};

}   // namespace mj2x

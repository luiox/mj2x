// sample_builder.hpp — mtest_set 共享的 .class bytes 构造辅助
//
// 用 libjbytecode 的 tree API（ClassNode/MethodNode）内存构造样本类，
// 再用 ClassWriter 序列化为 .class bytes。参考实现：
//   libs/libjbytecode/asm-test/src/extensions/core/compute_frames_test.cpp（CwBuilder）
//   mbase/unittest/pass/opt/constant_folder_pass_test.cpp（tree 风格）
//
// 构造约定：
//   - 字符串字面量用 ZUtf8StringRef::from_static（静态生命周期，非拥有）。
//   - Label 用 `new Label()`，通过 MethodNode::visitJumpInsn/visitLabel 转移所有权
//     （这些 visitor 方法内部调用 takeLabel）。
//   - maxStack/maxLocals/StackMapTable 交给 COMPUTE_FRAMES 自动计算（隐含 COMPUTE_MAXS），
//     visitMaxs(0,0) 即可。
//
// 因此生成器只需关心指令序列本身。

#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mtest_set {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace ca::str;

/// 一个可被产品测试装配进 JAR 的 class artifact。
struct SampleArtifact
{
    std::string          relativePath;
    std::vector<ca::u8>  bytes;
};

/// 一个语义样本可以包含多个 class（接口/实现/调用方等）。
/// 生成器只负责确定性地产出 bytes，不负责写文件。
struct SampleBundle
{
    std::vector<SampleArtifact> artifacts;
};

inline SampleBundle singleArtifact(const char* relativePath, std::vector<ca::u8> bytes)
{
    return SampleBundle{{SampleArtifact{relativePath, std::move(bytes)}}};
}

/// 用 MethodNode visitor API 构造一个 static public 方法体。
/// bodyBuilder 收到一个 MethodNode*，在其上调用 visitInsn/visitVarInsn/visitJumpInsn 等。
inline MethodNode* makeStaticMethod(const char* name, const char* desc,
                                    const std::function<void(MethodNode*)>& bodyBuilder)
{
    auto* method = new MethodNode(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        ZUtf8StringRef::from_static(name),
        ZUtf8StringRef::from_static(desc),
        ZUtf8StringRef::from_static(""),
        /*exceptions*/ {});
    method->visitCode();
    bodyBuilder(method);
    method->visitMaxs(0, 0);   // COMPUTE_FRAMES 填充
    method->visitEnd();
    return method;
}

/// 构造一个继承 java/lang/Object 的 public 类，挂载若干方法，序列化为 .class bytes。
/// arena 必须存活到本函数返回（ClassWriter 借用它做常量池 intern）。
inline std::vector<ca::u8> buildClass(Utf8StringArena& arena, const char* className,
                                      std::initializer_list<MethodNode*> methods)
{
    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ZUtf8StringRef::from_static(className);
    cn.superName = ZUtf8StringRef::from_static("java/lang/Object");

    for (auto* m : methods) {
        cn.methods.push_back(m);
    }

    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// 便捷：单 static 方法类。
inline std::vector<ca::u8> buildSingleStaticMethodClass(
    Utf8StringArena& arena, const char* className, const char* methodName,
    const char* methodDesc, const std::function<void(MethodNode*)>& bodyBuilder)
{
    return buildClass(arena, className, {makeStaticMethod(methodName, methodDesc, bodyBuilder)});
}

/// 用 ClassWriter 直接构造需要 fields/interface 等完整 visitor API 的样本类。
template <typename Builder>
inline std::vector<ca::u8> buildVisitorClass(Utf8StringArena& arena, const char* className,
                                             int access, const char* superName,
                                             const std::vector<const char*>& interfaces,
                                             const Builder& builder,
                                             int writerFlags = ClassWriter::COMPUTE_FRAMES)
{
    ClassWriter writer(arena, writerFlags);
    writer.visit(Opcodes::V1_8,
                 access,
                 ZUtf8StringRef::from_static(className),
                 Utf8StringRef(),
                 ZUtf8StringRef::from_static(superName),
                 [&] {
                     std::vector<Utf8StringRef> refs;
                     refs.reserve(interfaces.size());
                     for (const auto* name : interfaces) refs.push_back(ZUtf8StringRef::from_static(name));
                     return refs;
                 }());
    builder(writer, arena);
    writer.visitEnd();
    return writer.toByteArray();
}

/// 把 bytes 写到文件（生成器 main 用）。
inline void writeBytes(const std::filesystem::path& path, const std::vector<ca::u8>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

}   // namespace mtest_set

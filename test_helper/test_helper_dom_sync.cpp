#include "test_helper_dom_sync.hpp"

#include <stdexcept>

#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

namespace test {

namespace {

/// tree 序列化旗标恒 plain（0）：与退役的 class_dom_sync 同口径（COMPUTE 的
/// 帧/栈计算是 output 侧语义，见批 2 实证；plain 只求如实序列化当前状态）。
constexpr ca::i32 kPlainWriterFlags = 0;

std::vector<ca::u8> serializeTree(jbytecode::tree::ClassNode& node,
                                  ca::str::Utf8StringArena&   arena)
{
    jbytecode::core::ClassWriter writer(arena, kPlainWriterFlags);
    node.accept(&writer);
    return writer.toByteArray();
}

}   // namespace

mj2x::CompilerClass classFromTree(const std::string& name, jbytecode::tree::ClassNode& node,
                                  ca::str::Utf8StringArena& arena)
{
    auto bytes = serializeTree(node, arena);

    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::runtime_error("test_helper_dom_sync: readClass failed for " + name);
    }

    mj2x::CompilerClass cls;
    cls.dom = std::move(read).unwrap().file;
    mj2x::refreshMethodSnapshots(cls);
    return cls;
}

std::unique_ptr<jbytecode::tree::ClassNode> treeFromDom(const mj2x::CompilerClass& cls,
                                                        ca::str::Utf8StringArena&  arena)
{
    if (cls.dom == nullptr) return nullptr;

    auto written = jbc::core::writeClass(*cls.dom);
    if (written.is_err()) {
        throw std::runtime_error("test_helper_dom_sync: writeClass failed");
    }
    auto bytes = std::move(written).unwrap();

    auto        reader = std::make_unique<jbytecode::core::ClassReader>(
        arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    auto node = std::make_unique<jbytecode::tree::ClassNode>();
    reader->accept(node.get(), 0);
    return node;
}

jbytecode::tree::MethodNode* findMethodNode(jbytecode::tree::ClassNode* node,
                                            const std::string&          name)
{
    if (node == nullptr) return nullptr;
    for (auto* m : node->methods) {
        if (m->name.to_std_string() == name) return m;
    }
    return nullptr;
}

mj2x::CompilerMethod snapshotMethod(jbytecode::tree::MethodNode& mn, bool markedForCompile)
{
    mj2x::CompilerMethod cm;
    cm.name             = mn.name.to_std_string();
    cm.desc             = mn.desc.to_std_string();
    cm.access           = static_cast<ca::u16>(mn.access);
    cm.maxStack         = static_cast<ca::u16>(mn.maxStack);
    cm.maxLocals        = static_cast<ca::u16>(mn.maxLocals);
    cm.markedForCompile = markedForCompile;
    return cm;
}

std::vector<mj2x::Block*> convertMethodNode(mj2x::CodeConverter&         converter,
                                            jbytecode::tree::MethodNode& mn,
                                            ca::str::Utf8StringArena&    arena)
{
    // 批 6d 前产品同款测试垫片的移植（code_converter.cpp 退役的 MethodNode
    // 重载）：方法借用挂最小包装类，plain 序列化 → readClass → convert(cf, 0)。
    jbytecode::tree::ClassNode shim;
    shim.version   = jbytecode::core::Opcodes::V1_8;
    shim.access    = jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_SUPER;
    shim.name      = arena.intern("shim/Class");
    shim.superName = arena.intern("java/lang/Object");
    shim.methods.push_back(&mn);

    auto bytes = serializeTree(shim, arena);
    shim.methods.clear();   // 方法是借用（调用方持有），防 ClassNode 析构误删

    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::invalid_argument("test_helper_dom_sync: shim readClass failed");
    }
    auto dom = std::move(read).unwrap();
    return converter.convert(*dom.file, 0);
}

}   // namespace test

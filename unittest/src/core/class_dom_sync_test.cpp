#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/writer.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/core/class_dom_sync.hpp"
#include "mj2x/core/compiler_index.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace ca::str;

namespace {

/// @brief 最小单方法类（线性体，无控制流，COMPUTE_MAXS 足够）。
static std::unique_ptr<ClassNode> makeSyncClass(const char* className)
{
    auto cn = std::make_unique<ClassNode>();
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->name      = Utf8StringRef::from_cstr(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto mn = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static("run"),
                                           ZUtf8StringRef::from_static("(I)I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    mn->maxStack  = 2;
    mn->maxLocals = 2;

    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new InsnNode(Opcodes::ICONST_1));
    mn->instructions.add(new InsnNode(Opcodes::IADD));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));

    cn->methods.push_back(mn.release());
    return cn;
}

static std::vector<ca::u8> serialize(const CompilerClass& cls, Utf8StringArena& arena)
{
    ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
    cls.classNode->accept(&writer);
    return writer.toByteArray();
}

TEST(ClassDomSyncTest, SyncProducesFaithfulDom)
{
    CompilerIndex index;
    CompilerClass cls;
    cls.classNode = makeSyncClass("t/Sync");
    cls.modified  = false;
    index.addClass("t/Sync", std::move(cls));

    syncTreeToDom(index);

    auto*  back = index.getClass("t/Sync");
    ASSERT_NE(back, nullptr);
    ASSERT_NE(back->dom, nullptr);
    EXPECT_EQ(back->dom->thisName(), "t/Sync");
    ASSERT_EQ(back->dom->methods.size(), 1u);

    // 字节保真：DOM 未触碰时 writeClass 逐字节复现 tree 序列化产物
    // （copy-through：未触碰 span 原样 copy）。
    auto treeBytes  = serialize(*back, index.arena());
    auto domWritten = jbc::core::writeClass(*back->dom);
    ASSERT_TRUE(domWritten.is_ok());
    EXPECT_EQ(treeBytes, std::move(domWritten).unwrap());
}

TEST(ClassDomSyncTest, ResyncReflectsTreeMutation)
{
    CompilerIndex index;
    CompilerClass cls;
    cls.classNode = makeSyncClass("t/Sync2");
    cls.modified  = false;
    index.addClass("t/Sync2", std::move(cls));

    syncTreeToDom(index);
    auto* back = index.getClass("t/Sync2");
    ASSERT_NE(back->dom, nullptr);
    EXPECT_EQ(back->dom->methods[0].access & Opcodes::ACC_NATIVE, 0u);

    // 模拟预处理树改动：方法标 native。再同步后 DOM 反映该位。
    back->classNode->methods[0]->access |= Opcodes::ACC_NATIVE;
    back->modified = true;
    syncTreeToDom(index);

    ASSERT_NE(back->dom, nullptr);
    EXPECT_EQ(back->dom->methods.size(), 1u);
    EXPECT_NE(back->dom->methods[0].access & Opcodes::ACC_NATIVE, 0u);
}

}   // namespace

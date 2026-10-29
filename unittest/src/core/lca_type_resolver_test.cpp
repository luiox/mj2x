// LcaTypeResolver 单测（帧换装 M1）：谱系内对真 LCA、谱系外对回退硬编码表。
//
// 真问题背景：PortedCommonSuperResolver 对输入 jar 自定义谱系一律 Object 兜底
// （表里只有 JDK 常见链）——合并点下游对合并值直接 invokevirtual 时 VerifyError。
// LcaTypeResolver 用 mjbase ClassHierarchy（super 链最近公共祖先）精确求解；
// 未登记侧（JDK 类，M1 图只含输入 jar）回退旧表，行为与旧口径逐位一致。

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "test_helper_dom_sync.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/lca_type_resolver.hpp"

using namespace mj2x;
using namespace jbytecode::core;

namespace {

ca::str::Utf8StringRef U(const char* s)
{
    return ca::str::Utf8StringRef::from_cstr(s);
}

std::unique_ptr<jbytecode::tree::ClassNode> makeNode(const std::string&    name,
                                                     const std::string&    superName,
                                                     ca::str::Utf8StringArena& arena)
{
    auto cn            = std::make_unique<jbytecode::tree::ClassNode>();
    cn->name           = arena.intern(name.c_str());
    cn->version        = Opcodes::V1_8;
    cn->access         = Opcodes::ACC_PUBLIC;
    cn->superName      = superName.empty() ? U("java/lang/Object") : arena.intern(superName.c_str());
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<jbytecode::tree::MethodNode>(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, U("run"), U("()V"), U(""), noExcepts);
    mn->maxStack  = 1;
    mn->maxLocals = 0;
    cn->methods.push_back(mn.release());
    return cn;
}

/// Base ← Mid ← Derived 三级谱系 + 独立类 Sibling。
CompilerIndex makeIndex(ca::str::Utf8StringArena& arena)
{
    CompilerIndex index;
    auto          derived = test::classFromTree("pkg/Derived", *makeNode("pkg/Derived", "pkg/Mid", arena), arena);
    auto          mid     = test::classFromTree("pkg/Mid", *makeNode("pkg/Mid", "pkg/Base", arena), arena);
    auto          base    = test::classFromTree("pkg/Base", *makeNode("pkg/Base", "", arena), arena);
    auto          sibling = test::classFromTree("pkg/Sibling", *makeNode("pkg/Sibling", "", arena), arena);
    index.addClass("pkg/Derived", std::move(derived));
    index.addClass("pkg/Mid", std::move(mid));
    index.addClass("pkg/Base", std::move(base));
    index.addClass("pkg/Sibling", std::move(sibling));
    return index;
}

}   // namespace

TEST(LcaTypeResolverTest, InGraphPairResolvesToRealAncestor)
{
    ca::str::Utf8StringArena arena;
    CompilerIndex            index = makeIndex(arena);
    LcaTypeResolver          lca;
    lca.buildFromIndex(index);
    ASSERT_EQ(lca.registeredCount(), ca::usize{4});

    // Derived + Base → Base（旧硬编码表对自定义谱系给 Object——本断言即真问题
    // 回归锁：合并点下游 invokevirtual Base 方法不再 VerifyError）。
    auto r = lca.merge(jbc::analysis::VType::ref("pkg/Derived"), jbc::analysis::VType::ref("pkg/Base"));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::string(std::move(r).unwrap().name), "pkg/Base");

    // Mid + Sibling（同图不同链）→ 图内最近公共祖先 Object。
    auto r2 = lca.merge(jbc::analysis::VType::ref("pkg/Mid"), jbc::analysis::VType::ref("pkg/Sibling"));
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(std::string(std::move(r2).unwrap().name), "java/lang/Object");
}

TEST(LcaTypeResolverTest, UnregisteredPairFallsBackToPortedTable)
{
    ca::str::Utf8StringArena arena;
    CompilerIndex            index = makeIndex(arena);
    LcaTypeResolver          lca;
    lca.buildFromIndex(index);

    // 双侧未登记（JDK 类）：Integer + String → 硬编码表 → Object（旧口径）。
    auto r = lca.merge(jbc::analysis::VType::ref("java/lang/Integer"),
                       jbc::analysis::VType::ref("java/lang/String"));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::string(std::move(r).unwrap().name), "java/lang/Object");

    // 单侧登记：Derived + 未登记的 java/lang/Throwable → 硬编码表兜底不崩。
    auto r2 = lca.merge(jbc::analysis::VType::ref("pkg/Derived"),
                        jbc::analysis::VType::ref("java/lang/Throwable"));
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(std::string(std::move(r2).unwrap().name), "java/lang/Object");
}

TEST(LcaTypeResolverTest, StructuralMergesMatchStrictAdapter)
{
    ca::str::Utf8StringArena arena;
    CompilerIndex            index = makeIndex(arena);
    LcaTypeResolver          lca;
    lca.buildFromIndex(index);

    // Null 作万能引用底。
    auto r = lca.merge(jbc::analysis::VType{jbc::analysis::VKind::Null, 0, {}}, jbc::analysis::VType::ref("pkg/Base"));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::string(std::move(r).unwrap().name), "pkg/Base");

    // 同名自并。
    auto r2 = lca.merge(jbc::analysis::VType::ref("pkg/Mid"), jbc::analysis::VType::ref("pkg/Mid"));
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(std::string(std::move(r2).unwrap().name), "pkg/Mid");

    // 非 Ref 对 → Unresolvable（与 Strict 档口径一致）。
    auto r3 = lca.merge(jbc::analysis::VType{jbc::analysis::VKind::Int, 0, {}}, jbc::analysis::VType::ref("pkg/Base"));
    ASSERT_TRUE(r3.is_err());
}

TEST(LcaTypeResolverTest, EmptyIndexStillFallsBackCleanly)
{
    ca::str::Utf8StringArena arena;
    CompilerIndex            index;
    LcaTypeResolver          lca;
    lca.buildFromIndex(index);
    ASSERT_EQ(lca.registeredCount(), ca::usize{0});

    auto r = lca.merge(jbc::analysis::VType::ref("java/lang/Exception"),
                       jbc::analysis::VType::ref("java/lang/AssertionError"));
    ASSERT_TRUE(r.is_ok());
    // 硬编码表内链：Exception→Throwable 可达 AssertionError→Error→Throwable → Throwable。
    EXPECT_EQ(std::string(std::move(r).unwrap().name), "java/lang/Throwable");
}

#include <gtest/gtest.h>

#include <vector>

#include "libca/str/utf8_string.hpp"
#include "mj2x/core/jvm_type.hpp"

using mj2x::Type;
using ca::str::Utf8String;

namespace {

/// @brief 门面入参生命周期锚：JvmType 视图指向入参串，宿主串须活过断言。
struct DescFixture
{
    Utf8String desc;
    explicit DescFixture(const char* s) : desc(s) {}
    ca::str::Utf8StringRef ref() const { return desc.ref(); }
};

TEST(JvmTypeTest, ParsePrimitiveSorts)
{
    EXPECT_EQ(Type::getType(ca::str::Utf8StringRef::from_cstr("I")).getSort(), Type::INT);
    EXPECT_EQ(Type::getType(ca::str::Utf8StringRef::from_cstr("J")).getSort(), Type::LONG);
    EXPECT_EQ(Type::getType(ca::str::Utf8StringRef::from_cstr("V")).getSort(), Type::VOID);
    EXPECT_EQ(Type::getType(ca::str::Utf8StringRef::from_cstr("Z")).getSort(), Type::BOOLEAN);
}

TEST(JvmTypeTest, ObjectTypeInternalMasking)
{
    auto t  = Type::getObjectType(ca::str::Utf8StringRef::from_cstr("foo/Bar"));
    EXPECT_EQ(t.getSort(), Type::OBJECT);   // INTERNAL 掩蔽为 OBJECT（旧栈行为）
    auto arena = ca::str::Utf8StringArena{};
    EXPECT_EQ(t.getDescriptor(arena).to_std_string(), "Lfoo/Bar;");
    EXPECT_EQ(t.getInternalName(arena).to_std_string(), "foo/Bar");
    // INTERNAL 与 OBJECT 排序同值等价（旧栈 operator== 口径）
    auto obj = Type::getType(ca::str::Utf8StringRef::from_cstr("Lfoo/Bar;"));
    EXPECT_TRUE(t == obj);
}

TEST(JvmTypeTest, ArrayTypeDescriptorPassthrough)
{
    auto t = Type::getObjectType(ca::str::Utf8StringRef::from_cstr("[Ljava/lang/String;"));
    EXPECT_EQ(t.getSort(), Type::ARRAY);
    auto arena = ca::str::Utf8StringArena{};
    EXPECT_EQ(t.getDescriptor(arena).to_std_string(), "[Ljava/lang/String;");
}

TEST(JvmTypeTest, MethodTypeInstanceAccessors)
{
    auto mt = Type::getMethodType(ca::str::Utf8StringRef::from_cstr("(II)V"));
    EXPECT_EQ(mt.getSort(), Type::METHOD);
    auto args = mt.getArgumentTypes();
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[0].getSort(), Type::INT);
    EXPECT_EQ(args[1].getSort(), Type::INT);
    EXPECT_EQ(mt.getReturnType().getSort(), Type::VOID);
}

TEST(JvmTypeTest, StaticArgumentAndReturnSlicing)
{
    auto args = Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;J[I)V"));
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].getSort(), Type::OBJECT);
    {
        auto arena = ca::str::Utf8StringArena{};
        EXPECT_EQ(args[0].getDescriptor(arena).to_std_string(), "Ljava/lang/String;");
    }
    EXPECT_EQ(args[1].getSort(), Type::LONG);
    EXPECT_EQ(args[2].getSort(), Type::ARRAY);
    auto ret = Type::getReturnType(ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;J[I)[I"));
    EXPECT_EQ(ret.getSort(), Type::ARRAY);
    EXPECT_EQ(ret.toString(), "[I");
}

TEST(JvmTypeTest, EmptyDescriptorThrowsDeterministically)
{
    // J-6 根治锚：旧栈对空描述符 byte_at(1) 越界读（结果随栈残留字节漂移，
    // IndyBootstrapPlanTest.WalkEdgeShapes 因此非确定性翻车）；门面经 jbc::type
    // 校验前置，确定性 invalid_argument。
    EXPECT_THROW((void)Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr("")),
                 std::invalid_argument);
    EXPECT_THROW((void)Type::getReturnType(ca::str::Utf8StringRef::from_cstr("")),
                 std::invalid_argument);
    EXPECT_THROW((void)Type::getType(ca::str::Utf8StringRef::from_cstr("")),
                 std::invalid_argument);
    EXPECT_THROW((void)Type::getObjectType(ca::str::Utf8StringRef::from_cstr("")),
                 std::invalid_argument);
}

TEST(JvmTypeTest, MalformedDescriptorsThrow)
{
    EXPECT_THROW((void)Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr("(I")),
                 std::invalid_argument);
    EXPECT_THROW((void)Type::getType(ca::str::Utf8StringRef::from_cstr("Ljava/lang/String")),
                 std::invalid_argument);
    // V 只允许出现在返回位（JVMS 4.3）
    EXPECT_THROW((void)Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr("(V)V")),
                 std::invalid_argument);
}

TEST(JvmTypeTest, NonReferenceInternalNameThrows)
{
    auto t = Type::getType(ca::str::Utf8StringRef::from_cstr("I"));
    auto arena = ca::str::Utf8StringArena{};
    EXPECT_THROW((void)t.getInternalName(arena), std::logic_error);
}

}   // namespace

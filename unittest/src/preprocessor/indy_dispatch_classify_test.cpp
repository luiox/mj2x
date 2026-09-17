// indy native dispatch 静态判定单测（sprint 波次一，设计见 mj2x/doc_ai/indy_native_dispatch_design.md）。
//
// 锁定 classifyIndySite 的判定矩阵：无参/捕获 lambda、方法引用、string concat、
// record ObjectMethods、LMF 字段句柄精确子集、非标准 bootstrap——各形态断言
// fast（StaticExpanded / DirectFieldAccess / RecordObjectMethods）或
// fallback（RuntimeHelper）及稳定理由字面量；report 收集路径对字节码输出零影响
//（判定=标记，不改写行为；既有 invokedynamic_preprocessor_test.cpp 锁定展开行为）。
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/mir/indy_dispatch.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 在 index 里造一个类，含一个静态方法，方法体是给定指令。返回方法指针（归 index 所有）。
MethodNode* addMethodToIndex(CompilerIndex& index, const std::string& className,
                             const std::string& methodName, const std::string& methodDesc,
                             const std::vector<AbstractInsnNode*>& insns)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto*                               mn = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              arena.intern(methodName.c_str()),
                              arena.intern(methodDesc.c_str()), arena.intern(""), noExcepts);
    mn->maxStack  = 10;
    mn->maxLocals = 4;
    for (auto* insn : insns) mn->instructions.add(insn);
    cn->methods.push_back(mn);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass(className, std::move(cls));
    return index.getClass(className)->classNode->methods.back();
}

// 在 index 里注册一个空壳类（供 D1 分支「bsm owner 在编译输入内」判定用）。
void addEmptyClass(CompilerIndex& index, const std::string& className)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");
    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass(className, std::move(cls));
}

// 统计方法里 INVOKE_DYNAMIC_INSN 数量。
int countIndy(MethodNode* m)
{
    int n = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) ++n;
    }
    return n;
}

Handle stringConcatBsm(CompilerIndex& index, const char* name)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/StringConcatFactory"),
                  arena.intern(name),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/String;"
                               "[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;"));
}

Handle lmfBsm(CompilerIndex& index, const char* name)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                  arena.intern(name),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
                               "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
                               "Ljava/lang/invoke/CallSite;"));
}

Handle lmfAltBsm(CompilerIndex& index)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                  arena.intern("altMetafactory"),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)"
                               "Ljava/lang/invoke/CallSite;"));
}

Handle recordObjectMethodsBsm(CompilerIndex& index)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/runtime/ObjectMethods"),
                  arena.intern("bootstrap"),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/Class;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)"
                               "Ljava/lang/Object;"));
}

// lambda bsmArgs 三件套：[0]=sam、[1]=impl、[2]=instantiated。
std::vector<ca::core::Any> lambdaBsmArgs(CompilerIndex& index, const std::string& samDesc,
                                         const Handle& impl, const std::string& instantiatedDesc)
{
    auto&                      arena = index.arena();
    std::vector<ca::core::Any> args;
    args.emplace_back(Type::getMethodType(arena.intern(samDesc.c_str())));
    args.emplace_back(impl);
    args.emplace_back(Type::getMethodType(arena.intern(instantiatedDesc.c_str())));
    return args;
}

InvokeDynamicInsnNode* makeIndy(CompilerIndex& index, const std::string& name,
                                const std::string& desc, const Handle& bsm,
                                std::vector<ca::core::Any> bsmArgs)
{
    auto& arena = index.arena();
    return new InvokeDynamicInsnNode(arena.intern(name.c_str()), arena.intern(desc.c_str()), bsm,
                                     std::move(bsmArgs));
}

}   // namespace

// ---- T0：阶段1/2 静态展开形态（fast：StaticExpanded）----

// 无参 lambda（静态 impl）：StaticExpanded；process 后 indy 消失（判定不改写行为）。
TEST(IndyDispatchClassifyTest, StatelessLambdaIsStaticExpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                       arena.intern("(I)I"));
    auto* indy = makeIndy(index, "applyAsInt", "()Ljava/util/function/IntUnaryOperator;",
                          lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(I)I", impl, "(I)I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
    EXPECT_STREQ(d.reason, "lambda-metafactory: expandable by phase-2 (synthesized impl class)");
}

// 捕获 lambda（indy desc 带捕获参数，impl 布局 captured++sam）：StaticExpanded。
TEST(IndyDispatchClassifyTest, CapturingLambdaIsStaticExpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_INVOKESTATIC, arena.intern("Cap"), arena.intern("lambda$make$0"),
                       arena.intern("(II)I"));
    auto* indy = makeIndy(index, "applyAsInt", "(I)Ljava/util/function/IntUnaryOperator;",
                          lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(I)I", impl, "(I)I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
    EXPECT_STREQ(d.reason, "lambda-metafactory: expandable by phase-2 (synthesized impl class)");
}

// unbound 实例方法引用（H_INVOKEVIRTUAL）：StaticExpanded。
TEST(IndyDispatchClassifyTest, UnboundInstanceMethodRefIsStaticExpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Rec"), arena.intern("val"),
                       arena.intern("()I"));
    auto* indy = makeIndy(index, "apply", "()Lp/Fn;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(Lp/Rec;)I", impl, "(Lp/Rec;)I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
}

// 构造器方法引用（H_NEWINVOKESPECIAL）：StaticExpanded。
TEST(IndyDispatchClassifyTest, ConstructorRefIsStaticExpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_NEWINVOKESPECIAL, arena.intern("p/Rec"), arena.intern("<init>"),
                       arena.intern("(I)V"));
    auto* indy = makeIndy(index, "make", "()Lp/Factory;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(I)Lp/Rec;", impl, "(I)Lp/Rec;"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
}

// makeConcatWithConstants：StaticExpanded + 阶段1 理由。
TEST(IndyDispatchClassifyTest, MakeConcatWithConstantsIsStaticExpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(std::string("a=\x01,s=\x01"));
    auto* indy = makeIndy(index, "makeConcatWithConstants",
                          "(ILjava/lang/String;)Ljava/lang/String;",
                          stringConcatBsm(index, "makeConcatWithConstants"), std::move(bsmArgs));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
    EXPECT_STREQ(d.reason,
                 "string-concat: makeConcatWithConstants expanded to StringBuilder bytecode by phase-1");
}

// makeConcat（无 recipe）：StaticExpanded + makeConcat 理由。
TEST(IndyDispatchClassifyTest, MakeConcatIsStaticExpanded)
{
    CompilerIndex index;

    auto* indy = makeIndy(index, "makeConcat", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                          stringConcatBsm(index, "makeConcat"), std::vector<ca::core::Any>{});

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::StaticExpanded);
    EXPECT_STREQ(d.reason, "string-concat: makeConcat expanded to StringBuilder bytecode by phase-1");
}

// ---- F2：record ObjectMethods（fast：RecordObjectMethods / fallback：malformed）----

// 标准 record bootstrap 形态（Class + String + MethodType）：RecordObjectMethods。
TEST(IndyDispatchClassifyTest, RecordObjectMethodsIsDirectCandidate)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x;y"));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)Lp/Point;")));
    auto* indy = makeIndy(index, "toString", "(Lp/Point;)Ljava/lang/String;",
                          recordObjectMethodsBsm(index), std::move(bsmArgs));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RecordObjectMethods);
    EXPECT_STREQ(d.reason, "record ObjectMethods.bootstrap: statically nameable accessor targets");
}

// record bootstrap bsmArgs 缺失（< 3）：RuntimeHelper + malformed 理由。
TEST(IndyDispatchClassifyTest, RecordMalformedArgsFallsBack)
{
    CompilerIndex index;

    auto* indy = makeIndy(index, "hashCode", "(Lp/Point;)I", recordObjectMethodsBsm(index),
                          std::vector<ca::core::Any>{});

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(d.reason, "record ObjectMethods.bootstrap: malformed bsmArgs shape");
}

// ---- F1：LMF 字段句柄精确一致子集（fast：DirectFieldAccess / fallback：子集外）----

// GETSTATIC getter：sam=()I 与字段 desc 精确一致 → DirectFieldAccess。
TEST(IndyDispatchClassifyTest, StaticFieldGetterIsDirectFieldAccess)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_GETSTATIC, arena.intern("p/Rec"), arena.intern("SV"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "get", "()Lp/Sup;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "()I", impl, "()I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field getter (GETSTATIC): direct field access candidate");
}

// GETFIELD bound getter：captured=[receiver 引用] + sam=()I 与字段 desc 一致 → DirectFieldAccess。
TEST(IndyDispatchClassifyTest, InstanceFieldGetterIsDirectFieldAccess)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_GETFIELD, arena.intern("p/Rec"), arena.intern("f"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "get", "(Lp/Rec;)Lp/Sup;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "()I", impl, "()I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field getter (GETFIELD): direct field access candidate");
}

// PUTFIELD setter：captured=[receiver] + sam=(I)V 与字段 desc 一致 → DirectFieldAccess。
TEST(IndyDispatchClassifyTest, InstanceFieldSetterIsDirectFieldAccess)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_PUTFIELD, arena.intern("p/Rec"), arena.intern("f"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "set", "(Lp/Rec;)Lp/Csm;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(I)V", impl, "(I)V"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field setter (PUTFIELD): direct field access candidate");
}

// 字段句柄但类型不一致（sam=()J vs 字段 I）：子集外 → RuntimeHelper fallback。
TEST(IndyDispatchClassifyTest, FieldGetterTypeMismatchFallsBack)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_GETSTATIC, arena.intern("p/Rec"), arena.intern("SV"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "get", "()Lp/Sup;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "()J", impl, "()J"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field handle impl outside strict direct-access subset");
}

// ---- F1/F2 直派载荷 engagement（波次二 P1，发射端只消费载荷）----

// F1 直派形态：indy 描述符本身即字段读（()I 对 GETSTATIC SV:I）→ form 不变、
// 载荷 engage（isStatic/isWrite/owner/name/fieldDesc 供发射端直接发射字段访问）。
TEST(IndyDispatchClassifyTest, DirectFieldGetterShapeEngagesPayload)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_GETSTATIC, arena.intern("p/Rec"), arena.intern("SV"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "get", "()I", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "()I", impl, "()I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field getter (GETSTATIC): direct field access site "
                 "(descriptor-shaped)");
    ASSERT_TRUE(d.directField.has_value());
    EXPECT_TRUE(d.directField->isStatic);
    EXPECT_FALSE(d.directField->isWrite);
    EXPECT_EQ(d.directField->owner, "p/Rec");
    EXPECT_EQ(d.directField->name, "SV");
    EXPECT_EQ(d.directField->fieldDesc, "I");
    EXPECT_FALSE(d.recordMethods.has_value());
}

// F1 直派形态（写）：indy 描述符 (Lp/Rec;I)V 对 PUTFIELD f:I → 载荷 engage（isWrite）。
TEST(IndyDispatchClassifyTest, DirectFieldSetterShapeEngagesPayload)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_PUTFIELD, arena.intern("p/Rec"), arena.intern("f"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "set", "(Lp/Rec;I)V", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(I)V", impl, "(I)V"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field setter (PUTFIELD): direct field access site "
                 "(descriptor-shaped)");
    ASSERT_TRUE(d.directField.has_value());
    EXPECT_FALSE(d.directField->isStatic);
    EXPECT_TRUE(d.directField->isWrite);
    EXPECT_EQ(d.directField->owner, "p/Rec");
    EXPECT_EQ(d.directField->name, "f");
    EXPECT_EQ(d.directField->fieldDesc, "I");
}

// F1 lambda 捕获形态（站点=创建函数接口实例，indy 返回 Lp/Sup;）：form 保持
// DirectFieldAccess（波次一候选标记，reason 逐字不变），载荷不 engage——发射端
// 走既有 helper 路径（零回归边界）。
TEST(IndyDispatchClassifyTest, LambdaCaptureFieldHandleKeepsCandidateWithoutPayload)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_GETSTATIC, arena.intern("p/Rec"), arena.intern("SV"),
                       arena.intern("I"));
    auto* indy = makeIndy(index, "get", "()Lp/Sup;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "()I", impl, "()I"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::DirectFieldAccess);
    EXPECT_STREQ(d.reason,
                 "lambda-metafactory field getter (GETSTATIC): direct field access candidate");
    EXPECT_FALSE(d.directField.has_value());
    EXPECT_FALSE(d.recordMethods.has_value());
}

// F2 访问器直派载荷：indy 名/描述符与唯一访问器句柄逐位一致（x (Lp/Point;)I ←
// H_INVOKEVIRTUAL p/Point.x()I）→ accessor engage（tag/owner/name/desc）。
TEST(IndyDispatchClassifyTest, RecordAccessorMatchEngagesAccessorPayload)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x"));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Point;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Point"),
                                arena.intern("x"), arena.intern("()I")));
    auto* indy =
        makeIndy(index, "x", "(Lp/Point;)I", recordObjectMethodsBsm(index), std::move(bsmArgs));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RecordObjectMethods);
    EXPECT_STREQ(d.reason, "record ObjectMethods.bootstrap: statically nameable accessor targets");
    ASSERT_TRUE(d.recordMethods.has_value());
    EXPECT_EQ(d.recordMethods->recordClass, "p/Point");
    EXPECT_EQ(d.recordMethods->names, "x");
    EXPECT_EQ(d.recordMethods->methodsDesc, "(Lp/Point;)I");
    ASSERT_TRUE(d.recordMethods->accessor.has_value());
    EXPECT_EQ(d.recordMethods->accessor->tag, Opcodes::H_INVOKEVIRTUAL);
    EXPECT_EQ(d.recordMethods->accessor->owner, "p/Point");
    EXPECT_EQ(d.recordMethods->accessor->name, "x");
    EXPECT_EQ(d.recordMethods->accessor->name, indy->name.to_std_string());
    EXPECT_EQ(d.recordMethods->accessor->desc, "()I");
    EXPECT_FALSE(d.directField.has_value());
}

// F2 ObjectMethods 保留方法名（javac equals/hashCode/toString：target 是多组件
// 聚合运算，非单一访问器调用）→ form 不变、accessor 不 engage（发射端走 helper）。
TEST(IndyDispatchClassifyTest, ReservedObjectMethodsNameKeepsHelperFallbackPayload)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x;y"));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Point;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Point"),
                                arena.intern("x"), arena.intern("()I")));
    auto* indy = makeIndy(index, "hashCode", "(Lp/Point;)I", recordObjectMethodsBsm(index),
                          std::move(bsmArgs));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RecordObjectMethods);
    EXPECT_STREQ(d.reason, "record ObjectMethods.bootstrap: statically nameable accessor targets");
    ASSERT_TRUE(d.recordMethods.has_value());
    EXPECT_FALSE(d.recordMethods->accessor.has_value());
    EXPECT_FALSE(d.directField.has_value());
}

// ---- B4/T2：LMF 展开子集外的 fallback（RuntimeHelper + 分类理由）----

// 基本类型收窄（sam (J)V、impl (I)V）：展开子集外 → RuntimeHelper。
TEST(IndyDispatchClassifyTest, LmfPrimitiveNarrowingFallsBack)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_INVOKESTATIC, arena.intern("N"), arena.intern("useInt"),
                       arena.intern("(I)V"));
    auto* indy = makeIndy(index, "accept", "()Lp/LongSink;", lmfBsm(index, "metafactory"),
                          lambdaBsmArgs(index, "(J)V", impl, "(J)V"));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(
        d.reason,
        "lambda-metafactory: unsupported impl adaptation (narrowing / box+widen / layout mismatch)");
}

// altMetafactory 未知 flag 位：尾部解析失败 → RuntimeHelper。
TEST(IndyDispatchClassifyTest, AltMetafactoryUnknownFlagsFallBack)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto impl = Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                       arena.intern("(I)I"));
    auto args = lambdaBsmArgs(index, "(I)I", impl, "(I)I");
    args.emplace_back(8);   // 未知 flag 位
    auto* indy = makeIndy(index, "applyAsInt", "()Ljava/util/function/IntUnaryOperator;",
                          lmfAltBsm(index), std::move(args));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(
        d.reason, "lambda-metafactory: altMetafactory tail parse failed (unknown flags / shape)");
}

// ---- D：非标准（自定义）bootstrap 的 fallback（RuntimeHelper + D1/D2 细分理由）----

// bsm owner 在编译输入内（非 library 类）：D1——波次二静态求值候选。
TEST(IndyDispatchClassifyTest, CustomBootstrapInCompilationInputFallsBack)
{
    CompilerIndex index;
    addEmptyClass(index, "my/Bsm");

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(42);
    auto* indy = makeIndy(
        index, "call", "(I)I",
        Handle(Opcodes::H_INVOKESTATIC, index.arena().intern("my/Bsm"),
               index.arena().intern("bootstrap"),
               index.arena().intern(
                   "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
                   "MethodType;I)Ljava/lang/invoke/CallSite;")),
        std::move(bsmArgs));

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(
        d.reason,
        "custom bootstrap: bsm body in compilation input (static evaluation candidate, wave-2)");
}

// bsm owner 不在编译输入（库/JDK 类）：D2——仅运行期 crack 候选。
TEST(IndyDispatchClassifyTest, CustomBootstrapExternalBsmFallsBack)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* indy = makeIndy(
        index, "call", "(I)I",
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("some/lib/Hidden"),
               arena.intern("link"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/"
                            "invoke/MethodType;)Ljava/lang/invoke/CallSite;")),
        std::vector<ca::core::Any>{});

    auto d = classifyIndySite(index, indy);
    EXPECT_EQ(d.form, IndyDispatchForm::RuntimeHelper);
    EXPECT_STREQ(d.reason, "custom bootstrap: bsm owner external (runtime crack candidate only)");
}

// ---- report 收集路径：零回归 + 定位/序号/计数 ----

// 同一方法 3 个站点（concat / lambda / 自定义）：判定序号按指令序 0/1/2，计数一致；
// 展开行为不受 report 收集影响（concat+lambda 展开、自定义 indy 原样保留）。
TEST(IndyDispatchClassifyTest, ReportCollectsOrdinalsFormsAndCounts)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // 站点1：makeConcat（String,String→String，参数已压栈）。
    auto* concat = makeIndy(index, "makeConcat", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                            stringConcatBsm(index, "makeConcat"), std::vector<ca::core::Any>{});
    // 站点2：无参 lambda。
    auto impl = Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                       arena.intern("(I)I"));
    auto* lambda = makeIndy(index, "applyAsInt", "()Ljava/util/function/IntUnaryOperator;",
                            lmfBsm(index, "metafactory"), lambdaBsmArgs(index, "(I)I", impl, "(I)I"));
    // 站点3：自定义 bootstrap（外部 owner）。
    auto* custom = makeIndy(
        index, "call", "(I)I",
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("some/lib/Hidden"), arena.intern("link"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/"
                            "invoke/MethodType;)Ljava/lang/invoke/CallSite;")),
        std::vector<ca::core::Any>{});

    MethodNode* m = addMethodToIndex(
        index, "Mix", "run", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        {new VarInsnNode(Opcodes::ALOAD, 0), new VarInsnNode(Opcodes::ALOAD, 1), concat,
         new InsnNode(Opcodes::ICONST_0), lambda, new InsnNode(Opcodes::POP),
         new InsnNode(Opcodes::ICONST_0), custom, new InsnNode(Opcodes::POP),
         new InsnNode(Opcodes::ARETURN)});

    IndyDispatchReport report;
    InvokeDynamicPreprocessor().process(index, &report);

    // 判定报告：3 站点、指令序 0/1/2、形态与定位正确。
    ASSERT_EQ(report.sites.size(), 3u);
    EXPECT_EQ(report.sites[0].form, IndyDispatchForm::StaticExpanded);
    EXPECT_EQ(report.sites[0].ordinal, 0);
    EXPECT_STREQ(report.sites[0].reason,
                 "string-concat: makeConcat expanded to StringBuilder bytecode by phase-1");
    EXPECT_EQ(report.sites[1].form, IndyDispatchForm::StaticExpanded);
    EXPECT_EQ(report.sites[1].ordinal, 1);
    EXPECT_EQ(report.sites[2].form, IndyDispatchForm::RuntimeHelper);
    EXPECT_EQ(report.sites[2].ordinal, 2);
    for (const auto& site : report.sites) {
        EXPECT_EQ(site.className, "Mix");
        EXPECT_EQ(site.methodName, "run");
    }
    EXPECT_EQ(report.count(IndyDispatchForm::StaticExpanded), 2u);
    EXPECT_EQ(report.count(IndyDispatchForm::RuntimeHelper), 1u);
    EXPECT_EQ(report.count(IndyDispatchForm::DirectFieldAccess), 0u);
    EXPECT_EQ(report.count(IndyDispatchForm::RecordObjectMethods), 0u);

    // 字节码输出不受 report 收集影响：concat/lambda 展开、自定义 indy 保留。
    EXPECT_EQ(countIndy(m), 1);
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// report 重载与默认重载对同一形态产生相同改写（判定=标记，零回归证明）：
// 两个独立构建的等价 index，一个走 process(index)，一个走 process(index,&report)，
// 展开后的指令形态一致。
namespace {

// 构建一个含单个 makeConcatWithConstants 站点的 index，返回方法指针。
MethodNode* buildSingleConcatIndex(CompilerIndex& index)
{
    auto& arena = index.arena();
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(std::string("a=\x01"));
    auto* indy = makeIndy(index, "makeConcatWithConstants", "(I)Ljava/lang/String;",
                          stringConcatBsm(index, "makeConcatWithConstants"), std::move(bsmArgs));
    return addMethodToIndex(index, "Same", "join", "(I)Ljava/lang/String;",
                            {new VarInsnNode(Opcodes::ILOAD, 0), indy,
                             new InsnNode(Opcodes::ARETURN)});
}

}   // namespace

TEST(IndyDispatchClassifyTest, ReportOverloadMatchesDefaultOverloadBehavior)
{
    CompilerIndex index1;
    MethodNode*   method1 = buildSingleConcatIndex(index1);
    InvokeDynamicPreprocessor().process(index1);

    CompilerIndex   index2;
    MethodNode*     method2 = buildSingleConcatIndex(index2);
    IndyDispatchReport report;
    InvokeDynamicPreprocessor().process(index2, &report);

    EXPECT_EQ(countIndy(method1), 0);
    EXPECT_EQ(countIndy(method2), 0);
    ASSERT_EQ(report.sites.size(), 1u);
    EXPECT_EQ(report.sites[0].form, IndyDispatchForm::StaticExpanded);
    EXPECT_EQ(report.sites[0].className, "Same");
    EXPECT_EQ(report.sites[0].ordinal, 0);
}

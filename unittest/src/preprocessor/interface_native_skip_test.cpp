// 接口方法标记跳过契约测试（JLS 9.4/JVMS 4.6：接口方法禁止 ACC_NATIVE）。
//
// 背景：MarkAll / annotation / prefix 三条标记路径曾会把接口的 default /
// static / 私有方法（有 Code、非 abstract）标进 native 编译集，产出带
// ACC_NATIVE 接口方法的类文件 → JVM ClassFormatError。契约：接口整体不进
// native 集；<init>/<clinit> 搬运器同步跳过（搬运产物只会是永不标记的
// 死静态方法）。
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/preprocessor/init_clinit_mover_preprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/native_annotation_preprocessor.hpp"
#include "mj2x/preprocessor/native_method_prefix_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

bool isMarked(CompilerClass& cls, const std::string& name)
{
    for (auto& m : cls.methods) {
        if (m.methodNode->name.to_std_string() == name) return m.markedForCompile;
    }
    return false;
}

int markedCount(CompilerClass& cls)
{
    int n = 0;
    for (auto& m : cls.methods) {
        if (m.markedForCompile) ++n;
    }
    return n;
}

MethodNode* findMethod(CompilerClass& cls, const std::string& name)
{
    for (auto* m : cls.classNode->methods) {
        if (m->name.to_std_string() == name) return m;
    }
    return nullptr;
}

/// 造一个方法：access + name + desc + 体（默认一条 RETURN 类指令保非空）。
MethodNode* makeMethod(CompilerIndex& index, int access, const std::string& name,
                       const std::string& desc, bool withBody = true)
{
    auto& arena                          = index.arena();
    std::vector<ca::str::Utf8StringRef> noExc;
    auto*                               mn = new MethodNode(access, arena.intern(name.c_str()),
                                 arena.intern(desc.c_str()), arena.intern(""), noExc);
    mn->maxStack  = 4;
    mn->maxLocals = 4;
    if (withBody) mn->instructions.add(new InsnNode(Opcodes::RETURN));
    return mn;
}

/// 造一个类：isInterface 决定 ACC_INTERFACE|ACC_ABSTRACT 复合访问位。
CompilerClass& buildClass(CompilerIndex& index, const std::string& className,
                          bool isInterface, std::vector<MethodNode*> methods)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = isInterface ? (Opcodes::ACC_INTERFACE | Opcodes::ACC_ABSTRACT)
                                : Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");
    cn->methods   = std::move(methods);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    for (auto* m : cls.classNode->methods) {
        CompilerMethod cm;
        cm.methodNode = m;
        cls.methods.push_back(std::move(cm));
    }
    index.addClass(className, std::move(cls));
    return *index.getClass(className);
}

/// 接口样本：default（public+Code）/ static / private / abstract 四形态。
std::vector<MethodNode*> interfaceMethods(CompilerIndex& index)
{
    return {
        makeMethod(index, Opcodes::ACC_PUBLIC, "greet", "()Ljava/lang/String;"),
        makeMethod(index, Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "kind",
                   "()Ljava/lang/String;"),
        makeMethod(index, Opcodes::ACC_PRIVATE, "secret", "()Ljava/lang/String;"),
        makeMethod(index, Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT, "name",
                   "()Ljava/lang/String;", false),
    };
}

}   // namespace

// MarkAll：接口的 default/static/私有方法一个都不标；同 index 普通类照常全标。
TEST(InterfaceNativeSkipTest, MarkAllSkipsInterfaceConcreteMethods)
{
    CompilerIndex index;
    auto& iface = buildClass(index, "demo/Op", true, interfaceMethods(index));
    auto& cls   = buildClass(index, "demo/Impl", false,
                             {makeMethod(index, Opcodes::ACC_PUBLIC, "run", "()V")});

    MarkAllMethodsPreprocessor().process(index);

    EXPECT_EQ(markedCount(iface), 0);
    EXPECT_FALSE(isMarked(iface, "greet"));
    EXPECT_FALSE(isMarked(iface, "kind"));
    EXPECT_FALSE(isMarked(iface, "secret"));
    EXPECT_TRUE(isMarked(cls, "run"));
}

// 前缀标记：接口内命中前缀的 default 方法不标；类的同名方法照标。
TEST(InterfaceNativeSkipTest, PrefixMarkerSkipsInterfaceMethods)
{
    CompilerIndex index;
    auto& iface = buildClass(index, "demo/Op", true, interfaceMethods(index));
    auto& cls   = buildClass(index, "demo/Impl", false,
                             {makeMethod(index, Opcodes::ACC_PUBLIC, "run", "()V")});

    NativeMethodPrefixPreprocessor({"demo/"}).process(index);

    EXPECT_EQ(markedCount(iface), 0);
    EXPECT_TRUE(isMarked(cls, "run"));
}

// 注解标记：接口内带目标注解的 default 方法不标；类的带注解方法照标。
TEST(InterfaceNativeSkipTest, AnnotationMarkerSkipsInterfaceMethods)
{
    CompilerIndex index;

    const char*        annDesc = "Lnative/Native;";
    auto&              iface   = buildClass(index, "demo/Op", true, interfaceMethods(index));
    auto*              clsM    = makeMethod(index, Opcodes::ACC_PUBLIC, "run", "()V");
    clsM->visibleAnnotations.push_back(
        clsM->create<AnnotationNode>(clsM->arena(), index.arena().intern(annDesc)));
    auto& cls = buildClass(index, "demo/Impl", false, {clsM});

    NativeAnnotationPreprocessor(annDesc).process(index);

    EXPECT_EQ(markedCount(iface), 0);
    EXPECT_TRUE(isMarked(cls, "run"));
}

// 搬运器：接口 <clinit> 不搬（不产出 __regc_* 死静态方法），原体保持。
TEST(InterfaceNativeSkipTest, ClinitMoverSkipsInterfaceClinit)
{
    CompilerIndex index;
    auto* clinit = makeMethod(index, Opcodes::ACC_STATIC, "<clinit>", "()V");
    auto& iface  = buildClass(index, "demo/Op", true, {clinit});

    InitClinitMoverPreprocessor().process(index);

    EXPECT_EQ(findMethod(iface, "<clinit>"), clinit);
    bool hasMoved = false;
    for (auto* m : iface.classNode->methods) {
        if (m->name.to_std_string().rfind("__regc_", 0) == 0) hasMoved = true;
    }
    EXPECT_FALSE(hasMoved);
    EXPECT_EQ(iface.classNode->methods.size(), 1u);
}

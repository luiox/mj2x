#include <gmock/gmock.h>

#include <cstring>

#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string_arena.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

namespace {

ca::str::Utf8StringArena testArena;

}

// Helper: create MethodNode with instructions
static std::unique_ptr<MethodNode> makeMethod(int access, const std::string& name,
                                              const std::string&                       desc,
                                              std::initializer_list<AbstractInsnNode*> insns)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto                                mn = std::make_unique<MethodNode>(access,
                                           testArena.intern(name.c_str()),
                                           testArena.intern(desc.c_str()),
                                           testArena.intern(""),
                                           noExcepts);
    mn->maxStack                           = 10;
    mn->maxLocals                          = 10;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

static CompilerClass makeCompilerClass(const std::string& name)
{
    CompilerClass cls;
    auto          cn = std::make_unique<ClassNode>();
    cn->name         = testArena.intern(name.c_str());
    cn->version      = Opcodes::V1_8;
    cn->access       = Opcodes::ACC_PUBLIC;
    cn->superName    = testArena.intern("java/lang/Object");
    cls.classNode    = std::move(cn);
    return cls;
}

static MethodNode* findClassMethod(const CompilerClass& cls, const std::string& name,
                                   const std::string& desc)
{
    for (auto* method : cls.classNode->methods) {
        if (method->name.to_std_string() == name && method->desc.to_std_string() == desc) {
            return method;
        }
    }
    return nullptr;
}

// IMPORTANT: CodeConverter MUST outlive the Block* pointers it returns.
// Always declare CodeConverter and blocks in the same scope.

TEST(StackCBackendTest, SimpleReturn)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "empty", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("return;"), std::string::npos);
    EXPECT_NE(result.text.find("void"), std::string::npos);
}

TEST(StackCBackendTest, LabelAtFunctionEndEmitsEmptyStatement)
{
    auto* tailLabel                  = new LabelNode();
    tailLabel->label->bytecodeOffset = 1;
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "tailLabel",
                         "()V",
                                   {
                             new InsnNode(Opcodes::RETURN),
                             tailLabel,
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find(":\n    ;\n"), std::string::npos) << result.text;
}

TEST(StackCBackendTest, AddMethod)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                                   {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("jint"), std::string::npos);
    EXPECT_NE(result.text.find("jint _mj2x_ret = stack[sp--].i"), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, NULL);"),
              std::string::npos);
    EXPECT_NE(result.text.find("+"), std::string::npos);
}

TEST(StackCBackendTest, InvokeDynamicLowersThroughMethodHandleBridge)
{
    auto           bsm = Handle(Opcodes::H_INVOKESTATIC,
                      testArena.intern("java/lang/invoke/StringConcatFactory"),
                      testArena.intern("makeConcatWithConstants"),
                      testArena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                                                 "Ljava/lang/invoke/MethodType;Ljava/lang/String;"
                                                 "[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;"),
                      false);
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "concat",
                         "(I)Ljava/lang/String;",
                                   {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new InvokeDynamicInsnNode(testArena.intern("makeConcatWithConstants"),
                                                       testArena.intern("(I)Ljava/lang/String;"),
                                                       bsm,
                                                                 {std::string("v=\x01")}),
                             new InsnNode(Opcodes::ARETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(result.text.find("java/lang/invoke/StringConcatFactory"), std::string::npos);
    EXPECT_NE(result.text.find("invokeWithArguments"), std::string::npos);
}

TEST(StackCBackendTest, BooleanInvokeResultUsesFullStackSlot)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "eq",
                         "(Ljava/lang/String;Ljava/lang/Object;)Z",
                                   {
                             new VarInsnNode(Opcodes::ALOAD, 0),
                             new VarInsnNode(Opcodes::ALOAD, 1),
                             new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                testArena.intern("java/lang/String"),
                                                testArena.intern("equals"),
                                                testArena.intern("(Ljava/lang/Object;)Z"),
                                                false),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(result.text.find(".i = (*env)->CallBooleanMethod"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find(".z = (*env)->CallBooleanMethod"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("if ((*env)->ExceptionCheck(env))"), std::string::npos)
        << result.text;
}

TEST(StackCBackendTest, CastEmission)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "d2i",
                         "(D)I",
                                   {
                             new VarInsnNode(Opcodes::DLOAD, 0),
                             new InsnNode(Opcodes::D2I),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("isnan(v)"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp].i = (jint)v"), std::string::npos);
}

TEST(StackCBackendTest, LongShiftAndCompareEmission)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "shiftCmp",
                         "(JI)I",
                                   {
                             new VarInsnNode(Opcodes::LLOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 2),
                             new InsnNode(Opcodes::LSHL),
                             new InsnNode(Opcodes::LCONST_0),
                             new InsnNode(Opcodes::LCMP),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp].i & 63"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp - 1].i = (stack[sp - 1].j > stack[sp].j)"),
              std::string::npos);
}

TEST(StackCBackendTest, FloatRemainderEmission)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "rem",
                         "(FF)F",
                                   {
                             new VarInsnNode(Opcodes::FLOAD, 0),
                             new VarInsnNode(Opcodes::FLOAD, 1),
                             new InsnNode(Opcodes::FREM),
                             new InsnNode(Opcodes::FRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("fmodf("), std::string::npos);
}

TEST(StackCBackendTest, TypedWideStackOpsEmitConcreteShuffle)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "shape",
                         "()I",
                                   {
                             new InsnNode(Opcodes::LCONST_1),
                             new InsnNode(Opcodes::DUP2),
                             new InsnNode(Opcodes::POP2),
                             new InsnNode(Opcodes::POP2),
                             new InsnNode(Opcodes::ICONST_5),
                             new InsnNode(Opcodes::LCONST_1),
                             new InsnNode(Opcodes::DUP2_X1),
                             new InsnNode(Opcodes::POP2),
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::ICONST_2),
                             new InsnNode(Opcodes::ICONST_3),
                             new InsnNode(Opcodes::ICONST_4),
                             new InsnNode(Opcodes::DUP2_X2),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_EQ(result.text.find("Unsupported"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("stack[sp + 1] = stack[sp]; sp++;"), std::string::npos);
    EXPECT_NE(result.text.find("sp -= 1;"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp - 1] = v1;"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp - 3] = v2;"), std::string::npos);
}

TEST(StackCBackendTest, FieldAccess)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX",
                         "()I",
                                   {
                             new VarInsnNode(Opcodes::ALOAD, 0),
                             new FieldInsnNode(Opcodes::GETFIELD,
                                               ca::str::Utf8StringRef::from_cstr("Foo"),
                                               ca::str::Utf8StringRef::from_cstr("x"),
                                               ca::str::Utf8StringRef::from_cstr("I")),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("GetIntField"), std::string::npos);
    EXPECT_NE(result.text.find("static jfieldID fid_"), std::string::npos);
    EXPECT_NE(result.text.find("MonitorEnter(env, _mj2x_owner_class)"), std::string::npos);
    EXPECT_NE(result.text.find("jfieldID fid_local = NULL"), std::string::npos);
    EXPECT_NE(result.text.find("GetFieldID(env, lc, \"x\", \"I\")"), std::string::npos);
    EXPECT_NE(result.text.find("GetIntField(env, stack[sp].l, fid_local)"), std::string::npos);
}

TEST(StackCBackendTest, InvokeMethod)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call",
                         "()V",
                                   {
                             new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                ca::str::Utf8StringRef::from_cstr("Other"),
                                                ca::str::Utf8StringRef::from_cstr("run"),
                                                ca::str::Utf8StringRef::from_cstr("()V"),
                                                false),
                             new InsnNode(Opcodes::RETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("CallStaticVoidMethod"), std::string::npos);
    EXPECT_NE(result.text.find("static jmethodID smid_"), std::string::npos);
    EXPECT_NE(result.text.find("MonitorEnter(env, _mj2x_owner_class)"), std::string::npos);
    EXPECT_NE(result.text.find("jmethodID mid_local = NULL"), std::string::npos);
    EXPECT_NE(result.text.find("GetStaticMethodID"), std::string::npos);
    EXPECT_NE(result.text.find("CallStaticVoidMethod(env, lc, mid_local)"), std::string::npos);
}

TEST(StackCBackendTest, StaticInvokeReturnUsesArgumentBaseSlot)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call",
                         "(II)I",
                                   {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                ca::str::Utf8StringRef::from_cstr("Other"),
                                                ca::str::Utf8StringRef::from_cstr("add"),
                                                ca::str::Utf8StringRef::from_cstr("(II)I"),
                                                false),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp - 1].i = (*env)->CallStaticIntMethod"), std::string::npos);
    EXPECT_NE(result.text.find("sp -= 1;"), std::string::npos);
}

TEST(StackCBackendTest, InvokeSpecialLooksUpDeclaringClass)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "create",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find(
                  "jclass lc = (jclass)_mj2x_track_local_ref(env, _mj2x_refs, &_mj2x_ref_count, "
                  "_mj2x_find_class(env, _mj2x_owner_class, \"java/lang/Object\"))"),
              std::string::npos);
    EXPECT_NE(result.text.find("CallNonvirtualVoidMethod(env, stack[sp - 0].l, lc, mid"),
              std::string::npos);
}

TEST(StackCBackendTest, StringConstantEmission)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "literal",
                         "()Ljava/lang/String;",
                                   {
                             new LdcInsnNode(std::string("hello \"jni\"\\path")),
                             new InsnNode(Opcodes::POP),
                             new LdcInsnNode(std::string("hello \"jni\"\\path")),
                             new InsnNode(Opcodes::ARETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("NewStringUTF"), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_string_pool + 0"), std::string::npos);
    EXPECT_EQ(result.text.find("\"hello \\\"jni\\\"\\\\path\""), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_string_pool + 0"), result.text.rfind("_mj2x_string_pool + 0"))
        << result.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("static const unsigned char _mj2x_string_pool[]"), std::string::npos);
    EXPECT_EQ(source.find("\"hello \\\"jni\\\"\\\\path\""), std::string::npos) << source;
}

TEST(StackCBackendTest, PrimitiveArrayEmission)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "array",
                         "()I",
                                   {
                             new InsnNode(Opcodes::ICONST_2),
                             new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_INT),
                             new InsnNode(Opcodes::DUP),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::ICONST_5),
                             new InsnNode(Opcodes::IASTORE),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::IALOAD),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("NewIntArray"), std::string::npos);
    EXPECT_NE(result.text.find("SetIntArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("GetIntArrayRegion"), std::string::npos);
}

TEST(StackCBackendTest, ByteArrayOpsDisambiguateBooleanArrays)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "array",
                         "()I",
                                   {
                             new InsnNode(Opcodes::ICONST_2),
                             new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_BYTE),
                             new InsnNode(Opcodes::DUP),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::ICONST_5),
                             new InsnNode(Opcodes::BASTORE),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::BALOAD),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("FindClass(env, \"[Z\")"), std::string::npos);
    EXPECT_NE(result.text.find("IsInstanceOf(env, stack[sp - 1].l, boolean_array_class"),
              std::string::npos);
    EXPECT_NE(result.text.find("GetBooleanArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("GetByteArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("SetBooleanArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("SetByteArrayRegion"), std::string::npos);
}

TEST(StackCBackendTest, CheckCastEmission)
{
    StackCBackend backend;
    auto          mn =
        makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                   "cast",
                   "(Ljava/lang/Object;)Ljava/lang/String;",
                   {
                       new VarInsnNode(Opcodes::ALOAD, 0),
                       new TypeInsnNode(Opcodes::CHECKCAST,
                                        ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                       new InsnNode(Opcodes::ARETURN),
                   });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("ClassCastException"), std::string::npos);
    EXPECT_NE(result.text.find("IsInstanceOf"), std::string::npos);
}

TEST(StackCBackendTest, ClassLookupsUseOwnerClassLoaderHelper)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC,
        "refs",
        "(Ljava/lang/Object;)I",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("pkg/Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY, ca::str::Utf8StringRef::from_cstr("pkg/Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY, ca::str::Utf8StringRef::from_cstr("[Lpkg/Foo;")),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ALOAD, 1),
            new TypeInsnNode(Opcodes::INSTANCEOF, ca::str::Utf8StringRef::from_cstr("pkg/Foo")),
            new InsnNode(Opcodes::POP),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               ca::str::Utf8StringRef::from_cstr("pkg/Helper"),
                               ca::str::Utf8StringRef::from_cstr("value"),
                               ca::str::Utf8StringRef::from_cstr("()I"),
                               false),
            new InsnNode(Opcodes::IRETURN),
        });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("pkg/Owner", cm, blocks);

    EXPECT_NE(result.text.find("jclass _mj2x_owner_class = (jclass)_mj2x_track_local_ref(env, "
                               "_mj2x_refs, &_mj2x_ref_count, (*env)->GetObjectClass(env, this));"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("_mj2x_find_class(env, _mj2x_owner_class, \"pkg/Foo\")"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("_mj2x_find_class(env, _mj2x_owner_class, \"[Lpkg/Foo;\")"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("_mj2x_find_class(env, _mj2x_owner_class, \"pkg/Helper\")"),
              std::string::npos)
        << result.text;
    EXPECT_EQ(result.text.find("FindClass(env, \"pkg/Foo\")"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("FindClass(env, \"[Lpkg/Foo;\")"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("FindClass(env, \"pkg/Helper\")"), std::string::npos) << result.text;
}

TEST(StackCBackendTest, InjectMakesMethodNative)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("Test");

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_FALSE(mn->access & Opcodes::ACC_NATIVE);
    backend.injectIntoClass(cls, cm, result);
    EXPECT_TRUE(mn->access & Opcodes::ACC_NATIVE);
    EXPECT_EQ(mn->instructions.size(), 0);
}

TEST(StackCBackendTest, InjectAddsClinitCLoaderRegistrationCall)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    mn.release();

    CodeConverter cc;
    auto          blocks = cc.convert(*cm.methodNode);
    auto          result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* bridge = findClassMethod(cls, "__mj2x_registerClass", "(I)V");
    EXPECT_EQ(bridge, nullptr);

    auto* clinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(clinit, nullptr);
    EXPECT_EQ(clinit->instructions.size(), 4);
    auto* call = clinit->instructions.get(2);
    ASSERT_EQ(call->getType(), AbstractInsnNode::METHOD_INSN);
    auto* invoke = static_cast<MethodInsnNode*>(call);
    EXPECT_EQ(invoke->opcode, Opcodes::INVOKESTATIC);
    EXPECT_EQ(invoke->owner.to_std_string(), "mj2x/cloader/Loader");
    EXPECT_EQ(invoke->name.to_std_string(), "registerNativesForClass");
    EXPECT_EQ(invoke->desc.to_std_string(), "(ILjava/lang/Class;)V");
    EXPECT_EQ(clinit->instructions.get(3)->opcode, Opcodes::RETURN);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_EQ(source.find("Java_com_example_Foo__1_1mj2x_1registerClass__I"), std::string::npos)
        << source;
    EXPECT_NE(source.find("_mj2x_reg_methods[0] = _mj2x_register_com_2fexample_2fFoo"),
              std::string::npos)
        << source;
    EXPECT_NE(source.find("FindClass(env, \"mj2x/cloader/Loader\")"), std::string::npos) << source;
    EXPECT_NE(source.find("{(char*)\"registerNativesForClass\", (char*)\"(ILjava/lang/Class;)V\""),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("bridge_method"), std::string::npos) << source;
}

TEST(StackCBackendTest, JarNativeLoaderRunsBeforeRegistrationBridge)
{
    StackCBackend backend;
    backend.setJarNativeLoaderResourcePath("META-INF/mj2x/native/app.dll", ".dll");
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    mn.release();

    CodeConverter cc;
    auto          blocks = cc.convert(*cm.methodNode);
    auto          result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* clinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(clinit, nullptr);
    ASSERT_EQ(clinit->instructions.size(), 7);
    auto* loaderCall = clinit->instructions.get(2);
    ASSERT_EQ(loaderCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* loaderInvoke = static_cast<MethodInsnNode*>(loaderCall);
    EXPECT_EQ(loaderInvoke->owner.to_std_string(), "mj2x/runtime/NativeLoader");
    EXPECT_EQ(loaderInvoke->name.to_std_string(), "load");
    EXPECT_EQ(loaderInvoke->desc.to_std_string(), "(Ljava/lang/String;Ljava/lang/String;)V");

    auto* bridgeCall = clinit->instructions.get(5);
    ASSERT_EQ(bridgeCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* bridgeInvoke = static_cast<MethodInsnNode*>(bridgeCall);
    EXPECT_EQ(bridgeInvoke->owner.to_std_string(), "mj2x/cloader/Loader");
    EXPECT_EQ(bridgeInvoke->name.to_std_string(), "registerNativesForClass");
    EXPECT_EQ(bridgeInvoke->desc.to_std_string(), "(ILjava/lang/Class;)V");
}

TEST(StackCBackendTest, ExistingClinitBodyMovesToHiddenClassProxy)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "test", "()V", {new InsnNode(Opcodes::RETURN)});
    auto           clinit = makeMethod(Opcodes::ACC_STATIC,
                             "<clinit>",
                             "()V",
                                       {
                                 new InsnNode(Opcodes::ICONST_1),
                                 new FieldInsnNode(Opcodes::PUTSTATIC,
                                                   testArena.intern("com/example/Foo"),
                                                   testArena.intern("VALUE"),
                                                   testArena.intern("I")),
                                 new InsnNode(Opcodes::RETURN),
                             });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    cls.classNode->methods.push_back(clinit.get());
    mn.release();
    clinit.release();

    CodeConverter cc;
    auto          blocks = cc.convert(*cm.methodNode);
    auto          result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* rewrittenClinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(rewrittenClinit, nullptr);
    ASSERT_EQ(rewrittenClinit->instructions.size(), 4);
    auto* bridgeCall = rewrittenClinit->instructions.get(2);
    ASSERT_EQ(bridgeCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* bridgeInvoke = static_cast<MethodInsnNode*>(bridgeCall);
    EXPECT_EQ(bridgeInvoke->owner.to_std_string(), "mj2x/cloader/Loader");
    EXPECT_EQ(bridgeInvoke->name.to_std_string(), "registerNativesForClass");
    EXPECT_EQ(bridgeInvoke->desc.to_std_string(), "(ILjava/lang/Class;)V");
    EXPECT_EQ(rewrittenClinit->instructions.get(3)->opcode, Opcodes::RETURN);

    for (auto* method : cls.classNode->methods) {
        EXPECT_EQ(method->name.to_std_string().find("__mj2x_hidden_clinit_"), std::string::npos)
            << method->name.to_std_string();
    }

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("DefineClass(env, \"com/example/__mj2xHiddenClinit"), std::string::npos)
        << source;
    EXPECT_NE(source.find("{(char*)\"__mj2x_hiddenClinit\", (char*)\"(Ljava/lang/Class;)V\""),
              std::string::npos)
        << source;
    EXPECT_NE(source.find("_mj2x_hidden_clinit_wrapper_"), std::string::npos) << source;
    EXPECT_NE(source.find("static jweak _mj2x_hidden_clinit_class_ref_"), std::string::npos)
        << source;
    EXPECT_NE(source.find("NewLocalRef(env, _mj2x_hidden_clinit_class_ref_"), std::string::npos)
        << source;
    EXPECT_NE(source.find("NewWeakGlobalRef(env, hidden_class)"), std::string::npos) << source;
    EXPECT_NE(source.find("MonitorEnter(env, owner_class)"), std::string::npos) << source;
    EXPECT_EQ(source.find("{(char*)\"__mj2x_hidden_clinit_"), std::string::npos) << source;
    EXPECT_NE(source.find("SetStaticIntField"), std::string::npos) << source;
}

TEST(StackCBackendTest, InterfaceDefaultMethodUsesHiddenProxy)
{
    StackCBackend backend;
    auto          mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "inc",
                         "(I)I",
                                  {
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         });
    mn->maxStack     = 2;
    mn->maxLocals    = 2;
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Op");
    cls.classNode->access |= Opcodes::ACC_INTERFACE | Opcodes::ACC_ABSTRACT;
    cls.classNode->methods.push_back(mn.get());
    mn.release();

    CodeConverter cc;
    auto          blocks = cc.convert(*cm.methodNode);
    auto          result = backend.compileMethod("com/example/Op", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* rewritten = findClassMethod(cls, "inc", "(I)I");
    ASSERT_NE(rewritten, nullptr);
    EXPECT_EQ(rewritten->access & Opcodes::ACC_NATIVE, 0);
    ASSERT_EQ(rewritten->instructions.size(), 4);
    EXPECT_EQ(rewritten->instructions.get(0)->opcode, Opcodes::ALOAD);
    EXPECT_EQ(rewritten->instructions.get(1)->opcode, Opcodes::ILOAD);
    auto* call = rewritten->instructions.get(2);
    ASSERT_EQ(call->getType(), AbstractInsnNode::METHOD_INSN);
    auto* invoke = static_cast<MethodInsnNode*>(call);
    EXPECT_EQ(invoke->opcode, Opcodes::INVOKESTATIC);
    EXPECT_NE(invoke->owner.to_std_string().find("com/example/__mj2xHidden"), std::string::npos);
    EXPECT_NE(invoke->name.to_std_string().find("__mj2x_default_"), std::string::npos);
    EXPECT_EQ(invoke->desc.to_std_string(), "(Lcom/example/Op;I)I");
    EXPECT_EQ(rewritten->instructions.get(3)->opcode, Opcodes::IRETURN);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("_mj2x_interface_default_wrapper_"), std::string::npos) << source;
    EXPECT_NE(source.find("static jweak _mj2x_interface_default_class_ref_"), std::string::npos)
        << source;
    EXPECT_NE(source.find("NewLocalRef(env, _mj2x_interface_default_class_ref_"), std::string::npos)
        << source;
    EXPECT_NE(source.find("NewWeakGlobalRef(env, hidden_class)"), std::string::npos) << source;
    EXPECT_NE(source.find("MonitorEnter(env, owner_class)"), std::string::npos) << source;
    EXPECT_NE(source.find("DefineClass(env, \"com/example/__mj2xHidden"), std::string::npos)
        << source;
    EXPECT_NE(source.find("{(char*)\"__mj2x_default_"), std::string::npos) << source;
    EXPECT_EQ(source.find("{(char*)\"inc\", (char*)\"(I)I\""), std::string::npos) << source;
}

TEST(StackCBackendTest, BranchAndComparison)
{
    StackCBackend backend;
    auto*         targetLabel          = new LabelNode();
    targetLabel->label->bytecodeOffset = 10;

    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "cmp",
                         "(II)I",
                                   {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new JumpInsnNode(Opcodes::IF_ICMPEQ, targetLabel->label),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::IRETURN),
                             targetLabel,
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("goto"), std::string::npos);
    EXPECT_NE(result.text.find("=="), std::string::npos);
    EXPECT_NE(result.text.find("sp -= 2;\n        if (cond) goto"), std::string::npos);
}

TEST(StackCBackendTest, BranchZeroObjectUsesReferenceAccessor)
{
    StackCBackend backend;
    auto*         targetLabel          = new LabelNode();
    targetLabel->label->bytecodeOffset = 10;

    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull",
                         "(Ljava/lang/Object;)I",
                                   {
                             new VarInsnNode(Opcodes::ALOAD, 0),
                             new JumpInsnNode(Opcodes::IFNULL, targetLabel->label),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::IRETURN),
                             targetLabel,
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp].l == NULL"), std::string::npos);
    EXPECT_NE(result.text.find("sp--;\n        if (cond) goto"), std::string::npos);
}

TEST(StackCBackendTest, NewObject)
{
    StackCBackend backend;
    auto          mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "create",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("AllocObject"), std::string::npos);
}

TEST(StackCBackendTest, IincInstruction)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "inc",
                         "(I)I",
                                   {
                             new IincInsnNode(0, 5),
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("locals[0].i += 5"), std::string::npos);
}

TEST(StackCBackendTest, StaticNativeFunctionAndOnLoadRegistration)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                                   {new InsnNode(Opcodes::IRETURN)});
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    auto          result = backend.compileMethod("com/example/Foo", cm, blocks);

    EXPECT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_FALSE(result.text.empty());
    EXPECT_NE(result.text.find("static jint JNICALL"), std::string::npos);
    EXPECT_NE(result.text.find("__mj2x_c_"), std::string::npos);
    EXPECT_EQ(result.text.find("JNIEXPORT"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("Java_com_example_Foo_add__II"), std::string::npos) << result.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("JNI_OnLoad"), std::string::npos) << source;
    EXPECT_NE(source.find("RegisterNatives"), std::string::npos) << source;
    EXPECT_NE(source.find("{(char*)\"add\", (char*)\"(II)I\""), std::string::npos) << source;
    EXPECT_EQ(source.find("Java_com_example_Foo_add__II"), std::string::npos) << source;
}

TEST(StackCBackendTest, RegisterNativesKeepsObjectParameterDescriptor)
{
    StackCBackend  backend;
    auto           mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull",
                         "(Ljava/lang/Object;)I",
                                   {
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::IRETURN),
                         });
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto          blocks = cc.convert(*mn);
    (void)backend.compileMethod("Test", cm, blocks);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("{(char*)\"isNull\", (char*)\"(Ljava/lang/Object;)I\""),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("Java_Test_isNull__Ljava_lang_Object_2"), std::string::npos) << source;
}

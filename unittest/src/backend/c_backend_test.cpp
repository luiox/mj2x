#include <gmock/gmock.h>

#include <cstring>

#include "mj2x/backend/cbackend/c_backend.hpp"
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
static std::unique_ptr<MethodNode> makeMethod(
    int access, const std::string& name, const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access,
        testArena.intern(name.c_str()),
        testArena.intern(desc.c_str()),
        testArena.intern(""),
        noExcepts);
    mn->maxStack = 10;
    mn->maxLocals = 10;
    for (auto* insn : insns)
        mn->instructions.add(insn);
    return mn;
}

static CompilerClass makeCompilerClass(const std::string& name) {
    CompilerClass cls;
    auto cn = std::make_unique<ClassNode>();
    cn->name = testArena.intern(name.c_str());
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->superName = testArena.intern("java/lang/Object");
    cls.classNode = std::move(cn);
    return cls;
}

static MethodNode* findClassMethod(
    const CompilerClass& cls,
    const std::string& name,
    const std::string& desc)
{
    for (auto* method : cls.classNode->methods) {
        if (method->name.to_std_string() == name &&
            method->desc.to_std_string() == desc) {
            return method;
        }
    }
    return nullptr;
}

// IMPORTANT: CodeConverter MUST outlive the Block* pointers it returns.
// Always declare CodeConverter and blocks in the same scope.

TEST(CBackendTest, SimpleReturn) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "empty", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("return;"), std::string::npos);
    EXPECT_NE(result.text.find("void"), std::string::npos);
}

TEST(CBackendTest, AddMethod) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add", "(II)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 1),
        new InsnNode(Opcodes::IADD),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("jint"), std::string::npos);
    EXPECT_NE(result.text.find("jint _mj2x_ret = stack[sp--].i"), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_clear_local_refs(env, _mj2x_refs, &_mj2x_ref_count, NULL);"),
              std::string::npos);
    EXPECT_NE(result.text.find("+"), std::string::npos);
}

TEST(CBackendTest, InvokeDynamicEmitsUnsupportedException) {
    auto bsm = Handle(Opcodes::H_INVOKESTATIC,
                      testArena.intern("java/lang/invoke/StringConcatFactory"),
                      testArena.intern("makeConcatWithConstants"),
                      testArena.intern("()V"),
                      false);
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "concat", "()Ljava/lang/String;", {
        new InvokeDynamicInsnNode(testArena.intern("makeConcatWithConstants"),
                                  testArena.intern("()Ljava/lang/String;"),
                                  bsm, {}),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("UnsupportedOperationException"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("invokedynamic"), std::string::npos) << result.text;
}

TEST(CBackendTest, CastEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "d2i", "(D)I", {
        new VarInsnNode(Opcodes::DLOAD, 0),
        new InsnNode(Opcodes::D2I),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("isnan(v)"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp].i = (jint)v"), std::string::npos);
}

TEST(CBackendTest, LongShiftAndCompareEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "shiftCmp", "(JI)I", {
        new VarInsnNode(Opcodes::LLOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 2),
        new InsnNode(Opcodes::LSHL),
        new InsnNode(Opcodes::LCONST_0),
        new InsnNode(Opcodes::LCMP),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp].i & 63"), std::string::npos);
    EXPECT_NE(result.text.find("stack[sp - 1].i = (stack[sp - 1].j > stack[sp].j)"), std::string::npos);
}

TEST(CBackendTest, FloatRemainderEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "rem", "(FF)F", {
        new VarInsnNode(Opcodes::FLOAD, 0),
        new VarInsnNode(Opcodes::FLOAD, 1),
        new InsnNode(Opcodes::FREM),
        new InsnNode(Opcodes::FRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("fmodf("), std::string::npos);
}

TEST(CBackendTest, FieldAccess) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX", "()I", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new FieldInsnNode(Opcodes::GETFIELD, ca::str::Utf8StringRef::from_cstr("Foo"), ca::str::Utf8StringRef::from_cstr("x"), ca::str::Utf8StringRef::from_cstr("I")),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("GetIntField"), std::string::npos);
    EXPECT_NE(result.text.find("static jfieldID fid_"), std::string::npos);
    EXPECT_NE(result.text.find("if (fid_"), std::string::npos);
}

TEST(CBackendTest, InvokeMethod) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call", "()V", {
        new MethodInsnNode(Opcodes::INVOKESTATIC, ca::str::Utf8StringRef::from_cstr("Other"), ca::str::Utf8StringRef::from_cstr("run"), ca::str::Utf8StringRef::from_cstr("()V"), false),
        new InsnNode(Opcodes::RETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("CallStaticVoidMethod"), std::string::npos);
    EXPECT_NE(result.text.find("static jmethodID smid_"), std::string::npos);
    EXPECT_NE(result.text.find("GetStaticMethodID"), std::string::npos);
}

TEST(CBackendTest, StaticInvokeReturnUsesArgumentBaseSlot) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call", "(II)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 1),
        new MethodInsnNode(Opcodes::INVOKESTATIC, ca::str::Utf8StringRef::from_cstr("Other"), ca::str::Utf8StringRef::from_cstr("add"), ca::str::Utf8StringRef::from_cstr("(II)I"), false),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp - 1].i = (*env)->CallStaticIntMethod"), std::string::npos);
    EXPECT_NE(result.text.find("sp -= 1;"), std::string::npos);
}

TEST(CBackendTest, InvokeSpecialLooksUpDeclaringClass) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "create", "()Ljava/lang/Object;", {
        new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
        new InsnNode(Opcodes::DUP),
        new MethodInsnNode(Opcodes::INVOKESPECIAL, ca::str::Utf8StringRef::from_cstr("java/lang/Object"), ca::str::Utf8StringRef::from_cstr("<init>"), ca::str::Utf8StringRef::from_cstr("()V"), false),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("jclass lc = (jclass)_mj2x_track_local_ref(env, _mj2x_refs, &_mj2x_ref_count, _mj2x_find_class(env, _mj2x_owner_class, \"java/lang/Object\"))"), std::string::npos);
    EXPECT_NE(result.text.find("CallNonvirtualVoidMethod(env, stack[sp - 0].l, lc, mid"), std::string::npos);
}

TEST(CBackendTest, StringConstantEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "literal", "()Ljava/lang/String;", {
        new LdcInsnNode(std::string("hello \"jni\"\\path")),
        new InsnNode(Opcodes::POP),
        new LdcInsnNode(std::string("hello \"jni\"\\path")),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("NewStringUTF"), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_string_pool + 0"), std::string::npos);
    EXPECT_EQ(result.text.find("\"hello \\\"jni\\\"\\\\path\""), std::string::npos);
    EXPECT_NE(result.text.find("_mj2x_string_pool + 0"),
              result.text.rfind("_mj2x_string_pool + 0"))
        << result.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("static const unsigned char _mj2x_string_pool[]"), std::string::npos);
    EXPECT_EQ(source.find("\"hello \\\"jni\\\"\\\\path\""), std::string::npos) << source;
}

TEST(CBackendTest, PrimitiveArrayEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "array", "()I", {
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
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("NewIntArray"), std::string::npos);
    EXPECT_NE(result.text.find("SetIntArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("GetIntArrayRegion"), std::string::npos);
}

TEST(CBackendTest, ByteArrayOpsDisambiguateBooleanArrays) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "array", "()I", {
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
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("FindClass(env, \"[Z\")"), std::string::npos);
    EXPECT_NE(result.text.find("IsInstanceOf(env, stack[sp - 1].l, boolean_array_class"), std::string::npos);
    EXPECT_NE(result.text.find("GetBooleanArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("GetByteArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("SetBooleanArrayRegion"), std::string::npos);
    EXPECT_NE(result.text.find("SetByteArrayRegion"), std::string::npos);
}

TEST(CBackendTest, CheckCastEmission) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "cast", "(Ljava/lang/Object;)Ljava/lang/String;", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new TypeInsnNode(Opcodes::CHECKCAST, ca::str::Utf8StringRef::from_cstr("java/lang/String")),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("ClassCastException"), std::string::npos);
    EXPECT_NE(result.text.find("IsInstanceOf"), std::string::npos);
}

TEST(CBackendTest, ClassLookupsUseOwnerClassLoaderHelper) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "refs", "(Ljava/lang/Object;)I", {
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
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("pkg/Owner", cm, blocks);

    EXPECT_NE(result.text.find("jclass _mj2x_owner_class = (jclass)_mj2x_track_local_ref(env, _mj2x_refs, &_mj2x_ref_count, (*env)->GetObjectClass(env, this));"),
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

TEST(CBackendTest, InjectMakesMethodNative) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("Test");

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_FALSE(mn->access & Opcodes::ACC_NATIVE);
    backend.injectIntoClass(cls, cm, result);
    EXPECT_TRUE(mn->access & Opcodes::ACC_NATIVE);
    EXPECT_EQ(mn->instructions.size(), 0);
}

TEST(CBackendTest, InjectAddsClinitRegistrationBridge) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    mn.release();

    CodeConverter cc;
    auto blocks = cc.convert(*cm.methodNode);
    auto result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* bridge = findClassMethod(cls, "__mj2x_registerClass", "(I)V");
    ASSERT_NE(bridge, nullptr);
    EXPECT_NE(bridge->access & Opcodes::ACC_NATIVE, 0);
    EXPECT_NE(bridge->access & Opcodes::ACC_SYNTHETIC, 0);

    auto* clinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(clinit, nullptr);
    EXPECT_EQ(clinit->instructions.size(), 3);
    auto* call = clinit->instructions.get(1);
    ASSERT_EQ(call->getType(), AbstractInsnNode::METHOD_INSN);
    auto* invoke = static_cast<MethodInsnNode*>(call);
    EXPECT_EQ(invoke->opcode, Opcodes::INVOKESTATIC);
    EXPECT_EQ(invoke->owner.to_std_string(), "com/example/Foo");
    EXPECT_EQ(invoke->name.to_std_string(), "__mj2x_registerClass");
    EXPECT_EQ(invoke->desc.to_std_string(), "(I)V");
    EXPECT_EQ(clinit->instructions.get(2)->opcode, Opcodes::RETURN);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("JNIEXPORT void JNICALL Java_com_example_Foo__1_1mj2x_1registerClass__I"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("bridge_method"), std::string::npos) << source;
}

TEST(CBackendTest, JarNativeLoaderRunsBeforeRegistrationBridge) {
    CBackend backend;
    backend.setJarNativeLoaderResourcePath("META-INF/mj2x/native/app.dll", ".dll");
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "test", "()V", {new InsnNode(Opcodes::RETURN)});
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    mn.release();

    CodeConverter cc;
    auto blocks = cc.convert(*cm.methodNode);
    auto result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* clinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(clinit, nullptr);
    ASSERT_EQ(clinit->instructions.size(), 6);
    auto* loaderCall = clinit->instructions.get(2);
    ASSERT_EQ(loaderCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* loaderInvoke = static_cast<MethodInsnNode*>(loaderCall);
    EXPECT_EQ(loaderInvoke->owner.to_std_string(), "mj2x/runtime/NativeLoader");
    EXPECT_EQ(loaderInvoke->name.to_std_string(), "load");
    EXPECT_EQ(loaderInvoke->desc.to_std_string(), "(Ljava/lang/String;Ljava/lang/String;)V");

    auto* bridgeCall = clinit->instructions.get(4);
    ASSERT_EQ(bridgeCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* bridgeInvoke = static_cast<MethodInsnNode*>(bridgeCall);
    EXPECT_EQ(bridgeInvoke->name.to_std_string(), "__mj2x_registerClass");
}

TEST(CBackendTest, ExistingClinitBodyMovesToHiddenClassProxy) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "test", "()V", {new InsnNode(Opcodes::RETURN)});
    auto clinit = makeMethod(Opcodes::ACC_STATIC,
                             "<clinit>", "()V", {
        new InsnNode(Opcodes::ICONST_1),
        new FieldInsnNode(Opcodes::PUTSTATIC,
                          testArena.intern("com/example/Foo"),
                          testArena.intern("VALUE"),
                          testArena.intern("I")),
        new InsnNode(Opcodes::RETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    auto cls = makeCompilerClass("com/example/Foo");
    cls.classNode->methods.push_back(mn.get());
    cls.classNode->methods.push_back(clinit.get());
    mn.release();
    clinit.release();

    CodeConverter cc;
    auto blocks = cc.convert(*cm.methodNode);
    auto result = backend.compileMethod("com/example/Foo", cm, blocks);
    backend.injectIntoClass(cls, cm, result);

    auto* rewrittenClinit = findClassMethod(cls, "<clinit>", "()V");
    ASSERT_NE(rewrittenClinit, nullptr);
    ASSERT_EQ(rewrittenClinit->instructions.size(), 3);
    auto* bridgeCall = rewrittenClinit->instructions.get(1);
    ASSERT_EQ(bridgeCall->getType(), AbstractInsnNode::METHOD_INSN);
    auto* bridgeInvoke = static_cast<MethodInsnNode*>(bridgeCall);
    EXPECT_EQ(bridgeInvoke->owner.to_std_string(), "com/example/Foo");
    EXPECT_EQ(bridgeInvoke->name.to_std_string(), "__mj2x_registerClass");
    EXPECT_EQ(bridgeInvoke->desc.to_std_string(), "(I)V");
    EXPECT_EQ(rewrittenClinit->instructions.get(2)->opcode, Opcodes::RETURN);

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
              std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_hidden_clinit_wrapper_"), std::string::npos) << source;
    EXPECT_EQ(source.find("{(char*)\"__mj2x_hidden_clinit_"), std::string::npos) << source;
    EXPECT_NE(source.find("SetStaticIntField"), std::string::npos) << source;
}

TEST(CBackendTest, BranchAndComparison) {
    CBackend backend;
    auto* targetLabel = new LabelNode();
    targetLabel->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "cmp", "(II)I", {
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
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("goto"), std::string::npos);
    EXPECT_NE(result.text.find("=="), std::string::npos);
    EXPECT_NE(result.text.find("sp -= 2;\n        if (cond) goto"), std::string::npos);
}

TEST(CBackendTest, BranchZeroObjectUsesReferenceAccessor) {
    CBackend backend;
    auto* targetLabel = new LabelNode();
    targetLabel->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull", "(Ljava/lang/Object;)I", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new JumpInsnNode(Opcodes::IFNULL, targetLabel->label),
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::IRETURN),
        targetLabel,
        new InsnNode(Opcodes::ICONST_1),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("stack[sp].l == NULL"), std::string::npos);
    EXPECT_NE(result.text.find("sp--;\n        if (cond) goto"), std::string::npos);
}

TEST(CBackendTest, NewObject) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "create", "()Ljava/lang/Object;", {
        new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
        new InsnNode(Opcodes::DUP),
        new MethodInsnNode(Opcodes::INVOKESPECIAL,
                          ca::str::Utf8StringRef::from_cstr("java/lang/Object"), ca::str::Utf8StringRef::from_cstr("<init>"), ca::str::Utf8StringRef::from_cstr("()V"), false),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("AllocObject"), std::string::npos);
}

TEST(CBackendTest, IincInstruction) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "inc", "(I)I", {
        new IincInsnNode(0, 5),
        new VarInsnNode(Opcodes::ILOAD, 0),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("locals[0].i += 5"), std::string::npos);
}

TEST(CBackendTest, StaticNativeFunctionAndOnLoadRegistration) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add", "(II)I", {new InsnNode(Opcodes::IRETURN)});
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/example/Foo", cm, blocks);

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

TEST(CBackendTest, RegisterNativesKeepsObjectParameterDescriptor) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull", "(Ljava/lang/Object;)I", {
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    (void)backend.compileMethod("Test", cm, blocks);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("{(char*)\"isNull\", (char*)\"(Ljava/lang/Object;)I\""),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("Java_Test_isNull__Ljava_lang_Object_2"), std::string::npos) << source;
}


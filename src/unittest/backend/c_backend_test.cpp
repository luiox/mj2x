#include <gmock/gmock.h>

#include "mj2x/backend/c_backend.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

// Helper: create MethodNode with instructions
static std::unique_ptr<MethodNode> makeMethod(
    int access, const std::string& name, const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns)
{
    std::vector<std::string> noExcepts;
    auto mn = std::make_unique<MethodNode>(access, name, desc, "", noExcepts);
    mn->maxStack = 10;
    mn->maxLocals = 10;
    for (auto* insn : insns)
        mn->instructions.add(insn);
    return mn;
}

static CompilerClass makeCompilerClass(const std::string& name) {
    CompilerClass cls;
    auto cn = std::make_unique<ClassNode>();
    cn->name = name;
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC;
    cn->superName = "java/lang/Object";
    cls.classNode = std::move(cn);
    return cls;
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
    EXPECT_NE(result.text.find("return stack[sp--].i"), std::string::npos);
    EXPECT_NE(result.text.find("+"), std::string::npos);
}

TEST(CBackendTest, FieldAccess) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX", "()I", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new FieldInsnNode(Opcodes::GETFIELD, "Foo", "x", "I"),
        new InsnNode(Opcodes::IRETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("GetIntField"), std::string::npos);
}

TEST(CBackendTest, InvokeMethod) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call", "()V", {
        new MethodInsnNode(Opcodes::INVOKESTATIC, "Other", "run", "()V", false),
        new InsnNode(Opcodes::RETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("CallStaticVoidMethod"), std::string::npos);
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
}

TEST(CBackendTest, NewObject) {
    CBackend backend;
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "create", "()Ljava/lang/Object;", {
        new TypeInsnNode(Opcodes::NEW, "java/lang/Object"),
        new InsnNode(Opcodes::DUP),
        new MethodInsnNode(Opcodes::INVOKESPECIAL,
                          "java/lang/Object", "<init>", "()V", false),
        new InsnNode(Opcodes::ARETURN),
    });
    CompilerMethod cm;
    cm.methodNode = mn.get();
    cm.markedForCompile = true;

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto result = backend.compileMethod("Test", cm, blocks);

    EXPECT_NE(result.text.find("NewObject"), std::string::npos);
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

TEST(CBackendTest, NameMangling) {
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
    EXPECT_NE(result.text.find("JNIEXPORT"), std::string::npos);
}

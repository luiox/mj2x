#include "cloader_postprocessor.hpp"

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringArena;
using ca::str::Utf8StringRef;

namespace {

static constexpr const char* kCLoaderClass = "mj2x/cloader/Loader";
static constexpr const char* kCLoaderEntry = "mj2x/cloader/Loader.class";

std::unique_ptr<ClassNode> buildCLoaderClassNode(ca::str::Utf8StringArena& arena) {
    auto cn = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access = Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn->name = arena.intern(kCLoaderClass);
    cn->superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto* init = new MethodNode(
        Opcodes::ACC_PRIVATE,
        arena.intern("<init>"),
        arena.intern("()V"),
        arena.intern(""),
        noExceptions);
    init->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    init->instructions.add(new MethodInsnNode(
        Opcodes::INVOKESPECIAL,
        arena.intern("java/lang/Object"),
        arena.intern("<init>"),
        arena.intern("()V"),
        false));
    init->instructions.add(new InsnNode(Opcodes::RETURN));
    init->maxStack = 1;
    init->maxLocals = 1;
    cn->methods.push_back(init);

    auto* registerMethod = new MethodNode(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE | Opcodes::ACC_SYNTHETIC,
        arena.intern("registerNativesForClass"),
        arena.intern("(ILjava/lang/String;)V"),
        arena.intern(""),
        noExceptions);
    registerMethod->maxStack = 0;
    registerMethod->maxLocals = 2;
    cn->methods.push_back(registerMethod);

    return cn;
}

std::vector<ca::u8> buildCLoaderClass() {
    Utf8StringArena arena;
    auto cn = buildCLoaderClassNode(arena);
    ClassWriter cw(arena, 0);
    cn->accept(&cw);
    return cw.toByteArray();
}

} // namespace

void CLoaderPostprocessor::process(CompilerIndex& index) {
    if (!index.isJarInput()) {
        CompilerClass cls;
        cls.classNode = buildCLoaderClassNode(index.arena());
        cls.isLibrary = true;
        cls.modified = true;
        index.addClass(kCLoaderClass, std::move(cls));
        return;
    }
    index.upsertJarResource(kCLoaderEntry, buildCLoaderClass());
}

} // namespace mj2x

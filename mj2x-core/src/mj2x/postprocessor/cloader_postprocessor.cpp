#include "cloader_postprocessor.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringArena;
using ca::str::Utf8StringRef;

namespace {

static constexpr const char* kCLoaderClass = "mj2x/cloader/Loader";
static constexpr const char* kCLoaderEntry = "mj2x/cloader/Loader.class";

std::unique_ptr<ClassNode> buildCLoaderClassNode(ca::str::Utf8StringArena& arena,
                                                 const std::string&         resourcePath,
                                                 const std::string&         tempSuffix)
{
    auto cn     = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access =
        Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn->name      = arena.intern(kCLoaderClass);
    cn->superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto*                               init = new MethodNode(Opcodes::ACC_PRIVATE,
                                arena.intern("<init>"),
                                arena.intern("()V"),
                                arena.intern(""),
                                noExceptions);
    init->instructions.add(init->create<VarInsnNode>(Opcodes::ALOAD, 0));
    init->instructions.add(init->create<MethodInsnNode>(Opcodes::INVOKESPECIAL,
                                              arena.intern("java/lang/Object"),
                                              arena.intern("<init>"),
                                              arena.intern("()V"),
                                              false));
    init->instructions.add(init->create<InsnNode>(Opcodes::RETURN));
    init->maxStack  = 1;
    init->maxLocals = 1;
    cn->methods.push_back(init);

    // <clinit>：配置了 native 资源时注入一次 NativeLoader.load（load 只在
    // Loader 首次初始化时执行一次；转换类 clinit 只需 registerNativesForClass，
    // 避免每个类重复携带相同的资源路径字符串与调用）。
    if (!resourcePath.empty()) {
        auto* clinit = new MethodNode(Opcodes::ACC_STATIC,
                                      arena.intern("<clinit>"),
                                      arena.intern("()V"),
                                      arena.intern(""),
                                      noExceptions);
        clinit->instructions.add(clinit->create<LdcInsnNode>(resourcePath));
        clinit->instructions.add(clinit->create<LdcInsnNode>(tempSuffix.empty() ? ".mj2x" : tempSuffix));
        clinit->instructions.add(
            clinit->create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                               arena.intern(native_loader::kClassName),
                               arena.intern("load"),
                               arena.intern(native_loader::kLoadDesc),
                               false));
        clinit->instructions.add(clinit->create<InsnNode>(Opcodes::RETURN));
        clinit->maxStack  = 2;
        clinit->maxLocals = 0;
        cn->methods.push_back(clinit);
    }

    auto* registerMethod = new MethodNode(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_NATIVE | Opcodes::ACC_SYNTHETIC,
        arena.intern("registerNativesForClass"),
        arena.intern("(ILjava/lang/Class;)V"),
        arena.intern(""),
        noExceptions);
    registerMethod->maxStack  = 0;
    registerMethod->maxLocals = 2;
    cn->methods.push_back(registerMethod);

    auto* invokeOne = new MethodNode(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_SYNTHETIC,
        arena.intern("invokeMethodHandle1"),
        arena.intern("(Ljava/lang/invoke/MethodHandle;Ljava/lang/Object;)Ljava/lang/Object;"),
        arena.intern(""),
        noExceptions);
    invokeOne->instructions.add(invokeOne->create<VarInsnNode>(Opcodes::ALOAD, 0));
    invokeOne->instructions.add(invokeOne->create<VarInsnNode>(Opcodes::ALOAD, 1));
    invokeOne->instructions.add(
        invokeOne->create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
                           arena.intern("java/lang/invoke/MethodHandle"),
                           arena.intern("invoke"),
                           arena.intern("(Ljava/lang/Object;)Ljava/lang/Object;"),
                           false));
    invokeOne->instructions.add(invokeOne->create<InsnNode>(Opcodes::ARETURN));
    invokeOne->maxStack  = 2;
    invokeOne->maxLocals = 2;
    cn->methods.push_back(invokeOne);

    return cn;
}

std::vector<ca::u8> buildCLoaderClass(const std::string& resourcePath,
                                      const std::string& tempSuffix)
{
    Utf8StringArena arena;
    auto            cn = buildCLoaderClassNode(arena, resourcePath, tempSuffix);
    ClassWriter     cw(arena, 0);
    cn->accept(&cw);
    return cw.toByteArray();
}

}   // namespace

void CLoaderPostprocessor::process(CompilerIndex& index)
{
    if (!index.isJarInput()) {
        CompilerClass cls;
        cls.classNode = buildCLoaderClassNode(index.arena(), resourcePath_, tempSuffix_);
        cls.isLibrary = true;
        cls.modified  = true;
        index.addClass(kCLoaderClass, std::move(cls));
        return;
    }
    index.upsertJarResource(kCLoaderEntry, buildCLoaderClass(resourcePath_, tempSuffix_));
}

}   // namespace mj2x

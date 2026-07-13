#include "native_loader_postprocessor.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringArena;
using ca::str::Utf8StringRef;
using ca::str::ZUtf8StringRef;

namespace {

static constexpr const char* kLoaderEntry = "mj2x/runtime/NativeLoader.class";

std::vector<ca::u8> buildNativeLoaderClass()
{
    Utf8StringArena arena;
    ClassWriter     cw(arena, ClassWriter::COMPUTE_MAXS);
    cw.visit(49,
             Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SYNTHETIC,
             Utf8StringRef::from_cstr(native_loader::kClassName),
             Utf8StringRef(),
             Utf8StringRef::from_cstr("java/lang/Object"),
             {});

    cw.visitField(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC,
                  Utf8StringRef::from_cstr("loaded"),
                  Utf8StringRef::from_cstr("Z"),
                  Utf8StringRef(),
                  {});

    auto* init = cw.visitMethod(Opcodes::ACC_PRIVATE,
                                Utf8StringRef::from_cstr("<init>"),
                                Utf8StringRef::from_cstr("()V"),
                                Utf8StringRef(),
                                {});
    init->visitCode();
    init->visitVarInsn(Opcodes::ALOAD, 0);
    init->visitMethodInsn(Opcodes::INVOKESPECIAL,
                          Utf8StringRef::from_cstr("java/lang/Object"),
                          Utf8StringRef::from_cstr("<init>"),
                          Utf8StringRef::from_cstr("()V"),
                          false);
    init->visitInsn(Opcodes::RETURN);
    init->visitMaxs(1, 1);
    init->visitEnd();

    auto* load =
        cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_SYNCHRONIZED,
                       Utf8StringRef::from_cstr("load"),
                       Utf8StringRef::from_cstr("(Ljava/lang/String;Ljava/lang/String;)V"),
                       Utf8StringRef(),
                       {});
    auto* continueLabel = new Label();
    auto* streamOk      = new Label();
    auto* loop          = new Label();
    auto* done          = new Label();

    load->visitCode();
    load->visitFieldInsn(Opcodes::GETSTATIC,
                         Utf8StringRef::from_cstr(native_loader::kClassName),
                         Utf8StringRef::from_cstr("loaded"),
                         Utf8StringRef::from_cstr("Z"));
    load->visitJumpInsn(Opcodes::IFEQ, continueLabel);
    load->visitInsn(Opcodes::RETURN);

    load->visitLabel(continueLabel);
    load->visitVarInsn(Opcodes::ALOAD, 0);
    load->visitMethodInsn(Opcodes::INVOKESTATIC,
                          Utf8StringRef::from_cstr("java/lang/ClassLoader"),
                          Utf8StringRef::from_cstr("getSystemResourceAsStream"),
                          Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/io/InputStream;"),
                          false);
    load->visitVarInsn(Opcodes::ASTORE, 2);
    load->visitVarInsn(Opcodes::ALOAD, 2);
    load->visitJumpInsn(Opcodes::IFNONNULL, streamOk);
    load->visitTypeInsn(Opcodes::NEW, Utf8StringRef::from_cstr("java/lang/UnsatisfiedLinkError"));
    load->visitInsn(Opcodes::DUP);
    load->visitLdcInsn(ca::core::Any(std::string("mj2x native resource not found")));
    load->visitMethodInsn(Opcodes::INVOKESPECIAL,
                          Utf8StringRef::from_cstr("java/lang/UnsatisfiedLinkError"),
                          Utf8StringRef::from_cstr("<init>"),
                          Utf8StringRef::from_cstr("(Ljava/lang/String;)V"),
                          false);
    load->visitInsn(Opcodes::ATHROW);

    load->visitLabel(streamOk);
    load->visitLdcInsn(ca::core::Any(std::string("mj2x-")));
    load->visitVarInsn(Opcodes::ALOAD, 1);
    load->visitMethodInsn(
        Opcodes::INVOKESTATIC,
        Utf8StringRef::from_cstr("java/io/File"),
        Utf8StringRef::from_cstr("createTempFile"),
        Utf8StringRef::from_cstr("(Ljava/lang/String;Ljava/lang/String;)Ljava/io/File;"),
        false);
    load->visitVarInsn(Opcodes::ASTORE, 3);
    load->visitVarInsn(Opcodes::ALOAD, 3);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/File"),
                          Utf8StringRef::from_cstr("deleteOnExit"),
                          Utf8StringRef::from_cstr("()V"),
                          false);

    load->visitTypeInsn(Opcodes::NEW, Utf8StringRef::from_cstr("java/io/FileOutputStream"));
    load->visitInsn(Opcodes::DUP);
    load->visitVarInsn(Opcodes::ALOAD, 3);
    load->visitMethodInsn(Opcodes::INVOKESPECIAL,
                          Utf8StringRef::from_cstr("java/io/FileOutputStream"),
                          Utf8StringRef::from_cstr("<init>"),
                          Utf8StringRef::from_cstr("(Ljava/io/File;)V"),
                          false);
    load->visitVarInsn(Opcodes::ASTORE, 4);

    load->visitIntInsn(Opcodes::SIPUSH, 8192);
    load->visitIntInsn(Opcodes::NEWARRAY, Opcodes::T_BYTE);
    load->visitVarInsn(Opcodes::ASTORE, 5);

    load->visitLabel(loop);
    load->visitVarInsn(Opcodes::ALOAD, 2);
    load->visitVarInsn(Opcodes::ALOAD, 5);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/InputStream"),
                          Utf8StringRef::from_cstr("read"),
                          Utf8StringRef::from_cstr("([B)I"),
                          false);
    load->visitVarInsn(Opcodes::ISTORE, 6);
    load->visitVarInsn(Opcodes::ILOAD, 6);
    load->visitJumpInsn(Opcodes::IFLT, done);
    load->visitVarInsn(Opcodes::ALOAD, 4);
    load->visitVarInsn(Opcodes::ALOAD, 5);
    load->visitInsn(Opcodes::ICONST_0);
    load->visitVarInsn(Opcodes::ILOAD, 6);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/OutputStream"),
                          Utf8StringRef::from_cstr("write"),
                          Utf8StringRef::from_cstr("([BII)V"),
                          false);
    load->visitJumpInsn(Opcodes::GOTO, loop);

    load->visitLabel(done);
    load->visitVarInsn(Opcodes::ALOAD, 4);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/OutputStream"),
                          Utf8StringRef::from_cstr("close"),
                          Utf8StringRef::from_cstr("()V"),
                          false);
    load->visitVarInsn(Opcodes::ALOAD, 2);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/InputStream"),
                          Utf8StringRef::from_cstr("close"),
                          Utf8StringRef::from_cstr("()V"),
                          false);
    load->visitVarInsn(Opcodes::ALOAD, 3);
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/io/File"),
                          Utf8StringRef::from_cstr("getAbsolutePath"),
                          Utf8StringRef::from_cstr("()Ljava/lang/String;"),
                          false);
    load->visitMethodInsn(Opcodes::INVOKESTATIC,
                          Utf8StringRef::from_cstr("java/lang/System"),
                          Utf8StringRef::from_cstr("load"),
                          Utf8StringRef::from_cstr("(Ljava/lang/String;)V"),
                          false);
    load->visitInsn(Opcodes::ICONST_1);
    load->visitFieldInsn(Opcodes::PUTSTATIC,
                         Utf8StringRef::from_cstr(native_loader::kClassName),
                         Utf8StringRef::from_cstr("loaded"),
                         Utf8StringRef::from_cstr("Z"));
    load->visitInsn(Opcodes::RETURN);
    load->visitMaxs(0, 0);
    load->visitEnd();

    cw.visitEnd();
    return cw.toByteArray();
}

}   // namespace

NativeLoaderPostprocessor::NativeLoaderPostprocessor(std::string resourcePath,
                                                     std::string tempSuffix,
                                                     bool        injectClinitCalls)
    : resourcePath_(std::move(resourcePath))
    , tempSuffix_(std::move(tempSuffix))
    , injectClinitCalls_(injectClinitCalls)
{}

// 找到或创建类的 <clinit>，确保存在一个可前插 load 调用的静态初始化方法。
// 与原 RegCBackend::prependNativeLoaderCall 行为一致：缺失时新建带 RETURN 的 clinit。
MethodNode* ensureClinit(CompilerClass& clazz)
{
    for (auto* method : clazz.classNode->methods) {
        if (method->name.to_std_string() == "<clinit>" && method->desc.to_std_string() == "()V") {
            return method;
        }
    }
    std::vector<Utf8StringRef> noExceptions;
    auto* clinit = new MethodNode(Opcodes::ACC_STATIC,
                                  ZUtf8StringRef::from_static("<clinit>"),
                                  ZUtf8StringRef::from_static("()V"),
                                  ZUtf8StringRef::from_static(""),
                                  noExceptions);
    clinit->instructions.add(new InsnNode(Opcodes::RETURN));
    clazz.classNode->methods.push_back(clinit);
    return clinit;
}

void NativeLoaderPostprocessor::process(CompilerIndex& index)
{
    // 资源注入对 class 和 JAR 输出都需要；clinit 调用注入只在 JAR 场景有意义。
    const bool needResource = !resourcePath_.empty();
    const bool needClinit   = injectClinitCalls_ && index.isJarInput() && !resourcePath_.empty();
    if (!needResource && !needClinit) return;

    if (needResource) {
        index.upsertJarResource(kLoaderEntry, buildNativeLoaderClass());
    }

    if (!needClinit) return;

    // 对每个被后端改写过的非库类前插 NativeLoader.load 调用。
    // 对齐原 RegC 行为：只有 clazz.modified 的类（至少一个方法成功编译成 native）
    // 才需要运行时加载 native 库。
    for (auto& [name, cls] : index.getClasses()) {
        if (!cls.modified || cls.isLibrary || !cls.classNode) continue;
        MethodNode* clinit = ensureClinit(cls);
        native_loader::emitLoadCall(clinit, resourcePath_, tempSuffix_);
    }
}

}   // namespace mj2x

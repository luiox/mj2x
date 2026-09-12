#include "native_loader_postprocessor.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include "jbytecode/jbytecode.hpp"

#include <cstdint>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringArena;
using ca::str::Utf8StringRef;
using ca::str::ZUtf8StringRef;

namespace {

static constexpr const char* kLoaderEntry = "mj2x/runtime/NativeLoader.class";

/// XOR 解密字节码片段（xorshift32 密钥流，与 C++ 侧 native_lib_crypto.hpp
/// 逐字节一致）：`state = xorshift32(state); buf[i] ^= state & 0xFF`。
/// 循环前状态：
///   slot 6 = n（本次读取字节数），slot 7 = decrypt（boolean），slot 8 = state，
///   slot 9 = i（循环索引）。密钥 0 时用固定种子（与打包侧一致）。
void emitXorDecryptLoop(MethodVisitor* m, Label* xorLoop, Label* write)
{
    // if (!decrypt) goto write
    m->visitVarInsn(Opcodes::ILOAD, 7);
    m->visitJumpInsn(Opcodes::IFEQ, write);
    // i = 0
    m->visitInsn(Opcodes::ICONST_0);
    m->visitVarInsn(Opcodes::ISTORE, 9);
    m->visitLabel(xorLoop);
    // state = xorshift32(state)
    m->visitVarInsn(Opcodes::ILOAD, 8);
    m->visitInsn(Opcodes::DUP);
    m->visitIntInsn(Opcodes::BIPUSH, 13);
    m->visitInsn(Opcodes::ISHL);
    m->visitInsn(Opcodes::IXOR);
    m->visitInsn(Opcodes::DUP);
    m->visitIntInsn(Opcodes::BIPUSH, 17);
    m->visitInsn(Opcodes::IUSHR);
    m->visitInsn(Opcodes::IXOR);
    m->visitInsn(Opcodes::DUP);
    m->visitIntInsn(Opcodes::BIPUSH, 5);
    m->visitInsn(Opcodes::ISHL);
    m->visitInsn(Opcodes::IXOR);
    m->visitVarInsn(Opcodes::ISTORE, 8);
    // buf[i] = (byte)(buf[i] ^ (state & 0xFF))
    m->visitVarInsn(Opcodes::ALOAD, 5);
    m->visitVarInsn(Opcodes::ILOAD, 9);
    m->visitVarInsn(Opcodes::ALOAD, 5);
    m->visitVarInsn(Opcodes::ILOAD, 9);
    m->visitInsn(Opcodes::BALOAD);
    m->visitVarInsn(Opcodes::ILOAD, 8);
    m->visitIntInsn(Opcodes::BIPUSH, 255);
    m->visitInsn(Opcodes::IAND);
    m->visitInsn(Opcodes::IXOR);
    m->visitInsn(Opcodes::I2B);
    m->visitInsn(Opcodes::BASTORE);
    // i++; if (i < n) goto xorLoop
    m->visitIincInsn(9, 1);
    m->visitVarInsn(Opcodes::ILOAD, 9);
    m->visitVarInsn(Opcodes::ILOAD, 6);
    m->visitJumpInsn(Opcodes::IF_ICMPLT, xorLoop);
    // write 标签由调用方 visitLabel（本函数只引用，避免重复 visit 破坏栈映射）。
}

/// @param xorKey native lib 加密密钥（0 = 明文形态，load 不做解密）。
std::vector<ca::u8> buildNativeLoaderClass(std::uint32_t xorKey = 0)
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
    auto* streamLoop    = new Label();
    auto* xorLoop       = new Label();
    auto* writeBlock    = new Label();

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
    load->visitLdcInsn(ca::core::Any(arena.intern("mj2x native resource not found")));
    load->visitMethodInsn(Opcodes::INVOKESPECIAL,
                          Utf8StringRef::from_cstr("java/lang/UnsatisfiedLinkError"),
                          Utf8StringRef::from_cstr("<init>"),
                          Utf8StringRef::from_cstr("(Ljava/lang/String;)V"),
                          false);
    load->visitInsn(Opcodes::ATHROW);

    load->visitLabel(streamOk);
    load->visitLdcInsn(ca::core::Any(arena.intern("mj2x-")));
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

    // 加密形态：resourcePath 以 .enc 结尾时，读块后先 XOR 解密再写出。
    // decrypt = resourcePath.endsWith(".enc")
    load->visitVarInsn(Opcodes::ALOAD, 0);
    load->visitLdcInsn(ca::core::Any(arena.intern(mj2x::kNativeLibEncryptedSuffix)));
    load->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                          Utf8StringRef::from_cstr("java/lang/String"),
                          Utf8StringRef::from_cstr("endsWith"),
                          Utf8StringRef::from_cstr("(Ljava/lang/String;)Z"),
                          false);
    load->visitVarInsn(Opcodes::ISTORE, 7);
    // state = xorKey != 0 ? xorKey : 0x9E3779B9（三目：DUP 保留判定值）
    load->visitLdcInsn(ca::core::Any(static_cast<ca::i32>(xorKey)));
    load->visitInsn(Opcodes::DUP);
    load->visitJumpInsn(Opcodes::IFNE, streamLoop);
    load->visitInsn(Opcodes::POP);
    load->visitLdcInsn(ca::core::Any(static_cast<ca::i32>(0x9E3779B9)));
    load->visitLabel(streamLoop);
    load->visitVarInsn(Opcodes::ISTORE, 8);

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
    emitXorDecryptLoop(load, xorLoop, writeBlock);
    load->visitLabel(writeBlock);
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
                                                     bool        injectClinitCalls,
                                                     std::uint32_t xorKey)
    : resourcePath_(std::move(resourcePath))
    , tempSuffix_(std::move(tempSuffix))
    , injectClinitCalls_(injectClinitCalls)
    , xorKey_(xorKey)
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
    clinit->instructions.add(clinit->create<InsnNode>(Opcodes::RETURN));
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
        index.upsertJarResource(kLoaderEntry, buildNativeLoaderClass(xorKey_));
    }

    if (!needClinit) return;

    // 对每个被后端改写过的非库类前插 NativeLoader.load 调用。判定条件是
    // 「modified 且含 ACC_NATIVE 方法」：modified 现在也被 indy/ldc 展开置位
    //（那些类改写了指令流但不含 native 方法），仅凭 modified 会给纯字节码类
    // 误注入 clinit 加载调用。
    for (auto& [name, cls] : index.getClasses()) {
        if (!cls.modified || cls.isLibrary || !cls.classNode) continue;
        bool hasNative = false;
        for (const auto* m : cls.classNode->methods) {
            if ((m->access & Opcodes::ACC_NATIVE) != 0) {
                hasNative = true;
                break;
            }
        }
        if (!hasNative) continue;
        MethodNode* clinit = ensureClinit(cls);
        native_loader::emitLoadCall(clinit, resourcePath_, tempSuffix_);
    }
}

}   // namespace mj2x

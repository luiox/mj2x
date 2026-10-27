#include "native_loader_postprocessor.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include "garena/arena.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbytecode/jbytecode.hpp"

#include "mj2x/core/dom_edit.hpp"

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
    // Label 建点 scratch arena（B8 归属制，同 ClassReader labelArena_ 模式）：控制流
    // Label 全部由此分配并盖章，MethodWriter 仅在 toByteArray() 前借用改写，原件随本
    // 函数栈帧统一回收——不再堆泄漏（DEV-14 清账）。
    garena::Arena   labelScratch;
    auto            makeLabel = [&]() -> Label* {
        auto* l = labelScratch.create<Label>();
        l->setOwner(&labelScratch);
        return l;
    };
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
    auto* continueLabel = makeLabel();
    auto* streamOk      = makeLabel();
    auto* loop          = makeLabel();
    auto* done          = makeLabel();
    auto* streamLoop    = makeLabel();
    auto* xorLoop       = makeLabel();
    auto* writeBlock    = makeLabel();

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
    // 临时文件不走 File.createTempFile：其内部首次触达 SecureRandom（TempDirectory
    // 静态字段初始化），冷 JVM 实测 ~52-59ms（jdk21/jdk25，#1183 bootstrap 分解），
    // 是提取段乃至整个 bootstrap 的主导项。改走 newTempFile 私有助手（本类内发射）：
    // ThreadLocalRandom 随机名 + createNewFile 原子独占创建，防碰撞/防抢占语义与
    // createTempFile 一致；ThreadLocalRandom 首触实测 ~0.3ms（createNewFile 的
    // 磁盘成本冷态 ~13ms，新旧两路径共有，不计入收益口径）。
    load->visitVarInsn(Opcodes::ALOAD, 1);
    load->visitMethodInsn(
        Opcodes::INVOKESTATIC,
        Utf8StringRef::from_cstr(native_loader::kClassName),
        Utf8StringRef::from_cstr("newTempFile"),
        Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/io/File;"),
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

    // newTempFile(tempSuffix)：ThreadLocalRandom 随机名 + createNewFile 原子独占
    // 创建（32 次重试），替代 File.createTempFile 的 SecureRandom 冷启动税。
    // Java 参考形态（与本发射逐指令对应）：
    //   String dir = System.getProperty("java.io.tmpdir", ".");
    //   ThreadLocalRandom rnd = ThreadLocalRandom.current();
    //   for (int i = 0; i < 32; i++) {
    //       File f = new File(dir, "mj2x-" + Long.toHexString(rnd.nextLong()) + suffix);
    //       if (f.createNewFile()) return f;
    //   }
    //   throw new UnsatisfiedLinkError("mj2x native temp file create failed");
    // 与 createTempFile 的语义对齐点：同目录（java.io.tmpdir）、同前缀/后缀、
    // 原子独占（createNewFile 失败即换名重试，永不打开已存在文件——防符号链接
    // 抢占）、失败抛 UnsatisfiedLinkError。已知差异（记录不消除）：POSIX 下
    // 权限为默认 umask（如 644）而 createTempFile 为 600——Windows 产品线无差，
    // POSIX 侧提取物生命周期为单 JVM 会话（deleteOnExit），内容即 jar 内资源。
    auto* newTemp =
        cw.visitMethod(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_SYNTHETIC,
                       Utf8StringRef::from_cstr("newTempFile"),
                       Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/io/File;"),
                       Utf8StringRef(),
                       {});
    auto* tempLoop  = makeLabel();
    auto* tempRetry = makeLabel();
    auto* tempNext  = makeLabel();

    newTemp->visitCode();
    // dir = System.getProperty("java.io.tmpdir", ".")
    newTemp->visitLdcInsn(ca::core::Any(arena.intern("java.io.tmpdir")));
    newTemp->visitLdcInsn(ca::core::Any(arena.intern(".")));
    newTemp->visitMethodInsn(Opcodes::INVOKESTATIC,
                             Utf8StringRef::from_cstr("java/lang/System"),
                             Utf8StringRef::from_cstr("getProperty"),
                             Utf8StringRef::from_cstr(
                                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
                             false);
    newTemp->visitVarInsn(Opcodes::ASTORE, 1);
    // rnd = ThreadLocalRandom.current()
    newTemp->visitMethodInsn(Opcodes::INVOKESTATIC,
                             Utf8StringRef::from_cstr("java/util/concurrent/ThreadLocalRandom"),
                             Utf8StringRef::from_cstr("current"),
                             Utf8StringRef::from_cstr(
                                 "()Ljava/util/concurrent/ThreadLocalRandom;"),
                             false);
    newTemp->visitVarInsn(Opcodes::ASTORE, 2);
    // i = 0
    newTemp->visitInsn(Opcodes::ICONST_0);
    newTemp->visitVarInsn(Opcodes::ISTORE, 4);
    // loop: f = new File(dir, "mj2x-" + Long.toHexString(rnd.nextLong()) + suffix)
    // 注意：dir 为 String，构造器必须取 (Ljava/lang/String;Ljava/lang/String;)V
    // 重载——曾误写 (File,String) 描述符致 VerifyError（#1183 排障记录）。
    newTemp->visitLabel(tempLoop);
    newTemp->visitTypeInsn(Opcodes::NEW, Utf8StringRef::from_cstr("java/io/File"));
    newTemp->visitInsn(Opcodes::DUP);
    newTemp->visitVarInsn(Opcodes::ALOAD, 1);
    newTemp->visitTypeInsn(Opcodes::NEW, Utf8StringRef::from_cstr("java/lang/StringBuilder"));
    newTemp->visitInsn(Opcodes::DUP);
    newTemp->visitMethodInsn(Opcodes::INVOKESPECIAL,
                             Utf8StringRef::from_cstr("java/lang/StringBuilder"),
                             Utf8StringRef::from_cstr("<init>"),
                             Utf8StringRef::from_cstr("()V"),
                             false);
    newTemp->visitLdcInsn(ca::core::Any(arena.intern("mj2x-")));
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/lang/StringBuilder"),
                             Utf8StringRef::from_cstr("append"),
                             Utf8StringRef::from_cstr(
                                 "(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                             false);
    newTemp->visitVarInsn(Opcodes::ALOAD, 2);
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/util/concurrent/ThreadLocalRandom"),
                             Utf8StringRef::from_cstr("nextLong"),
                             Utf8StringRef::from_cstr("()J"),
                             false);
    newTemp->visitMethodInsn(Opcodes::INVOKESTATIC,
                             Utf8StringRef::from_cstr("java/lang/Long"),
                             Utf8StringRef::from_cstr("toHexString"),
                             Utf8StringRef::from_cstr("(J)Ljava/lang/String;"),
                             false);
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/lang/StringBuilder"),
                             Utf8StringRef::from_cstr("append"),
                             Utf8StringRef::from_cstr(
                                 "(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                             false);
    newTemp->visitVarInsn(Opcodes::ALOAD, 0);
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/lang/StringBuilder"),
                             Utf8StringRef::from_cstr("append"),
                             Utf8StringRef::from_cstr(
                                 "(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                             false);
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/lang/StringBuilder"),
                             Utf8StringRef::from_cstr("toString"),
                             Utf8StringRef::from_cstr("()Ljava/lang/String;"),
                             false);
    newTemp->visitMethodInsn(Opcodes::INVOKESPECIAL,
                             Utf8StringRef::from_cstr("java/io/File"),
                             Utf8StringRef::from_cstr("<init>"),
                             Utf8StringRef::from_cstr("(Ljava/lang/String;Ljava/lang/String;)V"),
                             false);
    newTemp->visitVarInsn(Opcodes::ASTORE, 3);
    // if (!f.createNewFile()) goto retry（IFEQ 前跳重试；成功则顺序落入 return）
    newTemp->visitVarInsn(Opcodes::ALOAD, 3);
    newTemp->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                             Utf8StringRef::from_cstr("java/io/File"),
                             Utf8StringRef::from_cstr("createNewFile"),
                             Utf8StringRef::from_cstr("()Z"),
                             false);
    newTemp->visitJumpInsn(Opcodes::IFEQ, tempRetry);
    // hit: return f（createNewFile 成功，唯一前驱为本直落路径）
    newTemp->visitVarInsn(Opcodes::ALOAD, 3);
    newTemp->visitInsn(Opcodes::ARETURN);
    // retry: i++; if (i < 32) goto loop（耗尽则顺序落入 throw）
    newTemp->visitLabel(tempRetry);
    newTemp->visitIincInsn(4, 1);
    newTemp->visitVarInsn(Opcodes::ILOAD, 4);
    newTemp->visitIntInsn(Opcodes::BIPUSH, 32);
    newTemp->visitJumpInsn(Opcodes::IF_ICMPLT, tempLoop);
    // next: throw new UnsatisfiedLinkError("mj2x native temp file create failed")
    newTemp->visitLabel(tempNext);
    newTemp->visitTypeInsn(Opcodes::NEW,
                           Utf8StringRef::from_cstr("java/lang/UnsatisfiedLinkError"));
    newTemp->visitInsn(Opcodes::DUP);
    newTemp->visitLdcInsn(ca::core::Any(arena.intern("mj2x native temp file create failed")));
    newTemp->visitMethodInsn(Opcodes::INVOKESPECIAL,
                             Utf8StringRef::from_cstr("java/lang/UnsatisfiedLinkError"),
                             Utf8StringRef::from_cstr("<init>"),
                             Utf8StringRef::from_cstr("(Ljava/lang/String;)V"),
                             false);
    newTemp->visitInsn(Opcodes::ATHROW);
    newTemp->visitMaxs(5, 5);
    newTemp->visitEnd();

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

void NativeLoaderPostprocessor::process(CompilerIndex& index)
{
    // 资源注入对 class 和 JAR 输出都需要；clinit 调用注入只在 JAR 场景有意义。
    const bool needResource = !resourcePath_.empty();
    const bool needClinit   = injectClinitCalls_ && index.isJarInput() && !resourcePath_.empty();
    if (!needResource && !needClinit) return;

    if (needResource) {
        // 从零建类发射面（buildNativeLoaderClass）批 6 随 output 侧一并切。
        index.upsertJarResource(kLoaderEntry, buildNativeLoaderClass(xorKey_));
    }

    if (!needClinit) return;

    // 对每个被后端改写过的非库类前插 NativeLoader.load 调用。判定条件是
    // 「modified 且含 ACC_NATIVE 方法」：modified 现在也被 indy/ldc 展开置位
    //（那些类改写了指令流但不含 native 方法），仅凭 modified 会给纯字节码类
    // 误注入 clinit 加载调用。批 4：改写载体切 DOM（findMethod/addMethod/
    // promote/insertAtStart/lower），native 方法判定读 DOM access。
    for (auto& [name, cls] : index.getClasses()) {
        if (!cls.modified || cls.isLibrary || !cls.dom) continue;
        bool hasNative = false;
        for (const auto& dm : cls.dom->methods) {
            if ((dm.access & Opcodes::ACC_NATIVE) != 0) {
                hasNative = true;
                break;
            }
        }
        if (!hasNative) continue;

        auto session = openDomEdit(cls, name);
        auto idx     = session->unit().findMethod("<clinit>", "()V");
        bool created = (idx == jbc::ir::kNoMethod);
        if (created) {
            // 缺失时新建（对齐树版 ensureClinit）：withCode 起步，load 序列
            // 前插 + RETURN 追加保证非空方法体。
            auto genId = session->addMethod(jbc::ir::MethodSeed{
                "<clinit>", "()V", Opcodes::ACC_STATIC, true});
            if (genId.is_err()) {
                throw std::runtime_error("native_loader: addMethod <clinit> failed for "
                                         + name);
            }
            idx = session->unit().indexOf(std::move(genId).unwrap());
        }

        auto promoted = jbc::ir::promoteMethod(*session, idx);
        if (promoted.is_err()) {
            throw std::runtime_error("native_loader: promote clinit failed for " + name);
        }
        auto unit = std::move(promoted).unwrap();
        native_loader::emitLoadCallDom(*session, *unit, resourcePath_, tempSuffix_);
        if (created) {
            jbc::ir::CodeEditor editor(*unit);
            (void)editor.append(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        }
        auto lowered = jbc::ir::lowerMethod(*session, idx);
        if (lowered.is_err()) {
            throw std::runtime_error("native_loader: lower clinit failed for " + name);
        }
        commitDomEdit(cls, name, session);
    }
}

}   // namespace mj2x

#include "native_loader_postprocessor.hpp"
#include "mj2x/core/native_lib_crypto.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mj2x/core/dom_edit.hpp"
#include "mj2x/core/frame_recompute.hpp"
#include "mj2x/core/jvm_opcodes.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mj2x {

namespace {

using jbc::core::Op;
using jbc::ir::CodeEditor;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

static constexpr const char* kLoaderEntry = "mj2x/runtime/NativeLoader.class";

static InsnId appendInsn(CodeEditor& editor, jbc::ir::Insn in)
{
    auto r = editor.append(std::move(in));
    if (r.is_err()) throw std::runtime_error("native_loader: append failed");
    return std::move(r).unwrap();
}

static LabelId createLabel(CodeEditor& editor)
{
    auto r = editor.createLabel();
    if (r.is_err()) throw std::runtime_error("native_loader: createLabel failed");
    return std::move(r).unwrap();
}

static void bindLabelAt(CodeEditor& editor, LabelId label, InsnId anchor)
{
    if (editor.bindLabel(label, anchor).is_err()) {
        throw std::runtime_error("native_loader: bindLabel failed");
    }
}

/// XOR 解密字节码片段（xorshift32 密钥流，与 C++ 侧 native_lib_crypto.hpp
/// 逐字节一致）：`state = xorshift32(state); buf[i] ^= state & 0xFF`。
/// 循环前状态：
///   slot 6 = n（本次读取字节数），slot 7 = decrypt（boolean），slot 8 = state，
///   slot 9 = i（循环索引）。密钥 0 时用固定种子（与打包侧一致）。
/// write 标签由调用方绑定（本函数只引用，避免重复绑破坏栈映射）。
void emitXorDecryptLoop(CodeEditor& editor, LabelId xorLoop, LabelId write)
{
    using jbc::ir::CodeEditor;
    // if (!decrypt) goto write
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 7));
    appendInsn(editor, CodeEditor::branch(Op::IFEQ, write));
    // i = 0
    appendInsn(editor, CodeEditor::simple(Op::ICONST_0));
    appendInsn(editor, CodeEditor::local(Op::ISTORE, 9));
    bindLabelAt(editor, xorLoop,
                appendInsn(editor, CodeEditor::local(Op::ILOAD, 8)));
    // state = xorshift32(state)
    appendInsn(editor, CodeEditor::simple(Op::DUP));
    appendInsn(editor, CodeEditor::local(Op::BIPUSH, 13));
    appendInsn(editor, CodeEditor::simple(Op::ISHL));
    appendInsn(editor, CodeEditor::simple(Op::IXOR));
    appendInsn(editor, CodeEditor::simple(Op::DUP));
    appendInsn(editor, CodeEditor::local(Op::BIPUSH, 17));
    appendInsn(editor, CodeEditor::simple(Op::IUSHR));
    appendInsn(editor, CodeEditor::simple(Op::IXOR));
    appendInsn(editor, CodeEditor::simple(Op::DUP));
    appendInsn(editor, CodeEditor::local(Op::BIPUSH, 5));
    appendInsn(editor, CodeEditor::simple(Op::ISHL));
    appendInsn(editor, CodeEditor::simple(Op::IXOR));
    appendInsn(editor, CodeEditor::local(Op::ISTORE, 8));
    // buf[i] = (byte)(buf[i] ^ (state & 0xFF))
    appendInsn(editor, CodeEditor::local(Op::ALOAD, 5));
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 9));
    appendInsn(editor, CodeEditor::local(Op::ALOAD, 5));
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 9));
    appendInsn(editor, CodeEditor::simple(Op::BALOAD));
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 8));
    // BIPUSH 值域 [−128,127]，255 走 SIPUSH（语义同）。
    appendInsn(editor, CodeEditor::local(Op::SIPUSH, 255));
    appendInsn(editor, CodeEditor::simple(Op::IAND));
    appendInsn(editor, CodeEditor::simple(Op::IXOR));
    appendInsn(editor, CodeEditor::simple(Op::I2B));
    appendInsn(editor, CodeEditor::simple(Op::BASTORE));
    // i++; if (i < n) goto xorLoop
    appendInsn(editor, CodeEditor::iinc(9, 1));
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 9));
    appendInsn(editor, CodeEditor::local(Op::ILOAD, 6));
    appendInsn(editor, CodeEditor::branch(Op::IF_ICMPLT, xorLoop));
}

/// @param xorKey native lib 加密密钥（0 = 明文形态，load 不做解密）。
std::vector<ca::u8> buildNativeLoaderClass(std::uint32_t xorKey = 0)
{
    static constexpr const char* kSelf = native_loader::kClassName;

    // 批 6d 树退役：与 cloader 同一 DOM 构建形态（addMethod/promote/append/
    // lower 发射）。major 49 = v1.5：无 StackMapTable（对齐旧 ClassWriter
    // visit(49) 形态）；maxes 由 recomputeSessionFrames 帧引擎计算（替代旧
    // COMPUTE_MAXS），v<50 只落 maxes 不建表。
    auto cf       = std::make_unique<jbc::core::ClassFile>();
    cf->major     = 49;
    cf->access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SYNTHETIC;
    cf->thisClass  = cf->cp.addClass(cf->cp.addUtf8(kSelf));
    cf->superClass = cf->cp.addClass(cf->cp.addUtf8("java/lang/Object"));

    auto opened = jbc::ir::makeSession(std::move(cf));
    if (opened.is_err()) {
        throw std::runtime_error("native_loader: makeSession failed");
    }
    auto ses = std::move(opened).unwrap();

    if (ses->addField(jbc::ir::FieldSeed{"loaded", "Z",
                                         Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC})
            .is_err()) {
        throw std::runtime_error("native_loader: addField loaded failed");
    }

    const ca::u32 cpLoadedField = ses->internFieldRef(kSelf, "loaded", "Z");
    const ca::u32 cpSelfClass   = ses->internClass(kSelf);
    const ca::u32 cpNewTempFile = ses->internMethodRef(
        kSelf, "newTempFile", "(Ljava/lang/String;)Ljava/io/File;");

    auto addMethod = [&](const jbc::ir::MethodSeed& seed) -> ca::usize {
        auto added = ses->addMethod(seed);
        if (added.is_err()) {
            throw std::runtime_error("native_loader: addMethod " + std::string(seed.name)
                                     + " failed");
        }
        return ses->backing().methods.size() - 1;
    };
    auto promote = [&](ca::usize mi) {
        auto promoted = jbc::ir::promoteMethod(*ses, mi);
        if (promoted.is_err()) {
            throw std::runtime_error("native_loader: promote failed");
        }
        return std::move(promoted).unwrap();
    };
    auto lower = [&](ca::usize mi) {
        auto lowered = jbc::ir::lowerMethod(*ses, mi);
        if (lowered.is_err()) {
            throw std::runtime_error("native_loader: lower failed");
        }
    };

    // <init>()V : super.<init>()。
    {
        const ca::usize mi = addMethod(jbc::ir::MethodSeed{
            "<init>", "()V", Opcodes::ACC_PRIVATE, true});
        auto  unit   = promote(mi);
        CodeEditor editor(*unit);
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 0));
        appendInsn(editor, CodeEditor::cpInsn(
                               Op::INVOKESPECIAL,
                               ses->internMethodRef("java/lang/Object", "<init>", "()V")));
        appendInsn(editor, CodeEditor::simple(Op::RETURN));
        lower(mi);
    }

    // load(String,String)V：委托链 = 查 loaded 幂等 → getSystemResourceAsStream →
    // newTempFile 落盘（可选 XOR 解密）→ System.load → loaded=true。
    {
        const ca::usize mi = addMethod(jbc::ir::MethodSeed{
            "load", "(Ljava/lang/String;Ljava/lang/String;)V",
            Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_SYNCHRONIZED, true});
        auto       unit   = promote(mi);
        CodeEditor editor(*unit);

        const LabelId continueLabel = createLabel(editor);
        const LabelId streamOk      = createLabel(editor);
        const LabelId loop          = createLabel(editor);
        const LabelId done          = createLabel(editor);
        const LabelId streamLoop    = createLabel(editor);
        const LabelId xorLoop       = createLabel(editor);
        const LabelId writeBlock    = createLabel(editor);

        appendInsn(editor, CodeEditor::cpInsn(Op::GETSTATIC, cpLoadedField));
        appendInsn(editor, CodeEditor::branch(Op::IFEQ, continueLabel));
        appendInsn(editor, CodeEditor::simple(Op::RETURN));

        bindLabelAt(editor, continueLabel,
                    appendInsn(editor, CodeEditor::local(Op::ALOAD, 0)));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESTATIC,
                                      ses->internMethodRef(
                                          "java/lang/ClassLoader",
                                          "getSystemResourceAsStream",
                                          "(Ljava/lang/String;)Ljava/io/InputStream;")));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 2));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 2));
        appendInsn(editor, CodeEditor::branch(Op::IFNONNULL, streamOk));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::NEW,
                                      ses->internClass("java/lang/UnsatisfiedLinkError")));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor,
                   CodeEditor::loadConst(ses->internString("mj2x native resource not found")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESPECIAL,
                                      ses->internMethodRef("java/lang/UnsatisfiedLinkError",
                                                           "<init>",
                                                           "(Ljava/lang/String;)V")));
        appendInsn(editor, CodeEditor::simple(Op::ATHROW));

        bindLabelAt(editor, streamOk,
                    appendInsn(editor, CodeEditor::local(Op::ALOAD, 1)));
        // 临时文件不走 File.createTempFile：其内部首次触达 SecureRandom（TempDirectory
        // 静态字段初始化），冷 JVM 实测 ~52-59ms（jdk21/jdk25，#1183 bootstrap 分解），
        // 是提取段乃至整个 bootstrap 的主导项。改走 newTempFile 私有助手（本类内发射）：
        // ThreadLocalRandom 随机名 + createNewFile 原子独占创建，防碰撞/防抢占语义与
        // createTempFile 一致；ThreadLocalRandom 首触实测 ~0.3ms（createNewFile 的
        // 磁盘成本冷态 ~13ms，新旧两路径共有，不计入收益口径）。
        appendInsn(editor, CodeEditor::cpInsn(Op::INVOKESTATIC, cpNewTempFile));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 3));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 3));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/File", "deleteOnExit",
                                                           "()V")));

        appendInsn(editor,
                   CodeEditor::cpInsn(Op::NEW,
                                      ses->internClass("java/io/FileOutputStream")));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 3));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESPECIAL,
                                      ses->internMethodRef("java/io/FileOutputStream",
                                                           "<init>", "(Ljava/io/File;)V")));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 4));

        appendInsn(editor, CodeEditor::local(Op::SIPUSH, 8192));
        appendInsn(editor, CodeEditor::local(Op::NEWARRAY, Opcodes::T_BYTE));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 5));

        // 加密形态：resourcePath 以 .enc 结尾时，读块后先 XOR 解密再写出。
        // decrypt = resourcePath.endsWith(".enc")
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 0));
        appendInsn(editor,
                   CodeEditor::loadConst(ses->internString(kNativeLibEncryptedSuffix)));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/lang/String", "endsWith",
                                                           "(Ljava/lang/String;)Z")));
        appendInsn(editor, CodeEditor::local(Op::ISTORE, 7));
        // state = xorKey != 0 ? xorKey : 0x9E3779B9（三目：DUP 保留判定值）
        appendInsn(editor,
                   CodeEditor::loadConst(ses->internInteger(static_cast<ca::i32>(xorKey))));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor, CodeEditor::branch(Op::IFNE, streamLoop));
        appendInsn(editor, CodeEditor::simple(Op::POP));
        appendInsn(editor,
                   CodeEditor::loadConst(
                       ses->internInteger(static_cast<ca::i32>(0x9E3779B9u))));
        bindLabelAt(editor, streamLoop,
                    appendInsn(editor, CodeEditor::local(Op::ISTORE, 8)));

        bindLabelAt(editor, loop,
                    appendInsn(editor, CodeEditor::local(Op::ALOAD, 2)));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 5));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/InputStream", "read",
                                                           "([B)I")));
        appendInsn(editor, CodeEditor::local(Op::ISTORE, 6));
        appendInsn(editor, CodeEditor::local(Op::ILOAD, 6));
        appendInsn(editor, CodeEditor::branch(Op::IFLT, done));
        emitXorDecryptLoop(editor, xorLoop, writeBlock);

        bindLabelAt(editor, writeBlock,
                    appendInsn(editor, CodeEditor::local(Op::ALOAD, 4)));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 5));
        appendInsn(editor, CodeEditor::simple(Op::ICONST_0));
        appendInsn(editor, CodeEditor::local(Op::ILOAD, 6));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/OutputStream", "write",
                                                           "([BII)V")));
        appendInsn(editor, CodeEditor::branch(Op::GOTO, loop));

        bindLabelAt(editor, done,
                    appendInsn(editor, CodeEditor::local(Op::ALOAD, 4)));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/OutputStream", "close",
                                                           "()V")));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 2));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/InputStream", "close",
                                                           "()V")));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 3));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef(
                                          "java/io/File", "getAbsolutePath",
                                          "()Ljava/lang/String;")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESTATIC,
                                      ses->internMethodRef("java/lang/System", "load",
                                                           "(Ljava/lang/String;)V")));
        appendInsn(editor, CodeEditor::simple(Op::ICONST_1));
        appendInsn(editor, CodeEditor::cpInsn(Op::PUTSTATIC, cpLoadedField));
        appendInsn(editor, CodeEditor::simple(Op::RETURN));
        lower(mi);
    }

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
    {
        const ca::usize mi = addMethod(jbc::ir::MethodSeed{
            "newTempFile", "(Ljava/lang/String;)Ljava/io/File;",
            Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_SYNTHETIC, true});
        auto       unit   = promote(mi);
        CodeEditor editor(*unit);

        const LabelId tempLoop  = createLabel(editor);
        const LabelId tempRetry = createLabel(editor);
        const LabelId tempNext  = createLabel(editor);

        // dir = System.getProperty("java.io.tmpdir", ".")
        appendInsn(editor, CodeEditor::loadConst(ses->internString("java.io.tmpdir")));
        appendInsn(editor, CodeEditor::loadConst(ses->internString(".")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESTATIC,
                                      ses->internMethodRef(
                                          "java/lang/System", "getProperty",
                                          "(Ljava/lang/String;Ljava/lang/String;)"
                                          "Ljava/lang/String;")));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 1));
        // rnd = ThreadLocalRandom.current()
        appendInsn(editor,
                   CodeEditor::cpInsn(
                       Op::INVOKESTATIC,
                       ses->internMethodRef("java/util/concurrent/ThreadLocalRandom",
                                            "current",
                                            "()Ljava/util/concurrent/ThreadLocalRandom;")));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 2));
        // i = 0
        appendInsn(editor, CodeEditor::simple(Op::ICONST_0));
        appendInsn(editor, CodeEditor::local(Op::ISTORE, 4));
        // loop: f = new File(dir, "mj2x-" + Long.toHexString(rnd.nextLong()) + suffix)
        // 注意：dir 为 String，构造器必须取 (Ljava/lang/String;Ljava/lang/String;)V
        // 重载——曾误写 (File,String) 描述符致 VerifyError（#1183 排障记录）。
        bindLabelAt(editor, tempLoop,
                    appendInsn(editor,
                               CodeEditor::cpInsn(Op::NEW,
                                                  ses->internClass("java/io/File"))));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 1));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::NEW,
                                      ses->internClass("java/lang/StringBuilder")));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESPECIAL,
                                      ses->internMethodRef("java/lang/StringBuilder",
                                                           "<init>", "()V")));
        appendInsn(editor, CodeEditor::loadConst(ses->internString("mj2x-")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef(
                                          "java/lang/StringBuilder", "append",
                                          "(Ljava/lang/String;)"
                                          "Ljava/lang/StringBuilder;")));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 2));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef(
                                          "java/util/concurrent/ThreadLocalRandom",
                                          "nextLong", "()J")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESTATIC,
                                      ses->internMethodRef("java/lang/Long", "toHexString",
                                                           "(J)Ljava/lang/String;")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef(
                                          "java/lang/StringBuilder", "append",
                                          "(Ljava/lang/String;)"
                                          "Ljava/lang/StringBuilder;")));
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 0));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef(
                                          "java/lang/StringBuilder", "append",
                                          "(Ljava/lang/String;)"
                                          "Ljava/lang/StringBuilder;")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/lang/StringBuilder",
                                                           "toString",
                                                           "()Ljava/lang/String;")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESPECIAL,
                                      ses->internMethodRef(
                                          "java/io/File", "<init>",
                                          "(Ljava/lang/String;Ljava/lang/String;)V")));
        appendInsn(editor, CodeEditor::local(Op::ASTORE, 3));
        // if (!f.createNewFile()) goto retry（IFEQ 前跳重试；成功则顺序落入 return）
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 3));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKEVIRTUAL,
                                      ses->internMethodRef("java/io/File",
                                                           "createNewFile", "()Z")));
        appendInsn(editor, CodeEditor::branch(Op::IFEQ, tempRetry));
        // hit: return f（createNewFile 成功，唯一前驱为本直落路径）
        appendInsn(editor, CodeEditor::local(Op::ALOAD, 3));
        appendInsn(editor, CodeEditor::simple(Op::ARETURN));
        // retry: i++; if (i < 32) goto loop（耗尽则顺序落入 throw）
        bindLabelAt(editor, tempRetry, appendInsn(editor, CodeEditor::iinc(4, 1)));
        appendInsn(editor, CodeEditor::local(Op::ILOAD, 4));
        appendInsn(editor, CodeEditor::local(Op::BIPUSH, 32));
        appendInsn(editor, CodeEditor::branch(Op::IF_ICMPLT, tempLoop));
        // next: throw new UnsatisfiedLinkError("mj2x native temp file create failed")
        bindLabelAt(editor, tempNext,
                    appendInsn(editor,
                               CodeEditor::cpInsn(
                                   Op::NEW,
                                   ses->internClass("java/lang/UnsatisfiedLinkError"))));
        appendInsn(editor, CodeEditor::simple(Op::DUP));
        appendInsn(editor,
                   CodeEditor::loadConst(
                       ses->internString("mj2x native temp file create failed")));
        appendInsn(editor,
                   CodeEditor::cpInsn(Op::INVOKESPECIAL,
                                      ses->internMethodRef("java/lang/UnsatisfiedLinkError",
                                                           "<init>",
                                                           "(Ljava/lang/String;)V")));
        appendInsn(editor, CodeEditor::simple(Op::ATHROW));
        lower(mi);
    }

    recomputeSessionFrames(kSelf, *ses);

    auto written = jbc::core::writeClass(ses->backing());
    if (written.is_err()) {
        throw std::runtime_error("native_loader: writeClass failed");
    }
    return std::move(written).unwrap();
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
        // 批 6d：从零建类发射面走 DOM 构建器（帧引擎出 maxes）。
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

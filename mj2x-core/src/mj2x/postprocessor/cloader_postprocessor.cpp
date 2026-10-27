#include "cloader_postprocessor.hpp"

#include <stdexcept>

#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

namespace mj2x {

namespace {

static constexpr const char* kCLoaderClass = "mj2x/cloader/Loader";
static constexpr const char* kCLoaderEntry = "mj2x/cloader/Loader.class";

/// @brief Loader 生成类 DOM 构建器（批 6d 树退役：与 lambda 合成类同一构建
///        形态——addMethod/promote/append/lower 发射，writeClass→readClass
///        字节往返收口）。
///
/// 方法形态与旧树构建器逐位同源（javac 直译；全部直线体无分支，v52 校验
/// 无 StackMapTable 依赖）。native 桥接方法（registerNativesForClass）无体。
std::unique_ptr<jbc::core::ClassFile> buildCLoaderClassDom(const std::string& resourcePath,
                                                           const std::string& tempSuffix)
{
    auto cf       = std::make_unique<jbc::core::ClassFile>();
    cf->major     = 52;   // V1_8
    cf->access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER
                 | Opcodes::ACC_SYNTHETIC;
    cf->thisClass  = cf->cp.addClass(cf->cp.addUtf8(kCLoaderClass));
    cf->superClass = cf->cp.addClass(cf->cp.addUtf8("java/lang/Object"));

    auto opened = jbc::ir::makeSession(std::move(cf));
    if (opened.is_err()) {
        throw std::runtime_error("cloader_postprocessor: makeSession failed");
    }
    auto ses = std::move(opened).unwrap();

    auto addAndEmit = [&](const jbc::ir::MethodSeed& seed,
                          std::vector<jbc::ir::Insn> seq) {
        const std::string what(seed.name);
        auto added = ses->addMethod(seed);
        if (added.is_err()) {
            throw std::runtime_error("cloader_postprocessor: addMethod " + what + " failed");
        }
        const ca::usize mi  = ses->backing().methods.size() - 1;
        auto            promoted = jbc::ir::promoteMethod(*ses, mi);
        if (promoted.is_err()) {
            throw std::runtime_error("cloader_postprocessor: promote " + what + " failed");
        }
        auto unit = std::move(promoted).unwrap();
        {
            jbc::ir::CodeEditor editor(*unit);
            for (auto& in : seq) {
                auto appendRes = editor.append(std::move(in));
                if (appendRes.is_err()) {
                    throw std::runtime_error("cloader_postprocessor: append " + what + " failed");
                }
            }
        }
        auto lowered = jbc::ir::lowerMethod(*ses, mi);
        if (lowered.is_err()) {
            throw std::runtime_error("cloader_postprocessor: lower " + what + " failed");
        }
    };

    // <init>()V : super.<init>()。
    {
        std::vector<jbc::ir::Insn> seq;
        seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
        jbc::ir::Insn superInit;
        superInit.op = jbc::core::Op::INVOKESPECIAL;
        superInit.cp = ses->internMethodRef("java/lang/Object", "<init>", "()V");
        seq.push_back(std::move(superInit));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        addAndEmit(jbc::ir::MethodSeed{"<init>", "()V", Opcodes::ACC_PRIVATE, true, 1, 1},
                   std::move(seq));
    }

    // <clinit>：配置了 native 资源时注入一次 NativeLoader.load（load 只在
    // Loader 首次初始化时执行一次；转换类 clinit 只需 registerNativesForClass，
    // 避免每个类重复携带相同的资源路径字符串与调用）。
    if (!resourcePath.empty()) {
        std::vector<jbc::ir::Insn> seq;
        seq.push_back(jbc::ir::CodeEditor::loadConst(ses->internString(resourcePath)));
        seq.push_back(jbc::ir::CodeEditor::loadConst(
            ses->internString(tempSuffix.empty() ? ".mj2x" : tempSuffix)));
        jbc::ir::Insn load;
        load.op = jbc::core::Op::INVOKESTATIC;
        load.cp = ses->internMethodRef(native_loader::kClassName, "load",
                                       native_loader::kLoadDesc);
        seq.push_back(std::move(load));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        addAndEmit(jbc::ir::MethodSeed{"<clinit>", "()V", Opcodes::ACC_STATIC, true, 2, 0},
                   std::move(seq));
    }

    // native 桥接：RegisterNatives 的目标（JNI 实现落在运行时库）。
    addAndEmit(jbc::ir::MethodSeed{"registerNativesForClass", "(ILjava/lang/Class;)V",
                                   static_cast<ca::u16>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC
                                                        | Opcodes::ACC_NATIVE
                                                        | Opcodes::ACC_SYNTHETIC),
                                   false, 0, 2},
               {});

    // invokeMethodHandle1：lambda 桥接直通车（MethodHandle.invoke(Object)Object）。
    {
        std::vector<jbc::ir::Insn> seq;
        seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
        seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 1));
        jbc::ir::Insn invoke;
        invoke.op = jbc::core::Op::INVOKEVIRTUAL;
        invoke.cp = ses->internMethodRef("java/lang/invoke/MethodHandle", "invoke",
                                         "(Ljava/lang/Object;)Ljava/lang/Object;");
        seq.push_back(std::move(invoke));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::ARETURN));
        addAndEmit(jbc::ir::MethodSeed{"invokeMethodHandle1",
                                       "(Ljava/lang/invoke/MethodHandle;Ljava/lang/Object;)Ljava/lang/Object;",
                                       static_cast<ca::u16>(Opcodes::ACC_PUBLIC
                                                            | Opcodes::ACC_STATIC
                                                            | Opcodes::ACC_SYNTHETIC),
                                       true, 2, 2},
                   std::move(seq));
    }

    // 会话收口：字节往返还回独立 ClassFile（真实编解码器纪律，与
    // dom_edit commitDomEdit 同通道；Loader 进 index 后还要过注入与输出面）。
    auto written = jbc::core::writeClass(ses->backing());
    if (written.is_err()) {
        throw std::runtime_error("cloader_postprocessor: writeClass failed");
    }
    auto bytes = std::move(written).unwrap();
    auto read  = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::runtime_error("cloader_postprocessor: readClass failed");
    }
    return std::move(read).unwrap().file;
}

}   // namespace

void CLoaderPostprocessor::process(CompilerIndex& index)
{
    if (!index.isJarInput()) {
        CompilerClass cls;
        cls.dom       = buildCLoaderClassDom(resourcePath_, tempSuffix_);
        cls.isLibrary = true;
        cls.modified  = true;
        refreshMethodSnapshots(cls);
        index.addClass(kCLoaderClass, std::move(cls));
        return;
    }
    auto dom     = buildCLoaderClassDom(resourcePath_, tempSuffix_);
    auto written = jbc::core::writeClass(*dom);
    if (written.is_err()) {
        throw std::runtime_error("cloader_postprocessor: resource writeClass failed");
    }
    index.upsertJarResource(kCLoaderEntry, std::move(written).unwrap());
}

}   // namespace mj2x

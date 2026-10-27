// mj2x LdcPreprocessor — 展开 LDC Handle / LDC MethodType 为 MethodHandles.Lookup 反射调用链。
//
// 详见 mj2x/doc_ai/preprocessor.md §3。Lookup 取 MethodHandles.lookup()（调用者即本类，
// 访问权限与原 ldc 常量一致）；MethodType 构造走 fromMethodDescriptorString(desc,
// caller.getClassLoader())（系统 ClassLoader 解析不了保护态 jar 的应用类引用）。
//
// 处理：
//   ldc Handle  → Lookup.findVirtual/findStatic/findSpecial/findConstructor/
//                 findGetter/findSetter 系（按 tag 分派，压栈顺序对齐 findXxx 描述符，
//                 栈形校验见 unittest ldc_preprocessor_test）
//   ldc MethodType → MethodType.fromMethodDescriptorString(desc, callerClassLoader)
//   其它 ldc（int/float/String/Class）→ 不动（L1 能直译）
//
// 批 4：改写载体切 DOM——ClassSession promote/CodeEditor/lower，指令 CP 操作数
// 经 session 语义 intern（去重追加）；展开序列逐条 insertBefore 到原 ldc 之前后
// erase 原 ldc，与树版逐指令同构。maxStack 保守 +4 的口径与树版一致（展开链
// 最深形态达原栈深 +5，见树版栈形注释）。

#include "ldc_preprocessor.hpp"

#include "jbc/ir/code_editor.hpp"

#include "mj2x/backend/indy_bootstrap_plan.hpp"
#include "mj2x/core/dom_edit.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace mj2x {

namespace {

/// 字段描述符 head 为 primitive 时，承载其 Class 对象的包装类（Integer.TYPE 等）。
const char* primitiveBoxClass(char head)
{
    switch (head) {
    case 'Z': return "java/lang/Boolean";
    case 'C': return "java/lang/Character";
    case 'B': return "java/lang/Byte";
    case 'S': return "java/lang/Short";
    case 'I': return "java/lang/Integer";
    case 'J': return "java/lang/Long";
    case 'F': return "java/lang/Float";
    case 'D': return "java/lang/Double";
    default: return nullptr;
    }
}

/// CP 成员引用三跳解析（Fieldref/Methodref/InterfaceMethodref → owner/name/desc/itf）。
/// 与 code_converter 的 resolveRef 同布局；preprocessor 侧独立持有（批 4 不动
/// code_converter 的私有面，共享收编留后续）。
void domResolveRef(const jbc::core::ClassFile& dom, ca::u32 cpIdx, std::string& owner,
                   std::string& name, std::string& desc, bool& itf)
{
    const auto& entry = dom.cp.at(cpIdx);
    if (entry.tag != jbc::core::CpTag::Fieldref && entry.tag != jbc::core::CpTag::Methodref &&
        entry.tag != jbc::core::CpTag::InterfaceMethodref) {
        throw std::runtime_error("ldc_preprocessor: CP is not a member ref");
    }
    itf   = (entry.tag == jbc::core::CpTag::InterfaceMethodref);
    owner = std::string(dom.className(entry.a));
    const auto& nat = dom.cp.at(entry.b);
    name  = std::string(dom.cp.utf8Of(nat.a));
    desc  = std::string(dom.cp.utf8Of(nat.b));
}

/// 压入 MethodType：LDC String(desc), LDC Class(caller), getClassLoader(),
/// MethodType.fromMethodDescriptorString(String, ClassLoader)。逐条 insertBefore
/// 到 at 之前（调用序即程序序）。
void pushMethodTypeSeq(jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor,
                       jbc::ir::InsnId at, const std::string& desc,
                       const std::string& callerClass)
{
    jbc::ir::Insn str = jbc::ir::CodeEditor::loadConst(session.internString(desc));
    (void)editor.insertBefore(at, std::move(str));

    jbc::ir::Insn caller = jbc::ir::CodeEditor::loadConst(session.internClass(callerClass));
    (void)editor.insertBefore(at, std::move(caller));

    jbc::ir::Insn getClassLoader;
    getClassLoader.op = jbc::core::Op::INVOKEVIRTUAL;
    getClassLoader.cp = session.internMethodRef("java/lang/Class", "getClassLoader",
                                                "()Ljava/lang/ClassLoader;");
    (void)editor.insertBefore(at, std::move(getClassLoader));

    jbc::ir::Insn fromMds;
    fromMds.op = jbc::core::Op::INVOKESTATIC;
    fromMds.cp = session.internMethodRef(
        "java/lang/invoke/MethodType", "fromMethodDescriptorString",
        "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;");
    (void)editor.insertBefore(at, std::move(fromMds));
}

/// 单条展开序列发射：构造 Insn 并插到 at 之前。
void emitInsn(jbc::ir::CodeEditor& editor, jbc::ir::InsnId at, jbc::core::Op op, ca::u32 cp)
{
    jbc::ir::Insn in;
    in.op = op;
    in.cp = cp;
    (void)editor.insertBefore(at, std::move(in));
}

void emitStringLdc(jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor,
                   jbc::ir::InsnId at, const std::string& value)
{
    emitInsn(editor, at, jbc::ir::CodeEditor::loadConst(session.internString(value)).op,
             session.internString(value));
}

void emitClassLdc(jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor,
                  jbc::ir::InsnId at, const std::string& internalName)
{
    emitInsn(editor, at, jbc::ir::CodeEditor::loadConst(session.internClass(internalName)).op,
             session.internClass(internalName));
}

/// 展开单个 ldc Handle 调用点（展开序列按序逐条 insertBefore 到原 ldc 之前）。
void expandHandleSite(jbc::ir::ClassSession& session, jbc::ir::CodeEditor& editor,
                      jbc::ir::InsnId at, ca::u32 handleCp, const std::string& callerClass)
{
    // Handle 细节（tag/owner/name/desc）constOf 不透出，直接读 DOM CP：
    // MethodHandle.a = reference_kind（1..9，与旧栈 Opcodes::H_* 同值域），
    // b = 成员引用索引。
    const auto& entry = session.backing().cp.at(handleCp);
    if (entry.tag != jbc::core::CpTag::MethodHandle) {
        throw std::runtime_error("ldc_preprocessor: ldc CP is not a MethodHandle");
    }
    std::string owner, name, desc;
    bool        itf = false;
    domResolveRef(session.backing(), entry.b, owner, name, desc, itf);

    // Lookup receiver + declaring Class。
    {
        jbc::ir::Insn lookup;
        lookup.op = jbc::core::Op::INVOKESTATIC;
        lookup.cp = session.internMethodRef("java/lang/invoke/MethodHandles", "lookup",
                                            "()Ljava/lang/invoke/MethodHandles$Lookup;");
        (void)editor.insertBefore(at, std::move(lookup));
    }
    emitClassLdc(session, editor, at, owner);

    // findXxx 选择（按 tag）——#1027 find4 收敛：tag→find* 形态单源 =
    // indyHandleFindFor；name/MethodType/fieldType/caller 按其描述符顺序入栈。
    const IndyHandleFind find = indyHandleFindFor(entry.a);
    switch (find.argShape) {
    case IndyHandleArgShape::NameFieldType:
    {
        // findGetter/findSetter 系：第三个参数是字段类型 Class，不是 MethodType。
        // primitive 字段类型没有 ldc 形态的 Class 常量，用 GETSTATIC <Box>.TYPE
        //（物化策略保留树版口径：primitive 走 TYPE 常量、其余发精确类型 LDC Class）。
        emitStringLdc(session, editor, at, name);
        if (const char* box = primitiveBoxClass(desc.empty() ? '\0' : desc[0])) {
            jbc::ir::Insn getType;
            getType.op = jbc::core::Op::GETSTATIC;
            getType.cp = session.internFieldRef(box, "TYPE", "Ljava/lang/Class;");
            (void)editor.insertBefore(at, std::move(getType));
        }
        else {
            emitClassLdc(session, editor, at, desc);
        }
        break;
    }
    case IndyHandleArgShape::NameTypeOwner:
        // findSpecial(Class, String, MethodType, Class specialCaller)。
        emitStringLdc(session, editor, at, name);
        pushMethodTypeSeq(session, editor, at, desc, callerClass);
        emitClassLdc(session, editor, at, callerClass);
        break;
    case IndyHandleArgShape::TypeOnly:
        // findConstructor(Class, MethodType)：无 name 参数。
        pushMethodTypeSeq(session, editor, at, desc, callerClass);
        break;
    case IndyHandleArgShape::NameType:
    default:
        // findVirtual/findStatic(Class, String, MethodType)。
        emitStringLdc(session, editor, at, name);
        pushMethodTypeSeq(session, editor, at, desc, callerClass);
        break;
    }

    {
        jbc::ir::Insn findCall;
        findCall.op = itf ? jbc::core::Op::INVOKEINTERFACE : jbc::core::Op::INVOKEVIRTUAL;
        findCall.cp = itf ? session.internInterfaceMethodRef(
                                "java/lang/invoke/MethodHandles$Lookup", find.findName,
                                find.findDesc)
                          : session.internMethodRef(
                                "java/lang/invoke/MethodHandles$Lookup", find.findName,
                                find.findDesc);
        (void)editor.insertBefore(at, std::move(findCall));
    }
}

}   // namespace

void LdcPreprocessor::process(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;

        auto session = openDomEdit(cls, name);
        bool changed = false;
        const std::string callerClass(session->backing().thisName());

        const ca::usize methodCount = session->unit().methods.size();
        for (ca::usize mi = 0; mi < methodCount; ++mi) {
            if (!jbc::ir::hasCode(*session, mi)) continue;

            auto promoted = jbc::ir::promoteMethod(*session, mi);
            if (promoted.is_err()) {
                throw std::runtime_error("ldc_preprocessor: promote failed for " + name
                                         + " method #" + std::to_string(mi));
            }
            auto unit = std::move(promoted).unwrap();

            // 先收集后改写（CodeDom 句柄域稳定，insertBefore 不失效既有句柄）。
            std::vector<jbc::ir::InsnId> targets;
            for (auto id : unit->dom.order()) {
                const auto* in = unit->dom.peek(id);
                if (in->op != jbc::core::Op::LDC && in->op != jbc::core::Op::LDC_W) continue;
                jbc::ir::ClassSession::ConstSem sem;
                if (!session->constOf(*in, sem)) continue;
                if (sem.kind == jbc::ir::ClassSession::ConstSem::Kind::MethodHandle ||
                    sem.kind == jbc::ir::ClassSession::ConstSem::Kind::MethodType) {
                    targets.push_back(id);
                }
            }
            if (targets.empty()) continue;

            // 展开链的操作数栈峰值高于原 ldc（树版口径）：maxStack 保守 +4。
            unit->maxStack = static_cast<ca::u16>(unit->maxStack + 4);

            jbc::ir::CodeEditor editor(*unit);
            for (auto id : targets) {
                const auto in = *unit->dom.peek(id);
                jbc::ir::ClassSession::ConstSem sem;
                if (!session->constOf(in, sem)) continue;
                if (sem.kind == jbc::ir::ClassSession::ConstSem::Kind::MethodHandle) {
                    expandHandleSite(*session, editor, id, in.cp, callerClass);
                }
                else {
                    // ldc MethodType → fromMethodDescriptorString(desc,
                    // callerClassLoader)；sem.str 即方法描述符。
                    pushMethodTypeSeq(*session, editor, id, std::string(sem.str), callerClass);
                }
                auto erased = editor.erase(id);
                if (erased.is_err()) {
                    throw std::runtime_error("ldc_preprocessor: erase ldc failed for " + name
                                             + " method #" + std::to_string(mi));
                }
            }

            auto lowered = jbc::ir::lowerMethod(*session, mi);
            if (lowered.is_err()) {
                throw std::runtime_error("ldc_preprocessor: lower failed for " + name
                                         + " method #" + std::to_string(mi));
            }
            changed = true;
        }

        // 展开改写了指令流：类必须标记 modified，否则 output 侧不重算帧
        //（对齐树版口径：旧 StackMapTable 配新指令流必 VerifyError）。
        if (changed) cls.modified = true;
        commitDomEdit(cls, name, session);
    }
}

}   // namespace mj2x

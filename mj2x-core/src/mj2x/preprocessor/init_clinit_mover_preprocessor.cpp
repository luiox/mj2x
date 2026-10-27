#include "init_clinit_mover_preprocessor.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "jbc/core/insn_codec.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "libca/log/log_macros.hpp"

#include "mj2x/core/dom_edit.hpp"
#include "mj2x/core/jvm_opcodes.hpp"

namespace mj2x {

namespace {

// FNV-1a 64 位（与 RegCBackend 命名同源，保证名字稳定）。
std::uint64_t fnv1a64(const std::string& text, std::uint64_t hash = 1469598103934665603ULL)
{
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex64(std::uint64_t value)
{
    std::ostringstream os;
    os << std::hex << value;
    return os.str();
}

// 参数描述符段 → xLOAD 通用形（opcode + u1 slot）字节。基类型按栈类别：
// J/D 占双槽，L/[ 引用形态，其余 int 类。方法参数上限 255，u1 编码够用。
void appendLoadBytes(std::vector<ca::u8>& code, char c, ca::u32 slot)
{
    ca::u8 op;
    switch (c) {
    case 'J': op = 0x16; break;   // LLOAD
    case 'D': op = 0x18; break;   // DLOAD
    case 'F': op = 0x17; break;   // FLOAD
    case 'L':
    case '[': op = 0x19; break;   // ALOAD
    default:  op = 0x15; break;   // ILOAD（B/C/S/Z/I 同栈类别）
    }
    code.push_back(op);
    code.push_back(static_cast<ca::u8>(slot & 0xFF));
}

/// 把 "(Lfoo;[JID)V" 的参数段逐槽发射 load 字节；返回消耗的槽总数。
ca::u32 appendParamLoads(std::vector<ca::u8>& code, const std::string& desc)
{
    ca::u32 slot = 1;
    for (std::size_t i = 1; i < desc.size() && desc[i] != ')';) {
        const char c = desc[i];
        if (c == '[') {
            // 数组：跳过全部维度符后按基类型收尾；load 按引用类别。
            std::size_t j = i;
            while (j < desc.size() && desc[j] == '[') ++j;
            appendLoadBytes(code, '[', slot);
            slot += 1;
            i = (j < desc.size() && desc[j] == 'L') ? desc.find(';', j) + 1 : j + 1;
        }
        else if (c == 'L') {
            appendLoadBytes(code, 'L', slot);
            slot += 1;
            i = desc.find(';', i) + 1;
        }
        else {
            appendLoadBytes(code, c, slot);
            slot += (c == 'J' || c == 'D') ? 2 : 1;
            i += 1;
        }
    }
    return slot;
}


/// 属性名槽的 Utf8 索引（findUtf8 命中既有池不膨胀，miss 才追加）。
/// 注意 session.internString 是 CONSTANT_String 条目（ldc 语义），属性名
/// 必须直接指 Utf8，两者不可混用。
ca::u32 attrNameIdx(jbc::core::ConstantPool& cp, std::string_view name)
{
    if (ca::u32 found = cp.findUtf8(name)) return found;
    return cp.addUtf8(name);
}

/// 通用 Code 属性槽构造（State::New，写出重编码）。
jbc::core::AttrSlot makeCodeSlot(ca::u32 codeNameCp,
                                 std::unique_ptr<jbc::core::AttrBody> body)
{
    jbc::core::AttrSlot slot;
    slot.nameCp = codeNameCp;
    slot.state  = jbc::core::AttrSlot::State::New;
    slot.body   = std::move(body);
    return slot;
}

/// 骨架 CodeAttr：INVOKESTATIC cpIdx + RETURN（搬运调用点保留）。
std::unique_ptr<jbc::core::CodeAttr> makeSkeletonCode(ca::u32 invokeCp, ca::u16 maxLocals)
{
    auto body       = std::make_unique<jbc::core::CodeAttr>();
    body->maxStack  = 1;
    body->maxLocals = maxLocals;
    body->code      = {0xB8,
                       static_cast<ca::u8>((invokeCp >> 8) & 0xFF),
                       static_cast<ca::u8>(invokeCp & 0xFF),
                       0xB1};
    return body;
}

/// 方法成员的 Code 槽定位（已解码；缺失返回 nullptr）。
jbc::core::AttrSlot* findCodeSlot(jbc::core::ClassFile& dom, jbc::core::MemberInfo& m)
{
    for (auto& slot : m.attrs) {
        if (slot.body == nullptr) {
            // 嵌套 lazy 槽按需解码（Code 槽在 decodeCode 后已就位；此处理
            // 类级/成员级旁路槽）。
            auto decoded = dom.decodeAttr(slot);
            if (decoded.is_err()) return nullptr;
            (void)std::move(decoded).unwrap();
        }
        if (slot.body != nullptr && slot.body->kind() == jbc::core::AttrKind::Code) {
            return &slot;
        }
    }
    return nullptr;
}

/// 嵌套属性里是否含局部变量表族（LVT/LVTT）——搬运与整段转移冲突（v1 保守）。
/// 嵌套槽 lazy，解码按需（失败 fail-loud）。
bool hasLocalVariableTable(jbc::core::ClassFile& dom, jbc::core::CodeAttr& code)
{
    for (auto& slot : code.attrs) {
        if (slot.body == nullptr) {
            auto decoded = dom.decodeAttr(slot);
            if (decoded.is_err()) {
                throw std::runtime_error("init_clinit_mover: nested attr decode failed");
            }
        }
        if (slot.body != nullptr &&
            slot.body->kind() == jbc::core::AttrKind::LocalVariableTable) {
            return true;
        }
    }
    return false;
}

/// 拆出嵌套 LNT 槽（若多条取首条；javac 单条常态），无则返回 nullptr。槽 lazy 解码。
/// 返回槽而非体：改 rows 必须经 mutate() 置脏——Decoded 槽写回仍 copy 源 span，
/// 裸改 body 会被写回静默丢弃（e2e ClassFormatError 实证）。
jbc::core::AttrSlot* findLnt(jbc::core::ClassFile& dom, jbc::core::CodeAttr& code)
{
    for (auto& slot : code.attrs) {
        if (slot.body == nullptr) {
            auto decoded = dom.decodeAttr(slot);
            if (decoded.is_err()) {
                throw std::runtime_error("init_clinit_mover: nested attr decode failed");
            }
        }
        if (slot.body != nullptr &&
            slot.body->kind() == jbc::core::AttrKind::LineNumberTable) {
            return &slot;
        }
    }
    return nullptr;
}

/// SMT（Frame）守卫（树版 hasForbiddenPrefix 的 FRAME 分支口径对齐）：骨架段
/// [0..cutEnd) 内锚定的帧 = 拒绝搬运（返回 false）；尾段锚定的帧随体丢弃
///（整槽抹除，output 侧 COMPUTE 重算帧）。绝对 pc 链式口径同 mjbase
/// frame_rewire：首帧 offset 即绝对 pc，其后为 offset_delta。
bool prefixFrameOrStripTailSmt(jbc::core::ClassFile& dom, jbc::core::CodeAttr& code,
                               ca::u32 cutEnd)
{
    for (auto it = code.attrs.begin(); it != code.attrs.end(); ++it) {
        if (it->body == nullptr) {
            auto decoded = dom.decodeAttr(*it);
            if (decoded.is_err()) {
                throw std::runtime_error("init_clinit_mover: nested attr decode failed");
            }
        }
        if (it->body == nullptr || it->body->kind() != jbc::core::AttrKind::StackMapTable) {
            continue;
        }
        auto* smt = static_cast<jbc::core::StackMapTableAttr*>(it->body.get());
        ca::u32   prev = 0;
        for (std::size_t fi = 0; fi < smt->frames.size(); ++fi) {
            const ca::u32 pc = (fi == 0)
                ? smt->frames[fi].offset
                : jbc::core::smtAbsPc(prev, smt->frames[fi].offset);
            if (pc < cutEnd) return false;
            prev = pc;
        }
        code.attrs.erase(it);
        break;   // SMT 至多一条（JVMS 4.7.4）
    }
    return true;
}

/// 指令流解码（失败 fail-loud；同步器保真字节里指令必然可解）。
std::vector<jbc::core::RawInsn> decodeOrThrow(const jbc::core::CodeAttr& code,
                                              const std::string& where)
{
    auto insns = jbc::core::decodeInsns(
        jbc::core::ByteView{code.code.data(), code.code.size()});
    if (insns.is_err()) {
        throw std::runtime_error("init_clinit_mover: decode failed for " + where);
    }
    return std::move(insns).unwrap();
}

/// <clinit> 搬运（批 4 DOM 形态）：整个 Code 体（字节+LNT 等嵌套属性）随槽
/// 所有权转移进 __regc_clinit_<hash>，原 <clinit> 换 INVOKESTATIC+RETURN 骨架。
/// 返回生成方法名（空 = 本次未搬运）。
std::string moveClinit(jbc::ir::ClassSession& session, CompilerClass& cls,
                       jbc::core::MemberInfo& clinit, const std::string& className)
{
    auto* codeSlot = findCodeSlot(session.backing(), clinit);
    if (codeSlot == nullptr || codeSlot->body == nullptr) return {};
    auto* code = static_cast<jbc::core::CodeAttr*>(codeSlot->body.get());
    if (code->code.empty()) return {};
    // v1 保守：异常表 / 局部变量表与整段搬移冲突，放弃（对齐树版口径）。
    if (!code->excTable.empty() || hasLocalVariableTable(session.backing(), *code)) return {};

    const std::string desc    = "()V";
    const auto        hash    = fnv1a64(className + "@<clinit>" + desc);
    const std::string genName = "__regc_clinit_" + hex64(hash);

    // 新方法：static private synthetic（CP intern 走 session 语义面）。
    auto genId = session.addMethod(jbc::ir::MethodSeed{
        genName, desc,
        Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE | Opcodes::ACC_SYNTHETIC,
        false});
    if (genId.is_err()) {
        throw std::runtime_error("init_clinit_mover: addMethod failed for " + genName);
    }
    (void)genId;

    // Code 体随槽所有权转移（Decoded 槽写回 copy 源 span，字节保真；嵌套
    // LNT/SMT 的 lazy span 同源共命）。
    session.backing().methods.back().attrs.push_back(std::move(*codeSlot));

    // 原 <clinit> 换骨架：INVOKESTATIC 生成方法 + RETURN；maxLocals 保持。
    const ca::u32 invokeCp = session.internMethodRef(className, genName, desc);
    *codeSlot = makeCodeSlot(attrNameIdx(session.backing().cp, "Code"), makeSkeletonCode(invokeCp, code->maxLocals));

    cls.modified = true;
    return genName;
}

/// 构造器搬运（v1，批 4 DOM 形态）：找到第一个 `<init>` invokespecial 作为切
/// 点；切点（含）之前保留骨架，之后的字节尾段平移进 __regc_init_<hash>——
/// 尾段控制流封闭（守卫：跨段引用拒绝），段内跳转相对偏移平移不变，字节
/// 原样搬运；嵌套 LNT 条目按 startPc 归属拆分（尾段条目 startPc -= cutEnd），
/// SMT 丢弃（output 侧 COMPUTE 重算帧）。
/// 返回新生成的方法名（空 = 本次未搬运）。
std::string moveInit(jbc::ir::ClassSession& session, CompilerClass& cls,
                     jbc::core::MemberInfo& init, const std::string& className)
{
    auto* codeSlot = findCodeSlot(session.backing(), init);
    if (codeSlot == nullptr || codeSlot->body == nullptr) return {};
    auto* code = static_cast<jbc::core::CodeAttr*>(codeSlot->body.get());
    if (code->code.empty()) return {};
    if (!code->excTable.empty() || hasLocalVariableTable(session.backing(), *code)) return {};

    const auto insns = decodeOrThrow(*code, className + ".<init>");
    const std::string desc(session.backing().descOf(init));

    // 找第一个 <init> 调用的 invokespecial 指令（切点；切点含在骨架段）。
    ca::u32 cutPc  = 0;
    bool    found  = false;
    for (const auto& in : insns) {
        if (in.op != jbc::core::Op::INVOKESPECIAL) continue;
        const auto& nat = session.backing().cp.at(session.backing().cp.at(in.cp).b);
        if (session.backing().cp.utf8Of(nat.a) == "<init>") {
            cutPc  = in.pc;
            found  = true;
            break;
        }
    }
    if (!found) return {};

    // 切点指令结束 = 尾段起点（下一条指令 pc）。切点即末条 → 尾段为空 →
    // 放弃（空体方法是非法形态；javac 构造器 super 调用后必有 return，
    // 不可达面）。
    ca::u32 cutEnd = code->code.size();
    for (const auto& in : insns) {
        if (in.pc > cutPc) {
            cutEnd = in.pc;
            break;
        }
    }
    if (cutEnd >= code->code.size()) return {};

    // 骨架段 [0..cutEnd) 禁止控制流（树版 hasForbiddenPrefix 对齐；FRAME 在
    // DOM 字节域不存在）。
    for (const auto& in : insns) {
        if (in.pc >= cutEnd) break;
        const auto shape = jbc::core::shapeOf(in.op);
        if (shape == jbc::core::Shape::Branch2 || shape == jbc::core::Shape::Branch4 ||
            shape == jbc::core::Shape::TableSwitch ||
            shape == jbc::core::Shape::LookupSwitch) {
            return {};
        }
    }
    // 尾段跳转目标必须封闭在尾段内（树版跨段引用守卫对齐）：目标 < cutEnd
    // 即指向骨架段，拒绝。骨架段无跳转（上守卫），反向引用不存在。
    for (const auto& in : insns) {
        if (in.pc < cutEnd) continue;
        switch (jbc::core::shapeOf(in.op)) {
        case jbc::core::Shape::Branch2:
        case jbc::core::Shape::Branch4:
            if (in.pc + in.t < cutEnd) return {};
            break;
        case jbc::core::Shape::TableSwitch:
        case jbc::core::Shape::LookupSwitch:
            if (in.pc + in.t < cutEnd) return {};
            for (const auto& off : in.offs) {
                if (in.pc + off < cutEnd) return {};
            }
            break;
        default: break;
        }
    }

    // SMT（Frame）守卫：骨架段内锚定的帧拒绝搬运；尾段帧随体丢弃。
    if (!prefixFrameOrStripTailSmt(session.backing(), *code, cutEnd)) return {};

    const auto        hash    = fnv1a64(className + "<init>" + desc);
    const std::string genName = "__regc_init_" + hex64(hash);

    // 生成方法：static，desc = "(L<owner>;<原参数>)V"，this 显式参数化。
    std::string genDesc = "(L" + className + ";" + desc.substr(1);
    auto genId = session.addMethod(jbc::ir::MethodSeed{
        genName, genDesc,
        Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE | Opcodes::ACC_SYNTHETIC,
        false});
    if (genId.is_err()) {
        throw std::runtime_error("init_clinit_mover: addMethod failed for " + genName);
    }
    (void)genId;

    // 尾段字节平移进生成方法（控制流封闭 ⇒ 相对偏移不变）。
    auto genCode       = std::make_unique<jbc::core::CodeAttr>();
    genCode->maxStack  = code->maxStack;
    genCode->maxLocals = code->maxLocals;
    genCode->code.assign(code->code.begin() + static_cast<std::ptrdiff_t>(cutEnd),
                         code->code.end());

    // 嵌套 LNT 拆分：尾段条目（startPc >= cutEnd）随体搬运（startPc -= cutEnd），
    // 骨架条目留在 init；SMT 丢弃（output 侧 COMPUTE 重算）。
    if (auto* lntSlot = findLnt(session.backing(), *code)) {
        auto*           lnt = static_cast<jbc::core::LineNumberTableAttr*>(lntSlot->mutate());
        auto genLntBody = std::make_unique<jbc::core::LineNumberTableAttr>();
        std::vector<jbc::core::LineNumberTableAttr::Row> kept;
        for (const auto& row : lnt->rows) {
            if (row.startPc >= cutEnd) {
                genLntBody->rows.push_back({row.startPc - cutEnd, row.line});
            }
            else {
                kept.push_back(row);
            }
        }
        lnt->rows = std::move(kept);
        if (!genLntBody->rows.empty()) {
            genCode->attrs.push_back(makeCodeSlot(
                attrNameIdx(session.backing().cp, "LineNumberTable"), std::move(genLntBody)));
        }
    }

    session.backing().methods.back().attrs.push_back(
        makeCodeSlot(attrNameIdx(session.backing().cp, "Code"), std::move(genCode)));

    // init 骨架：[0..cutEnd) + aload_0 + 参数 load + invokestatic + return。
    // mutate 置脏走重编码；原 CodeAttr 的 body 未被 move（只拆了嵌套属性）。
    auto* initCode = static_cast<jbc::core::CodeAttr*>(codeSlot->mutate());
    initCode->code.resize(cutEnd);
    initCode->code.push_back(0x2A);   // ALOAD_0
    appendParamLoads(initCode->code, desc);
    const ca::u32 invokeCp = session.internMethodRef(className, genName, genDesc);
    initCode->code.push_back(0xB8);
    initCode->code.push_back(static_cast<ca::u8>((invokeCp >> 8) & 0xFF));
    initCode->code.push_back(static_cast<ca::u8>(invokeCp & 0xFF));
    initCode->code.push_back(0xB1);   // RETURN

    cls.modified = true;
    return genName;
}

/// gen 元数据快照落位：批 6d 树退役后 CompilerMethod 快照即方法身份源，
/// markAll 等 pass 按 name+desc 锚定 cls.methods——生成方法直接追加快照
/// （指令体已在 DOM 会话内建好，快照只承载元数据与管线旗标）。
void mirrorGenToSnapshot(CompilerClass& cls, const std::string& genName,
                         const std::string& genDesc, ca::u32 access)
{
    CompilerMethod cm;
    cm.name   = genName;
    cm.desc   = genDesc;
    cm.access = static_cast<ca::u16>(access);
    cls.methods.push_back(std::move(cm));
}

}   // namespace

void InitClinitMoverPreprocessor::process(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        // 类级 glob 过滤器剔除的类整体不进 native 集，不生成 __regc_* 搬运方法
        //（否则会留下永不标记的死静态方法，并无谓改变类形态）。
        if (cls.filteredOut) continue;
        // 接口同上整体跳过：接口方法禁止 ACC_NATIVE（JLS 9.4/JVMS 4.6），
        // 标记路径全部跳过接口（MarkAll 等），搬运产物只会是死静态方法。
        if (cls.dom && (cls.dom->access & Opcodes::ACC_INTERFACE)) continue;

        auto session = openDomEdit(cls, name);
        // 注意：遍历的是原始方法列表；生成方法在循环内追加（backing.methods
        // 尾部），不回遍历——先取原方法数。
        const ca::usize count = session->backing().methods.size();
        for (ca::usize mi = 0; mi < count; ++mi) {
            auto&       dm    = session->backing().methods[mi];
            const auto  mname = std::string(session->backing().nameOf(dm));
            if (mname == "<clinit>") {
                auto genName = moveClinit(*session, cls, dm, name);
                if (!genName.empty()) {
                    mirrorGenToSnapshot(cls, genName, "()V",
                                        Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE
                                            | Opcodes::ACC_SYNTHETIC);
                }
            }
            else if (mname == "<init>") {
                const auto   desc    = std::string(session->backing().descOf(dm));
                auto         genName = moveInit(*session, cls, dm, name);
                if (!genName.empty()) {
                    mirrorGenToSnapshot(cls, genName,
                                        "(L" + name + ";" + desc.substr(1),
                                        Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE
                                            | Opcodes::ACC_SYNTHETIC);
                }
            }
        }
        commitDomEdit(cls, name, session);
    }
    CA_LOGT_INFO("preprocess", "<init>/<clinit> move pass done\n");
}

}   // namespace mj2x

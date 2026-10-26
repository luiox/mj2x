#include "init_clinit_mover_preprocessor.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "jbytecode/core/primitives.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

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

std::string toStd(const ca::str::Utf8StringRef& ref)
{
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

/// 按 Type sort 选 xLOAD 操作码（boolean/char/byte/short/int 都以 int 存栈）。
int loadOpcode(int sort)
{
    switch (sort) {
    case Type::LONG: return Opcodes::LLOAD;
    case Type::FLOAT: return Opcodes::FLOAD;
    case Type::DOUBLE: return Opcodes::DLOAD;
    case Type::ARRAY:
    case Type::OBJECT: return Opcodes::ALOAD;
    default: return Opcodes::ILOAD;
    }
}

int slotSize(int sort)
{
    return (sort == Type::LONG || sort == Type::DOUBLE) ? 2 : 1;
}

/// 骨架段（[0..cut]，含）内禁止控制流/帧节点；LABEL/LINE 允许，但要求
/// 骨架与克隆体的 label 引用各在自己的段内（无跨切点引用）。返回 false = 不可搬。
bool hasForbiddenPrefix(const MethodNode* mn, const AbstractInsnNode* cut)
{
    for (auto* insn = mn->instructions.getFirst(); insn && insn != cut->nextInsn;
         insn = insn->nextInsn) {
        switch (insn->getType()) {
        case AbstractInsnNode::FRAME:
        case AbstractInsnNode::JUMP_INSN:
        case AbstractInsnNode::TABLESWITCH_INSN:
        case AbstractInsnNode::LOOKUPSWITCH_INSN:
            return true;
        default:
            break;
        }
    }
    return false;
}

/// 收集一段指令（first..lastExclusive）内所有引用的 label 指针（跳转/switch）。
/// LINE 不参与：行号锚点由生成方法内的占位 label 承接（见 moveInit 的 pre 映射）。
void collectReferencedLabels(AbstractInsnNode* start, AbstractInsnNode* end,
                             std::unordered_set<jbytecode::core::Label*>& out)
{
    for (auto* insn = start; insn && insn != end; insn = insn->nextInsn) {
        switch (insn->getType()) {
        case AbstractInsnNode::JUMP_INSN: {
            out.insert(static_cast<JumpInsnNode*>(insn)->label);
            break;
        }
        case AbstractInsnNode::TABLESWITCH_INSN: {
            auto* ts = static_cast<TableSwitchInsnNode*>(insn);
            out.insert(ts->dflt);
            for (auto* l : ts->labels) out.insert(l);
            break;
        }
        case AbstractInsnNode::LOOKUPSWITCH_INSN: {
            auto* ls = static_cast<LookupSwitchInsnNode*>(insn);
            out.insert(ls->dflt);
            for (auto* l : ls->labels) out.insert(l);
            break;
        }
        default:
            break;
        }
    }
}

/// 推断两个段（骨架、克隆体）之间是否有 label 交叉引用。
/// - 骨架段的引用（跳转/switch/行号）必须指向骨架段 label；
/// - 克隆体的跳转/switch 必须指向克隆体 label；
/// - 克隆体的**行号**引用允许指向骨架段 label（javac 常态：LineNumberTable 条目
///   挂在方法尾部、锚点 label 在方法头部）——moveInit 克隆前会把这些 pre label
///   映射到生成方法的占位 label，随克隆一并重定向。
bool hasCrossSegmentLabelRefs(MethodNode* mn, AbstractInsnNode* cut)
{
    // 骨架段：[first .. cut]（含）。
    std::unordered_set<jbytecode::core::Label*> preLabels;
    for (auto* insn = mn->instructions.getFirst(); insn != cut->nextInsn;
         insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            preLabels.insert(static_cast<LabelNode*>(insn)->label);
        }
    }
    // 克隆体段：cut 之后。
    std::unordered_set<jbytecode::core::Label*> postLabels;
    for (auto* insn = cut->nextInsn; insn; insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            postLabels.insert(static_cast<LabelNode*>(insn)->label);
        }
    }
    bool bad = false;
    // 骨架段所有引用（含行号）都必须落在骨架段 label 上。
    std::unordered_set<jbytecode::core::Label*> preRefs, preLine;
    collectReferencedLabels(mn->instructions.getFirst(), cut->nextInsn, preRefs);
    for (auto* insn = mn->instructions.getFirst(); insn != cut->nextInsn;
         insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::LINE) {
            preLine.insert(static_cast<LineNumberNode*>(insn)->start);
        }
    }
    for (auto* l : preRefs) {
        if (!preLabels.count(l)) bad = true;
    }
    for (auto* l : preLine) {
        if (!preLabels.count(l)) bad = true;
    }
    // 克隆体跳转/switch 目标必须在克隆体段内（行号引用放行，见函数头注释）。
    std::unordered_set<jbytecode::core::Label*> postRefs;
    collectReferencedLabels(cut->nextInsn, nullptr, postRefs);
    for (auto* l : postRefs) {
        if (!postLabels.count(l)) bad = true;
    }
    return bad;
}

/// 构造器搬运（v1）：
/// - 找到第一个 `<init>` invokespecial（super/this 构造调用）作为切点；
/// - 切点及之前保留在骨架，之后的体克隆进静态方法 __regc_init_<hash>；
/// - 骨架末尾补「aload_0 + 按 desc 参数 reload + invokestatic __regc_init_* + return」。
/// 返回新生成的方法名（空 = 本次未搬运）。
std::string moveInit(CompilerIndex& index, CompilerClass& cls, MethodNode* mn)
{
    if (mn->access &
        (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE)) return {};
    if (mn->instructions.isEmpty()) return {};
    // 构造器搬运会注入 __regc_init_<hash> 新方法：Class.getDeclaredMethods() 反射
    // 可见注入方法（synthetic 亦可见），会被精确方法数断言（如 JavaObfuscatorTest
    // Test 2.1 Counter）判 FAIL。业界（JNIC/j2cc/native-obfuscator）均不转构造器逻辑，
    // 因此：凡含实际逻辑的构造器（LocalVariableTable 非空）一律保留字节码，仅对
    // javac 默认空构造器（super 调用后无逻辑）做骨架化。
    if (!mn->tryCatchBlocks.empty() || !mn->localVariables.empty()) return {};
    if (!mn->visibleLocalVariableAnnotations.empty() ||
        !mn->invisibleLocalVariableAnnotations.empty()) {
        return {};
    }

    // 找第一个 <init> 调用的 invokespecial 指令。
    AbstractInsnNode* cut = nullptr;
    for (auto* insn = mn->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* mi = static_cast<MethodInsnNode*>(insn);
        if (mi->getOpcode() == Opcodes::INVOKESPECIAL &&
            mi->name.to_std_string() == "<init>") {
            cut = insn;
            break;
        }
    }
    if (cut == nullptr) return {};
    // 骨架段禁止帧/分支；LABEL/LINE 允许（javac 行号锚点常在方法头部），
    // 但骨架与克隆体之间不得有跨切点引用（见 hasCrossSegmentLabelRefs）。
    if (hasForbiddenPrefix(mn, cut)) return {};
    if (hasCrossSegmentLabelRefs(mn, cut)) return {};

    const std::string className = cls.classNode->name.to_std_string();
    const std::string desc      = mn->desc.to_std_string();
    const auto hash = fnv1a64(className + mn->name.to_std_string() + desc);
    const std::string genName = "__regc_init_" + hex64(hash);

    // 生成方法：static，desc = "(L<owner>;<原参数>)V"，this 显式参数化。
    std::string genDesc = "(L" + className + ";" + desc.substr(1);
    auto&  arena = index.arena();
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* gen = new MethodNode(Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE | Opcodes::ACC_SYNTHETIC,
                               arena.intern(genName.c_str()),
                               arena.intern(genDesc.c_str()), arena.intern(""), noExc);
    gen->maxStack  = mn->maxStack;
    gen->maxLocals = mn->maxLocals;

    std::unordered_map<jbytecode::core::Label*, jbytecode::core::Label*> clonedLabels;
    // javac 常态：骨架段的行号锚点 label 被克隆体段尾的 LineNumberNode 引用。
    // 克隆时为这些 pre label 建占位 label（挂在生成方法指令头部），把行号
    // 引用重定向到生成方法内的合法 label，避免悬空引用。
    // B8：占位 label 经 resolveCloneLabel 落入 gen 的内容 arena；幂等——同一
    // 原 label 重复解析命中映射直返，不会重复建档。
    {
        std::unordered_set<jbytecode::core::Label*> preLabels;
        for (auto* insn = mn->instructions.getFirst(); insn != cut->nextInsn;
             insn = insn->nextInsn) {
            if (insn->getType() == AbstractInsnNode::LABEL) {
                preLabels.insert(static_cast<LabelNode*>(insn)->label);
            }
        }
        for (auto* l : preLabels) {
            resolveCloneLabel(gen->arena(), clonedLabels, l);
        }
        // 给占位 label 提供 LabelNode 挂点：克隆体的 LineNumberNode 引用它们，
        // writer 需要这些 label 出现在指令流中才能算出合法偏移。
        for (auto& [key, lbl] : clonedLabels) {
            (void)key;
            gen->instructions.insert(gen->create<LabelNode>(lbl));
        }
    }
    // 克隆体段的 label 无需预登记：B8 clone(arena, map) 的 Label 解析统一走
    // resolveCloneLabel，未命中的原 label 就地建档并登记——向前跳（跳转先于
    // 目标 LABEL 被克隆）与目标 LABEL 克隆收敛到同一新 label，旧协议
    // 「LabelNode::clone 登记 / JumpInsnNode::clone 只查」的时序依赖消除。
    // [cut->next .. end] 克隆进生成方法。
    for (auto* insn = cut->nextInsn; insn; insn = insn->nextInsn) {
        gen->instructions.add(insn->clone(gen->arena(), clonedLabels));
    }
    // 骨架：丢弃 [cut->next .. end] 的原节点（removeAndDelete 摘链并销毁）。
    std::vector<AbstractInsnNode*> tail;
    for (auto* insn = cut->nextInsn; insn;) {
        auto* next = insn->nextInsn;
        tail.push_back(insn);
        insn = next;
    }
    for (auto* insn : tail) {
        mn->instructions.removeAndDelete(insn);
    }

    // 骨架尾部：aload_0 + 参数 load + invokestatic + return。
    mn->instructions.add(mn->create<VarInsnNode>(Opcodes::ALOAD, 0));
    // 解析参数类型：Type::getArgumentTypes(desc 参数段)。
    const auto methodType = Type::getMethodType(arena.intern(desc.c_str()));
    int        slot       = 1;
    for (const auto& argType : methodType.getArgumentTypes()) {
        mn->instructions.add(mn->create<VarInsnNode>(loadOpcode(argType.getSort()), slot));
        slot += (argType.getSort() == Type::LONG || argType.getSort() == Type::DOUBLE) ? 2 : 1;
    }
    mn->instructions.add(mn->create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                                                    arena.intern(className.c_str()),
                                                    arena.intern(genName.c_str()),
                                                    arena.intern(genDesc.c_str()), false));
    mn->instructions.add(mn->create<InsnNode>(Opcodes::RETURN));

    // 安装到类与 CompilerMethod 列表（MarkAll 将会标记生成方法）。
    cls.classNode->methods.push_back(gen);
    CompilerMethod cm;
    cm.methodNode = gen;
    cls.methods.push_back(std::move(cm));
    cls.modified = true;
    return genName;
}

/// <clinit> 搬运：整个方法体搬进 __regc_clinit_<hash>，原 <clinit> 只留
/// INVOKESTATIC 生成方法 + RETURN（RegisterNatives 注入点保留在骨架头部，
/// 注册时机仍先于生成方法的执行）。
std::string moveClinit(CompilerIndex& index, CompilerClass& cls, MethodNode* mn)
{
    if (mn->access &
        (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE)) return {};
    if (mn->instructions.isEmpty()) return {};
    // v1 保守：异常表 / 局部变量表与整段搬移冲突，放弃。
    if (!mn->tryCatchBlocks.empty() || !mn->localVariables.empty()) return {};
    if (!mn->visibleLocalVariableAnnotations.empty() ||
        !mn->invisibleLocalVariableAnnotations.empty()) {
        return {};
    }

    const auto className = cls.classNode->name.to_std_string();
    const auto  desc     = mn->desc.to_std_string();
    const auto  hash     = fnv1a64(className + "@" + mn->name.to_std_string() + desc);
    const std::string genName = "__regc_clinit_" + hex64(hash);

    auto& arena = index.arena();
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* gen = new MethodNode(Opcodes::ACC_STATIC | Opcodes::ACC_PRIVATE | Opcodes::ACC_SYNTHETIC,
                               arena.intern(genName.c_str()),
                               arena.intern(desc.c_str()), arena.intern(""), noExc);
    gen->maxStack  = mn->maxStack;
    gen->maxLocals = mn->maxLocals;

    std::unordered_map<jbytecode::core::Label*, jbytecode::core::Label*> cloned;
    // 搬走全量指令：克隆到生成方法，原方法清空重建骨架。
    // B8：label 无需预登记——clone(gen.arena, map) 各节点统一走 resolveCloneLabel，
    // 未命中的原 label 就地建档登记，向前跳与目标 LABEL 克隆收敛同一新 label
    // （幂等），旧协议「LabelNode 登记 / Jump 只查」的时序依赖消除。
    for (auto* insn = mn->instructions.getFirst(); insn; insn = insn->nextInsn)
        gen->instructions.add(insn->clone(gen->arena(), cloned));

    // 原 <clinit> 骨架只留 invokestatic + return。
    mn->instructions.clear();
    mn->instructions.add(mn->create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                                                    arena.intern(className.c_str()),
                                                    arena.intern(genName.c_str()),
                                                    arena.intern(desc.c_str()), false));
    mn->instructions.add(mn->create<InsnNode>(Opcodes::RETURN));
    // 骨架 maxLocals 保持（invokestatic 不占 local）；maxStack 1 即够。
    mn->maxStack  = std::max(mn->maxStack, 1);
    mn->maxLocals = std::max(mn->maxLocals, 0);

    cls.classNode->methods.push_back(gen);
    CompilerMethod cm;
    cm.methodNode = gen;
    cls.methods.push_back(std::move(cm));
    cls.modified = true;
    return genName;
}

} // namespace

void InitClinitMoverPreprocessor::process(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        // 类级 glob 过滤器剔除的类整体不进 native 集，不生成 __regc_* 搬运方法
        // （否则会留下永不标记的死静态方法，并无谓改变类形态）。
        if (cls.filteredOut) continue;
        // 接口同上整体跳过：接口方法禁止 ACC_NATIVE（JLS 9.4/JVMS 4.6），
        // 标记路径全部跳过接口（MarkAll 等），搬运产物只会是死静态方法。
        if (cls.classNode->access & Opcodes::ACC_INTERFACE) continue;
        // 注意：遍历的是原始方法列表；生成方法在循环末尾追加，不回遍历。
        const size_t count = cls.classNode->methods.size();
        for (size_t i = 0; i < count; ++i) {
            auto* mn = cls.classNode->methods[i];
            auto  mname = mn->name.to_std_string();
            if (mname == "<clinit>") {
                moveClinit(index, cls, mn); // clinit desc 必须 ()V
            }
            else if (mname == "<init>") {
                moveInit(index, cls, mn);
            }
        }
    }
    CA_LOGT_INFO("preprocess", "<init>/<clinit> move pass done\n");
}

} // namespace mj2x
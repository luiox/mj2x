#include "code_converter.hpp"
#include "opcode_utils.hpp"
#include "type_utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <string>
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/reader.hpp"
#include "mj2x/core/dom_payload.hpp"
#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {


// -- StackMapTable → MIRFrame 重建 --
//
// DOM 读取路径（批 3b/6d）丢了树 reader 的帧节点：SSA 的块栈参数类型只能靠
// 前驱推断，verifier 分裂块 / handler 入口退 UNKNOWN，vmc4 边拷贝编码
// fail-closed（java_corpus ArithEdges 首曝）。这里从 Code 嵌套 SMT 重建帧序：
// lower 的 relocation 已把 SMT pc 重锚到新布局（与存活指令 srcPc 同域），
// 链式 delta 还原 pc，形态标签映射 ASM 语义帧型。消费面（SSA
// stackArgTypes/appendFrameStackTypes）只读 FULL/SAME1 的栈项，APPEND/CHOP
// 无需持久栈推导。

struct DomFrame
{
    int                        frameType = 0;
    std::vector<PrimitiveKind> stackTypes;
};

static PrimitiveKind smtTagToKind(ca::u8 tag)
{
    using VT = jbc::core::StackMapTableAttr::VType;
    switch (tag) {
    case VT::kInteger: return PrimitiveKind::INT;
    case VT::kFloat: return PrimitiveKind::FLOAT;
    case VT::kLong: return PrimitiveKind::LONG;
    case VT::kDouble: return PrimitiveKind::DOUBLE;
    default: return tag == VT::kTop ? PrimitiveKind::VOID : PrimitiveKind::OBJECT;
    }
}

static std::map<ca::u32, DomFrame> framesByPc(jbc::core::ClassFile& cf,
                                              jbc::core::CodeAttr& code)
{
    using Smt = jbc::core::StackMapTableAttr;
    std::map<ca::u32, DomFrame> out;
    const Smt*                  smt = nullptr;
    for (auto& slot : code.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "StackMapTable")) continue;
        if (slot.peek() != nullptr) {
            smt = ca::core::tag_cast::cast<Smt>(slot.peek());
            break;
        }
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) return out;
        smt = ca::core::tag_cast::cast<Smt>(std::move(decoded).unwrap());
        break;
    }
    if (smt == nullptr) return out;

    // 帧链：首帧 offset 直取；后续 pc = prev + delta + 1（JVMS 4.7.4）。
    ca::u32 prevPc    = 0;
    bool    first     = true;
    for (const auto& f : smt->frames) {
        ca::u32                    delta     = 0;
        int                        frameType = Opcodes::F_SAME;
        std::vector<PrimitiveKind> stack;
        if (f.type <= Smt::Frame::kSameMax) {
            delta     = f.type;
            frameType = Opcodes::F_SAME;
        }
        else if (f.type <= Smt::Frame::kSame1Max) {
            delta     = f.type - Smt::Frame::kSame1Base;
            frameType = Opcodes::F_SAME1;
        }
        else if (f.type == Smt::Frame::kSame1Ext) {
            delta     = f.offset;
            frameType = Opcodes::F_SAME1;
        }
        else if (f.type <= Smt::Frame::kChopMin) {
            delta     = f.offset;
            frameType = Opcodes::F_CHOP;
        }
        else if (f.type == Smt::Frame::kSameExt) {
            delta     = f.offset;
            frameType = Opcodes::F_SAME;
        }
        else if (f.type <= Smt::Frame::kAppendMax) {
            delta     = f.offset;
            frameType = Opcodes::F_APPEND;
        }
        else if (f.type == Smt::Frame::kFull) {
            delta     = f.offset;
            frameType = Opcodes::F_FULL;
        }
        else {
            continue;   // 128..246 保留段：不出现于合法输入
        }
        if (frameType == Opcodes::F_FULL || frameType == Opcodes::F_SAME1) {
            for (const auto& v : f.stack) {
                const PrimitiveKind k = smtTagToKind(v.tag);
                if (k != PrimitiveKind::VOID) stack.push_back(k);
            }
        }
        const ca::u32 pc = first ? delta : prevPc + delta + 1;
        first           = false;
        prevPc          = pc;
        out.emplace(pc, DomFrame{frameType, std::move(stack)});
    }
    return out;
}


static PrimitiveKind castSourceType(int opcode)
{
    switch (opcode) {
    case Opcodes::I2L:
    case Opcodes::I2F:
    case Opcodes::I2D:
    case Opcodes::I2B:
    case Opcodes::I2C:
    case Opcodes::I2S: return PrimitiveKind::INT;
    case Opcodes::L2I:
    case Opcodes::L2F:
    case Opcodes::L2D: return PrimitiveKind::LONG;
    case Opcodes::F2I:
    case Opcodes::F2L:
    case Opcodes::F2D: return PrimitiveKind::FLOAT;
    case Opcodes::D2I:
    case Opcodes::D2L:
    case Opcodes::D2F: return PrimitiveKind::DOUBLE;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind castTargetType(int opcode)
{
    switch (opcode) {
    case Opcodes::I2L: return PrimitiveKind::LONG;
    case Opcodes::I2F: return PrimitiveKind::FLOAT;
    case Opcodes::I2D: return PrimitiveKind::DOUBLE;
    case Opcodes::L2I: return PrimitiveKind::INT;
    case Opcodes::L2F: return PrimitiveKind::FLOAT;
    case Opcodes::L2D: return PrimitiveKind::DOUBLE;
    case Opcodes::F2I: return PrimitiveKind::INT;
    case Opcodes::F2L: return PrimitiveKind::LONG;
    case Opcodes::F2D: return PrimitiveKind::DOUBLE;
    case Opcodes::D2I: return PrimitiveKind::INT;
    case Opcodes::D2L: return PrimitiveKind::LONG;
    case Opcodes::D2F: return PrimitiveKind::FLOAT;
    case Opcodes::I2B: return PrimitiveKind::BYTE;
    case Opcodes::I2C: return PrimitiveKind::CHAR;
    case Opcodes::I2S: return PrimitiveKind::SHORT;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind arrayComponentType(int opcode)
{
    switch (opcode) {
    case Opcodes::IALOAD:
    case Opcodes::IASTORE: return PrimitiveKind::INT;
    case Opcodes::LALOAD:
    case Opcodes::LASTORE: return PrimitiveKind::LONG;
    case Opcodes::FALOAD:
    case Opcodes::FASTORE: return PrimitiveKind::FLOAT;
    case Opcodes::DALOAD:
    case Opcodes::DASTORE: return PrimitiveKind::DOUBLE;
    case Opcodes::AALOAD:
    case Opcodes::AASTORE: return PrimitiveKind::OBJECT;
    case Opcodes::BALOAD:
    case Opcodes::BASTORE: return PrimitiveKind::BYTE;
    case Opcodes::CALOAD:
    case Opcodes::CASTORE: return PrimitiveKind::CHAR;
    case Opcodes::SALOAD:
    case Opcodes::SASTORE: return PrimitiveKind::SHORT;
    default: return PrimitiveKind::INT;
    }
}

static PrimitiveKind newArrayComponentType(int atype)
{
    switch (atype) {
    case Opcodes::T_BOOLEAN: return PrimitiveKind::BOOLEAN;
    case Opcodes::T_CHAR: return PrimitiveKind::CHAR;
    case Opcodes::T_FLOAT: return PrimitiveKind::FLOAT;
    case Opcodes::T_DOUBLE: return PrimitiveKind::DOUBLE;
    case Opcodes::T_BYTE: return PrimitiveKind::BYTE;
    case Opcodes::T_SHORT: return PrimitiveKind::SHORT;
    case Opcodes::T_INT: return PrimitiveKind::INT;
    case Opcodes::T_LONG: return PrimitiveKind::LONG;
    default: return PrimitiveKind::OBJECT;
    }
}

static PrimitiveKind cmpOperandType(int opcode)
{
    switch (opcode) {
    case Opcodes::LCMP: return PrimitiveKind::LONG;
    case Opcodes::FCMPL:
    case Opcodes::FCMPG: return PrimitiveKind::FLOAT;
    case Opcodes::DCMPL:
    case Opcodes::DCMPG: return PrimitiveKind::DOUBLE;
    default: return PrimitiveKind::INT;
    }
}

/// @brief CP 位模式 → 浮点值（项目 C++ 标准档无 std::bit_cast，走 memcpy）。
static float floatFromBits(ca::i32 bits)
{
    static_assert(sizeof(float) == sizeof(ca::i32), "float/i32 位宽");
    float  f = 0.0f;
    ca::i32 b = bits;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

static double doubleFromBits(ca::i64 bits)
{
    static_assert(sizeof(double) == sizeof(ca::i64), "double/i64 位宽");
    double  d = 0.0;
    ca::i64 b = bits;
    std::memcpy(&d, &b, sizeof(d));
    return d;
}

static bool isCategory2Kind(PrimitiveKind kind)
{
    return kind == PrimitiveKind::LONG || kind == PrimitiveKind::DOUBLE;
}

static PrimitiveKind methodReturnPrimitive(const ca::str::Utf8StringRef& desc)
{
    // #921 staticArena 退役：同各 backend returnKind，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    return jvmDescToPrimitive(Type::getReturnType(desc).getDescriptor(scratch));
}

static std::vector<PrimitiveKind> methodArgumentPrimitives(const ca::str::Utf8StringRef& desc)
{
    std::vector<PrimitiveKind> result;
    auto                       args = Type::getArgumentTypes(desc);
    result.reserve(args.size());
    for (const auto& arg : args) result.push_back(jvmSortToPrimitive(arg.getSort()));
    return result;
}

struct CodeConverter::Impl
{
    ca::str::Utf8StringArena*                        arena = nullptr;   ///< MIR 字符串字段入池用，借用
    std::unordered_map<int, std::unique_ptr<Block>> blocks;
    std::vector<Block*>                             blockOrder;
    Block*                                          currentBlock = nullptr;
    std::vector<ExceptionHandler>                   handlers;
    std::vector<PrimitiveKind>                      operandStack;
    std::unordered_map<int, PrimitiveKind>          localTypes;

    // 块起始指令序号集合（批 3b：序号 = DOM decodeInsns 的下标，pc 升序）。
    // 一条指令是块起始，当且仅当：(a) 它是跳转/switch/异常表目标；或 (b) 它
    // 紧跟终结指令（jump/return/athrow/switch，fallthrough 块入口）。
    std::unordered_set<int> blockStartIndices;
    int nextSyntheticBlockId = -1000;   // fallthrough 块的合成 id 发放（负数，避开正数 label id）

    // label id → 指令序号（translateInstructions 填充）。
    // 异常表保护区用指令序号表达（ExceptionHandler::startInsnIdx/endInsnIdx），
    // 与块切分解耦（见 block.hpp ExceptionHandler 注释）。
    std::unordered_map<int, int> labelToInsnIdx;

    // -- 批 3b DOM 读路径状态（每次 convert 重置） --
    jbc::core::ClassFile* cf = nullptr;   ///< CP 解析借用（convert 期间稳定）

    /// 批 6d 标签 id 化：MIR/块 id/异常表键统一用顺序 id（同 pc 恒同 id）。
    /// 不得用指针截断 int 作块 id——x64 堆地址截断可为负（ASLR 依赖），
    /// 会把 isTarget/异常表回填随机打挂。
    std::unordered_map<ca::u32, int> labelIdByPc;
    /// @brief SMT 重建帧（pc → 帧型 + 栈项）；批 6f 恢复树 reader 的帧锚定。
    std::map<ca::u32, DomFrame>      framesByPcMap;
    int                              nextLabelId = 1;

    /// @brief 目标 pc → 标签 id（批 6d 标签 id 化：同一 pc 恒同一 id）。
    int labelAt(ca::u32 pc)
    {
        auto it = labelIdByPc.find(pc);
        if (it != labelIdByPc.end()) return it->second;
        const int id = nextLabelId++;
        return labelIdByPc.emplace(pc, id).first->second;
    }

    /// @brief 目标 pc 的标签 id（未建档返回 -1；id 从 1 起，与 entry 块 id=0 不撞）。
    int labelIdAt(ca::u32 pc)
    {
        auto it = labelIdByPc.find(pc);
        return it != labelIdByPc.end() ? it->second : -1;
    }

    /// @brief DOM 方法初始状态：this 槽 + 描述符参数槽。
    void initMethodState(ca::u16 access, std::string_view desc)
    {
        operandStack.clear();
        localTypes.clear();
        int slot = 0;
        if (!(access & Opcodes::ACC_STATIC)) localTypes[slot++] = PrimitiveKind::OBJECT;
        auto m = Type::getMethodType(ca::str::Utf8StringRef::from_string_view(desc));
        for (const auto& argType : m.getArgumentTypes()) {
            auto kind        = jvmSortToPrimitive(argType.getSort());
            localTypes[slot] = kind;
            slot += primitiveStackSize(kind);
        }
    }

    // -- CP 解析助手（convert 期间只读，utf8Ref 视图稳定） --

    /// @brief Utf8 条目内容视图；索引无效即抛（畸形类文件不静默）。
    ca::str::Utf8StringRef utf8(ca::u32 idx) const
    {
        if (!cf->cp.valid(idx)) {
            throw std::invalid_argument("code_converter: bad CP utf8 index "
                                        + std::to_string(idx));
        }
        return cf->cp.utf8Ref(idx);
    }

    /// @brief CP Class 条目的内部名 Utf8 索引。
    ca::u32 utf8ClassIndex(ca::u32 classCpIdx) const
    {
        const auto& e = cf->cp.at(classCpIdx);
        if (e.tag != jbc::core::CpTag::Class) {
            throw std::invalid_argument("code_converter: CP " + std::to_string(classCpIdx)
                                        + " is not a Class entry");
        }
        return e.a;
    }

    /// @brief CP Class 条目 → 内部名（数组形态 "[..." 原样）。
    ca::str::Utf8StringRef className(ca::u32 classCpIdx) const
    {
        return utf8(utf8ClassIndex(classCpIdx));
    }

    /// @brief 解析 Fieldref/Methodref/InterfaceMethodref → owner/name/desc/itf。
    void resolveRef(ca::u32 cpIdx,
                    ca::str::Utf8StringRef& owner,
                    ca::str::Utf8StringRef& name,
                    ca::str::Utf8StringRef& desc,
                    bool&                   itf) const
    {
        const auto tag = cf->cp.tagOf(cpIdx);
        if (tag != jbc::core::CpTag::Fieldref && tag != jbc::core::CpTag::Methodref &&
            tag != jbc::core::CpTag::InterfaceMethodref) {
            throw std::invalid_argument("code_converter: CP " + std::to_string(cpIdx)
                                        + " is not a member ref");
        }
        const auto& entry = cf->cp.at(cpIdx);
        itf   = (tag == jbc::core::CpTag::InterfaceMethodref);
        owner = utf8(utf8ClassIndex(entry.a));
        const auto& nat = cf->cp.at(entry.b);
        name  = utf8(nat.a);
        desc  = utf8(nat.b);
    }

    /// @brief 解析 CP MethodHandle → 旧栈 Handle（MIR bsm 槽与树契约同型）。
    Handle resolveHandle(ca::u32 cpIdx) const
    {
        return resolveDomHandle(*cf, cpIdx, *arena);
    }

    /// @brief 解析 CP 常量为树契约同型 Any 载荷（bsmArgs 用）。
    ///
    /// 载荷类型对齐旧栈 ClassReader::readConst：Integer→i32、Float→float、
    /// Long→i64、Double→double、String→Utf8StringRef、Class/MethodType→
    /// 旧栈 Type（批 3a 实证规则：树/Any 载荷一律旧栈类型，限定名构造）、
    /// MethodHandle→旧栈 Handle。ConstantDynamic 不支持（fail loud）。
    ca::core::Any resolveConstPayload(ca::u32 cpIdx) const
    {
        return resolveDomConstPayload(*cf, cpIdx, *arena);
    }

    /// @brief 类级 BootstrapMethods 属性（INVOKEDYNAMIC 的 bsm 解析用）。
    const jbc::core::BootstrapMethodsAttr* bootstrapMethods() const
    {
        return domBootstrapMethods(const_cast<jbc::core::ClassFile&>(*cf));
    }

    void push(PrimitiveKind kind)
    {
        if (kind != PrimitiveKind::VOID) operandStack.push_back(kind);
    }

    PrimitiveKind pop()
    {
        if (operandStack.empty()) return PrimitiveKind::INT;
        auto kind = operandStack.back();
        operandStack.pop_back();
        return kind;
    }

    PrimitiveKind peek(size_t depth = 0) const
    {
        if (depth >= operandStack.size()) return PrimitiveKind::INT;
        return operandStack[operandStack.size() - 1 - depth];
    }

    std::vector<PrimitiveKind> topSnapshot(size_t limit) const
    {
        std::vector<PrimitiveKind> result;
        const size_t               count = std::min(limit, operandStack.size());
        result.reserve(count);
        for (size_t i = 0; i < count; ++i) result.push_back(peek(i));
        return result;
    }

    void resetStack(const std::vector<PrimitiveKind>& stackTypes) { operandStack = stackTypes; }
};

CodeConverter::CodeConverter(ca::str::Utf8StringArena& arena)
    : impl_(std::make_unique<Impl>())
{
    impl_->arena = &arena;
}

CodeConverter::~CodeConverter() = default;

Block* CodeConverter::getOrCreateBlock(int labelId)
{
    auto it = impl_->blocks.find(labelId);
    if (it != impl_->blocks.end()) return it->second.get();
    auto  block            = std::make_unique<Block>(labelId);
    auto* ptr              = block.get();
    impl_->blocks[labelId] = std::move(block);
    impl_->blockOrder.push_back(ptr);
    return ptr;
}

void CodeConverter::analyzeControlFlow(const std::vector<jbc::core::RawInsn>& insns,
                                       const jbc::core::CodeAttr&             code)
{
    impl_->handlers.clear();
    impl_->blockStartIndices.clear();

    // pc → 指令序号（块入口换算）。
    std::unordered_map<ca::u32, int> ordOf;
    ordOf.reserve(insns.size());
    for (size_t i = 0; i < insns.size(); ++i) ordOf.emplace(insns[i].pc, static_cast<int>(i));

    // 收集跳转/switch/异常 handler 目标（块入口）。目标 pc 建档合成标签 + 顺序 id。
    auto noteTarget = [&](ca::u32 targetPc) {
        impl_->labelAt(targetPc);
        auto it = ordOf.find(targetPc);
        if (it != ordOf.end()) impl_->blockStartIndices.insert(it->second);
    };

    for (const auto& in : insns) {
        switch (jbc::core::shapeOf(in.op)) {
        case jbc::core::Shape::Branch2:
        case jbc::core::Shape::Branch4:
            noteTarget(static_cast<ca::u32>(in.pc + in.t));
            break;
        case jbc::core::Shape::TableSwitch:
            noteTarget(static_cast<ca::u32>(in.pc + in.t));
            for (auto off : in.offs) noteTarget(static_cast<ca::u32>(in.pc + off));
            break;
        case jbc::core::Shape::LookupSwitch:
            noteTarget(static_cast<ca::u32>(in.pc + in.t));
            for (const auto& c : in.cases) noteTarget(static_cast<ca::u32>(in.pc + c.off));
            break;
        default: break;
        }
    }

    for (const auto& e : code.excTable) {
        noteTarget(e.startPc);
        noteTarget(e.endPc);
        noteTarget(e.handlerPc);
        ExceptionHandler h;
        h.startLabel   = impl_->labelIdAt(e.startPc);
        h.endLabel     = impl_->labelIdAt(e.endPc);
        h.handlerLabel = impl_->labelIdAt(e.handlerPc);
        // catchType 0 = finally（catch 一切），树路径以空串表达。
        h.catchType = e.catchType == 0
                          ? impl_->arena->intern(ca::str::Utf8StringRef())
                          : impl_->arena->intern(impl_->className(e.catchType));
        impl_->handlers.push_back(h);
    }

    // Entry point：首条指令（idx 0）天然是块入口（afterTerminator 初值 true）。

    // 计算 blockStartIndices：(a) 跳转/switch/异常目标已入集合；(b) 终结指令
    // （jump/return/athrow/switch）后的下一条指令是 fallthrough 块入口。
    bool afterTerminator = true;
    for (size_t i = 0; i < insns.size(); ++i) {
        const auto& in  = insns[i];
        const int   opc = static_cast<int>(in.op);

        if (afterTerminator) {
            impl_->blockStartIndices.insert(static_cast<int>(i));
            afterTerminator = false;
        }

        const auto shape = jbc::core::shapeOf(in.op);
        if (shape == jbc::core::Shape::Branch2 || shape == jbc::core::Shape::Branch4 ||
            shape == jbc::core::Shape::TableSwitch || shape == jbc::core::Shape::LookupSwitch) {
            afterTerminator = true;
        }
        else if (opc == Opcodes::IRETURN || opc == Opcodes::LRETURN || opc == Opcodes::FRETURN ||
                 opc == Opcodes::DRETURN || opc == Opcodes::ARETURN || opc == Opcodes::RETURN ||
                 opc == Opcodes::ATHROW) {
            afterTerminator = true;
        }
    }
}

void CodeConverter::buildBlocks()
{
    impl_->blocks.clear();
    impl_->blockOrder.clear();
    impl_->nextSyntheticBlockId = -1000;
    // 块在 translateInstructions 按 blockStartIndices 边走边建（见下）。
}

void CodeConverter::processInsn(const jbc::core::RawInsn& in)
{
    auto& cur = impl_->currentBlock;
    if (!cur) {
        cur = impl_->blockOrder.empty() ? getOrCreateBlock(0) : impl_->blockOrder[0];
    }
    int    opc      = static_cast<int>(in.op);
    ca::i32 varIndex = in.a;
    // 旧栈 reader 把 ILOAD_0..ASTORE_3 短格式解码成规范形（VarInsn op + var）；
    // DOM decodeInsns 保留原始 opcode——此处对齐规范形，家族区间判定复用原样。
    if (opc >= Opcodes::ILOAD_0 && opc <= Opcodes::ALOAD_3) {
        varIndex = (opc - Opcodes::ILOAD_0) % 4;
        opc      = Opcodes::ILOAD + (opc - Opcodes::ILOAD_0) / 4;
    }
    else if (opc >= Opcodes::ISTORE_0 && opc <= Opcodes::ASTORE_3) {
        varIndex = (opc - Opcodes::ISTORE_0) % 4;
        opc      = Opcodes::ISTORE + (opc - Opcodes::ISTORE_0) / 4;
    }
    auto* cf = impl_->cf;

    auto addStackOp = [&](MIRKind kind) {
        auto node            = std::make_unique<MIRStackOp>(kind);
        node->stackBeforeTop = impl_->topSnapshot(4);
        cur->nodes.push_back(std::move(node));

        switch (kind) {
        case MIRKind::STACK_POP: impl_->pop(); break;
        case MIRKind::STACK_POP2:
        {
            auto v1 = impl_->pop();
            if (!isCategory2Kind(v1)) impl_->pop();
            break;
        }
        case MIRKind::STACK_DUP: impl_->push(impl_->peek()); break;
        case MIRKind::STACK_DUP_X1:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            impl_->push(v1);
            impl_->push(v2);
            impl_->push(v1);
            break;
        }
        case MIRKind::STACK_DUP_X2:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            if (isCategory2Kind(v2)) {
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            else {
                auto v3 = impl_->pop();
                impl_->push(v1);
                impl_->push(v3);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                impl_->push(v1);
                impl_->push(v1);
            }
            else {
                auto v2 = impl_->pop();
                impl_->push(v2);
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2_X1:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                auto v2 = impl_->pop();
                impl_->push(v1);
                impl_->push(v2);
                impl_->push(v1);
            }
            else {
                auto v2 = impl_->pop();
                auto v3 = impl_->pop();
                impl_->push(v2);
                impl_->push(v1);
                impl_->push(v3);
                impl_->push(v2);
                impl_->push(v1);
            }
            break;
        }
        case MIRKind::STACK_DUP2_X2:
        {
            auto v1 = impl_->pop();
            if (isCategory2Kind(v1)) {
                auto v2 = impl_->pop();
                if (isCategory2Kind(v2)) {
                    impl_->push(v1);
                    impl_->push(v2);
                    impl_->push(v1);
                }
                else {
                    auto v3 = impl_->pop();
                    impl_->push(v1);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
            }
            else {
                auto v2 = impl_->pop();
                auto v3 = impl_->pop();
                if (isCategory2Kind(v3)) {
                    impl_->push(v2);
                    impl_->push(v1);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
                else {
                    auto v4 = impl_->pop();
                    impl_->push(v2);
                    impl_->push(v1);
                    impl_->push(v4);
                    impl_->push(v3);
                    impl_->push(v2);
                    impl_->push(v1);
                }
            }
            break;
        }
        case MIRKind::STACK_SWAP:
        {
            auto v1 = impl_->pop();
            auto v2 = impl_->pop();
            impl_->push(v1);
            impl_->push(v2);
            break;
        }
        default: break;
        }
    };

    // -- 常量族（无操作数 / 立即数） --
    if (opc == Opcodes::NOP) {
        cur->nodes.push_back(std::make_unique<MIRNop>());
    }
    else if (opc == Opcodes::ACONST_NULL) {
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::OBJECT;
        node->value = nullptr;
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    else if (opc >= Opcodes::ICONST_M1 && opc <= Opcodes::ICONST_5) {
        // ICONST_M1..ICONST_5：int 常量 -1..5
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::INT;
        node->value = static_cast<int64_t>(opc - Opcodes::ICONST_0);
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    else if (opc == Opcodes::LCONST_0 || opc == Opcodes::LCONST_1) {
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::LONG;
        node->value = static_cast<int64_t>(opc - Opcodes::LCONST_0);
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    else if (opc == Opcodes::FCONST_0 || opc == Opcodes::FCONST_1 || opc == Opcodes::FCONST_2) {
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::FLOAT;
        node->value = static_cast<double>(opc - Opcodes::FCONST_0);
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    else if (opc == Opcodes::DCONST_0 || opc == Opcodes::DCONST_1) {
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::DOUBLE;
        node->value = static_cast<double>(opc - Opcodes::DCONST_0);
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    else if (opc == Opcodes::BIPUSH || opc == Opcodes::SIPUSH) {
        auto node   = std::make_unique<MIRConstant>();
        node->type  = PrimitiveKind::INT;
        node->value = static_cast<int64_t>(in.a);
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    // -- 数组 load/store --
    else if (opc >= Opcodes::IALOAD && opc <= Opcodes::SALOAD) {
        auto node           = std::make_unique<MIRArrayLoad>();
        node->componentType = arrayComponentType(opc);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->pop();
        impl_->push(arrayComponentType(opc));
    }
    else if (opc >= Opcodes::IASTORE && opc <= Opcodes::SASTORE) {
        auto node           = std::make_unique<MIRArrayStore>();
        node->componentType = arrayComponentType(opc);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->pop();
        impl_->pop();
    }
    // -- 栈操作族 --
    else if (opc == Opcodes::POP) addStackOp(MIRKind::STACK_POP);
    else if (opc == Opcodes::POP2) addStackOp(MIRKind::STACK_POP2);
    else if (opc == Opcodes::DUP) addStackOp(MIRKind::STACK_DUP);
    else if (opc == Opcodes::DUP_X1) addStackOp(MIRKind::STACK_DUP_X1);
    else if (opc == Opcodes::DUP_X2) addStackOp(MIRKind::STACK_DUP_X2);
    else if (opc == Opcodes::DUP2) addStackOp(MIRKind::STACK_DUP2);
    else if (opc == Opcodes::DUP2_X1) addStackOp(MIRKind::STACK_DUP2_X1);
    else if (opc == Opcodes::DUP2_X2) addStackOp(MIRKind::STACK_DUP2_X2);
    else if (opc == Opcodes::SWAP) addStackOp(MIRKind::STACK_SWAP);
    // -- 二元/一元/转换/比较 --
    else if ((opc >= Opcodes::IADD && opc <= Opcodes::DREM) ||
             (opc >= Opcodes::ISHL && opc <= Opcodes::LXOR)) {
        auto node  = std::make_unique<MIRBinaryOp>();
        node->op   = opcodeToBinaryOp(opc);
        node->type = opcodeToResultType(opc);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->pop();
        impl_->push(opcodeToResultType(opc));
    }
    else if (opc >= Opcodes::INEG && opc <= Opcodes::DNEG) {
        auto node  = std::make_unique<MIRUnaryOp>();
        node->op   = opcodeToUnaryOp(opc);
        node->type = opcodeToResultType(opc);
        cur->nodes.push_back(std::move(node));
    }
    else if ((opc >= Opcodes::I2L && opc <= Opcodes::D2F) || opc == Opcodes::I2B ||
             opc == Opcodes::I2C || opc == Opcodes::I2S) {
        auto node                = std::make_unique<MIRCast>();
        node->from               = castSourceType(opc);
        node->to                 = castTargetType(opc);
        PrimitiveKind targetType = node->to;
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->push(targetType);
    }
    else if (opc == Opcodes::LCMP || opc == Opcodes::FCMPL || opc == Opcodes::FCMPG ||
             opc == Opcodes::DCMPL || opc == Opcodes::DCMPG) {
        auto node        = std::make_unique<MIRCmp>();
        node->type       = cmpOperandType(opc);
        node->nanGreater = (opc == Opcodes::FCMPG || opc == Opcodes::DCMPG);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->pop();
        impl_->push(PrimitiveKind::INT);
    }
    // -- 返回/杂项 --
    else if (opc >= Opcodes::IRETURN && opc <= Opcodes::ARETURN) {
        auto node = std::make_unique<MIRReturnValue>();
        // 按 *RETURN opcode 推导返回值类型（与方法描述符一致）。
        auto retKind = [](int op) -> PrimitiveKind {
            switch (op) {
            case Opcodes::IRETURN: return PrimitiveKind::INT;
            case Opcodes::LRETURN: return PrimitiveKind::LONG;
            case Opcodes::FRETURN: return PrimitiveKind::FLOAT;
            case Opcodes::DRETURN: return PrimitiveKind::DOUBLE;
            case Opcodes::ARETURN: return PrimitiveKind::OBJECT;
            default: return PrimitiveKind::INT;
            }
        };
        node->type = retKind(opc);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
    }
    else if (opc == Opcodes::RETURN) {
        cur->nodes.push_back(std::make_unique<MIRReturn>());
    }
    else if (opc == Opcodes::ARRAYLENGTH) {
        cur->nodes.push_back(std::make_unique<MIRArrayLength>());
        impl_->pop();
        impl_->push(PrimitiveKind::INT);
    }
    else if (opc == Opcodes::ATHROW) {
        cur->nodes.push_back(std::make_unique<MIRThrow>());
        impl_->pop();
    }
    else if (opc == Opcodes::MONITORENTER) {
        auto node = std::make_unique<MIRMonitor>();   // enter=true default
        cur->nodes.push_back(std::move(node));
        impl_->pop();
    }
    else if (opc == Opcodes::MONITOREXIT) {
        auto node   = std::make_unique<MIRMonitor>();
        node->enter = false;
        cur->nodes.push_back(std::move(node));
        impl_->pop();
    }
    // -- NEWARRAY --
    else if (opc == Opcodes::NEWARRAY) {
        auto node           = std::make_unique<MIRNewArray>();
        node->componentType = newArrayComponentType(in.a);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->push(PrimitiveKind::OBJECT);
    }
    // -- 局部变量族 --
    else if (opc >= Opcodes::ILOAD && opc <= Opcodes::ALOAD) {
        // 局部变量类型直接由 opcode 推导。
        auto varKind = [](int op) -> PrimitiveKind {
            switch (op) {
            case Opcodes::ILOAD: return PrimitiveKind::INT;
            case Opcodes::LLOAD: return PrimitiveKind::LONG;
            case Opcodes::FLOAD: return PrimitiveKind::FLOAT;
            case Opcodes::DLOAD: return PrimitiveKind::DOUBLE;
            case Opcodes::ALOAD: return PrimitiveKind::OBJECT;
            default: return PrimitiveKind::INT;
            }
        };
        auto node      = std::make_unique<MIRLocalLoad>();
        node->varIndex = varIndex;
        node->type     = varKind(opc);
        cur->nodes.push_back(std::move(node));
        impl_->push(varKind(opc));
    }
    else if (opc >= Opcodes::ISTORE && opc <= Opcodes::ASTORE) {
        auto varKind = [](int op) -> PrimitiveKind {
            switch (op) {
            case Opcodes::ISTORE: return PrimitiveKind::INT;
            case Opcodes::LSTORE: return PrimitiveKind::LONG;
            case Opcodes::FSTORE: return PrimitiveKind::FLOAT;
            case Opcodes::DSTORE: return PrimitiveKind::DOUBLE;
            case Opcodes::ASTORE: return PrimitiveKind::OBJECT;
            default: return PrimitiveKind::INT;
            }
        };
        auto node      = std::make_unique<MIRLocalStore>();
        node->varIndex = varIndex;
        node->type     = varKind(opc);
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->localTypes[varIndex] = varKind(opc);
    }
    else if (opc == Opcodes::RET) {
        // JSR/RET subroutines are outside the MVP bytecode surface.  Keep
        // an explicit unsupported node so backends fail closed instead of
        // silently dropping the instruction as metadata.
        auto node  = std::make_unique<MIRInvokeDynamic>();
        node->name = impl_->arena->intern("ret");
        node->desc = impl_->arena->intern("()V");
        cur->nodes.push_back(std::move(node));
    }
    // -- 类型族（CP Class） --
    else if (opc == Opcodes::NEW) {
        auto node      = std::make_unique<MIRNewObject>();
        node->typeDesc = impl_->arena->intern(impl_->className(in.cp));
        cur->nodes.push_back(std::move(node));
        impl_->push(PrimitiveKind::OBJECT);
    }
    else if (opc == Opcodes::ANEWARRAY) {
        auto node           = std::make_unique<MIRNewArray>();
        node->typeDesc      = impl_->arena->intern(impl_->className(in.cp));
        node->componentType = PrimitiveKind::OBJECT;
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->push(PrimitiveKind::OBJECT);
    }
    else if (opc == Opcodes::CHECKCAST) {
        auto node      = std::make_unique<MIRCheckCast>();
        node->typeDesc = impl_->arena->intern(impl_->className(in.cp));
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->push(PrimitiveKind::OBJECT);
    }
    else if (opc == Opcodes::INSTANCEOF) {
        auto node      = std::make_unique<MIRInstanceOf>();
        node->typeDesc = impl_->arena->intern(impl_->className(in.cp));
        cur->nodes.push_back(std::move(node));
        impl_->pop();
        impl_->push(PrimitiveKind::INT);
    }
    // -- 字段族（CP 成员引用） --
    else if (opc == Opcodes::GETSTATIC || opc == Opcodes::PUTSTATIC || opc == Opcodes::GETFIELD ||
             opc == Opcodes::PUTFIELD) {
        ca::str::Utf8StringRef owner, name, desc;
        bool                   itf = false;
        impl_->resolveRef(in.cp, owner, name, desc, itf);
        std::unique_ptr<MIRNode> node;
        if (opc == Opcodes::GETSTATIC) {
            auto n   = std::make_unique<MIRStaticGet>();
            n->owner = impl_->arena->intern(owner);
            n->name  = impl_->arena->intern(name);
            n->desc  = impl_->arena->intern(desc);
            impl_->push(jvmDescToPrimitive(n->desc));
            node = std::move(n);
        }
        else if (opc == Opcodes::PUTSTATIC) {
            auto n   = std::make_unique<MIRStaticPut>();
            n->owner = impl_->arena->intern(owner);
            n->name  = impl_->arena->intern(name);
            n->desc  = impl_->arena->intern(desc);
            impl_->pop();
            node = std::move(n);
        }
        else if (opc == Opcodes::GETFIELD) {
            auto n   = std::make_unique<MIRFieldGet>();
            n->owner = impl_->arena->intern(owner);
            n->name  = impl_->arena->intern(name);
            n->desc  = impl_->arena->intern(desc);
            impl_->pop();
            impl_->push(jvmDescToPrimitive(n->desc));
            node = std::move(n);
        }
        else {
            auto n   = std::make_unique<MIRFieldPut>();
            n->owner = impl_->arena->intern(owner);
            n->name  = impl_->arena->intern(name);
            n->desc  = impl_->arena->intern(desc);
            impl_->pop();
            impl_->pop();
            node = std::move(n);
        }
        cur->nodes.push_back(std::move(node));
    }
    // -- 方法调用族（CP 成员引用） --
    else if (opc == Opcodes::INVOKEVIRTUAL || opc == Opcodes::INVOKESPECIAL ||
             opc == Opcodes::INVOKESTATIC || opc == Opcodes::INVOKEINTERFACE) {
        ca::str::Utf8StringRef owner, name, desc;
        bool                   itf = false;
        impl_->resolveRef(in.cp, owner, name, desc, itf);
        auto node         = std::make_unique<MIRInvoke>();
        node->owner       = impl_->arena->intern(owner);
        node->name        = impl_->arena->intern(name);
        node->desc        = impl_->arena->intern(desc);
        node->isInterface = itf;
        node->invokeOpcode = opc;
        auto args          = methodArgumentPrimitives(node->desc);
        for (size_t i = 0; i < args.size(); ++i) impl_->pop();
        if (opc != Opcodes::INVOKESTATIC) impl_->pop();
        impl_->push(methodReturnPrimitive(node->desc));
        cur->nodes.push_back(std::move(node));
    }
    // -- invokedynamic（CP InvokeDynamic + 类级 BootstrapMethods） --
    else if (opc == Opcodes::INVOKEDYNAMIC) {
        const auto& entry = cf->cp.at(in.cp);
        if (entry.tag != jbc::core::CpTag::InvokeDynamic) {
            throw std::invalid_argument("code_converter: CP " + std::to_string(in.cp)
                                        + " is not InvokeDynamic");
        }
        const auto& nat = cf->cp.at(entry.b);
        auto        node = std::make_unique<MIRInvokeDynamic>();
        node->name      = impl_->arena->intern(impl_->utf8(nat.a));
        node->desc      = impl_->arena->intern(impl_->utf8(nat.b));
        const auto* bsmAttr = impl_->bootstrapMethods();
        if (bsmAttr == nullptr || entry.a >= bsmAttr->entries.size()) {
            throw std::invalid_argument("code_converter: INDY bootstrap method "
                                        + std::to_string(entry.a) + " not resolvable");
        }
        const auto& be = bsmAttr->entries[entry.a];
        node->bsm      = impl_->resolveHandle(be.methodRef);
        for (ca::u32 argCp : be.args) node->bsmArgs.push_back(impl_->resolveConstPayload(argCp));
        auto args = methodArgumentPrimitives(node->desc);
        for (size_t i = 0; i < args.size(); ++i) impl_->pop();
        impl_->push(methodReturnPrimitive(node->desc));
        cur->nodes.push_back(std::move(node));
    }
    // -- 分支族（Branch2/Branch4，目标 = pc + 相对偏移） --
    else if (jbc::core::shapeOf(in.op) == jbc::core::Shape::Branch2 ||
             jbc::core::shapeOf(in.op) == jbc::core::Shape::Branch4) {
        const int target = impl_->labelAt(static_cast<ca::u32>(in.pc + in.t));

        if (opc == Opcodes::GOTO || opc == Opcodes::GOTO_W) {
            auto node         = std::make_unique<MIRUncondBranch>();
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
        }
        else if (opc == Opcodes::IF_ACMPEQ || opc == Opcodes::IF_ACMPNE) {
            auto node         = std::make_unique<MIRBranch>();
            node->op          = branchOpcodeToCmpOp(opc);
            node->type        = PrimitiveKind::OBJECT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
        }
        else if (opc >= Opcodes::IF_ICMPEQ && opc <= Opcodes::IF_ICMPLE) {
            auto node         = std::make_unique<MIRBranch>();
            node->op          = branchOpcodeToCmpOp(opc);
            node->type        = PrimitiveKind::INT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
            impl_->pop();
        }
        else if (opc == Opcodes::IFNULL || opc == Opcodes::IFNONNULL ||
                 (opc >= Opcodes::IFEQ && opc <= Opcodes::IFLE)) {
            auto node         = std::make_unique<MIRBranchZero>();
            node->op          = branchZeroOpcodeToCmpOp(opc);
            node->type        = (opc == Opcodes::IFNULL || opc == Opcodes::IFNONNULL)
                                    ? PrimitiveKind::OBJECT
                                    : PrimitiveKind::INT;
            node->targetLabel = target;
            cur->nodes.push_back(std::move(node));
            impl_->pop();
        }
        else {
            // JSR/JSR_W subroutines are unsupported for the MVP.  Route them
            // through the same explicit fail-closed boundary as invokedynamic.
            auto node  = std::make_unique<MIRInvokeDynamic>();
            node->name = impl_->arena->intern("jsr");
            node->desc = impl_->arena->intern("()V");
            cur->nodes.push_back(std::move(node));
        }
    }
    // -- LDC 族（CP 常量载荷） --
    else if (opc == Opcodes::LDC || opc == Opcodes::LDC_W || opc == Opcodes::LDC2_W) {
        // LDC 常量类型（按 CP tag 分派，对齐树路径 readConst 载荷口径）：
        //   Integer → i32, Float → float, Long → i64, Double → double,
        //   String → Utf8StringRef, Class/MethodType → 门面 Type,
        //   MethodHandle/Dynamic → fail-closed marker。
        // Wave 4：String LDC 保留字面量，由 StackC 生成 NewStringUTF。
        const auto& entry = cf->cp.at(in.cp);
        auto        node  = std::make_unique<MIRConstant>();
        switch (entry.tag) {
        case jbc::core::CpTag::Integer:
            node->type  = PrimitiveKind::INT;
            node->value = static_cast<int64_t>(entry.ival);
            break;
        case jbc::core::CpTag::Long:
            node->type  = PrimitiveKind::LONG;
            node->value = static_cast<int64_t>(entry.lval);
            break;
        case jbc::core::CpTag::Float:
            node->type  = PrimitiveKind::FLOAT;
            node->value = static_cast<double>(floatFromBits(entry.ival));
            break;
        case jbc::core::CpTag::Double:
            node->type  = PrimitiveKind::DOUBLE;
            node->value = doubleFromBits(entry.lval);
            break;
        case jbc::core::CpTag::String:
            // CP 内容恒为 modified UTF-8：intern 校验对孤代理对/C0 80 返回空视图
            // （TextUnicode 代理对常量整条变空首曝），一律 intern_raw（jbc addUtf8
            // 同口径）。
            node->type  = PrimitiveKind::OBJECT;
            node->value = impl_->arena->intern_raw(impl_->utf8(entry.a).data(),
                                                   impl_->utf8(entry.a).byte_length());
            break;
        case jbc::core::CpTag::Class:
            node->type  = PrimitiveKind::OBJECT;
            node->value = Type::getObjectType(impl_->arena->intern_raw(
                impl_->utf8(entry.a).data(), impl_->utf8(entry.a).byte_length()));
            break;
        case jbc::core::CpTag::MethodType:
            node->type  = PrimitiveKind::OBJECT;
            node->value = Type::getMethodType(impl_->arena->intern_raw(
                impl_->utf8(entry.a).data(), impl_->utf8(entry.a).byte_length()));
            break;
        default:
            // LDC Handle / ConstantDynamic：不能静默翻成 null（行为悄悄漂移），
            // 按 JSR/RET 先例落显式 unsupported 节点，后端 fail-closed
            //（StackC 运行期抛 UnsupportedOperationException）。
        {
            auto marker  = std::make_unique<MIRInvokeDynamic>();
            marker->name = impl_->arena->intern("ldc-constant");
            marker->desc = impl_->arena->intern("()Ljava/lang/Object;");
            cur->nodes.push_back(std::move(marker));
            impl_->push(PrimitiveKind::OBJECT);
            return;
        }
        }
        impl_->push(node->type);
        cur->nodes.push_back(std::move(node));
    }
    // -- IINC --
    else if (opc == Opcodes::IINC) {
        auto node       = std::make_unique<MIRIinc>();
        node->varIndex  = in.a;
        node->increment = in.b;
        cur->nodes.push_back(std::move(node));
    }
    // -- switch 族 --
    else if (jbc::core::shapeOf(in.op) == jbc::core::Shape::TableSwitch) {
        auto node          = std::make_unique<MIRSwitch>();
        node->defaultLabel = impl_->labelAt(static_cast<ca::u32>(in.pc + in.t));
        int key            = in.a;
        for (auto off : in.offs) {
            node->cases.push_back({key++, impl_->labelAt(static_cast<ca::u32>(in.pc + off))});
        }
        cur->nodes.push_back(std::move(node));
        impl_->pop();
    }
    else if (jbc::core::shapeOf(in.op) == jbc::core::Shape::LookupSwitch) {
        auto node          = std::make_unique<MIRSwitch>();
        node->defaultLabel = impl_->labelAt(static_cast<ca::u32>(in.pc + in.t));
        for (const auto& c : in.cases) {
            node->cases.push_back({c.key, impl_->labelAt(static_cast<ca::u32>(in.pc + c.off))});
        }
        cur->nodes.push_back(std::move(node));
        impl_->pop();
    }
    // -- 多维数组 --
    else if (opc == Opcodes::MULTIANEWARRAY) {
        // Keep the bytecode compact: downstream SSA/RegC lowers this to the
        // multi-array runtime helper instead of expanding nested loops here.
        auto node      = std::make_unique<MIRMultiNewArray>();
        node->typeDesc = impl_->arena->intern(impl_->className(in.cp));
        node->dims     = in.a;
        cur->nodes.push_back(std::move(node));
        for (int i = 0; i < in.a; ++i) impl_->pop();
        impl_->push(PrimitiveKind::OBJECT);
    }
    // 其余：无 Frame/LINE 伪指令（DOM 指令序列只有真指令）；JSR_W/GOTO_W 已并入分支族
}

void CodeConverter::translateInstructions(const std::vector<jbc::core::RawInsn>& insns)
{
    impl_->currentBlock = nullptr;

    bool firstBlock = true;   // 序列首个块固定 id=0（保持现有契约：entry block id==0）
    for (size_t idx = 0; idx < insns.size(); ++idx) {
        const auto& in = insns[idx];

        // 目标 pc 的合成标签（analyzeControlFlow 已为全部目标建档）。
        // labelIdByPc 命中即目标（仅 noteTarget/分支建档时插入）；id 顺序发放，
        // 与 entry 块 id=0、合成负 id 空间互斥。
        const int labelIdHere = impl_->labelIdAt(in.pc);
        const bool isTarget   = labelIdHere >= 0;

        // 块入口处切换/创建 currentBlock。块 id：
        //   - 序列首个块 → 0（entry 契约）
        //   - 目标块 → 顺序标签 id
        //   - fallthrough 块 → 合成负 id
        if (impl_->blockStartIndices.count(static_cast<int>(idx))) {
            int blockId;
            if (firstBlock) {
                blockId    = 0;
                firstBlock = false;
            }
            else if (isTarget) {
                blockId = labelIdHere;
            }
            else {
                blockId = impl_->nextSyntheticBlockId--;
            }
            impl_->currentBlock = getOrCreateBlock(blockId);
            // 每个跳转/异常目标产 MIRLabel（stackc labelToIndex 与 ssa
            // blockContainsLabel 的块内标签消费面）。
            if (isTarget) {
                impl_->currentBlock->nodes.push_back(std::make_unique<MIRLabel>(labelIdHere));
            }
        }

        if (!impl_->currentBlock) impl_->currentBlock = getOrCreateBlock(0);

        // 帧锚定：SMT 帧落在本 pc 的块首（lower 重锚后与 srcPc 同域）。树 reader
        // 时代的帧节点消费面（SSA 块栈参数类型）依赖此锚定。
        if (auto fit = impl_->framesByPcMap.find(in.pc); fit != impl_->framesByPcMap.end()) {
            auto node        = std::make_unique<MIRFrame>();
            node->frameType  = fit->second.frameType;
            node->stackTypes = fit->second.stackTypes;
            impl_->currentBlock->nodes.push_back(std::move(node));
        }

        // 维护块内指令序号范围。
        if (impl_->currentBlock->firstInsnIdx < 0) {
            impl_->currentBlock->firstInsnIdx = static_cast<int>(idx);
        }
        impl_->currentBlock->lastInsnIdx = static_cast<int>(idx);

        processInsn(in);

        // 记录目标 pc 的指令序号（异常表保护区用）。
        if (isTarget) impl_->labelToInsnIdx[labelIdHere] = static_cast<int>(idx);
    }
}

std::vector<Block*> CodeConverter::convert(jbc::core::ClassFile& cf, ca::usize methodIndex)
{
    if (methodIndex >= cf.methods.size()) {
        throw std::invalid_argument("code_converter: method index out of range");
    }
    auto& mi = cf.methods[methodIndex];

    // Code 属性 lazy 解码（缓存落在 DOM 上）；native/abstract 方法无 Code。
    auto codeRes = cf.decodeCode(mi);
    if (codeRes.is_err()) {
        throw std::invalid_argument("code_converter: decodeCode failed");
    }
    auto* code = std::move(codeRes).unwrap();

    // 指令解码：字节格式知识 → RawInsn 序列（pc 升序）。
    std::vector<jbc::core::RawInsn> insns;
    if (code != nullptr && !code->code.empty()) {
        auto insnsRes =
            jbc::core::decodeInsns(jbc::core::ByteView{code->code.data(), code->code.size()});
        if (insnsRes.is_err()) {
            throw std::invalid_argument("code_converter: decodeInsns failed");
        }
        insns = std::move(insnsRes).unwrap();
    }

    impl_->cf         = &cf;
    impl_->labelIdByPc.clear();
    impl_->framesByPcMap = (code != nullptr) ? framesByPc(cf, *code) : std::map<ca::u32, DomFrame>();
    impl_->initMethodState(mi.access, cf.descOf(mi));
    if (code != nullptr) analyzeControlFlow(insns, *code);
    buildBlocks();
    translateInstructions(insns);
    // 兜底：空方法（无指令）也要有 entry block（保持 id==0 契约）。
    if (impl_->blockOrder.empty()) {
        getOrCreateBlock(0);
    }
    if (!impl_->blockOrder.empty() && code != nullptr) {
        // 回填异常表保护区的指令序号（labelToInsnIdx 在 translateInstructions 填充）。
        for (auto& handler : impl_->handlers) {
            auto startIt = impl_->labelToInsnIdx.find(handler.startLabel);
            auto endIt   = impl_->labelToInsnIdx.find(handler.endLabel);
            if (startIt != impl_->labelToInsnIdx.end()) handler.startInsnIdx = startIt->second;
            if (endIt != impl_->labelToInsnIdx.end()) handler.endInsnIdx = endIt->second;
        }
        impl_->blockOrder.front()->exceptionTable = impl_->handlers;
    }
    return impl_->blockOrder;
}


// -- MIR dump --

static const char* mirKindName(MIRKind k)
{
    switch (k) {
    case MIRKind::NOP: return "NOP";
    case MIRKind::LABEL: return "LABEL";
    case MIRKind::CONSTANT: return "CONST";
    case MIRKind::LOCAL_LOAD: return "LOAD";
    case MIRKind::LOCAL_STORE: return "STORE";
    case MIRKind::IINC: return "IINC";
    case MIRKind::STACK_DUP: return "DUP";
    case MIRKind::STACK_DUP_X1: return "DUP_X1";
    case MIRKind::STACK_DUP_X2: return "DUP_X2";
    case MIRKind::STACK_DUP2: return "DUP2";
    case MIRKind::STACK_DUP2_X1: return "DUP2_X1";
    case MIRKind::STACK_DUP2_X2: return "DUP2_X2";
    case MIRKind::STACK_SWAP: return "SWAP";
    case MIRKind::STACK_POP: return "POP";
    case MIRKind::STACK_POP2: return "POP2";
    case MIRKind::BINARY_OP: return "BINARY";
    case MIRKind::UNARY_OP: return "UNARY";
    case MIRKind::CMP: return "CMP";
    case MIRKind::CAST: return "CAST";
    case MIRKind::BRANCH: return "BRANCH";
    case MIRKind::BRANCH_ZERO: return "BRANCH_ZERO";
    case MIRKind::UNCOND_BRANCH: return "GOTO";
    case MIRKind::SWITCH: return "SWITCH";
    case MIRKind::RETURN: return "RETURN";
    case MIRKind::RETURN_VALUE: return "RETURN_VALUE";
    case MIRKind::NEW_OBJECT: return "NEW";
    case MIRKind::NEW_ARRAY: return "NEW_ARRAY";
    case MIRKind::ARRAY_LENGTH: return "ARRAYLEN";
    case MIRKind::ARRAY_LOAD: return "ALOAD";
    case MIRKind::ARRAY_STORE: return "ASTORE";
    case MIRKind::FIELD_GET: return "GETFIELD";
    case MIRKind::FIELD_PUT: return "PUTFIELD";
    case MIRKind::STATIC_GET: return "GETSTATIC";
    case MIRKind::STATIC_PUT: return "PUTSTATIC";
    case MIRKind::INVOKE: return "INVOKE";
    case MIRKind::CHECK_CAST: return "CHECKCAST";
    case MIRKind::INSTANCE_OF: return "INSTANCEOF";
    case MIRKind::MONITOR_ENTER: return "MONITOR_ENTER";
    case MIRKind::THROW: return "ATHROW";
    case MIRKind::FRAME: return "FRAME";
    default: return "???";
    }
}

static const char* primitiveKindName(PrimitiveKind kind)
{
    switch (kind) {
    case PrimitiveKind::BOOLEAN: return "boolean";
    case PrimitiveKind::BYTE: return "byte";
    case PrimitiveKind::CHAR: return "char";
    case PrimitiveKind::SHORT: return "short";
    case PrimitiveKind::INT: return "int";
    case PrimitiveKind::LONG: return "long";
    case PrimitiveKind::FLOAT: return "float";
    case PrimitiveKind::DOUBLE: return "double";
    case PrimitiveKind::VOID: return "void";
    case PrimitiveKind::OBJECT: return "object";
    }
    return "?";
}

std::string CodeConverter::dumpMIR(const std::vector<Block*>& blocks)
{
    std::ostringstream oss;
    for (auto* blk : blocks) {
        oss << "B" << blk->id << ":\n";
        for (auto& n : blk->nodes) {
            oss << "  " << mirKindName(n->kind);
            switch (n->kind) {
            case MIRKind::LABEL:
                oss << " L" << static_cast<MIRLabel*>(n.get())->labelId;
                break;
            case MIRKind::LOCAL_LOAD:
                oss << " v" << static_cast<MIRLocalLoad*>(n.get())->varIndex;
                break;
            case MIRKind::LOCAL_STORE:
                oss << " v" << static_cast<MIRLocalStore*>(n.get())->varIndex;
                break;
            case MIRKind::IINC: oss << " v" << static_cast<MIRIinc*>(n.get())->varIndex; break;
            case MIRKind::FRAME:
            {
                auto* frame = static_cast<MIRFrame*>(n.get());
                oss << " type=" << frame->frameType << " stack=[";
                for (size_t i = 0; i < frame->stackTypes.size(); ++i) {
                    if (i) oss << ",";
                    oss << primitiveKindName(frame->stackTypes[i]);
                }
                oss << "]";
                break;
            }
            case MIRKind::BRANCH:
                oss << " -> L" << static_cast<MIRBranch*>(n.get())->targetLabel;
                break;
            case MIRKind::BRANCH_ZERO:
                oss << " -> L" << static_cast<MIRBranchZero*>(n.get())->targetLabel;
                break;
            case MIRKind::UNCOND_BRANCH:
                oss << " -> L" << static_cast<MIRUncondBranch*>(n.get())->targetLabel;
                break;
            case MIRKind::NEW_OBJECT:
                oss << " " << static_cast<MIRNewObject*>(n.get())->typeDesc;
                break;
            case MIRKind::FIELD_GET:
            case MIRKind::FIELD_PUT: oss << " " << static_cast<MIRFieldGet*>(n.get())->name; break;
            case MIRKind::INVOKE: oss << " " << static_cast<MIRInvoke*>(n.get())->name; break;
            default: break;
            }
            oss << "\n";
        }
    }
    return oss.str();
}

}   // namespace mj2x

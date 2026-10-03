/// @file llvm_ir_emitter.cpp
/// @brief LLVM IR 后端发射器：RegIR 函数 → textual LLVM IR（含异常分发）。
#include "llvm_ir_internal.hpp"
#include "llvm_intrinsic_registry.hpp"

#include <cmath>
#include <iomanip>
#include <type_traits>
#include <variant>

namespace mj2x {
namespace llvmir {

using namespace jbytecode::core;
using namespace jbytecode::tree;

class LlvmFunctionEmitter
{
public:
    LlvmFunctionEmitter(const RegFunction& function, const RegisterTypes& registerTypes,
                        const std::string& functionName, SSAType returnType,
                        InternalCallResolver* resolver, GlobalStringTable& strings,
                        size_t indySiteBase,
                        const codegen::RuntimeProtectionOptions* runtimeProtection,
                        uint32_t* checkpointCounter)
        : function_(function)
        , registerTypes_(registerTypes)
        , functionName_(functionName)
        , returnType_(returnType)
        , resolver_(resolver)
        , strings_(strings)
        , indySiteBase_(indySiteBase)
        , runtimeProtection_(runtimeProtection)
        , checkpointCounter_(checkpointCounter)
    {}

    bool emit(std::string& source, std::string& reason)
    {
        const char* returnLlvmType = llvmType(returnType_);
        if (returnLlvmType == nullptr || function_.entry == nullptr) {
            reason = "LLVM IR function has no supported return type or entry block";
            return false;
        }

        out_ << "define internal " << returnLlvmType << " @" << functionName_ << "(ptr %env";
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            out_ << ", " << llvmTypeWide(function_.parameters[index].type) << " %p" << index;
        }
        out_ << ") {\nentry:\n";
        // 包装类 shadow 逃逸分析（valueOf 结果仅被匹配拆箱使用时才直调 mj2x-rt
        // 值结构；任何逃逸 → 整根回退 JNI，语义对齐 RegC regc_intrinsics.cpp）。
        analyzeWrapperShadows();
        // Random 状态机结构定义（模块级；槽分配在其后）。
        for (const auto& [id, type] : nativeSlots_) {
            if (type == "%mj2x_random") {
                runtimeDeclares_.push_back("%mj2x_random = type { i64 }");
                break;
            }
        }
        // Builder 上下文（模块级类型 + crt 上下文初始化；调用方为 %env）。
        if (!nativeBuilders_.empty()) {
            usesBuilder_ = true;
            runtimeDeclares_.push_back("%mj2x_regc_context = type { ptr, ptr }");
            runtimeDeclares_.push_back(
                "declare void @mj2x_regc_init_context(ptr, ptr, ptr)");
            runtimeDeclares_.push_back(
                "declare void @mj2x_regc_throw_exception(ptr, ptr, ptr)");
            out_ << "  " << builderContext_ << " = alloca %mj2x_regc_context\n";
            out_ << "  call void @mj2x_regc_init_context(ptr " << builderContext_
                 << ", ptr %env, ptr @mj2x_regc_throw_exception)\n";
        }
        for (const auto& [id, type] : registerTypes_) {
            out_ << "  %slot_r" << id << " = alloca " << slotLlvmType(id) << "\n";
        }
        for (size_t index = 0; index < function_.parameters.size(); ++index) {
            const auto& parameter = function_.parameters[index];
            out_ << "  store " << llvmTypeWide(parameter.type) << " %p" << index << ", ptr %slot_r"
                 << parameter.id.value << "\n";
        }
        if (runtimeProtection_ != nullptr && runtimeProtection_->enabled()) {
            // 反侦测检查点（--runtime-protection）：mj2x_rt_checkpoint_run 命中时
            // 返回非零 → FAKE 语义直接返回默认值；检查点状态为函数级全局变量。
            // 实现由编译管线附加的 mj2x-rt rt_protect 源提供（见
            // emitRuntimeProtectionDeclarations / native_compiler 编译链）。
            const uint32_t cpIndex = (*checkpointCounter_)++;
            const std::string cpVar = "@mj2x_rt_cp_" + std::to_string(cpIndex);
            // 检查点状态变量是模块级全局（函数体内不允许全局定义）。
            checkpointGlobals_.push_back(cpVar +
                                         " = internal global %mj2x_rt_checkpoint "
                                         "zeroinitializer");
            const std::string cpResult = nextTemporary();
            out_ << "  " << cpResult << " = call i32 @mj2x_rt_checkpoint_run(ptr " << cpVar
                 << ", i32 " << static_cast<int>(runtimeProtection_->action) << ", i32 "
                 << runtimeProtection_->step << ", i32 " << runtimeProtection_->delayMs
                 << ", i32 1)\n";
            const std::string hit = nextTemporary();
            out_ << "  " << hit << " = icmp ne i32 " << cpResult << ", 0\n";
            out_ << "  br i1 " << hit << ", label %rt_fake_" << cpIndex << ", label %b"
                 << function_.entry->id.value << "\n";
            out_ << "rt_fake_" << cpIndex << ":\n";
            emitDefaultReturn();
            out_ << "\n";
        }
        else {
            out_ << "  br label %b" << function_.entry->id.value << "\n\n";
        }

        for (const auto& block : function_.blocks) {
            if (!emitBlock(*block, reason)) return false;
        }
        for (const auto& edge : pendingEdges_) {
            out_ << edge.label << ":\n";
            if (!emitCopies(edge.target->copies, reason)) return false;
            out_ << "  br label %b" << edge.target->block->id.value << "\n\n";
        }
        out_ << "}\n\n";
        source = out_.str();
        return true;
    }

private:
    std::string nextTemporary()
    {
        return "%v" + std::to_string(temporaryIndex_++);
    }

    std::string slotName(RegId id) const { return "%slot_r" + std::to_string(id.value); }

    /// @brief 槽类型：包装类 shadow 寄存器用结构类型（"%mj2x_int"），其余按
    /// registerTypes_（窄类型已宽化为 i32）。
    const char* slotLlvmType(uint32_t id) const
    {
        const auto native = nativeSlots_.find(id);
        if (native != nativeSlots_.end()) return native->second.c_str();
        const auto it = registerTypes_.find(id);
        return it == registerTypes_.end() ? nullptr : llvmType(it->second);
    }

    /// @brief native 对象逃逸分析（对齐 RegC regc_intrinsics.cpp）：包装类
    /// valueOf 结果与 Random 分配寄存器经 Copy（指令内 + 后继边 + 异常边）联通
    /// 为根；任何非匹配使用（运算/比较/字段/数组/调用实参/返回/抛出/分支）→
    /// 整根回退 JNI。回退根的所有寄存器保持普通 REF 槽。
    void analyzeWrapperShadows()
    {
        // 1) union-find：REF 寄存器经 copy 联通。
        std::map<uint32_t, uint32_t> parent;
        auto find = [&](uint32_t id) {
            uint32_t root = id;
            while (parent.count(root) != 0 && parent[root] != root) root = parent[root];
            while (parent.count(id) != 0 && parent[id] != id) {
                const uint32_t next = parent[id];
                parent[id]           = root;
                id                   = next;
            }
            return root;
        };
        auto unite = [&](uint32_t a, uint32_t b) {
            a = find(a);
            b = find(b);
            if (a != b) parent[a] = b;
        };
        auto uniteCopy = [&](const RegCopy& copy) {
            if (copy.source.kind == RegOperandKind::Register) {
                unite(copy.dest.id.value, copy.source.reg.value);
            }
        };
        auto uniteSuccessor = [&](const RegSuccessor& successor) {
            for (const auto& copy : successor.copies) uniteCopy(copy);
        };
        for (const auto& block : function_.blocks) {
            for (const auto& ins : block->instructions) {
                if (ins.kind == RegInstructionKind::Copy) uniteCopy(ins.copy);
            }
            std::visit(
                [&](const auto& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                        uniteSuccessor(t.target);
                    }
                    else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                        uniteSuccessor(t.trueTarget);
                        uniteSuccessor(t.falseTarget);
                    }
                    else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                        uniteSuccessor(t.defaultTarget);
                        for (const auto& entry : t.cases) uniteSuccessor(entry.target);
                    }
                },
                block->terminator);
        }
        for (const auto& region : function_.exceptionRegions) {
            for (const auto& edge : region.edges) {
                for (const auto& copy : edge.copies) uniteCopy(copy);
            }
        }

        // 2) valueOf 站点 → 根 kind（同根两种 kind 冲突 → 回退）。
        std::map<uint32_t, const LlvmWrapperBoxEntry*> boxRoots;
        std::set<uint32_t>                             fallbackRoots;
        for (const auto& block : function_.blocks) {
            for (const auto& ins : block->instructions) {
                if (ins.kind != RegInstructionKind::Call || !ins.hasDest ||
                    ins.dest.type != SSAType::REF ||
                    ins.method.invokeOpcode != Opcodes::INVOKESTATIC) {
                    continue;
                }
                const LlvmWrapperBoxEntry* box = matchLlvmWrapperBox(
                    ins.method.owner, ins.method.name, ins.method.desc);
                if (box == nullptr) continue;
                const uint32_t root = find(ins.dest.id.value);
                const auto [it, inserted] = boxRoots.emplace(root, box);
                if (!inserted && it->second != box) fallbackRoots.insert(root);
            }
        }

        // 2b) Random 状态机：new java/util/Random（Allocate）→ 根；无参
        // <init> 不匹配（时钟/seedUniquifier 不确定）→ 回退 JNI。
        std::set<uint32_t> randomRoots;
        for (const auto& block : function_.blocks) {
            for (const auto& ins : block->instructions) {
                if (ins.kind == RegInstructionKind::Allocate &&
                    ins.allocationKind == SSAAllocationKind::Object &&
                    ins.typeDesc == "java/util/Random") {
                    randomRoots.insert(find(ins.dest.id.value));
                }
            }
        }

        // 2c) StringBuilder/StringBuffer：Allocate → 根；append 结果与 receiver
        // 联通（append 返回同一 state 指针，对齐 RegC regc_intrinsics.cpp
        // ids.unite(dest, receiver)）。
        std::set<uint32_t> builderRoots;
        for (const auto& block : function_.blocks) {
            for (const auto& ins : block->instructions) {
                if (ins.kind != RegInstructionKind::Allocate ||
                    ins.allocationKind != SSAAllocationKind::Object ||
                    !isLlvmBuilderAllocation(ins.typeDesc)) {
                    continue;
                }
                builderRoots.insert(find(ins.dest.id.value));
            }
            for (const auto& ins : block->instructions) {
                if (ins.kind != RegInstructionKind::Call || !ins.hasDest ||
                    ins.dest.type != SSAType::REF || ins.args.empty() ||
                    ins.args[0].kind != RegOperandKind::Register) {
                    continue;
                }
                const LlvmBuilderEntry* builder = matchLlvmBuilder(
                    ins.method.owner, ins.method.name, ins.method.desc);
                if (builder != nullptr && builder->kind == 1) {
                    unite(ins.dest.id.value, ins.args[0].reg.value);
                }
            }
        }

        // 3) 使用扫描：非匹配拆箱/任何其他使用 → 根回退。
        auto isBoxRoot = [&](const RegOperand& operand, uint32_t& root) {
            if (operand.kind != RegOperandKind::Register) return false;
            root = find(operand.reg.value);
            return boxRoots.count(root) != 0;
        };
        auto isRandomRoot = [&](const RegOperand& operand, uint32_t& root) {
            if (operand.kind != RegOperandKind::Register) return false;
            root = find(operand.reg.value);
            return randomRoots.count(root) != 0;
        };
        auto isBuilderRoot = [&](const RegOperand& operand, uint32_t& root) {
            if (operand.kind != RegOperandKind::Register) return false;
            root = find(operand.reg.value);
            return builderRoots.count(root) != 0;
        };
        auto fallback = [&](const RegOperand& operand) {
            uint32_t root = 0;
            if (isBoxRoot(operand, root) || isRandomRoot(operand, root) ||
                isBuilderRoot(operand, root)) {
                fallbackRoots.insert(root);
            }
        };
        auto fallbackAll = [&](const std::vector<RegOperand>& operands, size_t first) {
            for (size_t index = first; index < operands.size(); ++index) {
                fallback(operands[index]);
            }
        };
        for (const auto& block : function_.blocks) {
            for (const auto& ins : block->instructions) {
                switch (ins.kind) {
                case RegInstructionKind::Copy:
                case RegInstructionKind::Allocate:
                case RegInstructionKind::Barrier: break;
                case RegInstructionKind::Unary: fallback(ins.value); break;
                case RegInstructionKind::Binary:
                case RegInstructionKind::Compare:
                    fallback(ins.lhs);
                    fallback(ins.rhs);
                    break;
                case RegInstructionKind::Convert: fallback(ins.value); break;
                case RegInstructionKind::LoadField: fallback(ins.receiver); break;
                case RegInstructionKind::StoreField:
                    fallback(ins.receiver);
                    fallback(ins.value);
                    break;
                case RegInstructionKind::ArrayLoad:
                    fallback(ins.receiver);
                    fallback(ins.index);
                    break;
                case RegInstructionKind::ArrayStore:
                    fallback(ins.receiver);
                    fallback(ins.index);
                    fallback(ins.value);
                    break;
                case RegInstructionKind::Intrinsic: fallbackAll(ins.args, 0); break;
                case RegInstructionKind::Call:
                {
                    const LlvmWrapperUnboxEntry* unbox = matchLlvmWrapperUnbox(
                        ins.method.owner, ins.method.name, ins.method.desc);
                    const LlvmRandomEntry* random =
                        ins.args.empty()
                            ? nullptr
                            : (matchLlvmRandomInit(ins.method.owner, ins.method.name,
                                                   ins.method.desc) != nullptr
                                   ? matchLlvmRandomInit(ins.method.owner, ins.method.name,
                                                         ins.method.desc)
                                   : matchLlvmRandomMethod(ins.method.owner, ins.method.name,
                                                           ins.method.desc));
                    const LlvmBuilderEntry* builder = ins.args.empty()
                                                          ? nullptr
                                                          : matchLlvmBuilder(ins.method.owner,
                                                                             ins.method.name,
                                                                             ins.method.desc);
                    if (unbox != nullptr && !ins.args.empty()) {
                        uint32_t root = 0;
                        if (isBoxRoot(ins.args[0], root) &&
                            std::string_view(boxRoots[root]->kind) !=
                                std::string_view(unbox->kind)) {
                            fallbackRoots.insert(root);
                        }
                        fallbackAll(ins.args, 1);
                    }
                    else if (random != nullptr && !ins.args.empty()) {
                        // Random <init>(J)V / nextXxx：receiver 为原生根即安全
                        // （种子/参数若是原生根 → 回退该根）；无参 <init> 不匹配
                        // → fallbackAll(0) 回退 receiver 根。
                        uint32_t root = 0;
                        if (isRandomRoot(ins.args[0], root)) {
                            fallbackAll(ins.args, 1);
                        }
                        else {
                            fallbackAll(ins.args, 0);
                        }
                    }
                    else if (builder != nullptr && !ins.args.empty()) {
                        // Builder init/append/method：receiver 为原生根即安全
                        // （String/Object 等参数若是原生根 → 回退该根）。
                        uint32_t root = 0;
                        if (isBuilderRoot(ins.args[0], root)) {
                            fallbackAll(ins.args, 1);
                        }
                        else {
                            fallbackAll(ins.args, 0);
                        }
                    }
                    else {
                        fallbackAll(ins.args, 0);
                    }
                    break;
                }
                }
            }
            std::visit(
                [&](const auto& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                        fallback(t.condition);
                    }
                    else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                        fallback(t.value);
                    }
                    else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                        fallback(t.value);
                    }
                    else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                        if (t.hasValue) fallback(t.value);
                    }
                },
                block->terminator);
        }

        // 4) native 寄存器：根在 boxRoots/randomRoots/builderRoots 且未回退 →
        // shadow 结构槽（builder 槽保持 ptr，仅标记 dispatch）。
        for (const auto& [id, type] : registerTypes_) {
            if (type != SSAType::REF) continue;
            const uint32_t root = find(id);
            if (fallbackRoots.count(root) != 0) continue;
            const auto boxIt   = boxRoots.find(root);
            const auto randIt  = randomRoots.find(root);
            const auto buildIt = builderRoots.find(root);
            if (boxIt != boxRoots.end()) {
                nativeSlots_[id] = boxIt->second->shadowType;
            }
            else if (randIt != randomRoots.end()) {
                nativeSlots_[id] = "%mj2x_random";
            }
            else if (buildIt != builderRoots.end()) {
                nativeBuilders_.insert(id);
            }
        }
    }

    bool emitOperand(const RegOperand& operand, std::string& expression, std::string& reason)
    {
        if (operand.kind == RegOperandKind::Register) {
            // 槽类型是权威（窄参数宽化为 i32；Compare 结果为 i1 谓词；
            // 包装类 shadow 为结构类型）。
            const char* type = slotLlvmType(operand.reg.value);
            if (type == nullptr) {
                reason = "LLVM IR encountered an unsupported operand type";
                return false;
            }
            const std::string temporary = nextTemporary();
            out_ << "  " << temporary << " = load " << type << ", ptr " << slotName(operand.reg)
                 << "\n";
            expression = temporary;
            return true;
        }
        const char* type = llvmTypeWide(operand.type);
        if (type == nullptr) {
            reason = "LLVM IR encountered an unsupported operand type";
            return false;
        }
        if (operand.kind == RegOperandKind::IntLiteral) {
            expression = std::to_string(operand.intValue);
            if (operand.type == SSAType::BOOL) expression = operand.intValue == 0 ? "0" : "1";
            return true;
        }
        if (operand.kind == RegOperandKind::FloatLiteral) {
            // NaN / ±Inf / -0.0 用 LLVM hex 字面量（十进制形式无法表示）；
            // 正常值保持十进制 scientific。
            const double value = operand.floatValue;
            std::string  bits;
            if (operand.type == SSAType::F32) {
                if (std::isnan(value)) bits = "0x7FC00000";
                else if (std::isinf(value)) bits = std::signbit(value) ? "0xFF800000" : "0x7F800000";
                else if (value == 0.0 && std::signbit(value)) bits = "0x80000000";
            }
            else if (operand.type == SSAType::F64) {
                if (std::isnan(value)) bits = "0x7FF8000000000000";
                else if (std::isinf(value))
                    bits = std::signbit(value) ? "0xFFF0000000000000" : "0x7FF0000000000000";
                else if (value == 0.0 && std::signbit(value)) bits = "0x8000000000000000";
            }
            else {
                reason = "LLVM IR floating literal has a non-floating type";
                return false;
            }
            if (!bits.empty()) {
                // 操作数位置不带类型前缀（LLVM 按指令上下文推断 float 类型）。
                expression = bits;
                return true;
            }
            std::ostringstream literal;
            literal << std::scientific << std::showpoint;
            if (operand.type == SSAType::F32) {
                literal << std::setprecision(9) << static_cast<float>(operand.floatValue);
            }
            else {
                literal << std::setprecision(17) << operand.floatValue;
            }
            expression = literal.str();
            return true;
        }
        if (operand.kind == RegOperandKind::NullLiteral) {
            expression = "null";
            return true;
        }
        if (operand.kind == RegOperandKind::Opaque) {
            // String/Class 常量：JNI NewStringUTF / FindClass 构造（物化为临时）。
            if (operand.tag.rfind("ldc_string:", 0) == 0) {
                const std::string temporary = nextTemporary();
                out_ << "  " << temporary
                     << " = call ptr @mj2x_llvm_string_const(ptr %env, ptr "
                     << strings_.intern(operand.tag.substr(11)) << ")\n";
                expression = temporary;
                return true;
            }
            if (operand.tag.rfind("ldc_class:", 0) == 0) {
                const std::string temporary = nextTemporary();
                out_ << "  " << temporary << " = call ptr @mj2x_llvm_class_const(ptr %env, ptr "
                     << strings_.intern(operand.tag.substr(10)) << ")\n";
                expression = temporary;
                return true;
            }
        }
        reason = "LLVM IR first slice rejects reference and opaque operands";
        return false;
    }

    bool emitStore(RegValue destination, const std::string& expression, std::string& reason)    {
        // 槽类型是权威（窄结果 i32；Compare 结果 i1 谓词；包装类 shadow 结构）。
        const char* type = slotLlvmType(destination.id.value);
        if (type == nullptr) {
            reason = "LLVM IR destination has an unsupported type";
            return false;
        }
        out_ << "  store " << type << " " << expression << ", ptr " << slotName(destination.id)
             << "\n";
        return true;
    }

    bool emitCopies(const std::vector<RegCopy>& copies, std::string& reason)
    {
        std::vector<std::pair<RegValue, std::string>> values;
        values.reserve(copies.size());
        for (const auto& copy : copies) {
            std::string expression;
            if (!emitOperand(copy.source, expression, reason)) return false;
            values.emplace_back(copy.dest, std::move(expression));
        }
        for (const auto& [destination, expression] : values) {
            if (!emitStore(destination, expression, reason)) return false;
        }
        return true;
    }

    std::string edgeTarget(const RegSuccessor& successor)
    {
        if (successor.copies.empty()) return "%b" + std::to_string(successor.block->id.value);
        PendingEdge edge;
        edge.label = "edge_" + std::to_string(edgeIndex_++);
        edge.target = &successor;
        pendingEdges_.push_back(edge);
        return "%" + edge.label;
    }

    bool emitInstruction(const RegBlock* block, const RegInstruction& instruction,
                         const std::vector<const RegExceptionRegion*>& regions,
                         std::string& reason)
    {
        std::string lhs;
        std::string rhs;
        std::string value;
        if (instruction.kind == RegInstructionKind::Copy) {
            return emitOperand(instruction.copy.source, value, reason) &&
                   emitStore(instruction.copy.dest, value, reason);
        }
        if (instruction.kind == RegInstructionKind::Unary) {
            if (!emitOperand(instruction.value, value, reason)) return false;
            const char* type = llvmTypeWide(instruction.dest.type);
            const std::string result = nextTemporary();
            if (instruction.unaryOp == SSAUnaryOp::BitNot) {
                out_ << "  " << result << " = xor " << type << " " << value << ", -1\n";
            }
            else if (instruction.dest.type == SSAType::F32 || instruction.dest.type == SSAType::F64) {
                out_ << "  " << result << " = fneg " << type << " " << value << "\n";
            }
            else if (instruction.dest.type == SSAType::I32 || instruction.dest.type == SSAType::I64) {
                out_ << "  " << result << " = sub " << type << " 0, " << value << "\n";
            }
            else {
                reason = "LLVM IR negation has an unsupported type";
                return false;
            }
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Binary) {
            if (!emitOperand(instruction.lhs, lhs, reason) ||
                !emitOperand(instruction.rhs, rhs, reason)) {
                return false;
            }
            const bool floating = instruction.dest.type == SSAType::F32 ||
                                  instruction.dest.type == SSAType::F64;
            if (!floating &&
                (instruction.binaryOp == SSABinaryOp::Div ||
                 instruction.binaryOp == SSABinaryOp::Rem)) {
                // 整数 div/rem：sdiv/srem——JVM 是有符号除法，udiv/urem 对负
                // 操作数结果错误（JVM -7/2=-3，udiv 位模式得 2147483644）。
                // INT_MIN/-1 组合是 sdiv/srem 的溢出 poison，JVM 语义为 div 回绕
                // INT_MIN、rem 得 0，显式分支防护（对齐 StackC/VMC4）；除零走
                // 共享 throw-arithmetic helper 抛 ArithmeticException 后返回 0
                // （JVM 检查 pending exception）。
                const bool        isDiv    = instruction.binaryOp == SSABinaryOp::Div;
                const std::string divIndex = std::to_string(divIndex_++);
                const std::string isZero   = nextTemporary();
                const std::string result   = nextTemporary();
                const char*       type     = llvmTypeWide(instruction.dest.type);
                const char*       minLiteral =
                    instruction.dest.type == SSAType::I64 ? "-9223372036854775808"
                                                          : "-2147483648";
                out_ << "  " << isZero << " = icmp eq " << type << " " << rhs << ", 0\n";
                out_ << "  br i1 " << isZero << ", label %b_divz_" << divIndex
                     << ", label %b_divok_" << divIndex << "\n";
                out_ << "b_divok_" << divIndex << ":\n";
                const std::string isNegOne = nextTemporary();
                out_ << "  " << isNegOne << " = icmp eq " << type << " " << rhs << ", -1\n";
                out_ << "  br i1 " << isNegOne << ", label %b_divn1_" << divIndex
                     << ", label %b_divgo_" << divIndex << "\n";
                out_ << "b_divn1_" << divIndex << ":\n";
                const std::string isMin = nextTemporary();
                out_ << "  " << isMin << " = icmp eq " << type << " " << lhs << ", "
                     << minLiteral << "\n";
                out_ << "  br i1 " << isMin << ", label %b_divovf_" << divIndex
                     << ", label %b_divgo_" << divIndex << "\n";
                out_ << "b_divovf_" << divIndex << ":\n";
                if (!emitStore(instruction.dest, isDiv ? minLiteral : "0", reason)) return false;
                out_ << "  br label %b_divafter_" << divIndex << "\n";
                out_ << "b_divgo_" << divIndex << ":\n";
                out_ << "  " << result << " = " << (isDiv ? "sdiv" : "srem") << " "
                     << type << " " << lhs << ", " << rhs << "\n";
                if (!emitStore(instruction.dest, result, reason)) return false;
                out_ << "  br label %b_divafter_" << divIndex << "\n";
                out_ << "b_divz_" << divIndex << ":\n";
                out_ << "  call void @mj2x_llvm_throw_arith(ptr %env)\n";
                // 除零在 try 块内时：抛出的 ArithmeticException 按异常表分发
                // （无匹配/无异常区域则直接返回，pending 异常由 JVM 处理）。
                if (!regions.empty()) {
                    if (!emitExceptionDispatch(block, regions, dispatchIndex_, false, reason)) {
                        return false;
                    }
                }
                else {
                    out_ << "  ret " << type << " 0\n";
                }
                out_ << "b_divafter_" << divIndex << ":\n";
                return true;
            }
            const char* opcode = nullptr;
            switch (instruction.binaryOp) {
            case SSABinaryOp::Add: opcode = floating ? "fadd" : "add"; break;
            case SSABinaryOp::Sub: opcode = floating ? "fsub" : "sub"; break;
            case SSABinaryOp::Mul: opcode = floating ? "fmul" : "mul"; break;
            case SSABinaryOp::Div: opcode = floating ? "fdiv" : nullptr; break;
            case SSABinaryOp::Rem: opcode = floating ? "frem" : nullptr; break;
            case SSABinaryOp::And: opcode = floating ? nullptr : "and"; break;
            case SSABinaryOp::Or: opcode = floating ? nullptr : "or"; break;
            case SSABinaryOp::Xor: opcode = floating ? nullptr : "xor"; break;
            case SSABinaryOp::Shl: opcode = floating ? nullptr : "shl"; break;
            case SSABinaryOp::Shr: opcode = floating ? nullptr : "ashr"; break;
            case SSABinaryOp::UShr: opcode = floating ? nullptr : "lshr"; break;
            }
            if (opcode == nullptr) {
                reason = "LLVM IR binary operation is outside the pure scalar subset";
                return false;
            }
            // JVM 移位计数按位宽掩码（int &31 / long &63，JLS 15.19）。LLVM
            // shl/ashr/lshr 的 shift >= 位宽产生 poison（并非“天然等价 Java
            // 掩码”），必须显式 and。掩码值来自 codegen/jvm_semantics.hpp 共享
            // 决策（g13.shift_out_of_range 回归守门）。long 移位的 JVM 计数是
            // int（RegIR rhs 为 I32），先 zext 到目标位宽再掩码。
            if (!floating && (instruction.binaryOp == SSABinaryOp::Shl ||
                              instruction.binaryOp == SSABinaryOp::Shr ||
                              instruction.binaryOp == SSABinaryOp::UShr)) {
                const int         shiftMask  = codegen::jvmShiftMask(instruction.dest.type);
                const std::string destLlvm   = llvmTypeWide(instruction.dest.type);
                std::string       countValue = rhs;
                if (llvmTypeWide(instruction.rhs.type) != destLlvm) {
                    const std::string widened = nextTemporary();
                    out_ << "  " << widened << " = zext " << llvmTypeWide(instruction.rhs.type)
                         << " " << rhs << " to " << destLlvm << "\n";
                    countValue = widened;
                }
                const std::string maskedRhs = nextTemporary();
                out_ << "  " << maskedRhs << " = and " << destLlvm << " " << countValue << ", "
                     << shiftMask << "\n";
                const std::string result = nextTemporary();
                out_ << "  " << result << " = " << opcode << " " << destLlvm << " " << lhs << ", "
                     << maskedRhs << "\n";
                return emitStore(instruction.dest, result, reason);
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << opcode << " " << llvmTypeWide(instruction.dest.type)
                 << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Call) {
            // 包装类 shadow 切片：valueOf 装箱（结果非逃逸）与 xxxValue 拆箱
            // （receiver 为 shadow）直调 mj2x-rt 值结构（0 JNI；逃逸根已在
            // analyzeWrapperShadows 回退，此处走 JNI 分派）。
            if (instruction.hasDest && instruction.dest.type == SSAType::REF &&
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC) {
                const LlvmWrapperBoxEntry* box = matchLlvmWrapperBox(
                    instruction.method.owner, instruction.method.name, instruction.method.desc);
                if (box != nullptr && nativeSlots_.count(instruction.dest.id.value) != 0) {
                    std::string argument;
                    if (!emitOperand(instruction.args[0], argument, reason)) return false;
                    const std::string argType = llvmTypeWide(instruction.args[0].type);
                    std::string        callArg = std::string(box->argLlvm) + " " + argument;
                    if (std::string_view(box->argLlvm) != std::string_view(argType)) {
                        // 窄装箱：crt 形参是 i16/i8，栈值 i32 → trunc。
                        const std::string narrowed = nextTemporary();
                        out_ << "  " << narrowed << " = trunc " << argType << " " << argument
                             << " to " << box->argLlvm << "\n";
                        callArg = std::string(box->argLlvm) + " " + narrowed;
                    }
                    runtimeDeclares_.push_back(box->shadowDecl);
                    runtimeDeclares_.push_back(box->declareText);
                    const std::string result = nextTemporary();
                    out_ << "  " << result << " = call " << box->shadowType << " @" << box->symbol
                         << "(" << callArg << ")\n";
                    return emitStore(instruction.dest, result, reason);
                }
            }
            if (!instruction.args.empty() &&
                instruction.args[0].kind == RegOperandKind::Register &&
                nativeSlots_.count(instruction.args[0].reg.value) != 0) {
                const LlvmWrapperUnboxEntry* unbox = matchLlvmWrapperUnbox(
                    instruction.method.owner, instruction.method.name, instruction.method.desc);
                if (unbox != nullptr) {
                    std::string receiver;
                    if (!emitOperand(instruction.args[0], receiver, reason)) return false;
                    runtimeDeclares_.push_back(unbox->shadowDecl);
                    runtimeDeclares_.push_back(unbox->declareText);
                    const std::string result = nextTemporary();
                    out_ << "  " << result << " = call " << unbox->retLlvm << " @" << unbox->symbol
                         << "(" << unbox->shadowType << " " << receiver << ")\n";
                    if (unbox->retExt[0] != '\0') {
                        // 窄拆箱：crt 返回 i16/i8，JVM 栈语义 i32 → sext/zext。
                        const std::string widened = nextTemporary();
                        out_ << "  " << widened << " = " << unbox->retExt << " " << unbox->retLlvm
                             << " " << result << " to i32\n";
                        return emitStore(instruction.dest, widened, reason);
                    }
                    return emitStore(instruction.dest, result, reason);
                }
                // Random 状态机：<init>(J)V 种子初始化 / nextXxx 直调（receiver
                // 即 %mj2x_random 槽指针；nextBoolean 的 i8 结果 zext 到 i32）。
                const LlvmRandomEntry* random =
                    matchLlvmRandomInit(instruction.method.owner, instruction.method.name,
                                        instruction.method.desc) != nullptr
                        ? matchLlvmRandomInit(instruction.method.owner,
                                              instruction.method.name,
                                              instruction.method.desc)
                        : matchLlvmRandomMethod(instruction.method.owner,
                                                instruction.method.name,
                                                instruction.method.desc);
                if (random != nullptr) {
                    runtimeDeclares_.push_back(random->declareText);
                    std::string callArgs = "ptr " + slotName(instruction.args[0].reg);
                    std::string boundArg;
                    for (size_t index = 1; index < instruction.args.size(); ++index) {
                        std::string argument;
                        if (!emitOperand(instruction.args[index], argument, reason)) {
                            return false;
                        }
                        if (index == 1) boundArg = argument;
                        callArgs += ", " + std::string(llvmTypeWide(instruction.args[index].type)) +
                                    " " + argument;
                    }
                    // 发射端预检位（#879 Y4 解耦）：Random.nextInt(bound)——JVM
                    // 对 bound<=0 抛 IllegalArgumentException；crt 直调无异常通道
                    // （bound<=0 返回哨兵值），发射端按权威表派生的 precheck 位
                    // 预检（fail-closed），不再 desc/name/argCount 硬编码。
                    if (random->precheck == RegCIntrinsicPrecheck::PositiveBound) {
                        const uint32_t    idx  = dispatchIndex_++;
                        const std::string bad  = nextTemporary();
                        const std::string tag  = std::to_string(idx);
                        out_ << "  " << bad << " = icmp sle i32 " << boundArg << ", 0\n";
                        out_ << "  br i1 " << bad << ", label %b_riae_" << tag
                             << ", label %b_riok_" << tag << "\n";
                        out_ << "b_riae_" << tag << ":\n";
                        out_ << "  call void @mj2x_llvm_throw_new(ptr %env, ptr "
                             << strings_.intern("java/lang/IllegalArgumentException")
                             << ", ptr " << strings_.intern("bound must be positive") << ")\n";
                        if (!emitExceptionDispatch(block, regions, dispatchIndex_, false,
                                                   reason)) {
                            return false;
                        }
                        out_ << "b_riok_" << tag << ":\n";
                    }
                    if (random->retLlvm[0] == 'v') {   // "void"
                        out_ << "  call void @" << random->symbol << "(" << callArgs << ")\n";
                        return true;
                    }
                    const std::string result = nextTemporary();
                    out_ << "  " << result << " = call " << random->retLlvm << " @"
                         << random->symbol << "(" << callArgs << ")\n";
                    if (std::string_view(random->retLlvm) == "i8") {
                        const std::string widened = nextTemporary();
                        out_ << "  " << widened << " = zext i8 " << result << " to i32\n";
                        return emitStore(instruction.dest, widened, reason);
                    }
                    return emitStore(instruction.dest, result, reason);
                }
            }
            // StringBuilder/StringBuffer 状态机：init/append/method 直调 crt
            // （状态为堆分配 mj2x_string_builder*，寄存器槽即 out_state 指针；
            // 调用返回 mj2x_regc_status，非 0 = pending exception → 分发/默认返回）。
            if (!instruction.args.empty() &&
                instruction.args[0].kind == RegOperandKind::Register &&
                nativeBuilders_.count(instruction.args[0].reg.value) != 0) {
                const LlvmBuilderEntry* builder = matchLlvmBuilder(
                    instruction.method.owner, instruction.method.name, instruction.method.desc);
                if (builder != nullptr) {
                    // 模块级 declare：declare i32 @sym(ptr, ptr[, args...][, ptr out])
                    //（发射端按 argLlvm/outLlvm 构造；窄形参 i8/i16 直用）。
                    {
                        std::string declare = std::string("declare i32 @") + builder->symbol +
                                              "(ptr, ptr";
                        for (int argIndex = 0; argIndex < 3; ++argIndex) {
                            if (builder->argLlvm[argIndex][0] == '\0') break;
                            declare += ", " + std::string(builder->argLlvm[argIndex]);
                        }
                        if (builder->outLlvm[0] != '\0') declare += ", ptr";
                        declare += ")";
                        runtimeDeclares_.push_back(declare);
                    }
                    const uint32_t index = builderIndex_++;
                    std::string    callArgs = "ptr " + builderContext_;
                    std::string    stateSlot = "ptr " + slotName(instruction.args[0].reg);
                    // 参数（context 之后；init 的参数在 out_state 之前，其余在
                    // state 之后——crt 签名：init (context, args..., out_state**)，
                    // append/method (context, state, args...[, out])）。
                    std::string pendingArgs;
                    if (builder->kind == 0) {
                        for (size_t argIndex = 1; argIndex < instruction.args.size(); ++argIndex) {
                            std::string argument;
                            if (!emitOperand(instruction.args[argIndex], argument, reason)) {
                                return false;
                            }
                            const char* want = builder->argLlvm[argIndex - 1];
                            if (std::string_view(want) == "i8" ||
                                std::string_view(want) == "i16") {
                                const std::string narrowed = nextTemporary();
                                out_ << "  " << narrowed << " = trunc i32 " << argument
                                     << " to " << want << "\n";
                                argument = narrowed;
                            }
                            const char* argType = (want[0] == '\0')
                                                      ? llvmTypeWide(
                                                            instruction.args[argIndex].type)
                                                      : want;
                            pendingArgs += ", " + std::string(argType) + " " + argument;
                        }
                        callArgs += pendingArgs + ", " + stateSlot;
                    }
                    else {
                        const std::string stateTemp = nextTemporary();
                        out_ << "  " << stateTemp << " = load ptr, ptr "
                             << slotName(instruction.args[0].reg) << "\n";
                        callArgs += ", ptr " + stateTemp;
                        for (size_t argIndex = 1; argIndex < instruction.args.size(); ++argIndex) {
                            std::string argument;
                            if (!emitOperand(instruction.args[argIndex], argument, reason)) {
                                return false;
                            }
                            const char* want = builder->argLlvm[argIndex - 1];
                            if (std::string_view(want) == "i8" ||
                                std::string_view(want) == "i16") {
                                // 窄实参：crt 形参 int8_t/uint16_t，栈值 i32 → trunc。
                                const std::string narrowed = nextTemporary();
                                out_ << "  " << narrowed << " = trunc i32 " << argument
                                     << " to " << want << "\n";
                                argument = narrowed;
                            }
                            const char* argType = (want[0] == '\0')
                                                      ? llvmTypeWide(
                                                            instruction.args[argIndex].type)
                                                      : want;
                            callArgs += ", " + std::string(argType) + " " + argument;
                        }
                    }
                    std::string outName;
                    if (builder->outLlvm[0] != '\0') {
                        outName = nextTemporary();
                        out_ << "  " << outName << " = alloca " << builder->outLlvm << "\n";
                        callArgs += ", ptr " + outName;
                    }
                    const std::string status = nextTemporary();
                    out_ << "  " << status << " = call i32 @" << builder->symbol << "("
                         << callArgs << ")\n";
                    const std::string statusOk = nextTemporary();
                    out_ << "  " << statusOk << " = icmp eq i32 " << status << ", 0\n";
                    out_ << "  br i1 " << statusOk << ", label %b_sbok_" << index
                         << ", label %b_sberr_" << index << "\n";
                    out_ << "b_sberr_" << index << ":\n";
                    if (!regions.empty()) {
                        if (!emitExceptionDispatch(block, regions, dispatchIndex_, false,
                                                   reason)) {
                            return false;
                        }
                    }
                    else {
                        emitDefaultReturn();
                    }
                    out_ << "b_sbok_" << index << ":\n";
                    if (builder->kind == 1) {
                        // append：dest 槽回存同一 state 指针。
                        const std::string stateTemp = nextTemporary();
                        out_ << "  " << stateTemp << " = load ptr, ptr "
                             << slotName(instruction.args[0].reg) << "\n";
                        return emitStore(instruction.dest, stateTemp, reason);
                    }
                    if (builder->kind == 2 && builder->outLlvm[0] != '\0') {
                        const std::string result = nextTemporary();
                        out_ << "  " << result << " = load " << builder->outLlvm << ", ptr "
                             << outName << "\n";
                        if (std::string_view(builder->outLlvm) == "i16") {
                            // charAt：uint16_t → zext i32。
                            const std::string widened = nextTemporary();
                            out_ << "  " << widened << " = zext i16 " << result << " to i32\n";
                            return emitStore(instruction.dest, widened, reason);
                        }
                        return emitStore(instruction.dest, result, reason);
                    }
                    return true;
                }
            }
            // JDK 纯函数 intrinsic（Math 切片）：直调 mj2x-rt 符号（0 JNI、
            // 不抛异常无 ExceptionCheck；对齐 RegC RuntimeFunction 语义）。
            if (instruction.method.invokeOpcode == Opcodes::INVOKESTATIC) {
                const LlvmIntrinsicEntry* intrinsic =
                    matchLlvmIntrinsic(instruction.method.owner, instruction.method.name,
                                       instruction.method.desc);
                if (intrinsic != nullptr) {
                    std::string args;
                    for (size_t index = 0; index < instruction.args.size(); ++index) {
                        std::string expression;
                        if (!emitOperand(instruction.args[index], expression, reason)) {
                            return false;
                        }
                        if (!args.empty()) args += ", ";
                        args += std::string(intrinsic->argLlvm[index]) + " " + expression;
                    }
                    runtimeDeclares_.push_back(intrinsic->declareText);
                    const std::string result = nextTemporary();
                    out_ << "  " << result << " = call " << intrinsic->returnLlvm << " @"
                         << intrinsic->symbol << "(" << args << ")\n";
                    return emitStore(instruction.dest, result, reason);
                }
            }
            // 同模块内已转换的 invokestatic → 直接调用（0 JNI，保持确定性）；
            // 其余（实例调用、外部类、未转换目标）→ JNI CallXxxMethodA 分派。
            CallTarget target;
            const bool direct =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC && resolver_ != nullptr &&
                resolver_->resolve(instruction.method, target, reason);
            if (direct) {
                // helper 首参为 JNIEnv*（%env），后续为 Java 参数。
                std::string args = "ptr %env";
                for (size_t index = 0; index < instruction.args.size(); ++index) {
                    std::string expression;
                    if (!emitOperand(instruction.args[index], expression, reason)) return false;
                    args += ", " + std::string(llvmType(target.argTypes[index])) + " " + expression;
                }
                if (target.returnType == SSAType::VOID) {
                    out_ << "  call void @" << target.symbol << "(" << args << ")\n";
                    return true;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call " << llvmType(target.returnType) << " @"
                     << target.symbol << "(" << args << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            // JNI 分派：jvalue 数组（[N x { i64 }] union 槽）承载全部参数。
            const std::string              desc       = instruction.method.desc.to_std_string();
            const PrimitiveKind            retKind    = returnKind(desc);
            const char*                    callKind   = callKindFor(retKind);
            const char*                    mode       =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC
                    ? "call_static_"
                    : (instruction.method.invokeOpcode == Opcodes::INVOKESPECIAL
                           ? "call_nonvirtual_"
                           : "call_");
            const size_t                   receiverOffset =
                instruction.method.invokeOpcode == Opcodes::INVOKESTATIC ? 0 : 1;
            if (callKind == nullptr) {
                reason = "LLVM IR call has an unsupported return type";
                return false;
            }
            const std::string jv = nextTemporary();
            out_ << "  " << jv << " = alloca [" << (instruction.args.size() - receiverOffset)
                 << " x { i64 }]\n";
            // jvalue 数组只承载方法描述符参数（实例调用的 receiver 单独传）。
            for (size_t index = receiverOffset; index < instruction.args.size(); ++index) {
                std::string expression;
                if (!emitOperand(instruction.args[index], expression, reason)) return false;
                const std::string slot = nextTemporary();
                out_ << "  " << slot << " = getelementptr ["
                     << (instruction.args.size() - receiverOffset) << " x { i64 }], ptr " << jv
                     << ", i64 0, i64 " << (index - receiverOffset) << "\n";
                const SSAType argType = widenNarrowType(instruction.args[index].type);
                out_ << "  store " << llvmType(argType) << " " << expression << ", ptr " << slot
                     << "\n";
            }
            std::string callArgs = "ptr %env";
            if (receiverOffset == 1) {
                std::string receiver;
                if (!emitOperand(instruction.args[0], receiver, reason)) return false;
                callArgs += ", ptr " + receiver;
            }
            callArgs += ", ptr " + strings_.intern(instruction.method.owner.to_std_string()) +
                        ", ptr " + strings_.intern(instruction.method.name.to_std_string()) +
                        ", ptr " + strings_.intern(desc) + ", ptr " + jv;
            if (retKind == PrimitiveKind::VOID) {
                out_ << "  call void @mj2x_llvm_" << mode << callKind << "(" << callArgs << ")\n";
                return true;
            }
            const std::string result = nextTemporary();
            const char*       boundaryRet = jniBoundaryType(retKind);
            out_ << "  " << result << " = call " << boundaryRet << " @mj2x_llvm_" << mode
                 << callKind << "(" << callArgs << ")\n";
            if (std::string(boundaryRet) == "i8" || std::string(boundaryRet) == "i16") {
                const bool  signed_ = retKind == PrimitiveKind::BYTE ||
                                     retKind == PrimitiveKind::SHORT;
                const std::string ext = nextTemporary();
                out_ << "  " << ext << " = " << (signed_ ? "sext" : "zext") << " "
                     << boundaryRet << " " << result << " to i32\n";
                return emitStore(instruction.dest, ext, reason);
            }
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Compare) {
            if (!emitOperand(instruction.lhs, lhs, reason) ||
                !emitOperand(instruction.rhs, rhs, reason)) {
                return false;
            }
            const bool floating = instruction.lhs.type == SSAType::F32 ||
                                  instruction.lhs.type == SSAType::F64;
            // REF：对象同一性必须走 IsSameObject（JNI local ref 是句柄，裸指针
            // 比较会把同一对象的两个句柄判为不等；对齐 RegC/VMC4 语义）。
            if (instruction.lhs.type == SSAType::REF) {
                const std::string eq   = nextTemporary();
                const std::string eqIn = nextTemporary();
                out_ << "  " << eqIn << " = call i1 @mj2x_llvm_ref_eq(ptr %env, ptr " << lhs
                     << ", ptr " << rhs << ")\n";
                if (instruction.comparePredicate == SSAComparePredicate::Ne) {
                    out_ << "  " << eq << " = xor i1 " << eqIn << ", true\n";
                    return emitStore(instruction.dest, eq, reason);
                }
                // Eq 直接采用 IsSameObject 结果；LLVM IR 无寄存器拷贝语法，
                // 不能发 "%dst = %src" 形态的伪指令。
                return emitStore(instruction.dest, eqIn, reason);
            }
            const char* predicate = nullptr;
            switch (instruction.comparePredicate) {
            case SSAComparePredicate::Eq: predicate = floating ? "oeq" : "eq"; break;
            case SSAComparePredicate::Ne: predicate = floating ? "une" : "ne"; break;
            case SSAComparePredicate::Lt: predicate = floating ? "olt" : "slt"; break;
            case SSAComparePredicate::Le: predicate = floating ? "ole" : "sle"; break;
            case SSAComparePredicate::Gt: predicate = floating ? "ogt" : "sgt"; break;
            case SSAComparePredicate::Ge: predicate = floating ? "oge" : "sge"; break;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << (floating ? "fcmp " : "icmp ") << predicate
                 << " " << llvmTypeWide(instruction.lhs.type) << " " << lhs << ", " << rhs << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::Convert) {
            if (instruction.convertKind == SSAConvertKind::ReferenceCast) {
                // checkcast：IsInstanceOf 失败挂起 ClassCastException，返回原引用。
                std::string value;
                if (!emitOperand(instruction.value, value, reason)) return false;
                const std::string desc = instruction.referenceTypeDesc.to_std_string();
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_check_cast(ptr %env, ptr "
                     << value << ", ptr " << strings_.intern(desc) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (!emitOperand(instruction.value, value, reason)) return false;
            const SSAType sourceType = widenNarrowType(instruction.value.type);
            const SSAType targetType = instruction.targetType;
            // i2b/i2s/i2c：目标为窄类型，trunc 后按符号/零扩展回 i32。
            if (targetType == SSAType::I8 || targetType == SSAType::I16 ||
                targetType == SSAType::CHAR) {
                if (sourceType != SSAType::I32) {
                    reason = "LLVM IR narrowing conversion requires an i32 source";
                    return false;
                }
                const char* narrow = (targetType == SSAType::CHAR || targetType == SSAType::I16)
                                         ? "i16"
                                         : "i8";
                const std::string trunc  = nextTemporary();
                const std::string ext    = nextTemporary();
                const bool        signed_ = targetType != SSAType::CHAR;
                out_ << "  " << trunc << " = trunc i32 " << value << " to " << narrow << "\n";
                out_ << "  " << ext << " = " << (signed_ ? "sext" : "zext") << " " << narrow
                     << " " << trunc << " to i32\n";
                return emitStore(instruction.dest, ext, reason);
            }
            if (!isSupportedValueType(targetType)) {
                reason = "LLVM IR numeric conversion has an unsupported target type";
                return false;
            }
            if (sourceType == targetType) return emitStore(instruction.dest, value, reason);
            const bool sourceInt = sourceType == SSAType::I32 || sourceType == SSAType::I64;
            const bool targetInt = targetType == SSAType::I32 || targetType == SSAType::I64;
            const bool sourceFloat = sourceType == SSAType::F32 || sourceType == SSAType::F64;
            const bool targetFloat = targetType == SSAType::F32 || targetType == SSAType::F64;
            const char* opcode = nullptr;
            if (sourceInt && targetInt) {
                opcode = sourceType == SSAType::I32 ? "sext" : "trunc";
            }
            else if (sourceInt && targetFloat) {
                opcode = "sitofp";
            }
            else if (sourceFloat && targetFloat) {
                opcode = sourceType == SSAType::F32 ? "fpext" : "fptrunc";
            }
            else {
                // 浮点 → 整数：llvm.fptosi.sat（NaN→0、溢出饱和，匹配 Java f2i/f2l
                // 语义；LLVM fptosi 对 NaN/溢出是 poison，不可直接用）。
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call "
                     << (targetType == SSAType::I32 ? "i32" : "i64")
                     << " @llvm.fptosi.sat."
                     << (targetType == SSAType::I32 ? "i32" : "i64") << "."
                     << (sourceType == SSAType::F32 ? "f32" : "f64") << "("
                     << llvmType(sourceType) << " " << value << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = " << opcode << " " << llvmType(sourceType) << " "
                 << value << " to " << llvmType(targetType) << "\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::LoadField) {
            const std::string fieldDesc = instruction.field.desc.to_std_string();
            const char*       kind      = fieldKindFor(fieldDesc);
            if (kind == nullptr) {
                reason = "LLVM IR field access has an unsupported descriptor";
                return false;
            }
            std::string receiver;
            if (instruction.receiver.kind == RegOperandKind::Opaque &&
                instruction.receiver.type == SSAType::UNKNOWN) {
                receiver = "null";
            }
            else if (!emitOperand(instruction.receiver, receiver, reason)) {
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call " << fieldHelperReturnType(kind)
                 << " @mj2x_llvm_field_load_" << kind << "(ptr %env, ptr " << receiver << ", ptr "
                 << strings_.intern(instruction.field.owner.to_std_string()) << ", ptr "
                 << strings_.intern(instruction.field.name.to_std_string()) << ", ptr "
                 << strings_.intern(fieldDesc) << ", i32 "
                 << (instruction.field.isStatic ? 1 : 0) << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::StoreField) {
            const std::string fieldDesc = instruction.field.desc.to_std_string();
            const char*       kind      = fieldKindFor(fieldDesc);
            if (kind == nullptr) {
                reason = "LLVM IR field access has an unsupported descriptor";
                return false;
            }
            std::string receiver;
            if (instruction.receiver.kind == RegOperandKind::Opaque &&
                instruction.receiver.type == SSAType::UNKNOWN) {
                receiver = "null";
            }
            else if (!emitOperand(instruction.receiver, receiver, reason)) {
                return false;
            }
            std::string value;
            if (!emitOperand(instruction.value, value, reason)) return false;
            out_ << "  call void @mj2x_llvm_field_store_" << kind << "(ptr %env, ptr " << receiver
                 << ", ptr " << strings_.intern(instruction.field.owner.to_std_string()) << ", ptr "
                 << strings_.intern(instruction.field.name.to_std_string()) << ", ptr "
                 << strings_.intern(fieldDesc) << ", i32 "
                 << (instruction.field.isStatic ? 1 : 0) << ", " << fieldHelperReturnType(kind)
                 << " " << value << ")\n";
            return true;
        }
        if (instruction.kind == RegInstructionKind::Allocate) {
            const std::string typeDesc = instruction.typeDesc.to_std_string();
            if (instruction.allocationKind == SSAAllocationKind::Object) {
                // Random 原生切片：非逃逸 new Random 的对象槽即 %mj2x_random
                // 状态（Allocate 无操作；<init>(J)V 处 mj2x_random_init）。
                if (nativeSlots_.count(instruction.dest.id.value) != 0) {
                    return true;
                }
                // Builder 原生切片：非逃逸 new StringBuilder 的槽即 out_state
                // 指针（Allocate 无操作；<init> 处 mj2x_string_builder_init）。
                if (nativeBuilders_.count(instruction.dest.id.value) != 0) {
                    return true;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_new_object(ptr %env, ptr "
                     << strings_.intern(typeDesc) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (instruction.allocationKind == SSAAllocationKind::Array) {
                if (instruction.args.size() != 1) {
                    reason = "LLVM IR array allocation requires exactly one dimension";
                    return false;
                }
                std::string count;
                if (!emitOperand(instruction.args[0], count, reason)) return false;
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_new_array(ptr %env, ptr "
                     << strings_.intern(typeDesc) << ", i32 " << count << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            // MULTIANEWARRAY：调用点构建维度数组（alloca [N x i32]）。
            const std::string dims = nextTemporary();
            out_ << "  " << dims << " = alloca [" << instruction.args.size() << " x i32]\n";
            for (size_t index = 0; index < instruction.args.size(); ++index) {
                std::string dim;
                if (!emitOperand(instruction.args[index], dim, reason)) return false;
                const std::string slot = nextTemporary();
                out_ << "  " << slot << " = getelementptr ["
                     << instruction.args.size() << " x i32], ptr " << dims << ", i64 0, i64 "
                     << index << "\n";
                out_ << "  store i32 " << dim << ", ptr " << slot << "\n";
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call ptr @mj2x_llvm_new_multi_array(ptr %env, ptr "
                 << strings_.intern(typeDesc) << ", ptr " << dims << ", i32 "
                 << instruction.args.size() << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::ArrayLoad) {
            const char* kind = arrayKindFor(instruction.elementType);
            if (kind == nullptr) {
                reason = "LLVM IR array load has an unsupported element type";
                return false;
            }
            std::string array;
            std::string index;
            if (!emitOperand(instruction.receiver, array, reason) ||
                !emitOperand(instruction.index, index, reason)) {
                return false;
            }
            const std::string result = nextTemporary();
            out_ << "  " << result << " = call " << fieldHelperReturnType(kind)
                 << " @mj2x_llvm_array_load_" << kind << "(ptr %env, ptr " << array << ", i32 "
                 << index << ")\n";
            return emitStore(instruction.dest, result, reason);
        }
        if (instruction.kind == RegInstructionKind::ArrayStore) {
            const char* kind = arrayKindFor(instruction.elementType);
            if (kind == nullptr) {
                reason = "LLVM IR array store has an unsupported element type";
                return false;
            }
            std::string array;
            std::string index;
            std::string value;
            if (!emitOperand(instruction.receiver, array, reason) ||
                !emitOperand(instruction.index, index, reason) ||
                !emitOperand(instruction.value, value, reason)) {
                return false;
            }
            out_ << "  call void @mj2x_llvm_array_store_" << kind << "(ptr %env, ptr " << array
                 << ", i32 " << index << ", " << fieldHelperReturnType(kind) << " " << value
                 << ")\n";
            return true;
        }
        if (instruction.kind == RegInstructionKind::Intrinsic) {
            const std::string& name = instruction.intrinsicName;
            if (name == "array.length") {
                std::string array;
                if (instruction.args.empty() || !emitOperand(instruction.args[0], array, reason)) {
                    reason = "LLVM IR array.length requires an array operand";
                    return false;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call i32 @mj2x_llvm_array_length(ptr %env, ptr "
                     << array << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (name.rfind("instanceof:", 0) == 0) {
                std::string value;
                if (instruction.args.empty() ||
                    !emitOperand(instruction.args[0], value, reason)) {
                    reason = "LLVM IR instanceof requires an object operand";
                    return false;
                }
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call i32 @mj2x_llvm_instance_of(ptr %env, ptr "
                     << value << ", ptr " << strings_.intern(name.substr(11)) << ")\n";
                return emitStore(instruction.dest, result, reason);
            }
            if (name == "monitor.enter" || name == "monitor.exit") {
                std::string value;
                if (instruction.args.empty() || !emitOperand(instruction.args[0], value, reason)) {
                    reason = "LLVM IR monitor requires an object operand";
                    return false;
                }
                out_ << "  call void @mj2x_llvm_monitor_"
                     << (name == "monitor.enter" ? "enter" : "exit") << "(ptr %env, ptr " << value
                     << ")\n";
                return true;
            }
            if (name == "cmp.nan_greater" || name == "cmp.nan_less") {
                // LCMP/FCMPG/FCMPL/DCMPG/DCMPL 三值比较（-1/0/1）：
                // (lhs > rhs) - (lhs < rhs)；浮点任一 NaN 时按 nanGreater 取 1/-1
                // （对齐 RegC regc_flow.cpp 的 cmp.nan_* 发射）。
                if (instruction.args.size() != 2) {
                    reason = "cmp.nan_* intrinsic requires two operands";
                    return false;
                }
                std::string lhs;
                std::string rhs;
                if (!emitOperand(instruction.args[0], lhs, reason) ||
                    !emitOperand(instruction.args[1], rhs, reason)) {
                    return false;
                }
                const SSAType type = widenNarrowType(instruction.args[0].type);
                const char*   t    = llvmType(type);
                if (t == nullptr) {
                    reason = "cmp.nan_* intrinsic has an unsupported operand type";
                    return false;
                }
                const bool floating = type == SSAType::F32 || type == SSAType::F64;
                const std::string gtCmp = nextTemporary();
                const std::string ltCmp = nextTemporary();
                out_ << "  " << gtCmp << " = " << (floating ? "fcmp ogt" : "icmp sgt") << " "
                     << t << " " << lhs << ", " << rhs << "\n";
                out_ << "  " << ltCmp << " = " << (floating ? "fcmp olt" : "icmp slt") << " "
                     << t << " " << lhs << ", " << rhs << "\n";
                const std::string gt = nextTemporary();
                const std::string lt = nextTemporary();
                out_ << "  " << gt << " = zext i1 " << gtCmp << " to i32\n";
                out_ << "  " << lt << " = zext i1 " << ltCmp << " to i32\n";
                const std::string threeWay = nextTemporary();
                out_ << "  " << threeWay << " = sub i32 " << gt << ", " << lt << "\n";
                if (floating) {
                    const bool        nanGreater = name == "cmp.nan_greater";
                    const std::string uno        = nextTemporary();
                    const std::string selected   = nextTemporary();
                    out_ << "  " << uno << " = fcmp uno " << t << " " << lhs << ", " << rhs
                         << "\n";
                    out_ << "  " << selected << " = select i1 " << uno << ", i32 "
                         << (nanGreater ? 1 : -1) << ", i32 " << threeWay << "\n";
                    return emitStore(instruction.dest, selected, reason);
                }
                return emitStore(instruction.dest, threeWay, reason);
            }
            if (name.rfind("indy:", 0) == 0) {
                // invokedynamic 运行时解析（自定义 bootstrap 残留站点，pipeline 级
                // InvokeDynamicPreprocessor 静态展开后的兜底）：参数装箱成
                // Object[] → bsm_N helper 返回目标 MethodHandle（含缓存）→
                // invokeWithArguments(Object[]) → 结果按站点返回类型拆箱。
                size_t localIndex = 0;
                try {
                    localIndex = static_cast<size_t>(std::stoull(name.substr(5)));
                }
                catch (const std::exception&) {
                    reason = "invalid indy site index in intrinsic name";
                    return false;
                }
                if (localIndex >= function_.indySites.size()) {
                    reason = "indy site index out of range";
                    return false;
                }
                const std::string siteDesc = function_.indySites[localIndex].desc;
                const auto        argKinds = argumentKinds(siteDesc);
                if (instruction.args.size() != argKinds.size()) {
                    reason = "indy arg count mismatch";
                    return false;
                }
                // Object[] args：NewObjectArray(Object.class, n) + 逐个装箱。
                const std::string objCls = nextTemporary();
                out_ << "  " << objCls << " = call ptr @mj2x_llvm_class_const(ptr %env, ptr "
                     << strings_.intern("java/lang/Object") << ")\n";
                const std::string objArray = nextTemporary();
                out_ << "  " << objArray << " = call ptr @mj2x_llvm_new_object_array(ptr %env, "
                        "i32 "
                     << argKinds.size() << ", ptr " << objCls << ")\n";
                for (size_t index = 0; index < instruction.args.size(); ++index) {
                    std::string expression;
                    if (!emitOperand(instruction.args[index], expression, reason)) return false;
                    const char* boxKind = boxKindFor(instruction.args[index].type);
                    std::string boxed;
                    if (boxKind != nullptr) {
                        boxed = nextTemporary();
                        out_ << "  " << boxed << " = call ptr @mj2x_llvm_box_" << boxKind
                             << "(ptr %env, " << llvmTypeWide(instruction.args[index].type) << " "
                             << expression << ")\n";
                    }
                    else {
                        // REF 参数无需装箱（Object[] 直接承载）。
                        boxed = expression;
                    }
                    out_ << "  call void @mj2x_llvm_set_object_array_element(ptr %env, ptr "
                         << objArray << ", i32 " << index << ", ptr " << boxed << ")\n";
                }
                // 目标 MethodHandle：bsm_N(env, owner_name)。owner 名由本方法
                // 所属类的名字给出，helper 内部 FindClass（与 RegC 的
                // _mj2x_regc_owner_class 等价）。
                const std::string target = nextTemporary();
                out_ << "  " << target << " = call ptr @mj2x_llvm_indy_bsm_"
                     << (indySiteBase_ + localIndex) << "(ptr %env, ptr "
                     << strings_.intern(function_.symbol.owner) << ")\n";
                // bsm 失败时 pending 异常已挂且返回 null：必须走异常分发/重抛，
                // 不得把 null 送进 call_object（其 ThrowNew NPE 会掩盖真实
                // 异常——StackC 对应路径是异常分发，对齐之）。
                {
                    const std::string siteTag = std::to_string(indySiteBase_ + localIndex);
                    const std::string tgtNull = nextTemporary();
                    out_ << "  " << tgtNull << " = icmp eq ptr " << target << ", null\n";
                    out_ << "  br i1 " << tgtNull << ", label %b_ibsmerr_" << siteTag
                         << ", label %b_ibsmok_" << siteTag << "\n";
                    out_ << "b_ibsmerr_" << siteTag << ":\n";
                    if (!emitExceptionDispatch(block, regions, dispatchIndex_, false, reason)) {
                        return false;
                    }
                    out_ << "b_ibsmok_" << siteTag << ":\n";
                }
                // MethodHandle.invokeWithArguments(Object[]) → Object。
                // 参数数组指针需包进 1 槽 jvalue 数组（否则 JNI 收到 null 参数）。
                const std::string jv = nextTemporary();
                out_ << "  " << jv << " = alloca [1 x { i64 }]\n";
                const std::string jvSlot = nextTemporary();
                out_ << "  " << jvSlot << " = getelementptr [1 x { i64 }], ptr " << jv
                     << ", i64 0, i64 0\n";
                out_ << "  store ptr " << objArray << ", ptr " << jvSlot << "\n";
                const std::string result = nextTemporary();
                out_ << "  " << result << " = call ptr @mj2x_llvm_call_object(ptr %env, ptr "
                     << target << ", ptr " << strings_.intern("java/lang/invoke/MethodHandle")
                     << ", ptr " << strings_.intern("invokeWithArguments") << ", ptr "
                     << strings_.intern("([Ljava/lang/Object;)Ljava/lang/Object;") << ", ptr "
                     << jv << ")\n";
                // invokeWithArguments 抛异常时 pending 挂着而 result 可为 null
                //（REF 站点合法返回 null）——调用后统一 ExceptionCheck 分发，
                // 再进拆箱/透传（否则异常会被后续 helper 的 NPE 掩盖）。
                if (!emitExceptionDispatch(block, regions, dispatchIndex_, true, reason)) {
                    return false;
                }
                if (!instruction.hasDest) return true;
                // REF 结果直接透传（invokeWithArguments 返回 jobject）；原语按
                // 站点返回类型拆箱。
                if (instruction.dest.type == SSAType::REF) {
                    return emitStore(instruction.dest, result, reason);
                }
                const char* unboxKind = boxKindFor(instruction.dest.type);
                if (unboxKind == nullptr) {
                    reason = "indy result has an unsupported type";
                    return false;
                }
                const std::string unboxed = nextTemporary();
                out_ << "  " << unboxed << " = call " << unboxReturnType(instruction.dest.type)
                     << " @mj2x_llvm_unbox_" << unboxKind << "(ptr %env, ptr " << result << ")\n";
                return emitStore(instruction.dest, unboxed, reason);
            }
            reason = "LLVM IR first slice rejects this intrinsic";
            return false;
        }
        reason = "LLVM IR encountered an unsupported instruction after validation";
        return false;
    }

    /// @brief block 的活跃异常区域（异常表顺序；无 protected 匹配则空）。
    std::vector<const RegExceptionRegion*> activeRegionsFor(const RegBlock* block) const
    {
        std::vector<const RegExceptionRegion*> result;
        for (const auto& region : function_.exceptionRegions) {
            for (const auto* protectedBlock : region.protectedBlocks) {
                if (protectedBlock == block) {
                    result.push_back(&region);
                    break;
                }
            }
        }
        return result;
    }

    /// @brief 默认返回（异常泄漏/无 handler 时保持 JVM 传播语义的兜底路径）。
    void emitDefaultReturn()
    {
        if (returnType_ == SSAType::VOID) {
            out_ << "  ret void\n";
            return;
        }
        const char* type = llvmType(returnType_);
        const char* zero = (type == nullptr) ? "0"
                           : (std::string(type) == "float" || std::string(type) == "double")
                               ? "0.0"
                               : (std::string(type) == "ptr" ? "null" : "0");
        out_ << "  ret " << (type == nullptr ? "i32" : type) << " " << zero << "\n";
    }

    /// @brief 异常分发：ExceptionCheck 后按异常表匹配 handler；无 handler/无匹配
    /// 重抛并默认返回。匹配成功时执行异常边拷贝 + exceptionValue 赋值后跳 handler。
    /// @param continueAfter 为 true 时 false 分支落到 cont_N 标签（调用方继续
    /// 发射指令/终止符）；为 false（athrow 终止符）时 false 分支走默认返回。
    bool emitExceptionDispatch(const RegBlock* block,
                               const std::vector<const RegExceptionRegion*>& regions,
                               uint32_t& dispatchIndex, bool continueAfter, std::string& reason)
    {
        const uint32_t   index = dispatchIndex++;
        const std::string chk  = nextTemporary();
        out_ << "  " << chk << " = call i1 @mj2x_llvm_exception_check(ptr %env)\n";
        const std::string exLabel   = "ex_" + std::to_string(index);
        const std::string contLabel = "cont_" + std::to_string(index);
        const std::string noExLabel = "nex_" + std::to_string(index);
        out_ << "  br i1 " << chk << ", label %" << exLabel << ", label %"
             << (continueAfter ? contLabel : noExLabel) << "\n";
        out_ << exLabel << ":\n";
        const std::string ex = nextTemporary();
        out_ << "  " << ex << " = call ptr @mj2x_llvm_exception_occurred(ptr %env)\n";
        out_ << "  call void @mj2x_llvm_exception_clear(ptr %env)\n";
        if (regions.empty()) {
            out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << ex << ")\n";
            emitDefaultReturn();
            if (continueAfter) {
                out_ << contLabel << ":\n";
            }
            else {
                out_ << noExLabel << ":\n";
                emitDefaultReturn();
            }
            return true;
        }
        for (size_t r = 0; r < regions.size(); ++r) {
            const auto& region     = *regions[r];
            const std::string match = nextTemporary();
            const bool   last       = r + 1 == regions.size();
            out_ << "  " << match << " = call i1 @mj2x_llvm_exception_matches(ptr %env, ptr "
                 << ex << ", ptr " << strings_.intern(region.caughtType) << ")\n";
            out_ << "  br i1 " << match << ", label %hx_" << index << "_" << r << ", label %"
                 << (last ? ("rx_" + std::to_string(index)) : ("exn_" + std::to_string(index) + "_" + std::to_string(r)))
                 << "\n";
            out_ << "hx_" << index << "_" << r << ":\n";
            const RegExceptionRegion::Edge* edge = nullptr;
            for (const auto& e : region.edges) {
                if (e.protectedBlock == block) {
                    edge = &e;
                    break;
                }
            }
            if (edge != nullptr) {
                for (const auto& copy : edge->copies) {
                    std::string expression;
                    if (!emitOperand(copy.source, expression, reason)) return false;
                    const auto it = registerTypes_.find(copy.dest.id.value);
                    const char* type = it == registerTypes_.end() ? nullptr : llvmType(it->second);
                    if (type == nullptr) {
                        reason = "LLVM IR exception edge copy has an unsupported type";
                        return false;
                    }
                    out_ << "  store " << type << " " << expression << ", ptr %slot_r"
                         << copy.dest.id.value << "\n";
                }
            }
            out_ << "  store ptr " << ex << ", ptr %slot_r" << region.exceptionValue.id.value
                 << "\n";
            out_ << "  br label %b" << region.handler->id.value << "\n";
            if (!last) {
                out_ << "exn_" << index << "_" << r << ":\n";
            }
        }
        out_ << "rx_" << index << ":\n";
        out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << ex << ")\n";
        emitDefaultReturn();
        if (continueAfter) {
            out_ << contLabel << ":\n";
        }
        else {
            out_ << noExLabel << ":\n";
            emitDefaultReturn();
        }
        return true;
    }

    bool emitBlock(const RegBlock& block, std::string& reason)
    {
        const auto regions = activeRegionsFor(&block);
        out_ << "b" << block.id.value << ":\n";
        for (const auto& instruction : block.instructions) {
            if (!emitInstruction(&block, instruction, regions, reason)) return false;
            // 方法含 try/catch 时，可能抛异常的指令后插入异常分发
            // （无异常区域时 pending 异常泄漏到 JNI 边界由 JVM 处理）。
            if (instruction.effect.mayThrow && !regions.empty()) {
                if (!emitExceptionDispatch(&block, regions, dispatchIndex_, true, reason)) {
                    return false;
                }
            }
        }
        return std::visit(
            [&](const auto& terminator) -> bool {
                using T = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    out_ << "  br label " << edgeTarget(terminator.target) << "\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    std::string condition;
                    if (!emitOperand(terminator.condition, condition, reason)) return false;
                    if (terminator.condition.type != SSAType::BOOL) {
                        if (terminator.condition.type != SSAType::I32 &&
                            terminator.condition.type != SSAType::I64) {
                            reason = "LLVM IR branch condition must be boolean or integer";
                            return false;
                        }
                        const std::string normalized = nextTemporary();
                        out_ << "  " << normalized << " = icmp ne " << llvmTypeWide(terminator.condition.type)
                             << " " << condition << ", 0\n";
                        condition = normalized;
                    }
                    out_ << "  br i1 " << condition << ", label " << edgeTarget(terminator.trueTarget)
                         << ", label " << edgeTarget(terminator.falseTarget) << "\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    if (terminator.value.type != SSAType::I32) {
                        reason = "LLVM IR first slice only supports i32 switch values";
                        return false;
                    }
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  switch i32 " << value << ", label "
                         << edgeTarget(terminator.defaultTarget) << " [\n";
                    for (const auto& entry : terminator.cases) {
                        out_ << "    i32 " << entry.key << ", label " << edgeTarget(entry.target)
                             << "\n";
                    }
                    out_ << "  ]\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (returnType_ == SSAType::VOID) {
                        if (terminator.hasValue) {
                            reason = "LLVM IR void method returned a value";
                            return false;
                        }
                        out_ << "  ret void\n\n";
                        return true;
                    }
                    if (!terminator.hasValue ||
                        widenNarrowType(terminator.value.type) != returnType_) {
                        reason = "LLVM IR return type does not match method descriptor";
                        return false;
                    }
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  ret " << llvmType(returnType_) << " " << value << "\n\n";
                    return true;
                }
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    // athrow：JNI Throw（null → NPE）；有异常区域时按表分发，
                    // 否则 pending 异常泄漏到 JVM 边界。
                    std::string value;
                    if (!emitOperand(terminator.value, value, reason)) return false;
                    out_ << "  call void @mj2x_llvm_throw(ptr %env, ptr " << value << ")\n";
                    if (!regions.empty()) {
                        if (!emitExceptionDispatch(&block, regions, dispatchIndex_, false, reason)) {
                            return false;
                        }
                    }
                    else {
                        emitDefaultReturn();
                    }
                    out_ << "\n";
                    return true;
                }
                else {
                    reason = "LLVM IR encountered an unsupported terminator after validation";
                    return false;
                }
            },
            block.terminator);
    }

    const RegFunction&       function_;
    const RegisterTypes&     registerTypes_;
    const std::string&       functionName_;
    SSAType                  returnType_;
    InternalCallResolver*    resolver_;
    GlobalStringTable&       strings_;
    size_t                   indySiteBase_ = 0;
    const codegen::RuntimeProtectionOptions* runtimeProtection_ = nullptr;
    uint32_t*                checkpointCounter_ = nullptr;   // 文件级检查点计数（跨函数唯一）
    std::ostringstream       out_;
    uint32_t                 temporaryIndex_ = 0;
    uint32_t                 edgeIndex_ = 0;
    uint32_t                 divIndex_ = 0;
    uint32_t                 dispatchIndex_ = 0;   // 函数级：异常分发 label 唯一
    std::vector<PendingEdge> pendingEdges_;
    std::vector<std::string> checkpointGlobals_;   // 模块级检查点全局（finalize 统一输出）
    std::vector<std::string> runtimeDeclares_;     // 本函数用到的 intrinsic 直调 declare
    /// 包装类 shadow 寄存器 → 结构槽类型（"%mj2x_int" 等；仅非逃逸根）。
    std::map<uint32_t, std::string> nativeSlots_;
    /// Builder 原生寄存器（槽为 ptr 状态指针，仅驱动 dispatch）。
    std::set<uint32_t> nativeBuilders_;
    /// 本函数是否使用 builder intrinsic（决定 %mj2x_regc_context 前导）。
    bool        usesBuilder_ = false;
    uint32_t    builderIndex_ = 0;   // builder 状态检查 label 唯一号
    std::string builderContext_ = "%mj2x_llvm_ctx";

public:
    /// @brief 本函数注入的检查点状态全局（模块级输出用）。
    const std::vector<std::string>& checkpointGlobals() const { return checkpointGlobals_; }

    /// @brief 本函数用到的 JDK intrinsic 直调声明（模块级输出用）。
    const std::vector<std::string>& runtimeDeclares() const { return runtimeDeclares_; }
};

bool emitLlvmFunction(const RegFunction& function, const RegisterTypes& registerTypes,
                      const std::string& functionName, SSAType returnType,
                      InternalCallResolver* resolver, GlobalStringTable& strings,
                      size_t indySiteBase,
                      const codegen::RuntimeProtectionOptions* runtimeProtection,
                      uint32_t* checkpointCounter, std::vector<std::string>* checkpointGlobals,
                      std::vector<std::string>* runtimeDeclares, std::string& source,
                      std::string& reason)
{
    LlvmFunctionEmitter emitter(function, registerTypes, functionName, returnType, resolver,
                                strings, indySiteBase, runtimeProtection, checkpointCounter);
    if (!emitter.emit(source, reason)) return false;
    if (checkpointGlobals != nullptr) *checkpointGlobals = emitter.checkpointGlobals();
    if (runtimeDeclares != nullptr) *runtimeDeclares = emitter.runtimeDeclares();
    return true;
}

}   // namespace llvmir
}   // namespace mj2x

#include "stackc_backend.hpp"
#include "mj2x/backend/stackc/stackc_backend_internal.hpp"
#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"
#include "mj2x/codegen/code_snippet.hpp"
#include "mj2x/codegen/include_set.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/postprocessor/native_loader_call.hpp"

#include <fstream>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using codegen::CSourceStream;
using codegen::fnv1a64;
using codegen::hex64;
using codegen::nativeFunctionName;

namespace {

/// UTF-8 -> UTF-16 code units（Java 字符串常量来自 class 文件，描述符/标识符均为 ASCII）。
/// 非法字节序列按替换字符处理，避免生成器侧崩溃。
std::vector<ca::u16> utf8ToUtf16Units(const std::string& value)
{
    std::vector<ca::u16> units;
    units.reserve(value.size());
    size_t i = 0;
    while (i < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        ca::u32 codePoint = 0;
        int extra = 0;
        if (c < 0x80) {
            codePoint = c;
        }
        else if ((c & 0xE0) == 0xC0) {
            codePoint = c & 0x1F;
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            codePoint = c & 0x0F;
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            codePoint = c & 0x07;
            extra = 3;
        }
        else {
            units.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(extra) + 1 > value.size()) {
            units.push_back(0xFFFD);
            break;
        }
        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(value[i + k]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(value[i + k]) & 0x3F);
        }
        if (!valid) {
            units.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (codePoint <= 0xFFFF) {
            units.push_back(static_cast<ca::u16>(codePoint));
        }
        else {
            const ca::u32 v = codePoint - 0x10000;
            units.push_back(static_cast<ca::u16>(0xD800 + (v >> 10)));
            units.push_back(static_cast<ca::u16>(0xDC00 + (v & 0x3FF)));
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return units;
}

/// @brief 把节点发射文本中的 `stack[sp ± k]` 动态索引静态化为 `s[<常量>]`，
///        并把 `sp` 维护语句删除（对齐 JNIC 命名槽模型，-O2 下编译器可寄存器化）。
/// @param baseDepth 节点执行前的编译期栈深度（块入口深度数据流 + 已发射节点净效应）。
/// @return 该节点的栈净效应（push/pop 差值），调用方累加到当前深度。
/// @details 逐行处理：`sp -= N`/`sp--` 等只改偏移不产出行；`stack[++sp]` 写
///          `s[baseDepth+off]`，`stack[sp--]` 读 `s[baseDepth-1+off]`，其余
///          `stack[sp ± K]` 读 `s[baseDepth±K+off]`。off 为节点内已发生的
///          深度偏移（覆盖 DUP 系列与 invokedynamic 的 `sp -= argCount` 段）。
int staticizeStackText(std::string& text, int baseDepth)
{
    std::string result;
    int         off = 0;
    size_t      pos = 0;
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string  line = text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                     : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;

        std::string out;
        if (line.find("sp = -1;") != std::string::npos) {
            continue;   // 异常 dispatch 的重置由 compileMethod 直接生成 s[0]，不会出现在节点文本
        }
        size_t       cursor = 0;
        std::string  replaced;
        // 行内顺序扫描替换（无「最早匹配」竞争）：栈访问模式与 sp 维护语句
        // 都按行内模式处理（sp 语句只更新偏移不产出行，避免行级 continue 吞掉
        // 同行含 stack[..] 的 DUP 类行）。顺序：
        //   1) `stack[++sp]` / `stack[sp--]` / `stack[sp ± N]`（N 可为 0，指栈顶）/
        //      `stack[sp]`
        //   2) `sp -= N` / `sp += N` / `sp--` / `sp++`（只改偏移）
        // 字符串字面量与 // 行注释内的文本逐字保留——字面量（NewStringUTF 内联
        // 进方法体）一旦含 `stack[sp`/`sp -= N` 等模式会被静默改写（X4 防碰撞）。
        bool inString = false;
        bool escaped  = false;
        bool inComment = false;
        while (cursor < line.size()) {
            if (inComment) {
                replaced += line[cursor];
                ++cursor;
                continue;
            }
            if (inString) {
                replaced += line[cursor];
                if (escaped) {
                    escaped = false;
                }
                else if (line[cursor] == '\\') {
                    escaped = true;
                }
                else if (line[cursor] == '"') {
                    inString = false;
                }
                ++cursor;
                continue;
            }
            if (line[cursor] == '"') {
                inString = true;
                replaced += line[cursor];
                ++cursor;
                continue;
            }
            if (line[cursor] == '/' && cursor + 1 < line.size() && line[cursor + 1] == '/') {
                inComment = true;
                continue;
            }
            if (line.compare(cursor, 11, "stack[++sp]") == 0) {
                // push 写：sp 原顶下标 = baseDepth-1，新元素下标 = baseDepth。
                replaced += "s[";
                replaced += std::to_string(baseDepth + off);
                replaced += "]";
                off += 1;
                cursor += 11;
            }
            else if (line.compare(cursor, 11, "stack[sp--]") == 0) {
                replaced += "s[";
                replaced += std::to_string(baseDepth - 1 + off);
                replaced += "]";
                off -= 1;
                cursor += 11;
            }
            else if (line.compare(cursor, 9, "stack[sp ") == 0 && cursor + 9 < line.size() &&
                     (line[cursor + 9] == '-' || line[cursor + 9] == '+')) {
                const bool neg = line[cursor + 9] == '-';
                size_t     j   = cursor + 10;
                // `stack[sp + 1]` 形态：符号后可带空格。
                while (j < line.size() && line[j] == ' ') ++j;
                if (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                    int n = 0;
                    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                        n = n * 10 + (line[j] - '0');
                        ++j;
                    }
                    if (j < line.size() && line[j] == ']') {
                        // sp 顶下标 = baseDepth-1，故 stack[sp-N] → 下标 baseDepth-1-N，
                        // stack[sp+N] → 下标 baseDepth-1+N；N=0 视为栈顶。
                        const int k = (n == 0) ? -1 : (neg ? -(n + 1) : (n - 1));
                        replaced += "s[";
                        replaced += std::to_string(baseDepth + k + off);
                        replaced += "]";
                        cursor = j + 1;
                    }
                    else {
                        replaced += line[cursor];
                        ++cursor;
                    }
                }
                else {
                    replaced += line[cursor];
                    ++cursor;
                }
            }
            else if (line.compare(cursor, 9, "stack[sp]") == 0) {
                replaced += "s[";
                replaced += std::to_string(baseDepth - 1 + off);
                replaced += "]";
                cursor += 9;
            }
            else if (line.compare(cursor, 6, "sp -= ") == 0) {
                size_t j = cursor + 6;
                int    n = 0;
                while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                    n = n * 10 + (line[j] - '0');
                    ++j;
                }
                off -= n;
                cursor = j;
            }
            else if (line.compare(cursor, 6, "sp += ") == 0) {
                size_t j = cursor + 6;
                int    n = 0;
                while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
                    n = n * 10 + (line[j] - '0');
                    ++j;
                }
                off += n;
                cursor = j;
            }
            else if (line.compare(cursor, 4, "sp--") == 0) {
                off -= 1;
                cursor += 4;
            }
            else if (line.compare(cursor, 4, "sp++") == 0) {
                off += 1;
                cursor += 4;
            }
            else {
                replaced += line[cursor];
                ++cursor;
            }
        }
        result += replaced;
        result += '\n';
    }
    text = result;
    return off;
}

}   // namespace

/// @brief 每块入口栈深度的数据流迭代（定义在匿名命名空间外：Impl 静态成员）。
///        语义见 internal.hpp 声明。
bool StackCBackend::Impl::computeBlockDepths(const std::vector<Block*>& blocks,
                                             const std::vector<jbytecode::core::Label*>& handlerLabels,
                                             std::vector<int>& entryDepths)
{
    const size_t n = blocks.size();
    entryDepths.assign(n, -1);
    if (n == 0) return true;

    std::unordered_map<jbytecode::core::Label*, size_t> labelToIndex;
    for (size_t bi = 0; bi < n; ++bi) {
        for (auto& node : blocks[bi]->nodes) {
            if (node->kind == MIRKind::LABEL) {
                labelToIndex[static_cast<const MIRLabel*>(node.get())->labelNode] = bi;
            }
        }
    }
    // handler 入口深度恒为 1（异常对象在 s[0]）。
    std::unordered_set<size_t> handlerBlocks;
    for (auto* lbl : handlerLabels) {
        auto found = labelToIndex.find(lbl);
        if (found != labelToIndex.end()) handlerBlocks.insert(found->second);
    }
    for (size_t hi : handlerBlocks) entryDepths[hi] = 1;

    entryDepths[0] = 0;
    const int maxRounds = static_cast<int>(n) * 8 + 64;
    int       rounds    = 0;
    bool      changed   = true;
    while (changed) {
        if (++rounds > maxRounds) {
            CA_LOGT_WARN("stackc", "depth analysis did not converge after {} rounds\n", maxRounds);
            return false;
        }
        changed = false;
        for (size_t bi = 0; bi < n; ++bi) {
            if (entryDepths[bi] < 0) continue;
            auto* b = blocks[bi];
            int   depth = entryDepths[bi];
            for (auto& node : b->nodes) {
                CSourceStream tmp;
                emitNode(node.get(), tmp);
                std::string text = tmp.str();
                depth += staticizeStackText(text, depth);
            }
            // 出边：终结器目标 + fallthrough（物理下一块）。
            std::vector<size_t> succs;
            Block*              fallthrough = (bi + 1 < n) ? blocks[bi + 1] : nullptr;
            // 终结指令判定需跳过块尾伪指令（FRAME/LABEL/NOP）：StackMap 帧可能
            // 落在终结指令之后（如 `goto 82` 后紧跟 82 的帧），若把 FRAME 当终结
            // 会误走 fallthrough 而丢失 goto 语义（ThreadsController.checkCollision）。
            const MIRNode* lastReal = nullptr;
            for (auto it = b->nodes.rbegin(); it != b->nodes.rend(); ++it) {
                const MIRKind k = (*it)->kind;
                if (k == MIRKind::FRAME || k == MIRKind::LABEL || k == MIRKind::NOP) continue;
                lastReal = it->get();
                break;
            }
            const MIRKind lastKind = lastReal != nullptr ? lastReal->kind : MIRKind::NOP;
            auto pushLabel = [&](jbytecode::core::Label* lbl) {
                auto found = labelToIndex.find(lbl);
                if (found != labelToIndex.end()) succs.push_back(found->second);
            };
            switch (lastKind) {
            case MIRKind::BRANCH:
                pushLabel(static_cast<const MIRBranch*>(lastReal)->targetLabel);
                if (fallthrough != nullptr) succs.push_back(bi + 1);
                break;
            case MIRKind::BRANCH_ZERO:
                pushLabel(static_cast<const MIRBranchZero*>(lastReal)->targetLabel);
                if (fallthrough != nullptr) succs.push_back(bi + 1);
                break;
            case MIRKind::UNCOND_BRANCH:
                pushLabel(static_cast<const MIRUncondBranch*>(lastReal)->targetLabel);
                break;
            case MIRKind::SWITCH: {
                const auto* sw = static_cast<const MIRSwitch*>(lastReal);
                for (const auto& c : sw->cases) pushLabel(c.targetLabel);
                pushLabel(sw->defaultLabel);
                break;
            }
            case MIRKind::RETURN:
            case MIRKind::RETURN_VALUE:
            case MIRKind::THROW: break;
            default:
                if (fallthrough != nullptr) succs.push_back(bi + 1);
                break;
            }
            for (size_t si : succs) {
                if (handlerBlocks.count(si) != 0) continue;   // handler 深度固定 1
                if (entryDepths[si] < 0) {
                    entryDepths[si] = depth;
                    changed = true;
                }
                else if (entryDepths[si] != depth) {
                    CA_LOGT_WARN("stackc", "depth conflict: block {} entry={} pred={} out={}\n", blocks[si]->id, entryDepths[si], b->id, depth);
                    return false;
                }
            }
        }
    }
    for (size_t bi = 0; bi < n; ++bi) {
        if (entryDepths[bi] < 0) {
            std::string labels;
            for (auto& node : blocks[bi]->nodes) {
                if (node->kind == MIRKind::LABEL) {
                    labels += std::to_string(
                                  static_cast<const MIRLabel*>(node.get())->labelNode
                                      ->bytecodeOffset) +
                              " ";
                }
            }
            CA_LOGT_WARN("stackc", "depth unresolved: block {} (idx {}) labels=[{}] preds? terminator={}\n", blocks[bi]->id, bi, labels.c_str(),
                   blocks[bi]->nodes.empty() ? -1
                                             : static_cast<int>(blocks[bi]->nodes.back()->kind));
            return false;
        }
    }
    return true;
}

StackCBackend::StackCBackend()
    : impl_(std::make_unique<Impl>())
{}

StackCBackend::~StackCBackend() = default;

void StackCBackend::setExceptionCheckLite(bool lite) { impl_->exceptionCheckLite = lite; }

void StackCBackend::setUserIntrinsics(const UserIntrinsicConfig& config)
{
    impl_->userIntrinsics = config.intrinsics;
}

// ============================================================
// Main compile method
// ============================================================

BackendResult StackCBackend::compileMethod(const std::string& className, CompilerMethod& method,
                                           const std::vector<Block*>& blocks,
                                           CompilerIndex* index)
{
    (void)index;
    auto* mn                = method.methodNode;
    impl_->currentClassName = className;
    auto& cls               = impl_->currentClassName;

    impl_->currentMethodName = mn->name.to_std_string();
    impl_->currentMethodDesc = mn->desc.to_std_string();

    CSourceStream funcOut;

    // JNI function signature
    bool isStatic = (mn->access & Opcodes::ACC_STATIC) != 0;
    auto retType =
        jvmDescToPrimitive(Type::getReturnType(mn->desc).getDescriptor());

    std::string methodName = mn->name.to_std_string();
    std::string methodDesc = mn->desc.to_std_string();
    auto        funcName   = nativeFunctionName(cls, methodName, methodDesc);
    if (!impl_->nativeFunctions.insert(funcName).second) {
        int         suffix = 1;
        std::string candidate;
        do {
            candidate = funcName + "_" + std::to_string(suffix++);
        } while (!impl_->nativeFunctions.insert(candidate).second);
        funcName = std::move(candidate);
    }
    auto [regsIt, insertedClass] =
        impl_->registrationsByClass.emplace(cls, std::vector<CNativeRegistration>{});
    if (insertedClass) {
        impl_->registrationClassIds.emplace(cls,
                                            static_cast<int>(impl_->registrationClassOrder.size()));
        impl_->registrationClassOrder.push_back(cls);
    }
    regsIt->second.push_back({methodName, methodDesc, funcName});

    funcOut.appendLineStart("static ").append(primitiveToJNIType(retType)).append(" JNICALL");
    funcOut.newLine();
    funcOut.append(funcName).append("(JNIEnv* env, ")
        .append(isStatic ? "jclass clazz" : "jobject this");

    // Add method parameters
    auto desc     = Type::getMethodType(mn->desc);
    auto argTypes = desc.getArgumentTypes();
    int  slot     = 0;
    int  paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut.append(", ").append(primitiveToJNIType(pk)).append(" p").appendInt(paramIdx);
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut.append(") {");
    funcOut.newLine();
    funcOut.indent();

    // Stack and locals declaration
    // 操作数栈用「静态槽」s[N]（对齐 JNIC 命名槽模型）：发射时按编译期栈深度
    // 把 stack[sp±k] 静态化为 s[<常量>]，-O2 下编译器可寄存器化；无运行期 sp。
    int maxLocalsVal = mn->maxLocals > 0 ? mn->maxLocals : 1;
    funcOut.appendLineStart("union jvalue s[")
        .appendInt(mn->maxStack > 0 ? mn->maxStack : 1).append("];");
    funcOut.newLine();
    funcOut.appendLineStart("union jvalue locals[").appendInt(maxLocalsVal).append("];");
    funcOut.newLine();
    if (isStatic) {
        funcOut.emitLine("jclass _mj2x_owner_class = clazz;");
    }
    else {
        funcOut.appendLineStart("jclass _mj2x_owner_class = (jclass)")
            .append(trackLocalRefExpr("(*env)->GetObjectClass(env, this)")).append(";");
        funcOut.newLine();
    }
    funcOut.emitBlankLine();

    // Param init
    slot = 0;
    if (!isStatic) {
        funcOut.appendLineStart("locals[").appendInt(slot).append("].l = this;");
        funcOut.newLine();
        slot++;
    }
    paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut.appendLineStart("locals[").appendInt(slot).append("]")
            .append(stackValueAccessor(pk)).append(" = (")
            .append(narrowOnOperandStack(pk) ? "jint" : primitiveToJNIType(pk)).append(")p")
            .appendInt(paramIdx).append(";");
        funcOut.newLine();
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut.emitBlankLine();

    std::vector<CExceptionHandlerInfo> exceptionHandlers;
    for (auto* tcb : mn->tryCatchBlocks) {
        CExceptionHandlerInfo h;
        h.start     = tcb->start;
        h.end       = tcb->end;
        h.handler   = tcb->handler;
        h.catchType = tcb->type.to_std_string();
        exceptionHandlers.push_back(std::move(h));
    }
    std::vector<int>                             activeExceptionHandlers;
    std::unordered_map<std::string, std::string> dispatchLabelByKey;
    std::vector<CExceptionDispatchInfo>          exceptionDispatches;
    auto dispatchTargetFor = [&](const std::vector<int>& handlers) -> std::string {
        if (handlers.empty()) return {};
        auto key   = exceptionDispatchKey(handlers);
        auto found = dispatchLabelByKey.find(key);
        if (found != dispatchLabelByKey.end()) return found->second;
        std::string label = "_mj2x_ex_dispatch_" + std::to_string(exceptionDispatches.size());
        dispatchLabelByKey.emplace(key, label);
        exceptionDispatches.push_back({label, handlers});
        return label;
    };

    // Build deterministic per-method label index. Walk blocks in order; assign a
    // sequential index to every label the first time it is referenced, whether as a
    // LABEL definition or as a jump/switch target. The Label* is used only as a map
    // key for identity within this process — it never reaches the output, so two
    // independent CLI processes produce identical C labels (L0, L1, ...).
    impl_->labelIndex.clear();
    auto assignLabelIndex = [&](jbytecode::core::Label* lbl) {
        if (lbl == nullptr) return;
        impl_->labelIndex.emplace(lbl, static_cast<unsigned>(impl_->labelIndex.size()));
    };
    for (auto* block : blocks) {
        for (auto& node : block->nodes) {
            switch (node->kind) {
            case MIRKind::LABEL:
                assignLabelIndex(static_cast<const MIRLabel*>(node.get())->labelNode);
                break;
            case MIRKind::BRANCH: assignLabelIndex(static_cast<const MIRBranch*>(node.get())->targetLabel); break;
            case MIRKind::BRANCH_ZERO:
                assignLabelIndex(static_cast<const MIRBranchZero*>(node.get())->targetLabel);
                break;
            case MIRKind::UNCOND_BRANCH:
                assignLabelIndex(static_cast<const MIRUncondBranch*>(node.get())->targetLabel);
                break;
            case MIRKind::SWITCH: {
                const auto* sw = static_cast<const MIRSwitch*>(node.get());
                for (const auto& c : sw->cases) assignLabelIndex(c.targetLabel);
                assignLabelIndex(sw->defaultLabel);
                break;
            }
            default: break;
            }
        }
    }
    for (const auto& h : exceptionHandlers) {
        assignLabelIndex(h.handler);
    }

    // 每块入口栈深度（数据流）；无法收敛（非法字节码）则 fail-closed 保留整方法。
    std::vector<int> blockEntryDepths;
    std::vector<jbytecode::core::Label*> handlerLabels;
    for (const auto& h : exceptionHandlers) handlerLabels.push_back(h.handler);
    if (!impl_->computeBlockDepths(blocks, handlerLabels, blockEntryDepths)) {
        CA_LOGT_ERROR("stackc", "Skipping {}.{}{}: stack depth analysis failed\n", className.c_str(), methodName.c_str(), methodDesc.c_str());
        impl_->removeRegistration(cls, methodName, methodDesc);
        BackendResult failed;
        failed.kind = BackendResult::Kind::NONE;
        return failed;
    }
    std::unordered_map<int, int> blockIndexById;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        blockIndexById[blocks[bi]->id] = static_cast<int>(bi);
    }

    // Emit blocks
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        auto* block = blocks[bi];
        funcOut.appendLineStart("/* B").appendInt(static_cast<int64_t>(bi)).append(" */");
        funcOut.newLine();

        impl_->currentStackDepth = blockEntryDepths[bi];
        for (auto& node : block->nodes) {
            if (node->kind == MIRKind::LABEL) {
                auto* label = static_cast<const MIRLabel*>(node.get())->labelNode;
                for (int i = 0; i < static_cast<int>(exceptionHandlers.size()); ++i) {
                    if (exceptionHandlers[static_cast<size_t>(i)].end == label)
                        removeActiveHandler(activeExceptionHandlers, i);
                }
                for (int i = 0; i < static_cast<int>(exceptionHandlers.size()); ++i) {
                    if (exceptionHandlers[static_cast<size_t>(i)].start == label)
                        activeExceptionHandlers.push_back(i);
                }
            }
            std::string dispatchTarget;
            if (!activeExceptionHandlers.empty() && isProtectedByExceptionHandler(node.get()))
                dispatchTarget = dispatchTargetFor(activeExceptionHandlers);
            impl_->emitNodeWithExceptionDispatch(node.get(), funcOut, dispatchTarget);
        }
        funcOut.emitBlankLine();
    }

    for (const auto& dispatch : exceptionDispatches) {
        funcOut.append(dispatch.label).append(":");
        funcOut.newLine();
        funcOut.emitLine("{");
        funcOut.indent();
        funcOut.appendLineStart("jthrowable _mj2x_ex = (jthrowable)")
            .append(trackLocalRefExpr("(*env)->ExceptionOccurred(env)")).append(";");
        funcOut.newLine();
        funcOut.emitLine("(*env)->ExceptionClear(env);");
        funcOut.emitLine("if (_mj2x_ex == NULL) {");
        funcOut.indent();
        emitExceptionReturn(impl_->currentMethodDesc, funcOut);
        funcOut.dedent();
        funcOut.emitLine("}");
        for (int handlerIndex : dispatch.handlerIndices) {
            const auto& handler = exceptionHandlers[static_cast<size_t>(handlerIndex)];
            if (handler.catchType.empty()) {
                // handler 入口深度恒为 1（异常对象），写静态槽 s[0]。
                funcOut.emitLine("s[0].l = _mj2x_ex;");
                funcOut.appendLineStart("goto ")
                    .append(labelIdent(impl_->labelIndex.at(handler.handler))).append(";");
                funcOut.newLine();
            }
            else {
                funcOut.appendLineStart("if ((*env)->IsInstanceOf(env, _mj2x_ex, ")
                    .append(classExprForDescriptor(handler.catchType)).append(")) {");
                funcOut.newLine();
                funcOut.indent();
                funcOut.emitLine("s[0].l = _mj2x_ex;");
                funcOut.appendLineStart("goto ")
                    .append(labelIdent(impl_->labelIndex.at(handler.handler))).append(";");
                funcOut.newLine();
                funcOut.dedent();
                funcOut.emitLine("}");
            }
        }
        funcOut.emitLine("(*env)->Throw(env, _mj2x_ex);");
        emitExceptionReturn(impl_->currentMethodDesc, funcOut);
        funcOut.dedent();
        funcOut.emitLine("}");
        funcOut.emitBlankLine();
    }

    funcOut.emitLine("}");
    funcOut.emitBlankLine();
    impl_->source.append(funcOut.str());

    BackendResult result;
    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = funcOut.str();
    return result;
}

void StackCBackend::injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                                    const BackendResult& result)
{
    if (result.kind != BackendResult::Kind::C_SOURCE) return;

    impl_->protectClassInitializer(*this, clazz);

    auto* mn = method.methodNode;
    if (isInterfaceDefaultMethod(clazz, *mn)) {
        const auto className    = clazz.classNode->name.to_std_string();
        const auto functionName = impl_->findCompiledFunctionName(
            className, mn->name.to_std_string(), mn->desc.to_std_string());
        if (!functionName.empty()) {
            impl_->registerInterfaceDefaultProxy(clazz, *mn, functionName);
            const auto& proxies = impl_->interfaceDefaultProxiesByClass[className];
            impl_->rewriteInterfaceDefaultMethodToProxy(clazz, *mn, proxies.back());
        }
        return;
    }

    // Mark method as native and clear bytecode
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack   = 0;
    mn->maxLocals  = 0;
    clazz.modified = true;
}

ca::str::Utf8StringRef StackCBackend::Impl::stableUtf8(const std::string& value)
{
    stableUtf8Storage.push_back(value);
    return ca::str::Utf8StringRef::from_cstr(stableUtf8Storage.back().c_str());
}

jbytecode::tree::MethodNode* StackCBackend::Impl::findMethod(CompilerClass& clazz, const char* name,
                                                             const char* desc) const
{
    for (auto* method : clazz.classNode->methods) {
        if (method->name.to_std_string() != name) continue;
        if (desc != nullptr && method->desc.to_std_string() != desc) continue;
        return method;
    }
    return nullptr;
}

// ============================================================
// finalize：组装源文件
// ============================================================

void StackCBackend::finalize(const CompilerIndex& index)
{
    // Build final source: header + all compiled function code
    codegen::IncludeSet includes;
    includes.addSystem("jni.h");
    includes.addSystem("stdint.h");   // uint32_t/uint64_t（整数环绕运算）
    includes.addSystem("math.h");     // NAN/INFINITY（浮点常量）

    CSourceStream full;
    full.emitLine("/* Generated by mj2x StackCBackend */");
    full.append(includes.render());
    full.emitBlankLine();
    full.append(backend::stackCRuntimePrelude());
    impl_->emitStringPool(full);
    full.append(impl_->source.str());
    impl_->emitRegistrationCode(full);

    headerCode_ = "/* Generated by mj2x StackCBackend */\n" + includes.render() + "\n";
    sourceCode_ = full.str();

    // Write to file if output path is set
    if (!cOutputPath_.empty()) {
        std::ofstream ofs(cOutputPath_, std::ios::binary);
        if (ofs) {
            ofs << sourceCode_;
            CA_LOGT_INFO("stackc", "Written: {} ({} bytes)\n", cOutputPath_.c_str(),
                   sourceCode_.size());
        }
        else {
            CA_LOGT_ERROR("stackc", "cannot write {}\n", cOutputPath_.c_str());
        }
    }
}

// ============================================================
// 字符串池（UTF-16 code units，防 strings 扫描）
// ============================================================

size_t StackCBackend::Impl::stringPoolOffsetFor(const std::string& value)
{
    auto found = stringPoolOffsets.find(value);
    if (found != stringPoolOffsets.end()) return found->second;

    const auto units   = utf8ToUtf16Units(value);
    const auto offset  = stringPoolUnits.size();
    stringPoolOffsets.emplace(value, offset);
    stringPoolLengths.emplace(value, units.size());
    stringPoolUnits.insert(stringPoolUnits.end(), units.begin(), units.end());
    stringPoolUnits.push_back(0);
    return offset;
}

size_t StackCBackend::Impl::stringPoolLengthFor(const std::string& value)
{
    return stringPoolLengths.at(value);
}

void StackCBackend::Impl::emitStringPool(codegen::CSourceStream& out) const
{
    if (stringPoolUnits.empty()) return;

    out.appendLineStart("static const uint16_t _mj2x_string_pool[] = {");
    for (size_t i = 0; i < stringPoolUnits.size(); ++i) {
        if (i % 16 == 0) out.append("\n    ");
        out.appendInt(static_cast<int64_t>(stringPoolUnits[i]));
        if (i + 1 < stringPoolUnits.size()) out.append(", ");
    }
    out.append("\n};");
    out.newLine();
    out.emitBlankLine();
}

// ============================================================
// MIR Node → C code（dispatch；各发射器分散在 stackc_mir_*.cpp）
// ============================================================

void StackCBackend::Impl::emitNode(const MIRNode* node, codegen::CSourceStream& out)
{
    switch (node->kind) {
    case MIRKind::LABEL:
        out.append(labelIdent(labelIndex.at(static_cast<const MIRLabel*>(node)->labelNode)))
            .append(":");
        out.newLine();
        out.emitLine(";");
        break;
    case MIRKind::CONSTANT: emitConstant(static_cast<const MIRConstant*>(node), out); break;
    case MIRKind::LOCAL_LOAD: emitLocalLoad(static_cast<const MIRLocalLoad*>(node), out); break;
    case MIRKind::LOCAL_STORE: emitLocalStore(static_cast<const MIRLocalStore*>(node), out); break;
    case MIRKind::IINC: emitIinc(static_cast<const MIRIinc*>(node), out); break;
    case MIRKind::STACK_DUP: out.emitLine("stack[sp + 1] = stack[sp]; sp++;"); break;
    case MIRKind::STACK_DUP_X1:
        out.emitLine("{");
        out.indent();
        out.emitLine("union jvalue v1 = stack[sp];");
        out.emitLine("union jvalue v2 = stack[sp - 1];");
        out.emitLine("stack[sp - 1] = v1;");
        out.emitLine("stack[sp] = v2;");
        out.emitLine("stack[sp + 1] = v1;");
        out.emitLine("sp++;");
        out.dedent();
        out.emitLine("}");
        break;
    case MIRKind::STACK_POP: out.emitLine("sp--;"); break;
    case MIRKind::STACK_POP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        out.appendLineStart("sp -= ").appendInt(stackOpTopIsCategory2(op) ? 1 : 2).append(";");
        out.newLine();
        break;
    }
    case MIRKind::STACK_SWAP:
    {
        out.emitLine("{");
        out.indent();
        out.emitLine("union jvalue tmp = stack[sp];");
        out.emitLine("stack[sp] = stack[sp - 1];");
        out.emitLine("stack[sp - 1] = tmp;");
        out.dedent();
        out.emitLine("}");
        break;
    }
    case MIRKind::STACK_DUP2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out.emitLine("stack[sp + 1] = stack[sp]; sp++;");
        }
        else {
            out.emitLine("stack[sp + 1] = stack[sp - 1];");
            out.emitLine("stack[sp + 2] = stack[sp];");
            out.emitLine("sp += 2;");
        }
        break;
    }
    case MIRKind::STACK_DUP2_X1:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v2;");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        break;
    }
    case MIRKind::STACK_DUP2_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op)) {
            if (stackOpTopIsCategory2(op, 1)) {
                out.emitLine("{");
                out.indent();
                out.emitLine("union jvalue v1 = stack[sp];");
                out.emitLine("union jvalue v2 = stack[sp - 1];");
                out.emitLine("stack[sp - 1] = v1;");
                out.emitLine("stack[sp] = v2;");
                out.emitLine("stack[sp + 1] = v1;");
                out.emitLine("sp++;");
                out.dedent();
                out.emitLine("}");
            }
            else {
                out.emitLine("{");
                out.indent();
                out.emitLine("union jvalue v1 = stack[sp];");
                out.emitLine("union jvalue v2 = stack[sp - 1];");
                out.emitLine("union jvalue v3 = stack[sp - 2];");
                out.emitLine("stack[sp - 2] = v1;");
                out.emitLine("stack[sp - 1] = v3;");
                out.emitLine("stack[sp] = v2;");
                out.emitLine("stack[sp + 1] = v1;");
                out.emitLine("sp++;");
                out.dedent();
                out.emitLine("}");
            }
        }
        else if (stackOpTopIsCategory2(op, 2)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v2;");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("union jvalue v4 = stack[sp - 3];");
            out.emitLine("stack[sp - 3] = v2;");
            out.emitLine("stack[sp - 2] = v1;");
            out.emitLine("stack[sp - 1] = v4;");
            out.emitLine("stack[sp] = v3;");
            out.emitLine("stack[sp + 1] = v2;");
            out.emitLine("stack[sp + 2] = v1;");
            out.emitLine("sp += 2;");
            out.dedent();
            out.emitLine("}");
        }
        break;
    }
    case MIRKind::STACK_DUP_X2:
    {
        const auto* op = static_cast<const MIRStackOp*>(node);
        if (stackOpTopIsCategory2(op, 1)) {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("stack[sp - 1] = v1;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
        }
        else {
            out.emitLine("{");
            out.indent();
            out.emitLine("union jvalue v1 = stack[sp];");
            out.emitLine("union jvalue v2 = stack[sp - 1];");
            out.emitLine("union jvalue v3 = stack[sp - 2];");
            out.emitLine("stack[sp - 2] = v1;");
            out.emitLine("stack[sp - 1] = v3;");
            out.emitLine("stack[sp] = v2;");
            out.emitLine("stack[sp + 1] = v1;");
            out.emitLine("sp++;");
            out.dedent();
            out.emitLine("}");
        }
        break;
    }
    case MIRKind::BINARY_OP: emitBinaryOp(static_cast<const MIRBinaryOp*>(node), out); break;
    case MIRKind::UNARY_OP: emitUnaryOp(static_cast<const MIRUnaryOp*>(node), out); break;
    case MIRKind::CMP: emitCmp(static_cast<const MIRCmp*>(node), out); break;
    case MIRKind::CAST: emitCast(static_cast<const MIRCast*>(node), out); break;
    case MIRKind::BRANCH: emitBranch(static_cast<const MIRBranch*>(node), out); break;
    case MIRKind::BRANCH_ZERO: emitBranchZero(static_cast<const MIRBranchZero*>(node), out); break;
    case MIRKind::UNCOND_BRANCH:
        emitUncondBranch(static_cast<const MIRUncondBranch*>(node), out);
        break;
    case MIRKind::SWITCH: emitSwitch(static_cast<const MIRSwitch*>(node), out); break;
    case MIRKind::RETURN: out.emitMultiline(renderStackCSnippet(StackCSnippet::ReturnVoid, {})); break;
    case MIRKind::RETURN_VALUE:
        emitReturnValue(static_cast<const MIRReturnValue*>(node), out);
        break;
    case MIRKind::NEW_OBJECT: emitNewObject(static_cast<const MIRNewObject*>(node), out); break;
    case MIRKind::NEW_ARRAY: emitNewArray(static_cast<const MIRNewArray*>(node), out); break;
    case MIRKind::MULTI_NEW_ARRAY:
        emitMultiNewArray(static_cast<const MIRMultiNewArray*>(node), out);
        break;
    case MIRKind::ARRAY_LENGTH:
        emitArrayLength(static_cast<const MIRArrayLength*>(node), out);
        break;
    case MIRKind::ARRAY_LOAD: emitArrayLoad(static_cast<const MIRArrayLoad*>(node), out); break;
    case MIRKind::ARRAY_STORE: emitArrayStore(static_cast<const MIRArrayStore*>(node), out); break;
    case MIRKind::FIELD_GET: emitFieldGet(static_cast<const MIRFieldGet*>(node), out); break;
    case MIRKind::FIELD_PUT: emitFieldPut(static_cast<const MIRFieldPut*>(node), out); break;
    case MIRKind::STATIC_GET: emitStaticGet(static_cast<const MIRStaticGet*>(node), out); break;
    case MIRKind::STATIC_PUT: emitStaticPut(static_cast<const MIRStaticPut*>(node), out); break;
    case MIRKind::INVOKE: emitInvoke(static_cast<const MIRInvoke*>(node), out); break;
    case MIRKind::INVOKE_DYNAMIC:
        emitInvokeDynamic(static_cast<const MIRInvokeDynamic*>(node), out);
        break;
    case MIRKind::CHECK_CAST: emitCheckCast(static_cast<const MIRCheckCast*>(node), out); break;
    case MIRKind::INSTANCE_OF: emitInstanceOf(static_cast<const MIRInstanceOf*>(node), out); break;
    case MIRKind::MONITOR_ENTER:
    case MIRKind::MONITOR_EXIT: emitMonitor(static_cast<const MIRMonitor*>(node), out); break;
    case MIRKind::THROW: emitThrow(static_cast<const MIRThrow*>(node), out); break;
    default: break;
    }
}

void StackCBackend::Impl::emitNodeWithExceptionDispatch(const MIRNode*      node,
                                                        codegen::CSourceStream& out,
                                                        const std::string&  dispatchTarget)
{
    currentExceptionDispatchTarget = dispatchTarget;
    CSourceStream tmp;
    emitNode(node, tmp);
    currentExceptionDispatchTarget.clear();
    if (!dispatchTarget.empty() && isProtectedByExceptionHandler(node)) {
        tmp.appendLineStart("if ((*env)->ExceptionCheck(env)) goto ")
            .append(dispatchTarget).append(";");
        tmp.newLine();
    }
    // 栈访问静态化：stack[sp±k] → s[<编译期深度>]，并统计净效应推进深度。
    std::string text = tmp.str();
    const int   effect = staticizeStackText(text, currentStackDepth);
    currentStackDepth += effect;
    out.append(text);
}

}   // namespace mj2x


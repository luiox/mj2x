#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"

#include "mj2x/codegen/c_source.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mj2x {

namespace {

using codegen::CSourceStream;

// ---------------------------------------------------------------------------
// 数值编码 helpers：把字节/字拆成 C 字面量。
// ---------------------------------------------------------------------------

/// 把字节流编码成 C 数组初始化列表（"0xNN, 0xNN, ..."），每行 16 字节。
std::string emitByteArray(const std::vector<uint8_t>& bytes, const std::string& indent)
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0 && i != 0) out += "\n" + indent;
        else if (i != 0) out += " ";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02x,", static_cast<unsigned>(bytes[i]));
        out += buf;
    }
    return out;
}

/// 把 u32 数组编码成 C 数组初始化列表。
std::string emitU32Array(const std::vector<uint32_t>& vals, const std::string& indent)
{
    std::string out;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i % 8 == 0 && i != 0) out += "\n" + indent;
        else if (i != 0) out += " ";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%uu,", vals[i]);
        out += buf;
    }
    return out;
}

// ===========================================================================
// Handler 逻辑片段：每个 opcode 一段 C 代码（读 operand、执行、推进 pc）。
//
// 关键设计：handler 片段假设 dispatcher 已声明局部变量 code/pc/regs/status/targets，
// 并以 break/goto mj2x_vm_done 结束。5 种 dispatcher 形态共享这些 handler 片段，
// 只在外层调度结构（switch / 函数指针表 / goto 表 / if 链 / state 平坦化）上不同。
//
// handler 读 u16 operand 的内联表达式统一为：
//   (unsigned)code[pc] | ((unsigned)code[pc+1] << 8)
// 配合 pc += 2 推进。这让 handler 片段对 dispatcher 形态无感。
// ===========================================================================

/// COPY: regs[dest] = regs[src]
const char* kHandlerCopy =
    "            unsigned char type = code[pc++]; (void)type;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            regs[dest] = regs[src];\n";

/// UNARY: 取负/位取反
/// 注意 I32（type 0）在 int32 域环绕：取负/取反结果必须截断回 int32（符号扩展），
/// 否则 INT32_MIN 取负得到 +2147483648，与后续截断存储的值比较时出错。
const char* kHandlerUnary =
    "            unsigned char op = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0) {\n"
    "                if (op == 0) regs[dest].i = (int32_t)(0u - (uint32_t)regs[src].i);\n"
    "                else regs[dest].i = (int32_t)(~(uint32_t)regs[src].i);\n"
    "            } else if (type == 1) {\n"
    "                if (op == 0) regs[dest].i = (int64_t)(0u - (uint64_t)regs[src].i);\n"
    "                else regs[dest].i = ~regs[src].i;\n"
    "            } else { regs[dest].f = -regs[src].f; }\n";

/// UNARY 等价变形（Phase 3 handler 形态）：位取反用 XOR 全 1 表达（与 ~ 语义一致）。
const char* kHandlerUnaryAlt =
    "            unsigned char op = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0) {\n"
    "                if (op == 0) regs[dest].i = (int32_t)(0u - (uint32_t)regs[src].i);\n"
    "                else regs[dest].i = (int32_t)((uint32_t)regs[src].i ^ 0xFFFFFFFFu);\n"
    "            } else if (type == 1) {\n"
    "                if (op == 0) regs[dest].i = (int64_t)(0u - (uint64_t)regs[src].i);\n"
    "                else regs[dest].i = regs[src].i ^ -1ll;\n"
    "            } else { regs[dest].f = -regs[src].f; }\n";

/// BINARY: 算术/位运算（含 JVM 整数环绕、除零、UShr 语义）
const char* kHandlerBinary =
    "            unsigned char op = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int lhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int rhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0 || type == 1) {\n"
    "                int64_t a = regs[lhs].i, b = regs[rhs].i; int64_t r = 0;\n"
    "                switch (op) {\n"
    "                case 0: r = (int64_t)((uint64_t)a + (uint64_t)b); break;\n"
    "                case 1: r = (int64_t)((uint64_t)a - (uint64_t)b); break;\n"
    "                case 2: r = (int64_t)((uint64_t)a * (uint64_t)b); break;\n"
    "                case 3: if (b==0){status=1;goto mj2x_vm_done;} r = (type==0)?mj2x_vm_div_i32((int32_t)a,(int32_t)b):mj2x_vm_div_i64(a,b); break;\n"
    "                case 4: if (b==0){status=1;goto mj2x_vm_done;} r = (type==0)?mj2x_vm_rem_i32((int32_t)a,(int32_t)b):mj2x_vm_rem_i64(a,b); break;\n"
    "                case 5: r = a & b; break;\n"
    "                case 6: r = a | b; break;\n"
    "                case 7: r = a ^ b; break;\n"
    "                case 8: r = (type==0)?(int32_t)((uint32_t)a<<(b&31)):(int64_t)((uint64_t)a<<(b&63)); break;\n"
    "                case 9: r = (type==0)?((int32_t)a>>(b&31)):(a>>(b&63)); break;\n"
    "                case 10: r = (type==0)?(int32_t)((uint32_t)a>>(b&31)):(int64_t)((uint64_t)a>>(b&63)); break;\n"
    "                }\n"
    "                regs[dest].i = (type==0) ? (int32_t)r : r;\n"
    "            } else {\n"
    "                double a = regs[lhs].f, b = regs[rhs].f, r = 0;\n"
    "                switch (op) {\n"
    "                case 0: r = a + b; break;\n"
    "                case 1: r = a - b; break;\n"
    "                case 2: r = a * b; break;\n"
    "                case 3: r = a / b; break;\n"
    "                case 4: r = (type==2)?fmodf((float)a,(float)b):fmod(a,b); break;\n"
    "                default: break;\n"
    "                }\n"
    "                regs[dest].f = r;\n"
    "            }\n";

/// COMPARE: 产生 0/1 的 int 条件
const char* kHandlerCompare =
    "            unsigned char pred = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int lhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int rhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            int result = 0;\n"
    "            if (type == 0 || type == 1) {\n"
    "                int64_t a = regs[lhs].i, b = regs[rhs].i;\n"
    "                switch (pred) { case 0: result=(a==b); break; case 1: result=(a!=b); break;\n"
    "                case 2: result=(a<b); break; case 3: result=(a<=b); break;\n"
    "                case 4: result=(a>b); break; case 5: result=(a>=b); break; }\n"
    "            } else {\n"
    "                double a = regs[lhs].f, b = regs[rhs].f;\n"
    "                switch (pred) { case 0: result=(a==b); break; case 1: result=(a!=b); break;\n"
    "                case 2: result=(a<b); break; case 3: result=(a<=b); break;\n"
    "                case 4: result=(a>b); break; case 5: result=(a>=b); break; }\n"
    "            }\n"
    "            regs[dest].i = result;\n";

/// COMPARE 等价变形（Phase 3 handler 形态）：Eq/Ne 用否定表达（== 语义不变）。
const char* kHandlerCompareAlt =
    "            unsigned char pred = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int lhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int rhs  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            int result = 0;\n"
    "            if (type == 0 || type == 1) {\n"
    "                int64_t a = regs[lhs].i, b = regs[rhs].i;\n"
    "                switch (pred) { case 0: result=!(a!=b); break; case 1: result=!(a==b); break;\n"
    "                case 2: result=(a<b); break; case 3: result=(a<=b); break;\n"
    "                case 4: result=(a>b); break; case 5: result=(a>=b); break; }\n"
    "            } else {\n"
    "                double a = regs[lhs].f, b = regs[rhs].f;\n"
    "                switch (pred) { case 0: result=!(a!=b); break; case 1: result=!(a==b); break;\n"
    "                case 2: result=(a<b); break; case 3: result=(a<=b); break;\n"
    "                case 4: result=(a>b); break; case 5: result=(a>=b); break; }\n"
    "            }\n"
    "            regs[dest].i = result;\n";

/// CONVERT: 数值宽窄/整浮转换（向零截断，JVM 语义）
/// 浮点→整数用饱和 helper（NaN→0、超界→min/max），C 原生 cast 对 NaN/超界是 UB。
const char* kHandlerConvert =
    "            unsigned char from = code[pc++];\n"
    "            unsigned char to = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (from == 0 || from == 1) {\n"
    "                int64_t v = regs[src].i;\n"
    "                if (to == 0) regs[dest].i = (int32_t)v;\n"
    "                else if (to == 1) regs[dest].i = v;\n"
    "                else if (to == 2) regs[dest].f = (float)v;\n"
    "                else regs[dest].f = (double)v;\n"
    "            } else {\n"
    "                double v = regs[src].f;\n"
    "                if (to == 0) regs[dest].i = mj2x_vm_f2i32(v);\n"
    "                else if (to == 1) regs[dest].i = mj2x_vm_f2i64(v);\n"
    "                else if (to == 2) regs[dest].f = (float)v;\n"
    "                else regs[dest].f = v;\n"
    "            }\n";

/// JUMP: pc = targets[target]
const char* kHandlerJump =
    "            unsigned int target = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            pc = targets[target];\n";

/// BRANCH: 条件跳转
const char* kHandlerBranch =
    "            unsigned int cond = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int t = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int f = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            pc = targets[regs[cond].i ? t : f];\n";

/// RETURN: has_value ? *retval = regs[src] : (void)0
const char* kHandlerReturn =
    "            unsigned char has_value = code[pc++];\n"
    "            unsigned int src = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (has_value) *retval = regs[src];\n"
    "            status = 0; goto mj2x_vm_done;\n";

/// SUPER_INC: regs[dest] = regs[src] + 1（super-operator，Binary(x,1,Add) 折叠）
const char* kHandlerSuperInc =
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0) regs[dest].i = (int32_t)((uint32_t)regs[src].i + 1u);\n"
    "            else if (type == 1) regs[dest].i = regs[src].i + 1;\n"
    "            else regs[dest].f = regs[src].f + 1.0;\n";

/// SUPER_DEC: regs[dest] = regs[src] - 1（super-operator，Binary(x,1,Sub) 折叠）
const char* kHandlerSuperDec =
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0) regs[dest].i = (int32_t)((uint32_t)regs[src].i - 1u);\n"
    "            else if (type == 1) regs[dest].i = regs[src].i - 1;\n"
    "            else regs[dest].f = regs[src].f - 1.0;\n";

/// opcode → handler 片段表（按 VmOpcode 语义索引，实例字节由 VmInstance 映射）。
/// bodyAlt 为等价变形形态（Phase 3 handler 形态维度），可为 nullptr（无变形）。
struct HandlerEntry
{
    VmOpcode    opcode;
    const char* name;
    const char* body;
    const char* bodyAlt = nullptr;
};

const HandlerEntry kHandlers[] = {
    {VmOpcode::Copy,     "COPY",       kHandlerCopy},
    {VmOpcode::Unary,    "UNARY",      kHandlerUnary, kHandlerUnaryAlt},
    {VmOpcode::Binary,   "BINARY",     kHandlerBinary},
    {VmOpcode::Compare,  "COMPARE",    kHandlerCompare, kHandlerCompareAlt},
    {VmOpcode::Convert,  "CONVERT",    kHandlerConvert},
    {VmOpcode::Jump,     "JUMP",       kHandlerJump},
    {VmOpcode::Branch,   "BRANCH",     kHandlerBranch},
    {VmOpcode::Return,   "RETURN",     kHandlerReturn},
    {VmOpcode::SuperInc, "SUPER_INC",  kHandlerSuperInc},
    {VmOpcode::SuperDec, "SUPER_DEC",  kHandlerSuperDec},
};
constexpr size_t kHandlerCount = sizeof(kHandlers) / sizeof(kHandlers[0]);

/// 按 VmOpcode 查找 handler 片段（不在支持集内返回 nullptr）。
const HandlerEntry* findHandler(VmOpcode op)
{
    for (size_t i = 0; i < kHandlerCount; ++i) {
        if (kHandlers[i].opcode == op) return &kHandlers[i];
    }
    return nullptr;
}

/// 取 handler body：实例按 seed 确定性选择等价变形形态（若有）。
const char* handlerBodyFor(const VmInstance& instance, const HandlerEntry* entry)
{
    if (entry->bodyAlt != nullptr && instance.usesAltHandlerForm(entry->opcode)) {
        return entry->bodyAlt;
    }
    return entry->body;
}

/// 把某语义的匹配表达式拼成 `opcode == b1 || opcode == b2` 链。
/// drift 开启时 opcode 已由 unmap 表解回 canonical，只需单字节比较。
std::string emitOpcodeByteOrChain(const VmInstance& instance, VmOpcode op)
{
    if (instance.driftEnabled()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "opcode == 0x%02x",
                      static_cast<unsigned>(instance.opcodeFor(op)));
        return buf;
    }
    std::string out;
    bool        first = true;
    for (uint8_t b : instance.opcodeBytesFor(op)) {
        if (!first) out += " || ";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "opcode == 0x%02x", static_cast<unsigned>(b));
        out += buf;
        first = false;
    }
    return out;
}

/// 发射一个 handler 的 case 块：canonical + 全部 ALT 别名共享同一 body。
/// 输出形如：
///   case 0xNN /*NAME*/:
///   case 0xMM /*NAME*/: {
///       <body>
///       break;
///   }
/// drift 开启时 unmap 已把原始字节解回 canonical，只发 canonical 一个 case。
std::string emitHandlerCase(const VmInstance& instance, const HandlerEntry* entry,
                            const char* indent)
{
    std::string s;
    if (instance.driftEnabled()) {
        char head[64];
        std::snprintf(head, sizeof(head), "%scase 0x%02x /*%s*/:\n", indent,
                      static_cast<unsigned>(instance.opcodeFor(entry->opcode)), entry->name);
        s += head;
    } else {
        for (uint8_t b : instance.opcodeBytesFor(entry->opcode)) {
            char head[64];
            std::snprintf(head, sizeof(head), "%scase 0x%02x /*%s*/:\n", indent,
                          static_cast<unsigned>(b), entry->name);
            s += head;
        }
    }
    s += std::string(indent) + "{\n";
    s += handlerBodyFor(instance, entry);
    s += std::string(indent) + "    break;\n";
    s += std::string(indent) + "}\n";
    return s;
}

// ===========================================================================
// 5 种 dispatcher 形态：共享 handler 片段，调度结构各异。
// ===========================================================================

/// 公共局部变量声明（所有 dispatcher 都在函数体开头需要这些）。
std::string emitCommonDecls(const std::string& progName, const std::string& targetsName)
{
    return "    const unsigned char* code = " + progName + ";\n"
           "    const unsigned int* targets = " + targetsName + ";\n"
           "    unsigned int pc = targets[0];\n"
           "    unsigned int status = 0;\n"
           "    mj2x_vm_slot* regs = slots;\n";
}

/// 生成 per-variant unmap 表（文件级 static const，drift 开启时在 exec 前发射）。
/// 表 d 把变体 d 下的原始字节解回 canonical 字节；未映射字节填 0xFF（走 default）。
std::string emitDriftUnmapTables(const VmInstance& instance, const std::string& tag)
{
    if (!instance.driftEnabled()) return "";
    std::string s;
    for (int d = 0; d < instance.driftVariants(); ++d) {
        char head[128];
        std::snprintf(head, sizeof(head), "static const unsigned char mj2x_vm_unmap_%s_%d[256] = {\n",
                      tag.c_str(), d);
        s += head;
        for (int i = 0; i < 256; ++i) {
            if (i % 16 == 0 && i != 0) s += "\n";
            else if (i != 0) s += " ";
            char buf[8];
            std::snprintf(buf, sizeof(buf), "0x%02x,",
                          static_cast<unsigned>(instance.unmapByte(static_cast<uint8_t>(i), d)));
            s += buf;
        }
        s += "\n};\n";
    }
    return s;
}

/// 生成 unmap 表的选择表达式：按运行时变体序号 d 选出对应表指针。
/// 形如 `(d == 0 ? T_0 : (d == 1 ? T_1 : T_2))`（嵌套右结合）。
std::string emitUnmapSelectExpr(const VmInstance& instance, const std::string& tag,
                                const std::string& dExpr)
{
    const int D = instance.driftVariants();
    std::string s;
    for (int d = 0; d < D; ++d) {
        if (d > 0) s += " : ";
        if (d + 1 < D) s += "(" + dExpr + " == " + std::to_string(d) + " ? ";
        s += "mj2x_vm_unmap_" + tag + "_" + std::to_string(d);
    }
    for (int d = 0; d + 1 < D; ++d) s += ")";
    return s;
}

/// 生成 dispatcher 的取码语句：drift 开启时按当前 pc 算变体序号再过 unmap 表；
/// 关闭时保持 `unsigned char opcode = code[pc++];` 原样（零回归）。
std::string emitOpcodeFetch(const VmInstance& instance, const std::string& tag,
                            const std::string& indent)
{
    if (!instance.driftEnabled()) {
        return indent + "unsigned char opcode = code[pc++];\n";
    }
    std::string s;
    s += indent + "unsigned int d = (unsigned)(pc / " + std::to_string(instance.driftPeriod()) +
         ") % " + std::to_string(instance.driftVariants()) + ";\n";
    s += indent + "unsigned char opcode = " +
         emitUnmapSelectExpr(instance, tag, "d") + "[code[pc++]];\n";
    return s;
}

/// 生成 Halt 的 case 头：drift 开启时只发 canonical（unmap 已解回）；
/// 关闭时覆盖 canonical + 全部别名（防御性停机面）。
std::string emitHaltCaseHeads(const VmInstance& instance, const std::string& indent)
{
    std::string s;
    if (instance.driftEnabled()) {
        char head[64];
        std::snprintf(head, sizeof(head), "%scase 0x%02x /*HALT*/:\n", indent.c_str(),
                      static_cast<unsigned>(instance.opcodeFor(VmOpcode::Halt)));
        s += head;
        return s;
    }
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Halt)) {
        char head[64];
        std::snprintf(head, sizeof(head), "%scase 0x%02x /*HALT*/:\n", indent.c_str(),
                      static_cast<unsigned>(b));
        s += head;
    }
    return s;
}

/// 形态 0：SwitchTable —— 经典 switch/case。
std::string emitSwitchDispatcher(const VmInstance& instance, const std::string& progName,
                                 const std::string& targetsName, const std::string& tag)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    for (;;) {\n";
    s += emitOpcodeFetch(instance, tag, "        ");
    s += "        switch (opcode) {\n";
    // handler 顺序随实例洗牌（handlerOrder），每个语义的 canonical+别名共享 body。
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        s += emitHandlerCase(instance, entry, "        ");
    }
    // Halt 哨兵：实例映射的 halt 字节全集（canonical + 别名）。落到 Halt 或
    // 任何未映射字节都走防御停机。
    s += emitHaltCaseHeads(instance, "        ");
    s += "        default: status = (status!=0)?status:2u /* InvalidBytecode */; goto mj2x_vm_done;\n";
    s += "        }\n";
    s += "    }\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
    return s;
}

/// 形态 1：FuncPtrTable —— 函数指针表分发。
///
/// @details
/// 每个 handler 包成独立的 static void 函数（签名统一，操作共享 ctx），
/// dispatcher 用 handlers[opcode](...) 间接调用。这是结构性差异：逆向者看到的是
/// 一组函数指针表 + 间接调用，不是 switch 跳表。
///
/// 共享上下文 struct 避免 handler 函数参数列表过长。
///
/// C 兼容性：ctx struct / typedef / dispatch body 必须是**文件级**定义——
/// MSVC 不支持嵌套函数（在函数体内定义函数是语法错误），而 per-opcode 独立
/// 函数又需要 C++ 或 GCC nested function。MVP 采用「单一 dispatch body + 函数
/// 指针表别名」折中：表里每项指向同一文件级函数，但函数指针表本身仍是间接
/// 调用结构。Phase 2 引入 per-opcode 独立函数（C++ 模式）。

/// 文件级 prelude：ctx struct + typedef + dispatch body 函数（在 exec 函数之前发射）。
std::string emitFuncPtrFilePrelude(const VmInstance& instance, const std::string& tag)
{
    std::string ctxType = "mj2x_vm_ctx_" + tag;
    std::string s;
    // C 模式必须 typedef 才能用裸类型名（`struct X {...}; X v;` 是 C++ 写法）。
    s += "struct " + ctxType + " { const unsigned char* code; const unsigned int* targets; "
         "unsigned int pc; unsigned int status; mj2x_vm_slot* regs; mj2x_vm_slot* retval; };\n";
    s += "typedef struct " + ctxType + " " + ctxType + ";\n";
    s += "static void mj2x_vm_dispatch_body_" + tag + "(" + ctxType + "* ctx) {\n";
    s += "    const unsigned char* code = ctx->code;\n";
    s += "    const unsigned int* targets = ctx->targets;\n";
    s += "    unsigned int pc = ctx->pc;\n";
    s += "    unsigned int status = ctx->status;\n";
    s += "    mj2x_vm_slot* regs = ctx->regs;\n";
    s += "    mj2x_vm_slot* retval = ctx->retval;\n";
    s += "    (void)targets;\n";
    s += "    (void)status;\n";
    s += "    (void)retval;\n";
    s += emitOpcodeFetch(instance, tag, "    ");
    s += "    switch (opcode) {\n";
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        s += emitHandlerCase(instance, entry, "    ");
    }
    s += "    default: ctx->status = 2; /* InvalidBytecode */ ctx->pc = pc; return;\n";
    s += "    }\n";
    // RETURN handler 与 BINARY 除零分支都经 goto mj2x_vm_done 退出：此标签必须存在，
    // 否则生成的 C 引用未定义 label 编译失败。
    s += "mj2x_vm_done:\n";
    s += "    ctx->pc = pc; ctx->status = status;\n";
    s += "    return;\n";
    s += "}\n";
    return s;
}

/// 函数体：ctx 初始化 + 函数指针表 + 分发循环（在 exec 函数内发射）。
std::string emitFuncPtrDispatcher(const VmInstance& instance, const std::string& progName,
                                  const std::string& targetsName, const std::string& tag)
{
    std::string ctxType = "mj2x_vm_ctx_" + tag;
    std::string s;
    s += "    " + ctxType + " _ctx;\n";
    s += "    _ctx.code = " + progName + ";\n";
    s += "    _ctx.targets = " + targetsName + ";\n";
    s += "    _ctx.pc = _ctx.targets[0];\n";
    s += "    _ctx.status = 0;\n";
    s += "    _ctx.regs = slots;\n";
    s += "    _ctx.retval = retval;\n";
    s += "    " + ctxType + "* ctx = &_ctx;\n";
    s += "    typedef void (*mj2x_vm_handler_" + tag + ")(" + ctxType + "*);\n";
    // 函数指针表（每项指向同一 dispatch_body，表的存在本身是间接调用的逆向特征）。
    // Halt 全字节槽 = 0（空指针），未映射字节也默认 0 → 走 InvalidBytecode 分支。
    // drift 开启时 unmap 已解回 canonical，只填 canonical 槽。
    s += "    static const mj2x_vm_handler_" + tag + " mj2x_vm_table_" + tag + "[256] = {\n";
    bool first = true;
    if (instance.driftEnabled()) {
        char e[64];
        std::snprintf(e, sizeof(e), "        [0x%02x] = 0",
                      static_cast<unsigned>(instance.opcodeFor(VmOpcode::Halt)));
        s += e;
        first = false;
    } else {
        for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Halt)) {
            char e[64];
            std::snprintf(e, sizeof(e), "%s[0x%02x] = 0", first ? "        " : ", ",
                          static_cast<unsigned>(b));
            s += e;
            first = false;
        }
    }
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        if (instance.driftEnabled()) {
            char e[96];
            std::snprintf(e, sizeof(e), ", [0x%02x] = mj2x_vm_dispatch_body_%s",
                          static_cast<unsigned>(instance.opcodeFor(op)), tag.c_str());
            s += e;
            continue;
        }
        for (uint8_t b : instance.opcodeBytesFor(op)) {
            char e[96];
            std::snprintf(e, sizeof(e), ", [0x%02x] = mj2x_vm_dispatch_body_%s",
                          static_cast<unsigned>(b), tag.c_str());
            s += e;
        }
    }
    s += "\n    };\n";
    s += "    for (;;) {\n";
    if (instance.driftEnabled()) {
        s += "        unsigned int d = (unsigned)(ctx->pc / " +
             std::to_string(instance.driftPeriod()) + ") % " +
             std::to_string(instance.driftVariants()) + ";\n";
        s += "        unsigned char opcode = " +
             emitUnmapSelectExpr(instance, tag, "d") + "[ctx->code[ctx->pc]];\n";
    } else {
        s += "        unsigned char opcode = ctx->code[ctx->pc];\n";
    }
    s += "        mj2x_vm_handler_" + tag + " h = mj2x_vm_table_" + tag + "[opcode];\n";
    s += "        if (!h) { ctx->status = 2; /* InvalidBytecode */ break; }\n";
    // RETURN 在 dispatch body 里经 goto mj2x_vm_done 返回，此时 ctx->pc 已越过
    // RETURN 指令（含 operand），不能靠 code[ctx->pc-1] 判断（那是 operand 末字节）。
    // 在调用前记录 opcode 位置，用 code[op_pos] 是否属于 Return 字节全集判断
    // （drift 开启时用 op_pos 的变体解回 canonical 后比对）。
    s += "        unsigned int op_pos = ctx->pc;\n";
    s += "        h(ctx);\n";
    if (instance.driftEnabled()) {
        s += "        unsigned int d2 = (unsigned)(op_pos / " +
             std::to_string(instance.driftPeriod()) + ") % " +
             std::to_string(instance.driftVariants()) + ";\n";
        s += "        unsigned char _op = " +
             emitUnmapSelectExpr(instance, tag, "d2") + "[ctx->code[op_pos]];\n";
        char e[64];
        std::snprintf(e, sizeof(e), "        int _is_ret = (_op == 0x%02x);\n",
                      static_cast<unsigned>(instance.opcodeFor(VmOpcode::Return)));
        s += e;
    } else {
        s += "        unsigned char _op = ctx->code[op_pos];\n";
        s += "        int _is_ret = 0;\n";
        for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Return)) {
            char e[48];
            std::snprintf(e, sizeof(e), "        if (_op == 0x%02x) _is_ret = 1;\n",
                          static_cast<unsigned>(b));
            s += e;
        }
    }
    s += "        if (ctx->status != 0 || _is_ret) break;\n";
    s += "    }\n";
    s += "    return ctx->status;\n";
    return s;
}

/// 形态 2：ComputedGoto —— GCC "Labels as Values" 扩展（MSVC 不支持，调用方需守卫）。
std::string emitComputedGotoDispatcher(const VmInstance& instance, const std::string& progName,
                                       const std::string& targetsName, const std::string& tag)
{
    std::string s = emitCommonDecls(progName, targetsName);
    // 所有槽默认指向 halt（防御非法 opcode：goto *NULL 是 UB），显式条目覆盖。
    // Halt 别名槽同样指向 halt；drift 开启时 unmap 已解回 canonical，只填 canonical 槽。
    s += "    static const void* mj2x_vm_labels[256] = { [0 ... 255] = &&mj2x_vm_halt";
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        if (instance.driftEnabled()) {
            char e[64];
            std::snprintf(e, sizeof(e), ", [0x%02x] = &&mj2x_vm_op_%02x",
                          static_cast<unsigned>(instance.opcodeFor(op)),
                          static_cast<unsigned>(instance.opcodeFor(op)));
            s += e;
            continue;
        }
        for (uint8_t b : instance.opcodeBytesFor(op)) {
            char e[64];
            std::snprintf(e, sizeof(e), ", [0x%02x] = &&mj2x_vm_op_%02x",
                          static_cast<unsigned>(b), static_cast<unsigned>(b));
            s += e;
        }
    }
    s += "\n    };\n";
    if (instance.driftEnabled()) {
        s += "    unsigned int d = (unsigned)(pc / " + std::to_string(instance.driftPeriod()) +
             ") % " + std::to_string(instance.driftVariants()) + ";\n";
        s += "    goto *mj2x_vm_labels[" + emitUnmapSelectExpr(instance, tag, "d") +
             "[code[pc++]]];\n";
    } else {
        s += "    goto *mj2x_vm_labels[code[pc++]];\n";
    }
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        if (instance.driftEnabled()) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "mj2x_vm_op_%02x:\n",
                          static_cast<unsigned>(instance.opcodeFor(op)));
            s += "    {\n";
            s += lbl;
            s += handlerBodyFor(instance, entry);
            s += "        { unsigned int nd = (unsigned)(pc / " +
                 std::to_string(instance.driftPeriod()) + ") % " +
                 std::to_string(instance.driftVariants()) +
                 "; goto *mj2x_vm_labels[" + emitUnmapSelectExpr(instance, tag, "nd") +
                 "[code[pc++]]]; }\n";
            s += "    }\n";
            continue;
        }
        for (uint8_t b : instance.opcodeBytesFor(op)) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "mj2x_vm_op_%02x:\n",
                          static_cast<unsigned>(b));
            s += "    {\n";
            s += lbl;
            s += handlerBodyFor(instance, entry);
            s += "        goto *mj2x_vm_labels[code[pc++]];\n";
            s += "    }\n";
        }
    }
    s += "mj2x_vm_halt:\n";
    s += "    status = (status!=0)?status:2u /* InvalidBytecode */;\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
    return s;
}

/// 形态 3：IfNest —— 线性 if/else if 链（无跳表）。
std::string emitIfNestDispatcher(const VmInstance& instance, const std::string& progName,
                                 const std::string& targetsName, const std::string& tag)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    for (;;) {\n";
    s += emitOpcodeFetch(instance, tag, "        ");
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        s += "        if (" + emitOpcodeByteOrChain(instance, op) + ") {\n";
        s += handlerBodyFor(instance, entry);
        s += "        } else ";
    }
    s += "{ status = (status!=0)?status:2u /* InvalidBytecode */; goto mj2x_vm_done; }\n";
    s += "    }\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
    return s;
}

/// 形态 4：StateDispatcher —— state 寄存器 + 控制流平坦化。
///
/// @details
/// 把 dispatch loop 包进一个 while(state==RUNNING) 的平坦结构，opcode 决定
/// 下一次迭代走哪个 case。这复刻 RegC CFF 的 state-dispatcher 思路
/// （reg_transform_passes.hpp 的 ControlFlowFlattening）。
std::string emitStateDispatcher(const VmInstance& instance, const std::string& progName,
                                 const std::string& targetsName, const std::string& tag)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    unsigned int state = 1; /* 1=RUNNING 0=DONE */\n";
    s += "    while (state) {\n";
    s += emitOpcodeFetch(instance, tag, "        ");
    s += "        switch (opcode) {\n";
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        s += emitHandlerCase(instance, entry, "        ");
    }
    s += emitHaltCaseHeads(instance, "        ");
    s += "        default: state = 0; status = (status!=0)?status:2u /* InvalidBytecode */; break;\n";
    s += "        }\n";
    s += "    }\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
    return s;
}

/// 注入 bogus handler：文件级 static 函数，dispatch 结构不引用它们
/// （只污染静态分析，运行时永不命中）。内容由实例 seed 确定性生成。
std::string emitBogusHandlers(const VmInstance& instance, const std::string& tag)
{
    if (instance.bogusHandlers() <= 0) return "";
    auto splitmix = [](uint64_t& x) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    std::string s;
    for (int i = 0; i < instance.bogusHandlers(); ++i) {
        uint64_t state = instance.instanceSeed() ^ (static_cast<uint64_t>(i + 1) * 0xABCDEF0123456789ull);
        const uint64_t a = splitmix(state);
        const uint64_t b = splitmix(state);
        const uint64_t c = splitmix(state);
        char name[64];
        std::snprintf(name, sizeof(name), "mj2x_vm_bogus_%s_%d", tag.c_str(), i);
        s += std::string("static unsigned int ") + name +
             "(mj2x_vm_slot* _slots, mj2x_vm_slot* _ret) {\n";
        s += "    (void)_slots; (void)_ret;\n";
        s += "    /* 不可达假 handler：不存在于任何 dispatch 结构，仅污染静态分析 */\n";
        s += "    unsigned int _acc = " + std::to_string(a) + "u;\n";
        s += "    _acc = (_acc * " + std::to_string(b) + "u) ^ " + std::to_string(c) + "u;\n";
        s += "    return _acc & 0u;\n";
        s += "}\n";
    }
    return s;
}

}   // namespace

// ===========================================================================
// VmInterpreterEmitter 公开入口
// ===========================================================================

std::string VmInterpreterEmitter::emitRuntimePrelude()
{
    // mj2x_vm_slot 联合体：寄存器槽。i 用于 I32/I64（I32 写入时由 handler 截断到 32 位），
    // f 用于 F32/F64（同理用 double 统一）。div/rem helper 精确复刻 JVM 语义
    // （INT_MIN / -1 = INT_MIN 不抛、除零报 status=1）。
    return std::string(
        "/* ===== VMC4 runtime prelude (auto-generated) ===== */\n"
        "typedef union mj2x_vm_slot {\n"
        "    int64_t i;\n"
        "    double  f;\n"
        "} mj2x_vm_slot;\n"
        "\n"
        "/* JVM 整数除法语义：INT_MIN / -1 不抛（回绕到 INT_MIN），其余交给硬件。 */\n"
        "static int32_t mj2x_vm_div_i32(int32_t a, int32_t b) {\n"
        "    if (a == INT32_MIN && b == -1) return INT32_MIN;\n"
        "    return a / b;\n"
        "}\n"
        "static int64_t mj2x_vm_div_i64(int64_t a, int64_t b) {\n"
        "    if (a == INT64_MIN && b == -1) return INT64_MIN;\n"
        "    return a / b;\n"
        "}\n"
        "static int32_t mj2x_vm_rem_i32(int32_t a, int32_t b) {\n"
        "    if (a == INT32_MIN && b == -1) return 0;\n"
        "    return a % b;\n"
        "}\n"
        "static int64_t mj2x_vm_rem_i64(int64_t a, int64_t b) {\n"
        "    if (a == INT64_MIN && b == -1) return 0;\n"
        "    return a % b;\n"
        "}\n"
        "/* JVM f2i/f2l 语义：NaN -> 0，向零截断；截断后超界才饱和（C cast 对超界/NaN 是 UB）。\n"
        " * 饱和点取 (double)INTxx_MAX + 1.0 = 2^31/2^63（(double)INT64_MAX 因 53 位尾数\n"
        " * 本就舍入为 2^63），不用硬编码魔数。 */\n"
        "static int32_t mj2x_vm_f2i32(double v) {\n"
        "    if (v != v) return 0;\n"
        "    if (v >= (double)INT32_MAX + 1.0) return INT32_MAX;\n"
        "    if (v < (double)INT32_MIN) return INT32_MIN;\n"
        "    return (int32_t)v;\n"
        "}\n"
        "static int64_t mj2x_vm_f2i64(double v) {\n"
        "    if (v != v) return 0;\n"
        "    if (v >= (double)INT64_MAX) return INT64_MAX;\n"
        "    if (v < (double)INT64_MIN) return INT64_MIN;\n"
        "    return (int64_t)v;\n"
        "}\n"
        "/* ===== end VMC4 runtime prelude ===== */\n\n");
}

std::string VmInterpreterEmitter::emit(const VmProgram& program, const VmInstance& instance,
                                       const std::string& methodTag)
{
    const std::string tag         = methodTag.empty() ? instance.instanceTag() : methodTag;
    const std::string progName    = "mj2x_vm_code_" + tag;
    const std::string targetsName = "mj2x_vm_targets_" + tag;
    const std::string constsName  = "mj2x_vm_consts_" + tag;
    const std::string funcName    = "mj2x_vm_exec_" + tag;

    CSourceStream out;

    // --- 常量数组（type + slot + 位模式）---
    {
        CSourceStream line;
        line.append("static const struct { unsigned char type; unsigned short slot; unsigned long long bits; } ");
        line.append(constsName);
        // 空常量池时用 {0}（MSVC C 模式不接受空初始化列表 {}）。
        if (program.constantPool.empty()) {
            line.append("[] = {0};");
            out.emitLine(line.build());
        } else {
            line.append("[] = {");
            out.emitLine(line.build());
            {
                CSourceStream::IndentGuard g(out);
                for (size_t i = 0; i < program.constantPool.size(); ++i) {
                    const auto& c = program.constantPool[i];
                    unsigned long long bits = 0;
                    if (c.type == VmType::F32 || c.type == VmType::F64) {
                        if (c.type == VmType::F32) {
                            float f = static_cast<float>(c.floatValue);
                            uint32_t b; std::memcpy(&b, &f, 4);
                            bits = b;
                        } else {
                            double d = c.floatValue;
                            uint64_t b; std::memcpy(&b, &d, 8);
                            bits = b;
                        }
                    } else {
                        bits = static_cast<unsigned long long>(c.intBits);
                    }
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "    {%u, %uu, 0x%016llxULL},",
                                  static_cast<unsigned>(c.type), c.destSlot,
                                  static_cast<unsigned long long>(bits));
                    out.emitLine(buf);
                }
            }
            out.emitLine("};");
        }
        out.emitBlankLine();
    }

    // --- blockTargets 数组 ---
    {
        CSourceStream line;
        line.append("static const unsigned int ");
        line.append(targetsName);
        line.append("[] = {");
        out.emitLine(line.build());
        CSourceStream body;
        body.append(emitU32Array(program.blockTargets, "    "));
        out.emitRaw(body.build().to_std_string());
        out.emitLine("");
        out.emitLine("};");
        out.emitBlankLine();
    }

    // --- bytecode 数组 ---
    {
        CSourceStream line;
        line.append("static const unsigned char ");
        line.append(progName);
        line.append("[] = {");
        out.emitLine(line.build());
        CSourceStream body;
        body.append(emitByteArray(program.bytecodes, "    "));
        out.emitRaw(body.build().to_std_string());
        out.emitLine("");
        out.emitLine("};");
        out.emitBlankLine();
    }

    // drift 的 per-variant unmap 表是文件级定义，必须在 exec 函数之前发射。
    if (instance.driftEnabled()) {
        out.emitRaw(emitDriftUnmapTables(instance, tag));
        out.emitBlankLine();
    }

    // FuncPtrTable 的 ctx struct / dispatch body 是文件级定义，必须在 exec 函数
    // 之前发射（MSVC 不支持嵌套函数）。其他 dispatcher 无文件级结构。
    if (instance.dispatcherKind() == VmDispatcherKind::FuncPtrTable) {
        out.emitRaw(emitFuncPtrFilePrelude(instance, tag));
        out.emitBlankLine();
    }

    // --- 解释器入口函数（按 dispatcherKind 选择形态）---
    {
        CSourceStream sig;
        sig.append("static unsigned int ");
        sig.append(funcName);
        sig.append("(mj2x_vm_slot* slots, mj2x_vm_slot* retval) {");
        out.emitLine(sig.build());
        out.emitLine("(void)retval;");

        std::string body;
        switch (instance.dispatcherKind()) {
        case VmDispatcherKind::SwitchTable:
            body = emitSwitchDispatcher(instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::FuncPtrTable:
            body = emitFuncPtrDispatcher(instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::ComputedGoto:
            body = emitComputedGotoDispatcher(instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::IfNest:
            body = emitIfNestDispatcher(instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::StateDispatcher:
            body = emitStateDispatcher(instance, progName, targetsName, tag);
            break;
        }
        out.emitRaw(body);

        out.emitLine("}");
        out.emitBlankLine();
    }

    // bogus handler 注入（文件级，dispatch 结构不引用，只污染静态分析）。
    const std::string bogus = emitBogusHandlers(instance, tag);
    if (!bogus.empty()) {
        out.emitRaw(bogus);
        out.emitBlankLine();
    }

    return out.str();
}

}   // namespace mj2x

#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"

#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "libca/str/format.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mj2x {

namespace {

using codegen::CSourceStream;
using ca::str::format_std;
using ca::str::format_to;

// ---------------------------------------------------------------------------
// 数值编码 helpers：把字节/字拆成 C 字面量。
// 这些是「数据格式化」片段（数组初始化列表），返回 std::string 供 emitRaw 嵌入，
// 不是源码结构拼接（codegen_style.md §3 禁止的是手拼缩进语句，非数据片段）。
// ---------------------------------------------------------------------------

/// 把字节流编码成 C 数组初始化列表（"0xNN, 0xNN, ..."），每行 16 字节。
std::string emitByteArray(const std::vector<uint8_t>& bytes, const std::string& indent)
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0 && i != 0) out += "\n" + indent;
        else if (i != 0) out += " ";
        format_to(out, "0x{:02x},", static_cast<unsigned>(bytes[i]));
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
        format_to(out, "{}u,", vals[i]);
    }
    return out;
}

// ===========================================================================
// Handler 逻辑片段：每个 opcode 一段 C 代码（读 operand、执行、推进 pc）。
//
// 这些片段是「操作码语义模板」（类似 raw-string runtime template），自带固定
// 缩进，经 CSourceStream::emitRaw 原样嵌入 case 体（emitRaw 的设计用途即
// 「pre-formatted blocks where the caller controls whitespace」，c_source.hpp:99）。
// 片段假设 dispatcher 已声明局部变量 code/pc/regs/status/targets，并以
// break/goto mj2x_vm_done 结束。5 种 dispatcher 共享这些片段。
//
// handler 读 u16 operand 的内联表达式统一为：
//   (unsigned)code[pc] | ((unsigned)code[pc+1] << 8)
// 配合 pc += 2 推进。
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

/// CALL: 经函数指针表分派到 per-callsite JNI helper（Phase 2.0）。
/// argBuf[32] 承载参数寄存器索引（argCount 上限 32，见 encodeCall）。
/// handler 引用 exec 参数 env 与 call_table（SwitchTable 等 dispatcher 内直接可见；
/// FuncPtrTable 经 ctx 字段传入）。
const char* kHandlerCall =
    "            unsigned int callIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            call_table[callIdx](env, regs, recv, argBuf, argc, dest);\n";

/// LOAD_FIELD（Phase 2.A）：经函数指针表分派到 per-site field load helper。
/// 结构与 CALL 对称：读 siteIdx + recv + dest + argc + argBuf，调 load_field_table[siteIdx]。
/// LoadField 的 argc 恒为 0（field load 无额外参数），argBuf 循环不执行。
/// 引用 exec 参数 load_field_table（SwitchTable 等直接可见；FuncPtrTable 经 ctx 字段）。
const char* kHandlerLoadField =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            load_field_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

/// STORE_FIELD（Phase 2.A）：经函数指针表分派到 per-site field store helper。
/// recv=receiver（static=0），dest 恒为 0（store 无结果），argc=1，argBuf[0]=valueIdx。
const char* kHandlerStoreField =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            store_field_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

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
    {VmOpcode::Call,     "CALL",       kHandlerCall},
    {VmOpcode::LoadField,  "LOAD_FIELD",  kHandlerLoadField},
    {VmOpcode::StoreField, "STORE_FIELD", kHandlerStoreField},
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

/// 把某语义的匹配表达式拼成 `opcode == b1 || opcode == b2` 链（IfNest 用）。
/// 返回表达式片段（非源码结构拼接），drift 开启时 opcode 已由 unmap 表解回 canonical。
std::string emitOpcodeByteOrChain(const VmInstance& instance, VmOpcode op)
{
    if (instance.driftEnabled()) {
        return format_std("opcode == 0x{:02x}", static_cast<unsigned>(instance.opcodeFor(op)));
    }
    std::string out;
    bool        first = true;
    for (uint8_t b : instance.opcodeBytesFor(op)) {
        if (!first) format_to(out, " || ");
        format_to(out, "opcode == 0x{:02x}", static_cast<unsigned>(b));
        first = false;
    }
    return out;
}

// ===========================================================================
// 5 种 dispatcher 形态：共享 handler 片段，调度结构各异。
// 所有 emit* 函数写入 CSourceStream&（流贯穿，缩进由调用上下文继承），
// 不再用 std::string s; s += "..." 手拼。
// ===========================================================================

/// 发射一个 handler 的 case 块：canonical + 全部 ALT 别名共享同一 body。
/// 多个 case 头（每字节一个）后跟一个 { handler body + break; } 块。
/// drift 开启时 unmap 已把原始字节解回 canonical，只发 canonical 一个 case。
void emitHandlerCase(CSourceStream& out, const VmInstance& instance, const HandlerEntry* entry)
{
    auto emitHead = [&](uint8_t byte) {
        out.emitLine(format_std("case 0x{:02x} /*{}*/:", byte, entry->name));
    };
    if (instance.driftEnabled()) {
        emitHead(instance.opcodeFor(entry->opcode));
    } else {
        for (uint8_t b : instance.opcodeBytesFor(entry->opcode)) emitHead(b);
    }
    out.emitLine("{");
    {
        CSourceStream::IndentGuard g(out);
        out.emitRaw(handlerBodyFor(instance, entry));
        out.emitLine("break;");
    }
    out.emitLine("}");
}

/// 公共局部变量声明（所有 dispatcher 都在函数体开头需要这些）。
void emitCommonDecls(CSourceStream& out, const std::string& progName, const std::string& targetsName)
{
    out.emitLine(format_std("const unsigned char* code = {};", progName));
    out.emitLine(format_std("const unsigned int* targets = {};", targetsName));
    out.emitLine("unsigned int pc = targets[0];");
    out.emitLine("unsigned int status = 0;");
    out.emitLine("mj2x_vm_slot* regs = slots;");
}

/// 生成 per-variant unmap 表（文件级 static const，drift 开启时在 exec 前发射）。
/// 表 d 把变体 d 下的原始字节解回 canonical 字节；未映射字节填 0xFF（走 default）。
void emitDriftUnmapTables(CSourceStream& out, const VmInstance& instance, const std::string& tag)
{
    if (!instance.driftEnabled()) return;
    for (int d = 0; d < instance.driftVariants(); ++d) {
        out.emitLine(format_std(
            "static const unsigned char mj2x_vm_unmap_{}_{}[256] = {{", tag, d));
        {
            CSourceStream::IndentGuard g(out);
            std::string body;
            for (int i = 0; i < 256; ++i) {
                if (i % 16 == 0 && i != 0) body += "\n";
                else if (i != 0) body += " ";
                format_to(body, "0x{:02x},",
                          static_cast<unsigned>(instance.unmapByte(static_cast<uint8_t>(i), d)));
            }
            out.emitRaw(body);
        }
        out.emitLine("");
        out.emitLine("};");
    }
}

/// 生成 unmap 表的选择表达式：按运行时变体序号 d 选出对应表指针。
/// 形如 `(d == 0 ? T_0 : (d == 1 ? T_1 : T_2))`（嵌套右结合）。
/// 返回表达式片段（嵌入语句，非结构拼接）。
std::string emitUnmapSelectExpr(const VmInstance& instance, const std::string& tag,
                                const std::string& dExpr)
{
    const int D = instance.driftVariants();
    std::string s;
    for (int d = 0; d < D; ++d) {
        if (d > 0) format_to(s, " : ");
        if (d + 1 < D) format_to(s, "({} == {} ? ", dExpr, d);
        format_to(s, "mj2x_vm_unmap_{}_{}", tag, d);
    }
    for (int d = 0; d + 1 < D; ++d) s += ")";
    return s;
}

/// 生成 dispatcher 的取码语句：drift 开启时按当前 pc 算变体序号再过 unmap 表；
/// 关闭时保持 `unsigned char opcode = code[pc++];` 原样（零回归）。
void emitOpcodeFetch(CSourceStream& out, const VmInstance& instance, const std::string& tag)
{
    if (!instance.driftEnabled()) {
        out.emitLine("unsigned char opcode = code[pc++];");
        return;
    }
    out.emitLine(format_std("unsigned int d = (unsigned)(pc / {}) % {};",
                            instance.driftPeriod(), instance.driftVariants()));
    out.emitLine(format_std("unsigned char opcode = {}[code[pc++]];",
                            emitUnmapSelectExpr(instance, tag, "d")));
}

/// 生成 Halt 的 case 头：drift 开启时只发 canonical（unmap 已解回）；
/// 关闭时覆盖 canonical + 全部别名（防御性停机面）。
void emitHaltCaseHeads(CSourceStream& out, const VmInstance& instance)
{
    auto emitHead = [&](uint8_t byte) {
        out.emitLine(format_std("case 0x{:02x} /*HALT*/:", byte));
    };
    if (instance.driftEnabled()) {
        emitHead(instance.opcodeFor(VmOpcode::Halt));
        return;
    }
    for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Halt)) emitHead(b);
}

/// 形态 0：SwitchTable —— 经典 switch/case。
void emitSwitchDispatcher(CSourceStream& out, const VmInstance& instance,
                          const std::string& progName, const std::string& targetsName,
                          const std::string& tag)
{
    emitCommonDecls(out, progName, targetsName);
    out.emitLine("for (;;) {");
    {
        CSourceStream::IndentGuard g(out);
        emitOpcodeFetch(out, instance, tag);
        out.emitLine("switch (opcode) {");
        {
            CSourceStream::IndentGuard g2(out);
            // handler 顺序随实例洗牌（handlerOrder），每个语义的 canonical+别名共享 body。
            for (const auto& op : instance.handlerOrder()) {
                const HandlerEntry* entry = findHandler(op);
                if (entry == nullptr) continue;
                emitHandlerCase(out, instance, entry);
            }
            // Halt 哨兵：实例映射的 halt 字节全集（canonical + 别名）。落到 Halt 或
            // 任何未映射字节都走防御停机。
            emitHaltCaseHeads(out, instance);
            out.emitLine("default: status = (status!=0)?status:2u /* InvalidBytecode */; goto mj2x_vm_done;");
        }
        out.emitLine("}");
    }
    out.emitLine("}");
    out.label_("mj2x_vm_done");
    out.emitLine("return status;");
}

/// 形态 1：FuncPtrTable —— 函数指针表分发。
///
/// @details
/// 每个 handler 包成独立的 static void 函数（签名统一，操作共享 ctx），
/// dispatcher 用 handlers[opcode](...) 间接调用。这是结构性差异：逆向者看到的是
/// 一组函数指针表 + 间接调用，不是 switch 跳表。
///
/// C 兼容性：ctx struct / typedef / dispatch body 必须是**文件级**定义——
/// MSVC 不支持嵌套函数。MVP 采用「单一 dispatch body + 函数指针表别名」折中：
/// 表里每项指向同一文件级函数，但函数指针表本身仍是间接调用结构。

/// 文件级 prelude：ctx struct + typedef + dispatch body 函数（在 exec 函数之前发射）。
void emitFuncPtrFilePrelude(CSourceStream& out, const VmInstance& instance, const std::string& tag)
{
    const std::string ctxType = format_std("mj2x_vm_ctx_{}", tag);
    out.emitLine(format_std(
        "struct {} {{ const unsigned char* code; const unsigned int* targets; "
        "unsigned int pc; unsigned int status; JNIEnv* env; const mj2x_vm_call_fn* call_table; "
        "const mj2x_vm_call_fn* load_field_table; const mj2x_vm_call_fn* store_field_table; "
        "mj2x_vm_slot* regs; mj2x_vm_slot* retval; }};", ctxType));
    out.emitLine(format_std("typedef struct {} {};", ctxType, ctxType));
    out.add_func(format_std("static void mj2x_vm_dispatch_body_{}({}* ctx)", tag, ctxType),
                 [&](CSourceStream& f) {
        f.emitLine("const unsigned char* code = ctx->code;");
        f.emitLine("const unsigned int* targets = ctx->targets;");
        f.emitLine("unsigned int pc = ctx->pc;");
        f.emitLine("unsigned int status = ctx->status;");
        f.emitLine("JNIEnv* env = ctx->env;");
        f.emitLine("const mj2x_vm_call_fn* call_table = ctx->call_table;");
        f.emitLine("const mj2x_vm_call_fn* load_field_table = ctx->load_field_table;");
        f.emitLine("const mj2x_vm_call_fn* store_field_table = ctx->store_field_table;");
        f.emitLine("mj2x_vm_slot* regs = ctx->regs;");
        f.emitLine("mj2x_vm_slot* retval = ctx->retval;");
        f.emitLine("(void)targets;");
        f.emitLine("(void)status;");
        f.emitLine("(void)retval;");
        f.emitLine("(void)load_field_table;");
        f.emitLine("(void)store_field_table;");
        emitOpcodeFetch(f, instance, tag);
        f.emitLine("switch (opcode) {");
        {
            CSourceStream::IndentGuard g(f);
            for (const auto& op : instance.handlerOrder()) {
                const HandlerEntry* entry = findHandler(op);
                if (entry == nullptr) continue;
                emitHandlerCase(f, instance, entry);
            }
            f.emitLine("default: ctx->status = 2; /* InvalidBytecode */ ctx->pc = pc; return;");
        }
        f.emitLine("}");
        // RETURN handler 与 BINARY 除零分支都经 goto mj2x_vm_done 退出：此标签必须存在，
        // 否则生成的 C 引用未定义 label 编译失败。
        f.label_("mj2x_vm_done");
        f.emitLine("ctx->pc = pc; ctx->status = status;");
        f.emitLine("return;");
    });
}

/// 函数体：ctx 初始化 + 函数指针表 + 分发循环（在 exec 函数内发射）。
void emitFuncPtrDispatcher(CSourceStream& out, const VmInstance& instance,
                           const std::string& progName, const std::string& targetsName,
                           const std::string& tag)
{
    const std::string ctxType = format_std("mj2x_vm_ctx_{}", tag);
    out.emitLine(format_std("{} _ctx;", ctxType));
    out.emitLine(format_std("_ctx.code = {};", progName));
    out.emitLine(format_std("_ctx.targets = {};", targetsName));
    out.emitLine("_ctx.pc = _ctx.targets[0];");
    out.emitLine("_ctx.status = 0;");
    out.emitLine("_ctx.env = env;");
    out.emitLine("_ctx.call_table = call_table;");
    out.emitLine("_ctx.load_field_table = load_field_table;");
    out.emitLine("_ctx.store_field_table = store_field_table;");
    out.emitLine("_ctx.regs = slots;");
    out.emitLine("_ctx.retval = retval;");
    out.emitLine(format_std("{}* ctx = &_ctx;", ctxType));
    out.emitLine(format_std("typedef void (*mj2x_vm_handler_{})({}*);", tag, ctxType));
    // 函数指针表（每项指向同一 dispatch_body，表的存在本身是间接调用的逆向特征）。
    // Halt 全字节槽 = 0（空指针），未映射字节也默认 0 → 走 InvalidBytecode 分支。
    // drift 开启时 unmap 已解回 canonical，只填 canonical 槽。
    out.emitLine(format_std("static const mj2x_vm_handler_{} mj2x_vm_table_{}[256] = {{", tag, tag));
    {
        CSourceStream::IndentGuard g(out);
        bool first = true;
        auto emitHaltSlot = [&](uint8_t byte) {
            out.emitLine(format_std("{}[0x{:02x}] = 0", first ? "" : ", ", byte));
            first = false;
        };
        if (instance.driftEnabled()) {
            emitHaltSlot(instance.opcodeFor(VmOpcode::Halt));
        } else {
            for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Halt)) emitHaltSlot(b);
        }
        auto emitHandlerSlot = [&](uint8_t byte) {
            out.emitLine(format_std("{}[0x{:02x}] = mj2x_vm_dispatch_body_{}",
                                    first ? "" : ", ", byte, tag));
            first = false;
        };
        for (const auto& op : instance.handlerOrder()) {
            if (instance.driftEnabled()) {
                emitHandlerSlot(instance.opcodeFor(op));
                continue;
            }
            for (uint8_t b : instance.opcodeBytesFor(op)) emitHandlerSlot(b);
        }
    }
    out.emitLine("};");
    out.emitLine("for (;;) {");
    {
        CSourceStream::IndentGuard g(out);
        if (instance.driftEnabled()) {
            out.emitLine(format_std("unsigned int d = (unsigned)(ctx->pc / {}) % {};",
                                    instance.driftPeriod(), instance.driftVariants()));
            out.emitLine(format_std("unsigned char opcode = {}[ctx->code[ctx->pc]];",
                                    emitUnmapSelectExpr(instance, tag, "d")));
        } else {
            out.emitLine("unsigned char opcode = ctx->code[ctx->pc];");
        }
        out.emitLine(format_std("mj2x_vm_handler_{} h = mj2x_vm_table_{}[opcode];", tag, tag));
        out.emitLine("if (!h) { ctx->status = 2; /* InvalidBytecode */ break; }");
        // RETURN 在 dispatch body 里经 goto mj2x_vm_done 返回，此时 ctx->pc 已越过
        // RETURN 指令（含 operand），不能靠 code[ctx->pc-1] 判断（那是 operand 末字节）。
        // 在调用前记录 opcode 位置，用 code[op_pos] 是否属于 Return 字节全集判断
        // （drift 开启时用 op_pos 的变体解回 canonical 后比对）。
        out.emitLine("unsigned int op_pos = ctx->pc;");
        out.emitLine("h(ctx);");
        if (instance.driftEnabled()) {
            out.emitLine(format_std("unsigned int d2 = (unsigned)(op_pos / {}) % {};",
                                    instance.driftPeriod(), instance.driftVariants()));
            out.emitLine(format_std("unsigned char _op = {}[ctx->code[op_pos]];",
                                    emitUnmapSelectExpr(instance, tag, "d2")));
            out.emitLine(format_std("int _is_ret = (_op == 0x{:02x});",
                                    static_cast<unsigned>(instance.opcodeFor(VmOpcode::Return))));
        } else {
            out.emitLine("unsigned char _op = ctx->code[op_pos];");
            out.emitLine("int _is_ret = 0;");
            for (uint8_t b : instance.opcodeBytesFor(VmOpcode::Return)) {
                out.emitLine(format_std("if (_op == 0x{:02x}) _is_ret = 1;", b));
            }
        }
        out.emitLine("if (ctx->status != 0 || _is_ret) break;");
    }
    out.emitLine("}");
    out.emitLine("return ctx->status;");
}

/// 形态 2：ComputedGoto —— GCC "Labels as Values" 扩展（MSVC 不支持，调用方需守卫）。
void emitComputedGotoDispatcher(CSourceStream& out, const VmInstance& instance,
                                const std::string& progName, const std::string& targetsName,
                                const std::string& tag)
{
    emitCommonDecls(out, progName, targetsName);
    // 所有槽默认指向 halt（防御非法 opcode：goto *NULL 是 UB），显式条目覆盖。
    out.emitLine("static const void* mj2x_vm_labels[256] = { [0 ... 255] = &&mj2x_vm_halt");
    {
        CSourceStream::IndentGuard g(out);
        auto emitLabel = [&](uint8_t byte) {
            out.emitLine(format_std(", [0x{:02x}] = &&mj2x_vm_op_{:02x}", byte, byte));
        };
        for (const auto& op : instance.handlerOrder()) {
            const HandlerEntry* entry = findHandler(op);
            if (entry == nullptr) continue;
            if (instance.driftEnabled()) {
                emitLabel(instance.opcodeFor(op));
                continue;
            }
            for (uint8_t b : instance.opcodeBytesFor(op)) emitLabel(b);
        }
    }
    out.emitLine("};");
    if (instance.driftEnabled()) {
        out.emitLine(format_std("unsigned int d = (unsigned)(pc / {}) % {};",
                                instance.driftPeriod(), instance.driftVariants()));
        out.emitLine(format_std("goto *mj2x_vm_labels[{}[code[pc++]]];",
                                emitUnmapSelectExpr(instance, tag, "d")));
    } else {
        out.emitLine("goto *mj2x_vm_labels[code[pc++]];");
    }
    for (const auto& op : instance.handlerOrder()) {
        const HandlerEntry* entry = findHandler(op);
        if (entry == nullptr) continue;
        auto emitOpBlock = [&](uint8_t byte) {
            out.label_(format_std("mj2x_vm_op_{:02x}:", byte));
            out.emitLine("{");
            {
                CSourceStream::IndentGuard g(out);
                out.emitRaw(handlerBodyFor(instance, entry));
                if (instance.driftEnabled()) {
                    out.emitLine(format_std(
                        "unsigned int nd = (unsigned)(pc / {}) % {}; goto *mj2x_vm_labels[{}[code[pc++]]];",
                        instance.driftPeriod(), instance.driftVariants(),
                        emitUnmapSelectExpr(instance, tag, "nd")));
                } else {
                    out.emitLine("goto *mj2x_vm_labels[code[pc++]];");
                }
            }
            out.emitLine("}");
        };
        if (instance.driftEnabled()) {
            emitOpBlock(instance.opcodeFor(op));
            continue;
        }
        for (uint8_t b : instance.opcodeBytesFor(op)) emitOpBlock(b);
    }
    out.label_("mj2x_vm_halt");
    out.emitLine("status = (status!=0)?status:2u /* InvalidBytecode */;");
    out.label_("mj2x_vm_done");
    out.emitLine("return status;");
}

/// 形态 3：IfNest —— 线性 if/else if 链（无跳表）。
void emitIfNestDispatcher(CSourceStream& out, const VmInstance& instance,
                          const std::string& progName, const std::string& targetsName,
                          const std::string& tag)
{
    emitCommonDecls(out, progName, targetsName);
    out.emitLine("for (;;) {");
    {
        CSourceStream::IndentGuard g(out);
        emitOpcodeFetch(out, instance, tag);
        for (const auto& op : instance.handlerOrder()) {
            const HandlerEntry* entry = findHandler(op);
            if (entry == nullptr) continue;
            out.emitLine(format_std("if ({}) {{", emitOpcodeByteOrChain(instance, op)));
            {
                CSourceStream::IndentGuard g2(out);
                out.emitRaw(handlerBodyFor(instance, entry));
            }
            out.emitLine("} else");
        }
        out.emitLine("{ status = (status!=0)?status:2u /* InvalidBytecode */; goto mj2x_vm_done; }");
    }
    out.emitLine("}");
    out.label_("mj2x_vm_done");
    out.emitLine("return status;");
}

/// 形态 4：StateDispatcher —— state 寄存器 + 控制流平坦化。
///
/// @details
/// 把 dispatch loop 包进一个 while(state==RUNNING) 的平坦结构，opcode 决定
/// 下一次迭代走哪个 case。这复刻 RegC CFF 的 state-dispatcher 思路
/// （reg_transform_passes.hpp 的 ControlFlowFlattening）。
void emitStateDispatcher(CSourceStream& out, const VmInstance& instance,
                         const std::string& progName, const std::string& targetsName,
                         const std::string& tag)
{
    emitCommonDecls(out, progName, targetsName);
    out.emitLine("unsigned int state = 1; /* 1=RUNNING 0=DONE */");
    out.emitLine("while (state) {");
    {
        CSourceStream::IndentGuard g(out);
        emitOpcodeFetch(out, instance, tag);
        out.emitLine("switch (opcode) {");
        {
            CSourceStream::IndentGuard g2(out);
            for (const auto& op : instance.handlerOrder()) {
                const HandlerEntry* entry = findHandler(op);
                if (entry == nullptr) continue;
                emitHandlerCase(out, instance, entry);
            }
            emitHaltCaseHeads(out, instance);
            out.emitLine("default: state = 0; status = (status!=0)?status:2u /* InvalidBytecode */; break;");
        }
        out.emitLine("}");
    }
    out.emitLine("}");
    out.label_("mj2x_vm_done");
    out.emitLine("return status;");
}

/// 注入 bogus handler：文件级 static 函数，dispatch 结构不引用它们
/// （只污染静态分析，运行时永不命中）。内容由实例 seed 确定性生成。
void emitBogusHandlers(CSourceStream& out, const VmInstance& instance, const std::string& tag)
{
    if (instance.bogusHandlers() <= 0) return;
    auto splitmix = [](uint64_t& x) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    for (int i = 0; i < instance.bogusHandlers(); ++i) {
        uint64_t state = instance.instanceSeed() ^ (static_cast<uint64_t>(i + 1) * 0xABCDEF0123456789ull);
        const uint64_t a = splitmix(state);
        const uint64_t b = splitmix(state);
        const uint64_t c = splitmix(state);
        out.add_func(format_std("static unsigned int mj2x_vm_bogus_{}_{}"
                                "(mj2x_vm_slot* _slots, mj2x_vm_slot* _ret)", tag, i),
                     [&](CSourceStream& f) {
            f.emitLine("(void)_slots; (void)_ret;");
            f.comment("不可达假 handler：不存在于任何 dispatch 结构，仅污染静态分析");
            f.emitLine(format_std("unsigned int _acc = {}u;", a));
            f.emitLine(format_std("_acc = (_acc * {}u) ^ {}u;", b, c));
            f.emitLine("return _acc & 0u;");
        });
    }
}

// ===========================================================================
// Call 基础设施（Phase 2.0）：per-callsite JNI helper + 函数指针表。
//
// methodRefs 非空时，为每个 VmMethodRef 发射一个 mj2x_vm_call_<tag>_<idx> helper
// （懒缓存 class/methodID，按 argKinds marshalling 参数，按 retKind 调 JNI 并写回），
// 再发射 mj2x_vm_call_table_<tag>[] 函数指针表。解释器 CALL handler 经该表分派。
// ===========================================================================

/// VmJniKind → jvalue 联合体字段名（"i"/"j"/"f"/"d"/"l"）。
const char* jvalueField(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Int32: return "i";
    case VmJniKind::Int64: return "j";
    case VmJniKind::Float: return "f";
    case VmJniKind::Double: return "d";
    case VmJniKind::Ref: return "l";
    default: return "i";
    }
}

/// VmJniKind → 寄存器槽读取字段（regs[idx].<field>）。
const char* slotReadField(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Float:
    case VmJniKind::Double: return ".f";
    case VmJniKind::Ref: return ".ref";
    default: return ".i";
    }
}

/// VmJniKind → jvalue 写入时的中间 cast（保证 JNI 侧拿到正确位宽）。
const char* jvalueCast(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Int32: return "(jint)";
    case VmJniKind::Int64: return "(jlong)";
    case VmJniKind::Float: return "(jfloat)";
    case VmJniKind::Double: return "(jdouble)";
    case VmJniKind::Ref: return "(jobject)";
    default: return "(jint)";
    }
}

/// VmJniKind → JNI Call 函数后缀（不含 Static/前缀），如 "Int"/"Void"/"Object"。
const char* jniCallTypeSuffix(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Void: return "Void";
    case VmJniKind::Int32: return "Int";
    case VmJniKind::Int64: return "Long";
    case VmJniKind::Float: return "Float";
    case VmJniKind::Double: return "Double";
    case VmJniKind::Ref: return "Object";
    default: return "Void";
    }
}

/// VmJniKind → JNI 返回值 C 类型名（jint/jlong/...）。
const char* jniReturnType(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Int32: return "jint";
    case VmJniKind::Int64: return "jlong";
    case VmJniKind::Float: return "jfloat";
    case VmJniKind::Double: return "jdouble";
    case VmJniKind::Ref: return "jobject";
    default: return "void";
    }
}

/// VmJniKind → 写回 regs[dest] 的 cast（slot .i/.f/.ref 位宽匹配；Double 无需 cast）。
const char* resultWriteCast(VmJniKind k)
{
    switch (k) {
    case VmJniKind::Int32: return "(int64_t)";
    case VmJniKind::Int64: return "(int64_t)";
    case VmJniKind::Float: return "(double)";
    case VmJniKind::Ref: return "(void*)";
    default: return "";
    }
}

/// @brief 发射 per-callsite JNI helper + call_table（methodRefs 非空时）。
///
/// @details
/// 每个 VmMethodRef 一个 helper：懒缓存 class（FindClass + NewGlobalRef）+ methodID
/// （Get[Static]MethodID），按 argKinds 把 regs[argIdx[i]] 装进 jvalue[]，调
/// Call[Static]<Type>MethodA，按 retKind 写回 regs[dest]（void 不写）。
/// helper 签名固定为 mj2x_vm_call_fn：参数 (env, regs, recv, argIdx, argc, dest)。
/// argc/recv/dest/argIdx 视 invokeKind/hasDest/argCount 可能未用，按需 (void) 抑制。
void emitCallInfrastructure(CSourceStream& out, const VmProgram& program, const std::string& tag)
{
    if (program.methodRefs.empty()) return;

    for (size_t i = 0; i < program.methodRefs.size(); ++i) {
        const auto&        m        = program.methodRefs[i];
        const bool         isStatic = (m.invokeKind == VmInvokeKind::Static);
        const bool         isVoid   = (m.retKind == VmJniKind::Void);
        const char*        getMid   = isStatic ? "GetStaticMethodID" : "GetMethodID";
        const std::string  suffix   = jniCallTypeSuffix(m.retKind);
        const std::string  callFn   = isStatic ? format_std("CallStatic{}MethodA", suffix)
                                               : format_std("Call{}MethodA", suffix);
        const std::string  helper   = format_std("mj2x_vm_call_{}_{}", tag, i);

        out.add_func(format_std("static void {}(JNIEnv* env, mj2x_vm_slot* regs, "
                                "unsigned int recv, unsigned int* argIdx, "
                                "unsigned char argc, unsigned int dest)", helper),
                     [&](CSourceStream& f) {
            f.emitLine("(void)argc;  /* 参数数编译期固定（argKinds.size），argc 仅签名兼容 */");
            if (isStatic) f.emitLine("(void)recv;  /* static 调用无 receiver */");
            if (isVoid) f.emitLine("(void)dest;  /* void 返回不写 dest */");
            if (m.argKinds.empty()) f.emitLine("(void)argIdx;  /* 无参数 */");

            // 懒缓存 owner class（FindClass 失败留 pending exception，直接返回让异常传播）。
            f.emitLine("static jclass _cls = NULL;");
            f.emitLine("if (_cls == NULL) {");
            {
                CSourceStream::IndentGuard g(f);
                f.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                      codegen::cStringLiteral(m.owner)));
                f.emitLine("if (_local == NULL) return;  /* pending exception */");
                f.emitLine("_cls = (*env)->NewGlobalRef(env, _local);");
                f.emitLine("(*env)->DeleteLocalRef(env, _local);");
            }
            f.emitLine("}");

            // 懒缓存 methodID（static 用 GetStaticMethodID，其余用 GetMethodID）。
            f.emitLine("static jmethodID _mid = NULL;");
            f.emitLine("if (_mid == NULL) {");
            {
                CSourceStream::IndentGuard g(f);
                f.emitLine(format_std("_mid = (*env)->{}(env, _cls, {}, {});",
                                      getMid, codegen::cStringLiteral(m.name),
                                      codegen::cStringLiteral(m.desc)));
                f.emitLine("if (_mid == NULL) return;  /* pending exception */");
            }
            f.emitLine("}");

            // 装配 jvalue[]（参数数编译期已知，按 argKinds 逐个 marshalling）。
            const size_t nargs = m.argKinds.size();
            f.emitLine(format_std("jvalue _a[{}];", nargs > 0 ? nargs : 1));
            for (size_t a = 0; a < nargs; ++a) {
                f.emitLine(format_std("_a[{}].{} = {}regs[argIdx[{}]]{};",
                                      a, jvalueField(m.argKinds[a]),
                                      jvalueCast(m.argKinds[a]), a,
                                      slotReadField(m.argKinds[a])));
            }

            // 调用 + 写回。instance 调用 receiver 取 regs[recv].ref（Call<Type>MethodA
            // 按 receiver 实际类虚分派；methodID 可来自声明 owner 类，JNI 规范保证有效）。
            if (isVoid) {
                if (isStatic) {
                    f.emitLine(format_std("(*env)->{}(env, _cls, _mid, _a);", callFn));
                } else {
                    f.emitLine(format_std(
                        "(*env)->{}(env, (jobject)regs[recv].ref, _mid, _a);", callFn));
                }
            } else {
                f.emitLine(format_std("{} _r = (*env)->{}(env, {}, _mid, _a);",
                                      jniReturnType(m.retKind), callFn,
                                      isStatic ? "_cls" : "(jobject)regs[recv].ref"));
                f.emitLine(format_std("regs[dest]{} = {}_r;",
                                      slotReadField(m.retKind), resultWriteCast(m.retKind)));
            }
        });
        out.emitBlankLine();
    }

    // 函数指针表：call_table[callSiteIdx] → helper。wrapper 传它给 exec。
    out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_call_table_{}[{}] = {{",
                            tag, program.methodRefs.size()));
    {
        CSourceStream::IndentGuard g(out);
        for (size_t i = 0; i < program.methodRefs.size(); ++i) {
            out.emitLine(format_std("mj2x_vm_call_{}_{},", tag, i));
        }
    }
    out.emitLine("};");
    out.emitBlankLine();
}

// ===========================================================================
// Field 基础设施（Phase 2.A）：per-site field load/store helper + 分派表。
//
// 为每个 LoadField/StoreField 访问点发射一个 helper（懒缓存 owner class + fieldID，
// static/instance 分别 Get/Set[Static]<Type>Field），再发射 mj2x_vm_load/store_field_table。
// 解释器 LOAD_FIELD/STORE_FIELD handler 经该表分派。helper 签名复用 mj2x_vm_call_fn。
// ===========================================================================

/// 字段描述符首字符 → JNI Get/Set accessor 后缀（精确，含 Boolean/Byte/Char/Short）。
///
/// @details
/// 字段访问必须用精确类型：GetBooleanField 与 GetIntField 不可互换（jfieldID 相同但
/// accessor 必须匹配声明类型，否则行为未定义）。故不沿用 Call 的粗粒度 jniKind，
/// 而按 desc 首字符精确选 accessor。VMC4 slot 统一用 .i 承载 Z/B/C/S/I/J。
const char* fieldJniSuffix(const std::string& desc)
{
    if (desc.empty()) return "Object";
    switch (desc[0]) {
    case 'Z': return "Boolean";
    case 'B': return "Byte";
    case 'C': return "Char";
    case 'S': return "Short";
    case 'I': return "Int";
    case 'J': return "Long";
    case 'F': return "Float";
    case 'D': return "Double";
    default: return "Object";   // 'L' / '['
    }
}

/// 字段描述符首字符 → 寄存器槽读写字段（.i/.f/.ref）。
const char* fieldSlotField(const std::string& desc)
{
    if (desc.empty()) return ".i";
    switch (desc[0]) {
    case 'F':
    case 'D': return ".f";
    case 'L':
    case '[': return ".ref";
    default: return ".i";
    }
}

/// 字段描述符首字符 → JNI 返回/参数 C 类型名（jint/jboolean/...）。
const char* fieldJniCType(const std::string& desc)
{
    if (desc.empty()) return "jobject";
    switch (desc[0]) {
    case 'Z': return "jboolean";
    case 'B': return "jbyte";
    case 'C': return "jchar";
    case 'S': return "jshort";
    case 'I': return "jint";
    case 'J': return "jlong";
    case 'F': return "jfloat";
    case 'D': return "jdouble";
    default: return "jobject";
    }
}

/// 字段描述符首字符 → 写回 regs[dest] 的 cast（load 用；Double 无需 cast）。
const char* fieldWriteCast(const std::string& desc)
{
    if (desc.empty()) return "";
    switch (desc[0]) {
    case 'L':
    case '[': return "(void*)";
    case 'F': return "(double)";
    case 'D': return "";
    default: return "(int64_t)";   // Z B C S I J → slot .i
    }
}

/// @brief 发射单个 field helper（load 或 store）。
///
/// @details
/// 懒缓存 owner class（FindClass + NewGlobalRef）+ fieldID（Get[Static]FieldID），
/// load 调 Get[Static]<Type>Field 写回 regs[dest]，store 调 Set[Static]<Type>Field
/// 从 regs[argIdx[0]] 取值。instance 访问 receiver 取 regs[recv].ref（null 时 JNI
/// 自身抛 NPE，与 JVM 语义一致）。
void emitFieldHelper(CSourceStream& out, const VmFieldRef& fld, size_t i, bool isStore,
                     const std::string& tag)
{
    const bool        isStatic = fld.isStatic;
    const std::string kind     = isStore ? "store_field" : "load_field";
    const std::string suffix   = fieldJniSuffix(fld.desc);
    const std::string helper   = format_std("mj2x_vm_{}_{}_{}", kind, tag, i);

    out.add_func(format_std("static void {}(JNIEnv* env, mj2x_vm_slot* regs, "
                            "unsigned int recv, unsigned int* argIdx, "
                            "unsigned char argc, unsigned int dest)", helper),
                 [&](CSourceStream& fnc) {
        if (isStore) {
            fnc.emitLine("(void)dest;  /* store 无结果 */");
        } else {
            fnc.emitLine("(void)argc; (void)argIdx;  /* LoadField 无参数 */");
            if (isStatic) fnc.emitLine("(void)recv;  /* static 字段无 receiver */");
        }

        // 懒缓存 owner class（fieldID 解析需要；FindClass 失败留 pending exception）。
        fnc.emitLine("static jclass _cls = NULL;");
        fnc.emitLine("if (_cls == NULL) {");
        {
            CSourceStream::IndentGuard g(fnc);
            fnc.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                    codegen::cStringLiteral(fld.owner)));
            fnc.emitLine("if (_local == NULL) return;  /* pending exception */");
            fnc.emitLine("_cls = (*env)->NewGlobalRef(env, _local);");
            fnc.emitLine("(*env)->DeleteLocalRef(env, _local);");
        }
        fnc.emitLine("}");

        // 懒缓存 fieldID（static 用 GetStaticFieldID，instance 用 GetFieldID）。
        fnc.emitLine("static jfieldID _fid = NULL;");
        fnc.emitLine("if (_fid == NULL) {");
        {
            CSourceStream::IndentGuard g(fnc);
            const char* getFid = isStatic ? "GetStaticFieldID" : "GetFieldID";
            fnc.emitLine(format_std("_fid = (*env)->{}(env, _cls, {}, {});",
                                    getFid, codegen::cStringLiteral(fld.name),
                                    codegen::cStringLiteral(fld.desc)));
            fnc.emitLine("if (_fid == NULL) return;  /* pending exception */");
        }
        fnc.emitLine("}");

        if (isStore) {
            const char* setFn = isStatic ? "SetStatic" : "Set";
            fnc.emitLine(format_std("(*env)->{}{}Field(env, {}, _fid, ({})regs[argIdx[0]]{});",
                                    setFn, suffix,
                                    isStatic ? "_cls" : "(jobject)regs[recv].ref",
                                    fieldJniCType(fld.desc), fieldSlotField(fld.desc)));
        } else {
            const char* getFn = isStatic ? "GetStatic" : "Get";
            fnc.emitLine(format_std("{} _r = (*env)->{}{}Field(env, {}, _fid);",
                                    fieldJniCType(fld.desc), getFn, suffix,
                                    isStatic ? "_cls" : "(jobject)regs[recv].ref"));
            fnc.emitLine(format_std("regs[dest]{} = {}_r;",
                                    fieldSlotField(fld.desc), fieldWriteCast(fld.desc)));
        }
    });
    out.emitBlankLine();
}

/// 发射 LoadField/StoreField 的 per-site helper + 分派表（refs 非空时）。
void emitFieldInfrastructure(CSourceStream& out, const VmProgram& program, const std::string& tag)
{
    if (program.loadFieldRefs.empty() && program.storeFieldRefs.empty()) return;

    for (size_t i = 0; i < program.loadFieldRefs.size(); ++i) {
        emitFieldHelper(out, program.loadFieldRefs[i], i, false, tag);
    }
    for (size_t i = 0; i < program.storeFieldRefs.size(); ++i) {
        emitFieldHelper(out, program.storeFieldRefs[i], i, true, tag);
    }

    if (!program.loadFieldRefs.empty()) {
        out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_load_field_table_{}[{}] = {{",
                                tag, program.loadFieldRefs.size()));
        {
            CSourceStream::IndentGuard g(out);
            for (size_t i = 0; i < program.loadFieldRefs.size(); ++i) {
                out.emitLine(format_std("mj2x_vm_load_field_{}_{},", tag, i));
            }
        }
        out.emitLine("};");
        out.emitBlankLine();
    }
    if (!program.storeFieldRefs.empty()) {
        out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_store_field_table_{}[{}] = {{",
                                tag, program.storeFieldRefs.size()));
        {
            CSourceStream::IndentGuard g(out);
            for (size_t i = 0; i < program.storeFieldRefs.size(); ++i) {
                out.emitLine(format_std("mj2x_vm_store_field_{}_{},", tag, i));
            }
        }
        out.emitLine("};");
        out.emitBlankLine();
    }
}

}   // namespace

// ===========================================================================
// VmInterpreterEmitter 公开入口
// ===========================================================================

std::string VmInterpreterEmitter::emitRuntimePrelude()
{
    // mj2x_vm_slot 联合体：寄存器槽。i 用于 I32/I64（I32 写入时由 handler 截断到 32 位），
    // f 用于 F32/F64（同理用 double 统一）。ref 承载 jobject（对象引用，Phase 2.0 起）。
    // ref 用 void* 而非 jobject，使 prelude 自包含（不依赖 jni.h）；wrapper/handler 侧
    // 在存取时做 (jobject) 与 (void*) 的双向 cast。
    // mj2x_vm_call_fn 是 Call helper 函数指针类型（解释器经函数指针表分派到 per-callsite
    // 的 JNI helper）。div/rem helper 精确复刻 JVM 语义（INT_MIN / -1 = INT_MIN 不抛、
    // 除零报 status=1）。
    return std::string(
        "/* ===== VMC4 runtime prelude (auto-generated) ===== */\n"
        "typedef union mj2x_vm_slot {\n"
        "    int64_t i;\n"
        "    double  f;\n"
        "    void*   ref;\n"
        "} mj2x_vm_slot;\n"
        "\n"
        "/* Call helper 函数指针类型（Phase 2.0）：解释器经函数指针表分派到 per-callsite\n"
        " * 的 JNI helper。参数：(env, regs, recv, argIdxArr, argc, dest)。 */\n"
        "typedef void (*mj2x_vm_call_fn)(JNIEnv*, mj2x_vm_slot*, unsigned int, unsigned int*,\n"
        "                               unsigned char, unsigned int);\n"
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
    const std::string progName    = format_std("mj2x_vm_code_{}", tag);
    const std::string targetsName = format_std("mj2x_vm_targets_{}", tag);
    const std::string constsName  = format_std("mj2x_vm_consts_{}", tag);
    const std::string funcName    = format_std("mj2x_vm_exec_{}", tag);

    CSourceStream out;

    // --- 常量数组（type + slot + 位模式）---
    {
        // 空常量池时用 {0}（MSVC C 模式不接受空初始化列表 {}）。
        if (program.constantPool.empty()) {
            out.emitLine(format_std(
                "static const struct {{ unsigned char type; unsigned short slot; unsigned long long bits; }} {}[] = {{0}};",
                constsName));
        } else {
            out.emitLine(format_std(
                "static const struct {{ unsigned char type; unsigned short slot; unsigned long long bits; }} {}[] = {{",
                constsName));
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
                    out.emitLine(format_std("{{{}, {}u, 0x{:016x}ULL}},",
                                            static_cast<unsigned>(c.type),
                                            static_cast<unsigned>(c.destSlot),
                                            static_cast<unsigned long long>(bits)));
                }
            }
            out.emitLine("};");
        }
        out.emitBlankLine();
    }

    // --- blockTargets 数组 ---
    {
        out.emitLine(format_std("static const unsigned int {}[] = {{", targetsName));
        {
            CSourceStream::IndentGuard g(out);
            out.emitRaw(emitU32Array(program.blockTargets, "    "));
        }
        out.emitLine("");
        out.emitLine("};");
        out.emitBlankLine();
    }

    // --- bytecode 数组 ---
    {
        out.emitLine(format_std("static const unsigned char {}[] = {{", progName));
        {
            CSourceStream::IndentGuard g(out);
            out.emitRaw(emitByteArray(program.bytecodes, "    "));
        }
        out.emitLine("");
        out.emitLine("};");
        out.emitBlankLine();
    }

    // drift 的 per-variant unmap 表是文件级定义，必须在 exec 函数之前发射。
    if (instance.driftEnabled()) {
        emitDriftUnmapTables(out, instance, tag);
        out.emitBlankLine();
    }

    // FuncPtrTable 的 ctx struct / dispatch body 是文件级定义，必须在 exec 函数
    // 之前发射（MSVC 不支持嵌套函数）。其他 dispatcher 无文件级结构。
    if (instance.dispatcherKind() == VmDispatcherKind::FuncPtrTable) {
        emitFuncPtrFilePrelude(out, instance, tag);
        out.emitBlankLine();
    }

    // Call 基础设施（methodRefs 非空时）：per-callsite helper + mj2x_vm_call_table_<tag>。
    // 文件级定义，须在 exec 之前（wrapper 在其后引用 mj2x_vm_call_table_<tag>）。
    if (!program.methodRefs.empty()) {
        emitCallInfrastructure(out, program, tag);
    }

    // Field 基础设施（loadFieldRefs/storeFieldRefs 非空时）：per-site helper + 分派表。
    if (!program.loadFieldRefs.empty() || !program.storeFieldRefs.empty()) {
        emitFieldInfrastructure(out, program, tag);
    }

    // --- 解释器入口函数（按 dispatcherKind 选择形态）---
    out.add_func(format_std("static unsigned int {}(JNIEnv* env, mj2x_vm_slot* slots, "
                            "mj2x_vm_slot* retval, const mj2x_vm_call_fn* call_table, "
                            "const mj2x_vm_call_fn* load_field_table, "
                            "const mj2x_vm_call_fn* store_field_table)", funcName),
                 [&](CSourceStream& f) {
        f.emitLine("(void)env;");
        f.emitLine("(void)retval;");
        f.emitLine("(void)call_table;");
        f.emitLine("(void)load_field_table;");
        f.emitLine("(void)store_field_table;");
        switch (instance.dispatcherKind()) {
        case VmDispatcherKind::SwitchTable:
            emitSwitchDispatcher(f, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::FuncPtrTable:
            emitFuncPtrDispatcher(f, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::ComputedGoto:
            emitComputedGotoDispatcher(f, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::IfNest:
            emitIfNestDispatcher(f, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::StateDispatcher:
            emitStateDispatcher(f, instance, progName, targetsName, tag);
            break;
        }
    });
    out.emitBlankLine();

    // bogus handler 注入（文件级，dispatch 结构不引用，只污染静态分析）。
    emitBogusHandlers(out, instance, tag);

    return out.str();
}

}   // namespace mj2x

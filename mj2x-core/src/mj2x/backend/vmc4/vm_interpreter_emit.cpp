#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"

#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "libca/str/format.hpp"

#include <algorithm>
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
/// 除零/除模抛 ArithmeticException（JVM 语义，Phase 2.C）并进入异常检查点
/// （mj2x_vm_exc 由每个 dispatcher 无条件提供）——有异常区域则匹配，否则传播。
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
    "                case 3: if (b==0){mj2x_vm_throw_arith(env);goto mj2x_vm_exc;} r = (type==0)?mj2x_vm_div_i32((int32_t)a,(int32_t)b):mj2x_vm_div_i64(a,b); break;\n"
    "                case 4: if (b==0){mj2x_vm_throw_arith(env);goto mj2x_vm_exc;} r = (type==0)?mj2x_vm_rem_i32((int32_t)a,(int32_t)b):mj2x_vm_rem_i64(a,b); break;\n"
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
    "            if (type == 4) {\n"
    "                /* REF：对象身份比较必须走 IsSameObject（JNI 引用是句柄，\n"
    "                 * 同对象的不同句柄指针不等；数值/浮点双关会误判）。\n"
    "                 * 编码器已保证 REF 只带 Eq/Ne（有序比较 fail-closed）。 */\n"
    "                jobject a = (jobject)regs[lhs].ref, b = (jobject)regs[rhs].ref;\n"
    "                if (pred == 0) result = (*env)->IsSameObject(env, a, b);\n"
    "                else result = !(*env)->IsSameObject(env, a, b);\n"
    "            } else if (type == 0 || type == 1) {\n"
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
    "            if (type == 4) {\n"
    "                jobject a = (jobject)regs[lhs].ref, b = (jobject)regs[rhs].ref;\n"
    "                if (pred == 0) result = (*env)->IsSameObject(env, a, b);\n"
    "                else result = !(*env)->IsSameObject(env, a, b);\n"
    "            } else if (type == 0 || type == 1) {\n"
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

/// ARRAY_LOAD（Phase 2.A）：经函数指针表分派到 per-site array load helper。
/// recv=arrayIdx，argIdx[0]=indexIdx，dest=结果槽。结构与 CALL/LOAD_FIELD 对称。
const char* kHandlerArrayLoad =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            array_load_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

/// ARRAY_STORE（Phase 2.A）：recv=arrayIdx，argIdx[0]=indexIdx，argIdx[1]=valueIdx，dest=0。
const char* kHandlerArrayStore =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            array_store_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

/// ALLOCATE（Phase 2.A）：经函数指针表分派到 per-site alloc helper（Object/Array）。
/// recv=0（分配无 receiver），argc = 维度数（Object=0, Array=1），dest=新对象/数组。
const char* kHandlerAllocate =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            alloc_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

/// INTRINSIC（Phase 2.D 泛化）：经函数指针表分派到 per-intrinsic helper。
/// recv=0，操作数全在 argBuf[]（array.length 的数组 / cmp 双操作数 / instanceof
/// 与 monitor 的引用），dest=结果槽（void 的 monitor 不写）。siteIdx 按 name 去重。
const char* kHandlerIntrinsic =
    "            unsigned int siteIdx = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int recv = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned char argc = code[pc++];\n"
    "            unsigned int argBuf[32];\n"
    "            for (unsigned char ai = 0; ai < argc; ++ai) { argBuf[ai] = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2; }\n"
    "            intrinsic_table[siteIdx](env, regs, recv, argBuf, argc, dest);\n";

/// THROW（Phase 2.C）：athrow。mj2x_vm_throw 设 pending exception（null → NPE），
/// 随后 goto 异常检查点 mj2x_vm_exc（每个 dispatcher 无条件提供）——有异常区域则
/// 按当前块活跃区域匹配，否则传播（wrapper 检测 status=1 返回，Java 侧抛）。
/// 注意：进入检查点前 pc 必须回退到 [Throw][src] 起点——检查点的块反查按 pc
/// 落在哪个 blockTargets 区间归属异常，而 pc 已被推进到块尾之后（athrow 是
/// 块 terminator），不回退会把异常归属到布局顺序的下一个块；保护块后紧跟
/// 未保护块（try 内 throw、try 外 return）时 catch 会漏接、异常逃逸。
/// 回退安全：匹配成功路径会覆盖 pc（= handler 目标），未匹配传播路径不读 pc。
const char* kHandlerThrow =
    "            unsigned int src = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            mj2x_vm_throw(env, regs[src].ref);\n"
    "            pc -= 3;  /* 异常归属 = athrow 所在块（块反查按抛出指令 pc） */\n"
    "            goto mj2x_vm_exc;\n";

/// SWITCH（tableswitch/lookupswitch 统一形态）：线性扫描 key 表，命中取对应
/// target，未命中取 default；pc = targets[target]。key 按 I32 编码（字节码
/// switch 只支持 int）。
const char* kHandlerSwitch =
    "            unsigned int value_reg = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int default_t = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int count = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            int32_t key = regs[value_reg].i;\n"
    "            unsigned int target = default_t;\n"
    "            for (unsigned int i = 0; i < count; ++i) {\n"
    "                int32_t ck = (int32_t)((unsigned)code[pc] | ((unsigned)code[pc+1] << 8) |\n"
    "                                       ((unsigned)code[pc+2] << 16) | ((unsigned)code[pc+3] << 24)); pc += 4;\n"
    "                unsigned int t = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "                if (key == ck) target = t;\n"
    "            }\n"
    "            pc = targets[target];\n";

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
    {VmOpcode::ArrayLoad,  "ARRAY_LOAD",  kHandlerArrayLoad},
    {VmOpcode::ArrayStore, "ARRAY_STORE", kHandlerArrayStore},
    {VmOpcode::Allocate,   "ALLOCATE",    kHandlerAllocate},
    {VmOpcode::Intrinsic,  "INTRINSIC",   kHandlerIntrinsic},
    {VmOpcode::Throw,      "THROW",       kHandlerThrow},
    {VmOpcode::Switch,     "SWITCH",      kHandlerSwitch},
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

/// @brief 异常检查点（Phase 2.C）：所有 dispatcher 无条件提供。
///
/// @details
/// 语义：检查 JNI pending exception（可能由 Call/Field/Array/Alloc/Intrinsic helper、
/// 除零、athrow 产生）——有则取异常对象并清除，按「当前 pc 所在块的活跃异常区域」
/// （异常表顺序）匹配：命中执行该块的边拷贝（handler 参数 ← 来源寄存器）、把异常
/// 对象写进 exceptionReg、pc 跳到 handler 块；全不匹配保持传播（failure 语句）。
///
/// 生成参数：
///   - targetsName：blockTargets 数组名（块反查用）
///   - pcExpr：当前 pc 表达式（SwitchTable 等 "pc"，FuncPtrTable "ctx->pc"）
///   - matchedLabel：匹配成功后的跳转目标（紧随检查点之后的 label，跳过 failure）
///   - failure：未匹配时的传播语句（Switch 等 "status = 1; goto mj2x_vm_done;"，
///     FuncPtrTable 循环里 "ctx->status = 1; break;"）
///
/// 块反查：pc 落在哪个 blockTargets 区间（blockTargets 含真实块 + 合成块；合成块
/// 无活跃区域——异常边只在真实块上，反查到合成块时 switch 无 case 直接传播）。
void emitExceptionCheckpoint(CSourceStream& out, const VmProgram& program,
                             const std::string& targetsName, const std::string& pcExpr,
                             const std::string& matchedLabel, const std::string& failure)
{
    const unsigned int totalBlocks = static_cast<unsigned int>(program.blockTargets.size());
    out.emitLine("mj2x_vm_exc:");
    out.emitLine("if ((*env)->ExceptionCheck(env)) {");
    {
        CSourceStream::IndentGuard g(out);
        // 取异常对象并清除（JNI 规定 pending 未清时只能调 ExceptionCheck/Occurred/
        // Clear——IsInstanceOf/FindClass 在有 pending 时行为未定义）。匹配成功即
        // 消费；未匹配时重新 Throw 挂起（对齐 RegC _mj2x_regc_throw 语义），
        // wrapper 检测 status=1 返回后 JVM 抛原异常。
        out.emitLine("jthrowable _ex = (*env)->ExceptionOccurred(env);");
        out.emitLine("(*env)->ExceptionClear(env);");
        // 块反查：线性扫 blockTargets 找 pc 所在区间（块数少，MVP 正确性优先）。
        out.emitLine("unsigned int _bi = 0;");
        out.emitLine(format_std("while (_bi + 1 < {}u && {} >= targets[_bi + 1]) _bi++;",
                                totalBlocks, pcExpr));
        if (program.excRegions.empty()) {
            // 无异常区域：无法匹配，重新挂起异常并传播。
            out.emitLine("(*env)->Throw(env, _ex);");
            out.emitLine(failure);
        } else {
            out.emitLine("switch (_bi) {");
            {
                CSourceStream::IndentGuard g2(out);
                for (unsigned int bi = 0; bi < totalBlocks; ++bi) {
                    const auto& entries =
                        bi < program.blockExcRegions.size() ? program.blockExcRegions[bi]
                                                            : decltype(program.blockExcRegions[bi]){};
                    if (entries.empty()) continue;
                    out.emitLine(format_std("case {}: {{", bi));
                    {
                        CSourceStream::IndentGuard g3(out);
                        for (uint16_t entryIdx : entries) {
                            const auto& entry = program.excRegions[entryIdx];
                            const std::string typeExpr =
                                entry.caughtTypeIdx == 0xFFFF
                                    ? std::string("NULL")
                                    : (entry.caughtTypeIdx < program.catchTypes.size()
                                           ? codegen::cStringLiteral(
                                                 program.catchTypes[entry.caughtTypeIdx])
                                           : std::string("NULL"));
                            out.emitLine(format_std(
                                "if (mj2x_vm_exception_matches(env, (jobject)_ex, {})) {{",
                                typeExpr));
                            {
                                CSourceStream::IndentGuard g4(out);
                                for (const auto& copy : entry.copies) {
                                    out.emitLine(format_std("regs[{}] = regs[{}];", copy.first,
                                                            copy.second));
                                }
                                out.emitLine(format_std("regs[{}].ref = (void*)_ex;",
                                                        entry.exceptionReg));
                                out.emitLine(format_std("pc = targets[{}];", entry.handlerBlock));
                                out.emitLine(format_std("goto {};", matchedLabel));
                            }
                            out.emitLine("}");
                        }
                        // 本块所有区域都不匹配 → 重新挂起异常并传播。
                        out.emitLine("(*env)->Throw(env, _ex);");
                        out.emitLine("break;");
                    }
                    out.emitLine("}");
                }
            }
            out.emitLine("}");
            out.emitLine("(*env)->Throw(env, _ex);");
            out.emitLine(failure);
        }
    }
    out.emitLine("}");
    // label 后必须跟语句（MSVC：label 后直接 `}` 是 C2143）。
    out.emitLine(format_std("{}: ;", matchedLabel));
    out.emitLine("/* end exception checkpoint */");
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
void emitSwitchDispatcher(CSourceStream& out, const VmProgram& program, const VmInstance& instance,
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
        // 异常检查点（Phase 2.C）：循环体末尾，所有 handler 之后。
        emitExceptionCheckpoint(out, program, targetsName, "pc", "mj2x_vm_exc_matched",
                                "status = 1; goto mj2x_vm_done;");
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
void emitFuncPtrFilePrelude(CSourceStream& out, const VmProgram& program,
                            const VmInstance& instance, const std::string& tag)
{
    const std::string ctxType = format_std("mj2x_vm_ctx_{}", tag);
    out.emitLine(format_std(
        "struct {} {{ const unsigned char* code; const unsigned int* targets; "
        "unsigned int pc; unsigned int status; JNIEnv* env; const mj2x_vm_call_fn* call_table; "
        "const mj2x_vm_call_fn* load_field_table; const mj2x_vm_call_fn* store_field_table; "
        "const mj2x_vm_call_fn* array_load_table; const mj2x_vm_call_fn* array_store_table; "
        "const mj2x_vm_call_fn* alloc_table; const mj2x_vm_call_fn* intrinsic_table; "
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
        f.emitLine("const mj2x_vm_call_fn* array_load_table = ctx->array_load_table;");
        f.emitLine("const mj2x_vm_call_fn* array_store_table = ctx->array_store_table;");
        f.emitLine("const mj2x_vm_call_fn* alloc_table = ctx->alloc_table;");
        f.emitLine("const mj2x_vm_call_fn* intrinsic_table = ctx->intrinsic_table;");
        f.emitLine("mj2x_vm_slot* regs = ctx->regs;");
        f.emitLine("mj2x_vm_slot* retval = ctx->retval;");
        f.emitLine("(void)targets;");
        f.emitLine("(void)status;");
        f.emitLine("(void)retval;");
        f.emitLine("(void)load_field_table;");
        f.emitLine("(void)store_field_table;");
        f.emitLine("(void)array_load_table;");
        f.emitLine("(void)array_store_table;");
        f.emitLine("(void)alloc_table;");
        f.emitLine("(void)intrinsic_table;");
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
        // 异常检查点（Phase 2.C）：handler 的 goto mj2x_vm_exc 汇到这里（除零/athrow/
        // JNI helper pending）。匹配成功：写回 ctx->pc 并返回（exec 循环继续）；
        // 未匹配：重新挂起异常 + 局部 status=1（经 mj2x_vm_done 提交 ctx->status）
        // 返回（exec 循环 break 传播）。注意 failure 必须写局部 status 而非
        // ctx->status：mj2x_vm_done 的 `ctx->status = status` 会覆盖直接写
        // ctx->status 的赋值（局部 status 恒 0 → 传播被吞 + pc 停在异常点
        // → 无限重抛死循环）。
        emitExceptionCheckpoint(f, program, "targets", "pc", "mj2x_vm_exc_matched",
                                "status = 1; goto mj2x_vm_done;");
        f.emitLine("ctx->pc = pc; goto mj2x_vm_done;");
        // RETURN handler 与 BINARY 除零分支都经 goto mj2x_vm_done 退出：此标签必须存在，
        // 否则生成的 C 引用未定义 label 编译失败。
        f.label_("mj2x_vm_done");
        f.emitLine("ctx->pc = pc; ctx->status = status;");
        f.emitLine("return;");
    });
}

/// 函数体：ctx 初始化 + 函数指针表 + 分发循环（在 exec 函数内发射）。
void emitFuncPtrDispatcher(CSourceStream& out, const VmProgram& program,
                           const VmInstance& instance, const std::string& progName,
                           const std::string& targetsName, const std::string& tag)
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
    out.emitLine("_ctx.array_load_table = array_load_table;");
    out.emitLine("_ctx.array_store_table = array_store_table;");
    out.emitLine("_ctx.alloc_table = alloc_table;");
    out.emitLine("_ctx.intrinsic_table = intrinsic_table;");
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
        // 注意：异常分派在 dispatch body 内完成（emitFuncPtrFilePrelude 的检查点），
        // 匹配成功写回 ctx->pc 返回（此处 status=0 继续），未匹配 status=1（break 传播）。
    }
    out.emitLine("}");
    out.emitLine("return ctx->status;");
}

/// 形态 2：ComputedGoto —— GCC "Labels as Values" 扩展（MSVC 不支持，调用方需守卫）。
void emitComputedGotoDispatcher(CSourceStream& out, const VmProgram& program,
                                const VmInstance& instance, const std::string& progName,
                                const std::string& targetsName, const std::string& tag)
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
    // 异常检查点（Phase 2.C）：函数级公共块（label 必须唯一，ComputedGoto 无循环
    // 结构）。handler body 的 goto mj2x_vm_exc 都汇到这里；匹配成功 pc 已设，落到
    // matched label 后重新取码；未匹配传播。
    emitExceptionCheckpoint(out, program, targetsName, "pc", "mj2x_vm_exc_fetch",
                            "status = 1; goto mj2x_vm_done;");
    out.emitLine("goto *mj2x_vm_labels[code[pc++]];");
    out.label_("mj2x_vm_done");
    out.emitLine("return status;");
}

/// 形态 3：IfNest —— 线性 if/else if 链（无跳表）。
void emitIfNestDispatcher(CSourceStream& out, const VmProgram& program,
                          const VmInstance& instance, const std::string& progName,
                          const std::string& targetsName, const std::string& tag)
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
        // 异常检查点（Phase 2.C）：循环体末尾，所有 if 分支之后。
        emitExceptionCheckpoint(out, program, targetsName, "pc", "mj2x_vm_exc_matched",
                                "status = 1; goto mj2x_vm_done;");
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
void emitStateDispatcher(CSourceStream& out, const VmProgram& program,
                         const VmInstance& instance, const std::string& progName,
                         const std::string& targetsName, const std::string& tag)
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
        // 异常检查点（Phase 2.C）：while 循环体末尾，所有 case 之后。
        emitExceptionCheckpoint(out, program, targetsName, "pc", "mj2x_vm_exc_matched",
                                "status = 1; goto mj2x_vm_done;");
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
///
/// 四种 dispatch 的 JNI 语义（Phase 2.B）：
///   - Virtual/Interface：Call<Type>MethodA（按 receiver 实际类虚分派；interface
///     方法用 GetMethodID 取 methodID 后同样经 CallMethodA 分派，JNI 规范允许）
///   - Special（invokespecial：<init>/private/super 调用）：CallNonvirtual<Type>MethodA
///     ——必须带定义类参数，禁止虚分派（super 调用语义：调父类实现而非子类覆盖）
///   - Static：CallStatic<Type>MethodA（无 receiver）
/// instance 调用（Virtual/Special/Interface）的 null receiver 显式抛 NPE（与 JVM
/// 语义一致；JNI 对 null receiver 的调用行为未定义，不能依赖）。
void emitCallInfrastructure(CSourceStream& out, const VmProgram& program, const std::string& tag)
{
    if (program.methodRefs.empty()) return;

    for (size_t i = 0; i < program.methodRefs.size(); ++i) {
        const auto&        m         = program.methodRefs[i];
        const bool         isStatic  = (m.invokeKind == VmInvokeKind::Static);
        const bool         isSpecial = (m.invokeKind == VmInvokeKind::Special);
        const bool         isVoid    = (m.retKind == VmJniKind::Void);
        const char*        getMid    = isStatic ? "GetStaticMethodID" : "GetMethodID";
        const std::string  suffix    = jniCallTypeSuffix(m.retKind);
        // special 必须 CallNonvirtual（super/<init>/private 语义：禁止虚分派）。
        const std::string callFn =
            isStatic ? format_std("CallStatic{}MethodA", suffix)
                     : (isSpecial ? format_std("CallNonvirtual{}MethodA", suffix)
                                  : format_std("Call{}MethodA", suffix));
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

            // instance 调用（Virtual/Special/Interface）：null receiver 显式抛 NPE
            // （JNI 对 null receiver 调用行为未定义；JVM 语义是 NPE）。
            if (!isStatic) {
                f.emitLine("if (regs[recv].ref == NULL) { mj2x_vm_throw_npe(env, "
                           "\"instance method call\"); return; }");
            }

            // 调用 + 写回。Virtual/Interface 经 Call<Type>MethodA 按 receiver 实际类
            // 虚分派（methodID 可来自声明 owner 类，JNI 规范保证有效）；Special 经
            // CallNonvirtual<Type>MethodA 直接调定义类实现（_cls = 方法定义类），
            // super 调用语义正确。Static 无 receiver。
            if (isVoid) {
                if (isStatic) {
                    f.emitLine(format_std("(*env)->{}(env, _cls, _mid, _a);", callFn));
                } else if (isSpecial) {
                    f.emitLine(format_std(
                        "(*env)->{}(env, (jobject)regs[recv].ref, _cls, _mid, _a);", callFn));
                } else {
                    f.emitLine(format_std(
                        "(*env)->{}(env, (jobject)regs[recv].ref, _mid, _a);", callFn));
                }
            } else {
                if (isSpecial) {
                    f.emitLine(format_std("{} _r = (*env)->{}(env, (jobject)regs[recv].ref, _cls, "
                                          "_mid, _a);",
                                          jniReturnType(m.retKind), callFn));
                } else {
                    f.emitLine(format_std("{} _r = (*env)->{}(env, {}, _mid, _a);",
                                          jniReturnType(m.retKind), callFn,
                                          isStatic ? "_cls" : "(jobject)regs[recv].ref"));
                }
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

// ===========================================================================
// Array 基础设施（Phase 2.A）：per-site array load/store helper + 分派表。
//
// 为每个 ArrayLoad/ArrayStore 元素类型发射一个 helper（按 elemKind 去重），base 类型
// 用 Get/Set<Type>ArrayRegion(len=1)，ref 用 Get/SetObjectArrayElement。null 数组显式
// 抛 NPE（与 JVM 语义一致），越界交由 JNI 抛 AIOOBE。helper 签名复用 mj2x_vm_call_fn。
// ===========================================================================

/// VmArrayElemKind → JNI Region accessor 后缀（仅 base 类型；Ref 单独处理）。
const char* arrayRegionSuffix(VmArrayElemKind k)
{
    switch (k) {
    case VmArrayElemKind::Boolean: return "Boolean";
    case VmArrayElemKind::Byte: return "Byte";
    case VmArrayElemKind::Char: return "Char";
    case VmArrayElemKind::Short: return "Short";
    case VmArrayElemKind::Int: return "Int";
    case VmArrayElemKind::Long: return "Long";
    case VmArrayElemKind::Float: return "Float";
    case VmArrayElemKind::Double: return "Double";
    default: return "";   // Ref
    }
}

/// VmArrayElemKind → 元素 C 类型名。
const char* arrayElemCType(VmArrayElemKind k)
{
    switch (k) {
    case VmArrayElemKind::Boolean: return "jboolean";
    case VmArrayElemKind::Byte: return "jbyte";
    case VmArrayElemKind::Char: return "jchar";
    case VmArrayElemKind::Short: return "jshort";
    case VmArrayElemKind::Int: return "jint";
    case VmArrayElemKind::Long: return "jlong";
    case VmArrayElemKind::Float: return "jfloat";
    case VmArrayElemKind::Double: return "jdouble";
    default: return "jobject";
    }
}

/// VmArrayElemKind → 寄存器槽读写字段。
const char* arraySlotField(VmArrayElemKind k)
{
    switch (k) {
    case VmArrayElemKind::Float:
    case VmArrayElemKind::Double: return ".f";
    case VmArrayElemKind::Ref: return ".ref";
    default: return ".i";
    }
}

/// VmArrayElemKind → 写回 regs[dest] 的 cast（load 用）。
const char* arrayWriteCast(VmArrayElemKind k)
{
    switch (k) {
    case VmArrayElemKind::Float: return "(double)";
    case VmArrayElemKind::Double: return "";
    case VmArrayElemKind::Ref: return "(void*)";
    default: return "(int64_t)";
    }
}

/// @brief 发射单个 array helper（load 或 store）。
void emitArrayHelper(CSourceStream& out, VmArrayElemKind elemKind, size_t i, bool isStore,
                     const std::string& tag)
{
    const bool        isRef  = (elemKind == VmArrayElemKind::Ref);
    const std::string kind   = isStore ? "array_store" : "array_load";
    const std::string helper = format_std("mj2x_vm_{}_{}_{}", kind, tag, i);

    out.add_func(format_std("static void {}(JNIEnv* env, mj2x_vm_slot* regs, "
                            "unsigned int recv, unsigned int* argIdx, "
                            "unsigned char argc, unsigned int dest)", helper),
                 [&](CSourceStream& fnc) {
        if (isStore) fnc.emitLine("(void)dest;  /* store 无结果 */");
        // null 数组显式抛 NPE（JNI Get/SetArrayRegion 对 NULL 行为未定义）。
        fnc.emitLine("jobject _arr = (jobject)regs[recv].ref;");
        fnc.emitLine(format_std("if (_arr == NULL) {{ mj2x_vm_throw_npe(env, \"{}\"); return; }}",
                                isStore ? "array store" : "array load"));
        fnc.emitLine("jsize _idx = (jsize)regs[argIdx[0]].i;");

        if (isStore) {
            // value 在 argIdx[1]。
            if (isRef) {
                fnc.emitLine("(*env)->SetObjectArrayElement(env, _arr, _idx, "
                             "(jobject)regs[argIdx[1]].ref);");
            } else {
                fnc.emitLine(format_std("{} _v = ({})regs[argIdx[1]]{};",
                                        arrayElemCType(elemKind), arrayElemCType(elemKind),
                                        arraySlotField(elemKind)));
                fnc.emitLine(format_std("(*env)->Set{}ArrayRegion(env, _arr, _idx, 1, &_v);",
                                        arrayRegionSuffix(elemKind)));
            }
        } else {
            if (isRef) {
                fnc.emitLine(format_std("regs[dest]{} = {}(*env)->GetObjectArrayElement(env, _arr, _idx);",
                                        arraySlotField(elemKind), arrayWriteCast(elemKind)));
            } else {
                fnc.emitLine(format_std("{} _v;", arrayElemCType(elemKind)));
                fnc.emitLine(format_std("(*env)->Get{}ArrayRegion(env, _arr, _idx, 1, &_v);",
                                        arrayRegionSuffix(elemKind)));
                fnc.emitLine(format_std("regs[dest]{} = {}_v;",
                                        arraySlotField(elemKind), arrayWriteCast(elemKind)));
            }
        }
    });
    out.emitBlankLine();
}

/// 发射 ArrayLoad/ArrayStore 的 per-site helper + 分派表（refs 非空时）。
void emitArrayInfrastructure(CSourceStream& out, const VmProgram& program, const std::string& tag)
{
    if (program.arrayLoadRefs.empty() && program.arrayStoreRefs.empty()) return;

    for (size_t i = 0; i < program.arrayLoadRefs.size(); ++i) {
        emitArrayHelper(out, program.arrayLoadRefs[i].elemKind, i, false, tag);
    }
    for (size_t i = 0; i < program.arrayStoreRefs.size(); ++i) {
        emitArrayHelper(out, program.arrayStoreRefs[i].elemKind, i, true, tag);
    }

    if (!program.arrayLoadRefs.empty()) {
        out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_array_load_table_{}[{}] = {{",
                                tag, program.arrayLoadRefs.size()));
        {
            CSourceStream::IndentGuard g(out);
            for (size_t i = 0; i < program.arrayLoadRefs.size(); ++i) {
                out.emitLine(format_std("mj2x_vm_array_load_{}_{},", tag, i));
            }
        }
        out.emitLine("};");
        out.emitBlankLine();
    }
    if (!program.arrayStoreRefs.empty()) {
        out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_array_store_table_{}[{}] = {{",
                                tag, program.arrayStoreRefs.size()));
        {
            CSourceStream::IndentGuard g(out);
            for (size_t i = 0; i < program.arrayStoreRefs.size(); ++i) {
                out.emitLine(format_std("mj2x_vm_array_store_{}_{},", tag, i));
            }
        }
        out.emitLine("};");
        out.emitBlankLine();
    }
}

// ===========================================================================
// Allocate / array.length 基础设施（Phase 2.A）。
// ===========================================================================

/// 数组描述符首字符 → New<Type>Array 函数名后缀（base 数组）。ref 单独处理。
const char* newArraySuffix(const std::string& typeDesc)
{
    if (typeDesc.size() < 2) return "Int";
    switch (typeDesc[1]) {
    case 'Z': return "Boolean";
    case 'B': return "Byte";
    case 'C': return "Char";
    case 'S': return "Short";
    case 'I': return "Int";
    case 'J': return "Long";
    case 'F': return "Float";
    case 'D': return "Double";
    default: return "";   // 'L' / '[' → ref array
    }
}

/// 数组分配是否为引用数组（typeDesc[1] == 'L' 或 '['）。
bool isRefArrayAlloc(const std::string& typeDesc)
{
    return typeDesc.size() >= 2 && (typeDesc[1] == 'L' || typeDesc[1] == '[');
}

/// @brief 发射 Allocate 的共享多维奇数组递归 helper（Phase 2.E）。
///
/// @details
/// MULTIANEWARRAY 语义：按 JVM 规定 eager 分配所有声明的维度（多余维数保持
/// NULL，对齐 RegC _mj2x_regc_alloc_multi_array）。递归按描述符逐层剥离前导
/// '['；typeDesc 是完整数组描述符（如 "[[I"），dim_count ≤ '[' 数量。base 层
/// 用 New<Type>Array，ref 层用 FindClass(component)+NewObjectArray。
/// 名字带 tag（每个方法一份 C 文件/翻译单元，避免跨方法重复定义）。
void emitAllocMultiArraySharedHelper(CSourceStream& out, const std::string& tag)
{
    const std::string helper = format_std("mj2x_vm_alloc_multi_array_{}", tag);
    out.add_func(format_std("static void* {}(JNIEnv* env, const char* type_desc, "
                            "int dim_count, const int* dims)", helper),
                 [&](CSourceStream& fnc) {
        fnc.emitLine("if (type_desc == NULL || dims == NULL || dim_count <= 0 || "
                     "type_desc[0] != '[') {");
        {
            CSourceStream::IndentGuard g(fnc);
            fnc.emitLine("(*env)->ThrowNew(env, (*env)->FindClass(env, "
                         "\"java/lang/InternalError\"), \"mj2x_vm_alloc_multi_array: "
                         "malformed descriptor\");");
            fnc.emitLine("return NULL;");
        }
        fnc.emitLine("}");
        fnc.emitLine("if (dim_count == 1) {");
        {
            CSourceStream::IndentGuard g(fnc);
            fnc.emitLine("switch (type_desc[1]) {");
            {
                CSourceStream::IndentGuard g2(fnc);
                // base 层（'Z'/'B'/'C'/'S'/'I'/'J'/'F'/'D'）：New<Type>Array。
                struct BaseCase
                {
                    char        code;
                    const char* suffix;
                };
                const BaseCase kBaseCases[] = {
                    {'Z', "Boolean"}, {'B', "Byte"},   {'C', "Char"}, {'S', "Short"},
                    {'I', "Int"},    {'J', "Long"},    {'F', "Float"}, {'D', "Double"},
                };
                for (const auto& bc : kBaseCases) {
                    fnc.emitLine(format_std("case '{}': return (*env)->New{}Array(env, "
                                            "(jsize)dims[0]);",
                                            bc.code, bc.suffix));
                }
                fnc.emitLine("default: {");
                {
                    CSourceStream::IndentGuard g3(fnc);
                    // ref 层：component = type_desc+1（'L' 内部名或更深数组描述符，
                    // FindClass 两者都接受）。
                    fnc.emitLine("jclass _cc = (*env)->FindClass(env, type_desc + 1);");
                    fnc.emitLine("if (_cc == NULL) return NULL;");
                    fnc.emitLine("return (*env)->NewObjectArray(env, (jsize)dims[0], "
                                 "_cc, NULL);");
                }
                fnc.emitLine("}");
            }
            fnc.emitLine("}");
        }
        fnc.emitLine("}");
        fnc.emitLine("jclass _component = (*env)->FindClass(env, type_desc + 1);");
        fnc.emitLine("if (_component == NULL) return NULL;");
        fnc.emitLine("jobjectArray _array = (*env)->NewObjectArray(env, (jsize)dims[0], "
                     "_component, NULL);");
        fnc.emitLine("if (_array == NULL || (*env)->ExceptionCheck(env)) return _array;");
        fnc.emitLine("for (int _i = 0; _i < dims[0]; ++_i) {");
        {
            CSourceStream::IndentGuard g(fnc);
            fnc.emitLine(format_std("void* _child = {}(env, type_desc + 1, "
                                    "dim_count - 1, dims + 1);",
                                    helper));
            fnc.emitLine("if ((*env)->ExceptionCheck(env)) return _array;");
            fnc.emitLine("(*env)->SetObjectArrayElement(env, _array, _i, (jobject)_child);");
            fnc.emitLine("if ((*env)->ExceptionCheck(env)) return _array;");
        }
        fnc.emitLine("}");
        fnc.emitLine("return _array;");
    });
    out.emitBlankLine();
}

/// @brief 发射单个 Allocate helper（Object / Array / MultiArray）。
///
/// @details
/// Object：懒缓存 owner class + AllocObject（不调构造器，<init> 由独立 Call 调）。
/// Array base：New<Type>Array(length)。Array ref：懒缓存 component class + NewObjectArray。
/// MultiArray：维度寄存器全在 argIdx[]（argc=维度数），交给共享递归 helper。
/// 结果一律 jobject 写 regs[dest].ref。
void emitAllocHelper(CSourceStream& out, const VmAllocRef& aref, size_t i, const std::string& tag)
{
    const std::string helper = format_std("mj2x_vm_alloc_{}_{}", tag, i);
    out.add_func(format_std("static void {}(JNIEnv* env, mj2x_vm_slot* regs, "
                            "unsigned int recv, unsigned int* argIdx, "
                            "unsigned char argc, unsigned int dest)", helper),
                 [&](CSourceStream& fnc) {
        fnc.emitLine("(void)recv;  /* 分配无 receiver */");
        if (aref.kind == VmAllocKind::Object) {
            // 懒缓存 owner class + AllocObject。
            fnc.emitLine("static jclass _cls = NULL;");
            fnc.emitLine("if (_cls == NULL) {");
            {
                CSourceStream::IndentGuard g(fnc);
                fnc.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                        codegen::cStringLiteral(aref.typeDesc)));
                fnc.emitLine("if (_local == NULL) return;  /* pending exception */");
                fnc.emitLine("_cls = (*env)->NewGlobalRef(env, _local);");
                fnc.emitLine("(*env)->DeleteLocalRef(env, _local);");
            }
            fnc.emitLine("}");
            fnc.emitLine("regs[dest].ref = (void*)(*env)->AllocObject(env, _cls);");
            fnc.emitLine("(void)argc; (void)argIdx;");
        } else if (aref.kind == VmAllocKind::MultiArray) {
            // 维度寄存器全在 argIdx[]（argc = 维度数，字节流按 dimCount 写入）。
            // 共享递归 helper 按描述符逐层分配（Phase 2.E）。
            fnc.emitLine("int _dims[32];");
            fnc.emitLine("for (unsigned char _di = 0; _di < argc; ++_di) "
                         "_dims[_di] = (int)regs[argIdx[_di]].i;");
            fnc.emitLine(format_std("regs[dest].ref = (void*)mj2x_vm_alloc_multi_array_{}(env, "
                                    "{}, (int)argc, _dims);",
                                    tag, codegen::cStringLiteral(aref.typeDesc)));
        } else {
            // Array：length 在 argIdx[0]。
            fnc.emitLine("(void)argc;");
            if (isRefArrayAlloc(aref.typeDesc)) {
                // 懒缓存 component class + NewObjectArray。
                fnc.emitLine("static jclass _cc = NULL;");
                fnc.emitLine("if (_cc == NULL) {");
                {
                    CSourceStream::IndentGuard g(fnc);
                    // component 描述符 = typeDesc 去掉前导 '['（如 "[LBox;" → "LBox;"）。
                    fnc.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                            codegen::cStringLiteral(aref.typeDesc.substr(1))));
                    fnc.emitLine("if (_local == NULL) return;");
                    fnc.emitLine("_cc = (*env)->NewGlobalRef(env, _local);");
                    fnc.emitLine("(*env)->DeleteLocalRef(env, _local);");
                }
                fnc.emitLine("}");
                fnc.emitLine("regs[dest].ref = (void*)(*env)->NewObjectArray(env, "
                             "(jsize)regs[argIdx[0]].i, _cc, NULL);");
            } else {
                fnc.emitLine(format_std("regs[dest].ref = (void*)(*env)->New{}Array(env, "
                                        "(jsize)regs[argIdx[0]].i);",
                                        newArraySuffix(aref.typeDesc)));
            }
        }
    });
    out.emitBlankLine();
}

/// 发射 Allocate 的 per-site helper + 分派表（allocRefs 非空时）。
void emitAllocInfrastructure(CSourceStream& out, const VmProgram& program, const std::string& tag)
{
    if (program.allocRefs.empty()) return;
    const bool hasMulti = std::any_of(program.allocRefs.begin(), program.allocRefs.end(),
                                      [](const VmAllocRef& a) {
                                          return a.kind == VmAllocKind::MultiArray;
                                      });
    if (hasMulti) emitAllocMultiArraySharedHelper(out, tag);
    for (size_t i = 0; i < program.allocRefs.size(); ++i) {
        emitAllocHelper(out, program.allocRefs[i], i, tag);
    }
    out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_alloc_table_{}[{}] = {{",
                            tag, program.allocRefs.size()));
    {
        CSourceStream::IndentGuard g(out);
        for (size_t i = 0; i < program.allocRefs.size(); ++i) {
            out.emitLine(format_std("mj2x_vm_alloc_{}_{},", tag, i));
        }
    }
    out.emitLine("};");
    out.emitBlankLine();
}

/// 发射 Intrinsic 的 per-intrinsic helper + 分派表（intrinsics 非空时，Phase 2.D）。
///
/// @details
/// 按 name 分派语义（SSA 产生的 intrinsic 全集）：
///   - array.length：GetArrayLength（null → NPE）
///   - cmp.nan_greater/nan_less：三值比较 (lhs>rhs)-(lhs<rhs)，浮点 NaN-aware
///     （nan_greater → 1，nan_less → -1），纯计算无 JNI
///   - instanceof:<desc>：懒缓存 class + IsInstanceOf（null → false）
///   - monitor.enter/exit：MonitorEnter/MonitorExit（null → NPE）
/// helper 签名复用 mj2x_vm_call_fn。
void emitIntrinsicInfrastructure(CSourceStream& out, const VmProgram& program,
                                 const std::string& tag)
{
    if (program.intrinsics.empty()) return;
    for (size_t i = 0; i < program.intrinsics.size(); ++i) {
        const auto&        iref   = program.intrinsics[i];
        const std::string  helper = format_std("mj2x_vm_intrinsic_{}_{}", tag, i);
        out.add_func(format_std("static void {}(JNIEnv* env, mj2x_vm_slot* regs, "
                                "unsigned int recv, unsigned int* argIdx, "
                                "unsigned char argc, unsigned int dest)", helper),
                     [&](CSourceStream& f) {
            f.emitLine("(void)recv;  /* intrinsic 无 receiver（操作数全在 argBuf） */");
            if (!iref.hasArgs) f.emitLine("(void)argIdx; (void)argc;");
            if (iref.name == "array.length") {
                f.emitLine("(void)argc; (void)argIdx;  /* array.length 无参数 */");
                f.emitLine("jobject _arr = (jobject)regs[argIdx[0]].ref;");
                f.emitLine("if (_arr == NULL) { mj2x_vm_throw_npe(env, \"array.length\"); return; }");
                f.emitLine("regs[dest].i = (int64_t)(*env)->GetArrayLength(env, _arr);");
            } else if (iref.name == "cmp.nan_greater" || iref.name == "cmp.nan_less") {
                const bool nanGreater = iref.name == "cmp.nan_greater";
                f.emitLine("(void)env;  /* 纯计算，无 JNI */");
                if (iref.arg0Kind == VmJniKind::Float || iref.arg0Kind == VmJniKind::Double) {
                    // 浮点：任一 NaN → nanGreater ? 1 : -1；否则三值比较。
                    f.emitLine("double _a = regs[argIdx[0]].f, _b = regs[argIdx[1]].f;");
                    f.emitLine(format_std(
                        "regs[dest].i = (isnan(_a) || isnan(_b)) ? {} : (int64_t)((_a > _b) - (_a < _b));",
                        nanGreater ? "1" : "-1"));
                } else {
                    // 整型（LCMP）：无 NaN，直接三值比较。
                    f.emitLine("int64_t _a = regs[argIdx[0]].i, _b = regs[argIdx[1]].i;");
                    f.emitLine("regs[dest].i = (_a > _b) - (_a < _b);");
                }
            } else if (iref.name.rfind("instanceof:", 0) == 0) {
                const std::string typeName = iref.name.substr(std::string("instanceof:").size());
                f.emitLine("(void)argc;");
                // null instanceof T → false（JVM 语义，不抛异常）。
                f.emitLine("if (regs[argIdx[0]].ref == NULL) { regs[dest].i = 0; return; }");
                f.emitLine("static jclass _cls = NULL;");
                f.emitLine("if (_cls == NULL) {");
                {
                    CSourceStream::IndentGuard g(f);
                    f.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                          codegen::cStringLiteral(typeName)));
                    f.emitLine("if (_local == NULL) return;  /* pending exception */");
                    f.emitLine("_cls = (*env)->NewGlobalRef(env, _local);");
                    f.emitLine("(*env)->DeleteLocalRef(env, _local);");
                }
                f.emitLine("}");
                f.emitLine("regs[dest].i = (*env)->IsInstanceOf(env, (jobject)regs[argIdx[0]].ref, "
                           "_cls) ? 1 : 0;");
            } else if (iref.name.rfind("castcheck:", 0) == 0) {
                // CHECKCAST（encodeConvert 重定向）：null → 通过（JVM 语义）；
                // IsInstanceOf 命中 → 原值传递；未命中 → 抛 ClassCastException
                // （pending exception 由检查点分派，归属 = checkcast 所在块）。
                const std::string typeName = iref.name.substr(std::string("castcheck:").size());
                f.emitLine("(void)argc;");
                f.emitLine("jobject _v = (jobject)regs[argIdx[0]].ref;");
                f.emitLine("if (_v == NULL) { regs[dest].ref = NULL; return; }");
                f.emitLine("static jclass _cls = NULL;");
                f.emitLine("if (_cls == NULL) {");
                {
                    CSourceStream::IndentGuard g(f);
                    f.emitLine(format_std("jclass _local = (*env)->FindClass(env, {});",
                                          codegen::cStringLiteral(typeName)));
                    f.emitLine("if (_local == NULL) return;  /* pending exception */");
                    f.emitLine("_cls = (*env)->NewGlobalRef(env, _local);");
                    f.emitLine("(*env)->DeleteLocalRef(env, _local);");
                }
                f.emitLine("}");
                f.emitLine("if ((*env)->IsInstanceOf(env, _v, _cls)) { regs[dest].ref = (void*)_v; "
                           "return; }");
                f.emitLine("jclass _cc = (*env)->FindClass(env, \"java/lang/ClassCastException\");");
                f.emitLine("if (_cc != NULL) { (*env)->ThrowNew(env, _cc, \"\"); "
                           "(*env)->DeleteLocalRef(env, _cc); }");
                f.emitLine("regs[dest].ref = NULL;");
            } else if (iref.name == "monitor.enter" || iref.name == "monitor.exit") {
                const bool isEnter = iref.name == "monitor.enter";
                f.emitLine("(void)argc; (void)dest;  /* monitor 无结果 */");
                f.emitLine("jobject _o = (jobject)regs[argIdx[0]].ref;");
                f.emitLine("if (_o == NULL) { mj2x_vm_throw_npe(env, \"monitor\"); return; }");
                f.emitLine(isEnter ? "(*env)->MonitorEnter(env, _o);"
                                   : "(*env)->MonitorExit(env, _o);");
            } else {
                // 编码器已 fail-closed，理论上不可达（防御）。
                f.emitLine("(void)argIdx; (void)argc; (void)dest; (void)env;");
            }
        });
        out.emitBlankLine();
    }
    out.emitLine(format_std("static const mj2x_vm_call_fn mj2x_vm_intrinsic_table_{}[{}] = {{",
                            tag, program.intrinsics.size()));
    {
        CSourceStream::IndentGuard g(out);
        for (size_t i = 0; i < program.intrinsics.size(); ++i) {
            out.emitLine(format_std("mj2x_vm_intrinsic_{}_{},", tag, i));
        }
    }
    out.emitLine("};");
    out.emitBlankLine();
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
        "/* 抛 NullPointerException（数组/对象 null 访问，Phase 2.A）。pending exception\n"
        " * 由调用方检测并 return，让异常传播回 Java。 */\n"
        "static void mj2x_vm_throw_npe(JNIEnv* env, const char* msg) {\n"
        "    jclass _c = (*env)->FindClass(env, \"java/lang/NullPointerException\");\n"
        "    if (_c != NULL) { (*env)->ThrowNew(env, _c, msg); (*env)->DeleteLocalRef(env, _c); }\n"
        "}\n"
        "/* 抛已有异常对象（athrow，Phase 2.C）：null -> NPE（JVM 语义）。 */\n"
        "static void mj2x_vm_throw(JNIEnv* env, void* value) {\n"
        "    if (value == NULL) { mj2x_vm_throw_npe(env, \"athrow null\"); return; }\n"
        "    (*env)->Throw(env, (jthrowable)value);\n"
        "}\n"
        "/* 除零抛 ArithmeticException（JVM 语义，Phase 2.C）。 */\n"
        "static void mj2x_vm_throw_arith(JNIEnv* env) {\n"
        "    jclass _c = (*env)->FindClass(env, \"java/lang/ArithmeticException\");\n"
        "    if (_c != NULL) { (*env)->ThrowNew(env, _c, \"divide by zero\"); (*env)->DeleteLocalRef(env, _c); }\n"
        "}\n"
        "/* 异常类型匹配（Phase 2.C）：catch-all（caught_type 空/NULL）恒匹配；\n"
        " * FindClass 失败（类型不可解析）清 pending 并按不匹配处理（对齐 RegC 语义）。 */\n"
        "static int32_t mj2x_vm_exception_matches(JNIEnv* env, jobject ex, const char* caught_type) {\n"
        "    if (caught_type == NULL || caught_type[0] == '\\0') return 1;\n"
        "    jclass _c = (*env)->FindClass(env, caught_type);\n"
        "    if (_c == NULL) { (*env)->ExceptionClear(env); return 0; }\n"
        "    int32_t _r = (*env)->IsInstanceOf(env, ex, _c) ? 1 : 0;\n"
        "    (*env)->DeleteLocalRef(env, _c);\n"
        "    return _r;\n"
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
        emitFuncPtrFilePrelude(out, program, instance, tag);
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

    // Array 基础设施（arrayLoadRefs/arrayStoreRefs 非空时）：per-site helper + 分派表。
    if (!program.arrayLoadRefs.empty() || !program.arrayStoreRefs.empty()) {
        emitArrayInfrastructure(out, program, tag);
    }

    // Allocate 基础设施（allocRefs 非空时）+ Intrinsic 表（intrinsics 非空时，Phase 2.D
    // 泛化：array.length/cmp.nan_*/instanceof/monitor 全部经 intrinsic_table 分派）。
    if (!program.allocRefs.empty()) {
        emitAllocInfrastructure(out, program, tag);
    }
    if (!program.intrinsics.empty()) {
        emitIntrinsicInfrastructure(out, program, tag);
    }

    // --- 解释器入口函数（按 dispatcherKind 选择形态）---
    out.add_func(format_std("static unsigned int {}(JNIEnv* env, mj2x_vm_slot* slots, "
                            "mj2x_vm_slot* retval, const mj2x_vm_call_fn* call_table, "
                            "const mj2x_vm_call_fn* load_field_table, "
                            "const mj2x_vm_call_fn* store_field_table, "
                            "const mj2x_vm_call_fn* array_load_table, "
                            "const mj2x_vm_call_fn* array_store_table, "
                            "const mj2x_vm_call_fn* alloc_table, "
                            "const mj2x_vm_call_fn* intrinsic_table)", funcName),
                 [&](CSourceStream& f) {
        f.emitLine("(void)env;");
        f.emitLine("(void)retval;");
        f.emitLine("(void)call_table;");
        f.emitLine("(void)load_field_table;");
        f.emitLine("(void)store_field_table;");
        f.emitLine("(void)array_load_table;");
        f.emitLine("(void)array_store_table;");
        f.emitLine("(void)alloc_table;");
        f.emitLine("(void)intrinsic_table;");
        switch (instance.dispatcherKind()) {
        case VmDispatcherKind::SwitchTable:
            emitSwitchDispatcher(f, program, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::FuncPtrTable:
            emitFuncPtrDispatcher(f, program, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::ComputedGoto:
            emitComputedGotoDispatcher(f, program, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::IfNest:
            emitIfNestDispatcher(f, program, instance, progName, targetsName, tag);
            break;
        case VmDispatcherKind::StateDispatcher:
            emitStateDispatcher(f, program, instance, progName, targetsName, tag);
            break;
        }
    });
    out.emitBlankLine();

    // bogus handler 注入（文件级，dispatch 结构不引用，只污染静态分析）。
    emitBogusHandlers(out, instance, tag);

    return out.str();
}

}   // namespace mj2x

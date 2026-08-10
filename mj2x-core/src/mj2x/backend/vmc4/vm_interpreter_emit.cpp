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
const char* kHandlerUnary =
    "            unsigned char op = code[pc++];\n"
    "            unsigned char type = code[pc++];\n"
    "            unsigned int dest = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            unsigned int src  = (unsigned)code[pc] | ((unsigned)code[pc+1] << 8); pc += 2;\n"
    "            if (type == 0 || type == 1) {\n"
    "                if (op == 0) regs[dest].i = -regs[src].i;\n"
    "                else regs[dest].i = ~regs[src].i;\n"
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

/// CONVERT: 数值宽窄/整浮转换（向零截断，JVM 语义）
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
    "                if (to == 0) regs[dest].i = (int32_t)v;\n"
    "                else if (to == 1) regs[dest].i = (int64_t)v;\n"
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

/// opcode → handler 片段表。
struct HandlerEntry
{
    unsigned char opcode;
    const char*   name;
    const char*   body;
};

const HandlerEntry kHandlers[] = {
    {0x10, "COPY",    kHandlerCopy},
    {0x11, "UNARY",   kHandlerUnary},
    {0x12, "BINARY",  kHandlerBinary},
    {0x13, "COMPARE", kHandlerCompare},
    {0x14, "CONVERT", kHandlerConvert},
    {0x20, "JUMP",    kHandlerJump},
    {0x21, "BRANCH",  kHandlerBranch},
    {0x22, "RETURN",  kHandlerReturn},
};
constexpr size_t kHandlerCount = sizeof(kHandlers) / sizeof(kHandlers[0]);

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

/// 形态 0：SwitchTable —— 经典 switch/case。
std::string emitSwitchDispatcher(const std::string& progName, const std::string& targetsName)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    for (;;) {\n";
    s += "        unsigned char opcode = code[pc++];\n";
    s += "        switch (opcode) {\n";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char head[64];
        std::snprintf(head, sizeof(head), "        case 0x%02x /*%s*/: {\n",
                      kHandlers[i].opcode, kHandlers[i].name);
        s += head;
        s += kHandlers[i].body;
        s += "            break;\n        }\n";
    }
    s += "        case 0x00 /*HALT*/:\n";
    s += "        default: status = (status!=0)?status:3u; goto mj2x_vm_done;\n";
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
std::string emitFuncPtrDispatcher(const std::string& progName, const std::string& targetsName,
                                  const std::string& tag)
{
    std::string ctxType = "mj2x_vm_ctx_" + tag;
    std::string s;
    // 上下文 struct 定义（共享 pc/status/regs/code/targets/retval）。
    s += "    struct " + ctxType + " { const unsigned char* code; const unsigned int* targets; "
         "unsigned int pc; unsigned int status; mj2x_vm_slot* regs; mj2x_vm_slot* retval; };\n";
    s += "    " + ctxType + " _ctx;\n";
    s += "    _ctx.code = " + progName + ";\n";
    s += "    _ctx.targets = " + targetsName + ";\n";
    s += "    _ctx.pc = _ctx.targets[0];\n";
    s += "    _ctx.status = 0;\n";
    s += "    _ctx.regs = slots;\n";
    s += "    _ctx.retval = retval;\n";
    s += "    struct " + ctxType + "* ctx = &_ctx;\n";

    // 每个 handler 一个 static 函数（前置声明 + 定义在 dispatcher 之后会破坏可读性，
    // 这里改为在函数内用 lambda 不行（C89）；所以改为：dispatcher 内联 handler，
    // 但用函数指针表指向"opcode 分拣器"——即一个总 handler 函数按 opcode 内联 switch，
    // 函数指针表的每个槽指向它。这保留了"间接调用"的逆向特征，同时不破坏 C89 兼容。
    //
    // 真正的 per-opcode 独立函数需要 C++ 或 GCC nested function，MVP 用 C89 故采用
    // "单一 dispatch handler + 函数指针表别名"折中：表里每项指向同一函数，但函数指针
    // 表本身仍是间接调用结构。Phase 2 引入 per-opcode 独立函数（C++ 模式）。
    s += "    typedef void (*mj2x_vm_handler_" + tag + ")(" + ctxType + "*);\n";
    s += "    static void mj2x_vm_dispatch_body_" + tag + "(" + ctxType +
         "* ctx) {\n";
    s += "        const unsigned char* code = ctx->code;\n";
    s += "        const unsigned int* targets = ctx->targets;\n";
    s += "        unsigned int pc = ctx->pc;\n";
    s += "        unsigned int status = ctx->status;\n";
    s += "        mj2x_vm_slot* regs = ctx->regs;\n";
    s += "        mj2x_vm_slot* retval = ctx->retval;\n";
    s += "        (void)status;\n";
    s += "        switch (code[pc++]) {\n";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char head[64];
        std::snprintf(head, sizeof(head), "        case 0x%02x: {\n", kHandlers[i].opcode);
        s += head;
        s += kHandlers[i].body;
        s += "            break;\n        }\n";
    }
    s += "        default: ctx->status = 3; ctx->pc = pc; return;\n";
    s += "        }\n";
    s += "        ctx->pc = pc; ctx->status = status;\n";
    // 把 RETURN 的 goto mj2x_vm_done 转成 status 触发退出。
    s += "        if (status != 0 || (pc > 0 && code[pc-1] == 0x22)) { ctx->status = (status!=0)?status:0; return; }\n";
    s += "    }\n";
    // 函数指针表（每项指向同一 dispatch_body，表的存在本身是间接调用的逆向特征）。
    s += "    static const mj2x_vm_handler_" + tag + " mj2x_vm_table_" + tag + "[256] = {\n";
    s += "        [0x00] = 0";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char e[96];
        std::snprintf(e, sizeof(e), ", [0x%02x] = mj2x_vm_dispatch_body_%s",
                      kHandlers[i].opcode, tag.c_str());
        s += e;
    }
    s += "\n    };\n";
    s += "    for (;;) {\n";
    s += "        unsigned char opcode = ctx->code[ctx->pc];\n";
    s += "        mj2x_vm_handler_" + tag + " h = mj2x_vm_table_" + tag + "[opcode];\n";
    s += "        if (!h) { ctx->status = 3; break; }\n";   // HALT 或非法
    s += "        h(ctx);\n";
    s += "        if (ctx->status != 0 || (ctx->pc > 0 && ctx->code[ctx->pc-1] == 0x22)) break;\n";
    s += "    }\n";
    s += "    return ctx->status;\n";
    return s;
}

/// 形态 2：ComputedGoto —— GCC "Labels as Values" 扩展（MSVC 不支持，调用方需守卫）。
std::string emitComputedGotoDispatcher(const std::string& progName,
                                       const std::string& targetsName)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    static const void* mj2x_vm_labels[] = {\n";
    s += "        [0x00] = &&mj2x_vm_halt";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char e[64];
        std::snprintf(e, sizeof(e), ", [0x%02x] = &&mj2x_vm_op_%02x",
                      kHandlers[i].opcode, kHandlers[i].opcode);
        s += e;
    }
    s += "\n    };\n";
    s += "    goto *mj2x_vm_labels[code[pc++]];\n";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "mj2x_vm_op_%02x:\n", kHandlers[i].opcode);
        s += "    {\n";
        s += kHandlers[i].body;
        s += "        goto *mj2x_vm_labels[code[pc++]];\n";
        s += "    }\n";
    }
    s += "mj2x_vm_halt:\n";
    s += "    status = (status!=0)?status:3u;\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
    return s;
}

/// 形态 3：IfNest —— 线性 if/else if 链（无跳表）。
std::string emitIfNestDispatcher(const std::string& progName, const std::string& targetsName)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    for (;;) {\n";
    s += "        unsigned char opcode = code[pc++];\n";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char cond[64];
        std::snprintf(cond, sizeof(cond), "        if (opcode == 0x%02x) {\n",
                      kHandlers[i].opcode);
        s += cond;
        s += kHandlers[i].body;
        s += "        } else ";
    }
    s += "{ status = (status!=0)?status:3u; goto mj2x_vm_done; }\n";
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
std::string emitStateDispatcher(const std::string& progName, const std::string& targetsName)
{
    std::string s = emitCommonDecls(progName, targetsName);
    s += "    unsigned int state = 1; /* 1=RUNNING 0=DONE */\n";
    s += "    while (state) {\n";
    s += "        unsigned char opcode = code[pc++];\n";
    s += "        switch (opcode) {\n";
    for (size_t i = 0; i < kHandlerCount; ++i) {
        char head[64];
        std::snprintf(head, sizeof(head), "        case 0x%02x: {\n", kHandlers[i].opcode);
        s += head;
        s += kHandlers[i].body;
        // RETURN handler 已含 goto mj2x_vm_done；这里 break 继续 while。
        s += "            break;\n        }\n";
    }
    s += "        case 0x00:\n";
    s += "        default: state = 0; status = (status!=0)?status:3u; break;\n";
    s += "        }\n";
    s += "        if (pc > 0 && code[pc-1] == 0x22) { state = 0; }\n";   // RETURN 触发退出
    s += "    }\n";
    s += "mj2x_vm_done:\n";
    s += "    return status;\n";
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
            body = emitSwitchDispatcher(progName, targetsName);
            break;
        case VmDispatcherKind::FuncPtrTable:
            body = emitFuncPtrDispatcher(progName, targetsName, tag);
            break;
        case VmDispatcherKind::ComputedGoto:
            body = emitComputedGotoDispatcher(progName, targetsName);
            break;
        case VmDispatcherKind::IfNest:
            body = emitIfNestDispatcher(progName, targetsName);
            break;
        case VmDispatcherKind::StateDispatcher:
            body = emitStateDispatcher(progName, targetsName);
            break;
        }
        out.emitRaw(body);

        out.emitLine("}");
        out.emitBlankLine();
    }

    return out.str();
}

}   // namespace mj2x

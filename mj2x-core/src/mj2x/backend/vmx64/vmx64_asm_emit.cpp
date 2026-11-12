#include "mj2x/backend/vmx64/vmx64_asm_emit.hpp"

#include "libca/random/split_mix64.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mj2x {

namespace {

/// @brief ks64 的 rotate 步（C++ 侧镜像；.asm 侧为 `rol r64, 13`）。
uint64_t rotl64(uint64_t value, unsigned bits)
{
    return (value << bits) | (value >> (64u - bits));
}

/// 小工具：数值 → 小写 hex 文本（无前导零；width=0 不补位）。
std::string hexNum(uint64_t value, int width)
{
    char buffer[32];
    if (width > 0)
        std::snprintf(buffer, sizeof(buffer), "%0*llx", width,
                      static_cast<unsigned long long>(value));
    else
        std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

/// 数值 → MASM 十六进制字面量：h 后缀，首字符为字母时补前导 0（如
/// 0x0FEDC… → "0fedc…h"）。ml64 不认 0x 前缀，解析为 0 后跟标识符，
/// A2206 missing operator in expression（本机 ml64 14.44 实证）。发射侧
/// 数值一律走这里，禁止 0x 进产物。
std::string masmHex(uint64_t value)
{
    std::string h = hexNum(value, 0);
    if (h.empty()) h = "0";
    if (h[0] >= 'a' && h[0] <= 'f') h.insert(h.begin(), '0');
    return h + "h";
}

/// prologue 在 rbp 之下压栈的非易失寄存器字节数：rbx/rsi/rdi/r12/r13/r14/r15
/// 共 7 个（rbp 自身的保存在 [rbp]，不占 rbp 下方）。帧头与 vreg 池必须位于
/// 该区之下，按 rbp-0x20 起算会把 slot 9/10/11 叠在 saved r15/r14/r13 上，
/// epilogue pop 出垃圾寄存器回 JVM native 存根（E2E 实证：sum 的回边拷贝
/// 写 slot9/slot11 后 r13/r15 高低字拼接成 heap_base|slot 值，存根解引用崩）。
constexpr uint32_t kSavedRegsBytes = 7u * 8u;

/// 入口帧头区字节数：env/thisOrClass/两个保留槽（Phase 2 C helper 边界），
/// 位于被保存寄存器区之下。
constexpr uint32_t kFrameHeaderBytes = 0x20;

/// vreg slot 池字节数（16 对齐）。
uint32_t vregBytesFor(uint32_t numSlots)
{
    return ((numSlots * 8u + 15u) / 16u) * 16u;
}

}   // namespace

uint64_t vmx64DeriveInstanceSeed(uint64_t mainSeed, uint64_t identityHash)
{
    // Phase 1 最简版：splitmix64 单步混合（libca canonical 实现）。
    // 接口形状对齐 vmc4DeriveInstanceSeed，后续换 VMC4 式 seed_derivation
    // 家族时只动这里与 K1 派生点。
    return ca::random::SplitMix64(mainSeed ^ identityHash).next();
}

uint64_t vmx64Ks64(uint64_t x, uint64_t k1)
{
    // 与 .asm 内 vmx64_NEXT 的 imul/rol/xor/imul 序列逐位一致（改式双侧同步）。
    // 这是位置驱动的密钥流；另一种（mj2x/core/native_lib_crypto.hpp 的
    // xorshift32 字节流，镜像 NativeLoader 字节码）机制与镜像边界都不同，
    // 勿混淆、勿合并。
    uint64_t v = x * kVmx64P1;
    v          = rotl64(v, 13);
    v ^= k1;
    v *= kVmx64P2;
    return v;
}

// ===========================================================================
// 共享 handler 集（含 vmx64_NEXT 宏）：每个 .asm 一份，跨方法共享。
// handler 语义只依赖站点操作数布局，与具体方法无关；NEXT 宏是 dispatch 的
// 核心，从指令流解出站点号、查站点表、解回 handler 地址（表项两种落盘
// 形态的分支算术见头文件 post-link 站点表加密一节，数字例子见
// doc:vm-site-table-encryption）。发射出的 .asm 注释文本是字符串字面量，
// 与 C++ 侧注释各自维护，不要在此处重复其内容。
// ===========================================================================
std::string Vmx64AsmEmitter::emitSharedHandlers()
{
    std::string out;
    out += "; ============================================================================\n";
    out += "; vmx64 dispatch core (shared across methods)\n";
    out += ";\n";
    out += "; Register contract (see vmx64_asm_emit.hpp):\n";
    out += ";   rbx=pc(stream offset)  rsi=vreg slots  rdi=site table  r14=stream base\n";
    out += ";   r13=done label         r15=K1         r12=image delta holdover\n";
    out += ";     r12 = sites table preferred VA (base qword static value) minus its\n";
    out += ";     runtime address, computed once in the entry prologue. Unencrypted\n";
    out += ";     artifacts resolve to 0 (the base qword is live-relocated); post-link\n";
    out += ";     encrypted artifacts carry the static preferred value.\n";
    out += "; Scratch: rax rcx rdx r8-r11 (not preserved across handlers).\n";
    out += "; Every handler ends with vmx64_NEXT: fetch next handler number from the\n";
    out += "; stream (XOR keystream keyed by pc) -> decrypt. The site-table entry is\n";
    out += "; form-branching on r12: zero = unencrypted artifact (entry is the plain\n";
    out += "; ks64-addend form, live relocation applied) — add-only; nonzero = post-link\n";
    out += "; encrypted artifact — undo the entry XOR first, then add/sub the image\n";
    out += "; delta. Both forms converge on jmp rdx.\n";
    out += "; ============================================================================\n";
    out += "\n";
    out += "vmx64_NEXT macro\n";
    out += "    movzx eax, word ptr [r14+rbx]\n";
    out += "    imul rcx, rbx, 61C88647h\n";
    out += "    rol rcx, 13\n";
    out += "    xor rcx, r15\n";
    out += "    imul rcx, rcx, 045D9F3Bh\n";
    out += "    xor ax, cx\n";
    out += "    imul r11, rax, 61C88647h\n";
    out += "    rol r11, 13\n";
    out += "    xor r11, r15\n";
    out += "    imul r11, r11, 045D9F3Bh\n";
    out += "    mov rdx, [rdi+rax*8]\n";
    out += "    test r12, r12\n";
    out += "    jz @F\n";
    out += "    xor rdx, r11\n";
    out += "    @@:\n";
    out += "    add rdx, r11\n";
    out += "    sub rdx, r12\n";
    out += "    jmp rdx\n";
    out += "endm\n";
    out += "\n";
    // 宏定义后必须先开 .code 段再落 handler 标签/指令：MASM 要求一切指令在
    // 段内（段外代码报 "must be in segment block"）。public 声明与 handler
    // 标签都收进 .code；后续方法段自带 .data/.code 切换，与共享段共存于
    // 同一 TU。
    out += ".code\n";
    out += "\n";
    out += "public mj2x_vmx64_h_iconst\n";
    out += "public mj2x_vmx64_h_lconst\n";
    out += "public mj2x_vmx64_h_iload\n";
    out += "public mj2x_vmx64_h_lload\n";
    out += "public mj2x_vmx64_h_istore\n";
    out += "public mj2x_vmx64_h_lstore\n";
    out += "public mj2x_vmx64_h_iadd\n";
    out += "public mj2x_vmx64_h_ladd\n";
    out += "public mj2x_vmx64_h_isub\n";
    out += "public mj2x_vmx64_h_lsub\n";
    out += "public mj2x_vmx64_h_imul\n";
    out += "public mj2x_vmx64_h_lmul\n";
    out += "public mj2x_vmx64_h_icmpeq\n";
    out += "public mj2x_vmx64_h_icmpne\n";
    out += "public mj2x_vmx64_h_icmplt\n";
    out += "public mj2x_vmx64_h_icmple\n";
    out += "public mj2x_vmx64_h_icmpgt\n";
    out += "public mj2x_vmx64_h_icmpge\n";
    out += "public mj2x_vmx64_h_goto\n";
    out += "public mj2x_vmx64_h_iret\n";
    out += "public mj2x_vmx64_h_lret\n";
    out += "public mj2x_vmx64_h_vret\n";
    out += "public mj2x_vmx64_h_halt\n";
    out += "\n";

    auto next = [&out]() { out += "    vmx64_NEXT\n"; };

    // --- 常量物化 ---
    // [tEnc:u16][dst:u16][imm:i32]
    out += "; slot[dst] = imm32\n";
    out += "mj2x_vmx64_h_iconst:\n";
    out += "    mov eax, dword ptr [r14+rbx+4]\n";
    out += "    movzx ecx, word ptr [r14+rbx+2]\n";
    out += "    mov dword ptr [rsi+rcx*8], eax\n";
    out += "    add rbx, 8\n";
    next();
    out += "\n";
    // [tEnc:u16][dst:u16][imm:i64]
    out += "; slot[dst] = imm64\n";
    out += "mj2x_vmx64_h_lconst:\n";
    out += "    mov rax, qword ptr [r14+rbx+4]\n";
    out += "    movzx ecx, word ptr [r14+rbx+2]\n";
    out += "    mov qword ptr [rsi+rcx*8], rax\n";
    out += "    add rbx, 12\n";
    next();
    out += "\n";

    // --- 槽间拷贝（load/store 语义孪生）---
    for (const auto& pair : {std::pair<const char*, const char*>{"iload", "eax"},
                             {"istore", "eax"}, {"lload", "rax"}, {"lstore", "rax"}}) {
        const bool is64 = pair.second[0] == 'r';
        out += "; slot[dst] = slot[src] (";
        out += is64 ? "i64" : "i32";
        out += " copy)\n";
        out += std::string("mj2x_vmx64_h_") + pair.first + ":\n";
        out += "    movzx ecx, word ptr [r14+rbx+2]\n";
        out += "    movzx r8d, word ptr [r14+rbx+4]\n";
        if (is64) {
            out += "    mov rax, qword ptr [rsi+r8*8]\n";
            out += "    mov qword ptr [rsi+rcx*8], rax\n";
        } else {
            out += "    mov eax, dword ptr [rsi+r8*8]\n";
            out += "    mov dword ptr [rsi+rcx*8], eax\n";
        }
        out += "    add rbx, 6\n";
        next();
        out += "\n";
    }

    // --- 算术 ---
    struct Arith
    {
        const char* name;
        const char* instr;   // "add"/"sub"；nullptr → imul 双操作数形态
        bool        is64;
    };
    for (const auto& a : {Arith{"iadd", "add", false}, Arith{"ladd", "add", true},
                          Arith{"isub", "sub", false}, Arith{"lsub", "sub", true},
                          Arith{"imul", nullptr, false}, Arith{"lmul", nullptr, true}}) {
        out += "; slot[dst] = slot[a] ";
        out += a.instr != nullptr ? a.instr : "imul";
        out += " slot[b] (";
        out += a.is64 ? "i64" : "i32";
        out += " wraparound)\n";
        out += std::string("mj2x_vmx64_h_") + a.name + ":\n";
        out += "    movzx ecx, word ptr [r14+rbx+2]\n";
        out += "    movzx r8d, word ptr [r14+rbx+4]\n";
        out += "    movzx r9d, word ptr [r14+rbx+6]\n";
        if (a.is64) {
            out += "    mov rax, qword ptr [rsi+r8*8]\n";
            if (a.instr != nullptr)
                out += std::string("    ") + a.instr + " rax, qword ptr [rsi+r9*8]\n";
            else
                out += "    imul rax, qword ptr [rsi+r9*8]\n";
            out += "    mov qword ptr [rsi+rcx*8], rax\n";
        } else {
            out += "    mov eax, dword ptr [rsi+r8*8]\n";
            if (a.instr != nullptr)
                out += std::string("    ") + a.instr + " eax, dword ptr [rsi+r9*8]\n";
            else
                out += "    imul eax, dword ptr [rsi+r9*8]\n";
            out += "    mov dword ptr [rsi+rcx*8], eax\n";
        }
        out += "    add rbx, 8\n";
        next();
        out += "\n";
    }

    // --- if_icmp 分支族 ---
    // [tEnc:u16][a:u16][b:u16][true:u32][false:u32]
    struct Cmp
    {
        const char* name;
        const char* cc;   // cmov 条件（取 true 边）
    };
    for (const auto& c : {Cmp{"icmpeq", "e"}, Cmp{"icmpne", "ne"}, Cmp{"icmplt", "l"},
                          Cmp{"icmple", "le"}, Cmp{"icmpgt", "g"}, Cmp{"icmpge", "ge"}}) {
        out += "; pc = (slot[a] ";
        out += c.name + 4;   // "eq"/"ne"/"lt"...
        out += " slot[b]) ? true : false (i32 signed)\n";
        out += std::string("mj2x_vmx64_h_") + c.name + ":\n";
        out += "    movzx r8d, word ptr [r14+rbx+2]\n";
        out += "    movzx r9d, word ptr [r14+rbx+4]\n";
        // 操作数序 [true:u32][false:u32]，cmov 覆盖语义要求默认值装 false
        //（+10）、条件成立时以 true（+6）覆盖，此前 true 装在 r10d 被反向
        // 覆盖，分支整体取反（CI 实证 sum(100)=0：i<n 成立却走 false 边）。
        out += "    mov r10d, dword ptr [r14+rbx+10]\n";
        out += "    mov r11d, dword ptr [r14+rbx+6]\n";
        out += "    mov eax, dword ptr [rsi+r8*8]\n";
        out += "    cmp eax, dword ptr [rsi+r9*8]\n";
        out += std::string("    cmov") + c.cc + " r10d, r11d\n";
        out += "    mov ebx, r10d\n";
        next();
        out += "\n";
    }

    // --- goto ---
    // [tEnc:u16][target:u32]
    out += "; pc = target\n";
    out += "mj2x_vmx64_h_goto:\n";
    out += "    mov eax, dword ptr [r14+rbx+2]\n";
    out += "    mov ebx, eax\n";
    next();
    out += "\n";

    // --- 返回族 ---
    out += "; return slot[src] (i32)\n";
    out += "mj2x_vmx64_h_iret:\n";
    out += "    movzx ecx, word ptr [r14+rbx+2]\n";
    out += "    mov eax, dword ptr [rsi+rcx*8]\n";
    out += "    jmp r13\n";
    out += "\n";
    out += "; return slot[src] (i64)\n";
    out += "mj2x_vmx64_h_lret:\n";
    out += "    movzx ecx, word ptr [r14+rbx+2]\n";
    out += "    mov rax, qword ptr [rsi+rcx*8]\n";
    out += "    jmp r13\n";
    out += "\n";
    out += "; return (void)\n";
    out += "mj2x_vmx64_h_vret:\n";
    out += "    xor eax, eax\n";
    out += "    jmp r13\n";
    out += "\n";
    out += "; defensive halt sentinel (unreachable when all paths return)\n";
    out += "mj2x_vmx64_h_halt:\n";
    out += "    xor eax, eax\n";
    out += "    jmp r13\n";
    out += "\n";
    return out;
}

// ===========================================================================
// 方法段：加密指令流 + 站点表 + k1/base 尾 qword（.data）+ JNI 入口（.code）。
// ===========================================================================
std::string Vmx64AsmEmitter::emitMethod(const Vmx64Program& program, uint64_t instanceSeed,
                                        const std::string& methodTag)
{
    // K1 从实例 seed 独立派生（与站点号/流偏移输入解耦；常数 0x766D783634
    // 即 ASCII "vmx64"，域分隔）。这一个 k1 后文三处使用：流加密字 ks64、表项
    // addend/掩码 ks64、prologue `mov r15, imm` 与表尾 dq，四份落点同一
    // C++ 局部变量，无漂移面。
    const uint64_t k1 = ca::random::SplitMix64(instanceSeed ^ 0x766D783634ULL).next();

    // --- 指令流加密（副本上改写 tEnc 字）---
    // tEnc = 站点号 t XOR (u16)ks64(流偏移)。handler 由 rbx（当前流偏移）
    // 重算同一 keystream 异或解回站点号，流里没有明文 opcode。
    std::vector<uint8_t> stream = program.stream;
    for (size_t t = 0; t < program.sites.size(); ++t) {
        const uint32_t off = program.sites[t].streamOffset;
        // 编码器契约破坏（site 越界）时不静默跳过，那会留下 tEnc 未加密的
        // 可汇编产物。发射 .err 让汇编期硬失败（fail-closed 到工具链层）。
        if (off + 1 >= stream.size())
            return "; vmx64: site stream offset out of range (encoder contract violated)\n.err\n";
        const uint16_t ks      = static_cast<uint16_t>(vmx64Ks64(off, k1) & 0xFFFFULL);
        const uint16_t plain   = static_cast<uint16_t>(t);
        const uint16_t cipher  = static_cast<uint16_t>(plain ^ ks);
        stream[off]            = static_cast<uint8_t>(cipher & 0xFF);
        stream[off + 1]        = static_cast<uint8_t>((cipher >> 8) & 0xFF);
    }

    std::string out;
    out += "; ----------------------------------------------------------------------------\n";
    out += "; method " + methodTag + "  (instance seed " + masmHex(instanceSeed) + ")\n";
    out += "; ----------------------------------------------------------------------------\n";
    out += "\n";
    out += ".data\n";
    out += "mj2x_vmx64_code_" + methodTag + " label byte\n";
    // 指令流：16 字节/行 DB。
    {
        char line[160];
        for (size_t i = 0; i < stream.size(); i += 16) {
            int  pos  = std::snprintf(line, sizeof(line), "    db ");
            for (size_t j = i; j < i + 16 && j < stream.size(); ++j) {
                // 恒三位数字开头（%03x）：MASM 十六进制字面量首字符必须是数字
                // （c0h 会被当标识符，CI 实证 A2006 undefined symbol），0c0h 合法。
                pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "%03xh%s",
                                     stream[j], (j + 1 < stream.size() && j + 1 < i + 16) ? "," : "");
            }
            out += std::string(line) + "\n";
        }
        // 编码器保证流非空；空流是契约破坏，.err 汇编期硬失败，不产占位产物。
        if (stream.empty())
            out += "    .err   ; vmx64: empty instruction stream (encoder contract violated)\n";
    }
    out += "\n";
    out += "align 8\n";
    // 表头两 qword：magic（post-link 加密器的字节级定位符，加密后清零）+
    // 站点数（加密器布局校验用，加密后清零）。二者不落 label，消费方只有
    // 字节级扫描的加密器，符号名帮不上忙（release DLL 无 COFF 符号表）。
    out += "    dq " + masmHex(kVmx64TableMagic) + "\n";
    out += "    dq " + masmHex(static_cast<uint64_t>(program.sites.size())) + "\n";
    out += "mj2x_vmx64_sites_" + methodTag + " label qword\n";
    // 站点表 n 个 qword。源级形态 `dq handler符号 - ks64(t)`：链接器把符号
    // 解析成首选 VA，落盘字节 = handler 首选 VA - ks64(t)，且每个表项带一
    // 条 DIR64 重定位（装载器往里加镜像基差）。未加密产物运行时 handler
    // `add r11`（r11 = ks64(t)）直解回真地址。post-link 加密器再把落盘
    // 字节改写为 (handler 首选 VA - ks64(t)) XOR ks64(t, k1) 纯异或形态、
    // 失效重定位，两条路径的运行时算术见 doc:vm-site-table-encryption。
    for (size_t t = 0; t < program.sites.size(); ++t) {
        const uint64_t addend = vmx64Ks64(static_cast<uint64_t>(t), k1);
        out += "    dq mj2x_vmx64_h_" + std::string(vmx64OpHandlerName(program.sites[t].op)) +
               "-" + masmHex(addend) + "\n";
    }
    // 表尾两 qword：
    //   k1：加密器的密钥材料（与 prologue 的 mov r15 imm 同一 C++ 局部变量，
    //       无漂移面），加密后清零。
    //   base：自引用指针，链接期落盘值 = 站点表首选 VA。它是运行时形态
    //       分支的支点，未加密产物装载器照常改写它（运行时 = 表运行时
    //       地址，prologue 的 r12 = 0，走明文 add-only dispatch）；加密后
    //       重定位失效，静态首选 VA 常驻（r12 = 首选 VA - 表运行时地址 =
    //       -delta），handler 侧 sub rdx, r12 补回镜像基差。
    out += "mj2x_vmx64_k1_" + methodTag + " label qword\n";
    out += "    dq " + masmHex(k1) + "\n";
    out += "mj2x_vmx64_base_" + methodTag + " label qword\n";
    out += "    dq mj2x_vmx64_sites_" + methodTag + "\n";
    out += "\n";

    // --- JNI 入口 ---
    // 帧布局（rbp 之下）：[saved rbp][7 个非易失寄存器 0x38][帧头 0x20：
    // env/thisOrClass/保留][vreg 池]。`sub rsp` 发生在 7 个 push 之后，
    // locals 区 = 帧头 + vreg，全部位于 saved 区之下（见 kSavedRegsBytes）。
    const uint32_t vregBytes  = vregBytesFor(program.numSlots);
    const uint32_t localsSize = kFrameHeaderBytes + vregBytes;
    const uint32_t envDisp    = kSavedRegsBytes + 8;   // [rbp-0x40]
    const uint32_t thisDisp   = kSavedRegsBytes + 16;  // [rbp-0x48]
    const uint32_t vregBaseDisp =
        kSavedRegsBytes + kFrameHeaderBytes + vregBytes;   // rsi = rbp - vregBaseDisp

    out += ".code\n";
    out += "public mj2x_vmx64_exec_" + methodTag + "\n";
    out += "; JNI native entry: (JNIEnv* env [rcx], jclass/jobject thisOrClass [rdx],\n";
    out += ";                     p0 [r8], p1 [r9], p2+ on stack)\n";
    out += "; Mixed-linking boundary (Phase 1): env / thisOrClass homed into frame slots\n";
    out += "; [rbp-40h] / [rbp-48h] (below the saved-register area) for Phase 2 extern C\n";
    out += "; helpers (mj2x-rt field access, allocation, exceptions). The Phase 1\n";
    out += "; opcode subset never calls out.\n";
    out += "; Alignment: 8 pushes (incl. rbp) + 16-aligned locals leave rsp = 8 (mod 16)\n";
    out += "; at runtime — any future extern C helper call must re-align to 16 before\n";
    out += "; `call` per the Windows x64 ABI.\n";
    out += "mj2x_vmx64_exec_" + methodTag + " proc frame\n";
    out += "    push rbp\n";
    out += "    .pushreg rbp\n";
    out += "    mov rbp, rsp\n";
    out += "    .setframe rbp, 0\n";
    out += "    push rbx\n";
    out += "    .pushreg rbx\n";
    out += "    push rsi\n";
    out += "    .pushreg rsi\n";
    out += "    push rdi\n";
    out += "    .pushreg rdi\n";
    out += "    push r12\n";
    out += "    .pushreg r12\n";
    out += "    push r13\n";
    out += "    .pushreg r13\n";
    out += "    push r14\n";
    out += "    .pushreg r14\n";
    out += "    push r15\n";
    out += "    .pushreg r15\n";
    out += "    sub rsp, " + masmHex(localsSize) + "\n";
    out += "    .allocstack " + masmHex(localsSize) + "\n";
    out += "    .endprolog\n";
    out += "    mov [rbp-" + masmHex(envDisp) + "], rcx\n";
    out += "    mov [rbp-" + masmHex(thisDisp) + "], rdx\n";
    out += "    lea rsi, [rbp-" + masmHex(vregBaseDisp) + "]\n";
    out += "    lea rdi, [mj2x_vmx64_sites_" + methodTag + "]\n";
    out += "    lea r14, [mj2x_vmx64_code_" + methodTag + "]\n";
    out += "    lea r13, [mj2x_vmx64_done_" + methodTag + "]\n";
    out += "    mov r15, " + masmHex(k1) + "\n";
    // 参数落槽：p0=r8 / p1=r9 / p(i>=2)=[rbp+0x30+8*(i-2)]。
    for (size_t i = 0; i < program.args.size(); ++i) {
        const auto& arg     = program.args[i];
        const auto  slotStr = "+" + masmHex(static_cast<uint64_t>(arg.slot) * 8ull);
        std::string srcReg;
        std::string srcMem;
        if (i == 0)
            srcReg = arg.is64 ? "r8" : "r8d";
        else if (i == 1)
            srcReg = arg.is64 ? "r9" : "r9d";
        else
            srcMem = "[rbp+" + masmHex(static_cast<uint64_t>(0x30 + 8 * (i - 2))) + "]";
        if (!srcMem.empty()) {
            if (arg.is64) {
                out += "    mov rax, qword ptr " + srcMem + "\n";
                out += "    mov qword ptr [rsi" + slotStr + "], rax\n";
            } else {
                out += "    mov eax, dword ptr " + srcMem + "\n";
                out += "    mov dword ptr [rsi" + slotStr + "], eax\n";
            }
        } else {
            const std::string reg = arg.is64 ? "rax" : "eax";
            if (arg.is64)
                out += "    mov rax, " + srcReg + "\n";
            else
                out += "    mov eax, " + srcReg + "\n";
            out += "    mov " + (arg.is64 ? std::string("qword") : std::string("dword")) +
                   " ptr [rsi" + slotStr + "], " + reg + "\n";
        }
    }
    out += "    xor ebx, ebx\n";
    // 形态/基差算定：r12 = base qword 落盘值 - 表运行时地址（rdi）。未加密
    // 产物装载器已把 base 改写成表运行时地址 → r12 = 0（明文 dispatch）；
    // 加密产物 base 保持首选 VA → r12 = -delta，NEXT 用 sub rdx, r12 把基
    // 差补回。NEXT 的 test/jz 以 r12 是否为 0 分流两条算术路径。参数 homing
    // 之后才可动 r8 之外的易变寄存器，这里只用 r12/rdi。
    out += "    lea r12, [mj2x_vmx64_base_" + methodTag + "]\n";
    out += "    mov r12, [r12]\n";
    out += "    sub r12, rdi\n";
    out += "    vmx64_NEXT\n";
    out += "mj2x_vmx64_done_" + methodTag + ":\n";
    out += "    add rsp, " + masmHex(localsSize) + "\n";
    out += "    pop r15\n";
    out += "    pop r14\n";
    out += "    pop r13\n";
    out += "    pop r12\n";
    out += "    pop rdi\n";
    out += "    pop rsi\n";
    out += "    pop rbx\n";
    out += "    pop rbp\n";
    out += "    ret\n";
    out += "mj2x_vmx64_exec_" + methodTag + " endp\n";
    out += "\n";
    return out;
}

}   // namespace mj2x

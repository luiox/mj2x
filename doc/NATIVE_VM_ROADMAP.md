# mj2x — Native VM 路线设计与保护方案

> ⚠️ **Repositioning Notice（2026-06-24 架构重构）**
>
> 本文档原定 VMC4 自研 VM 与 CBackend **并行为两条主路线**。经竞品分析
> （`ref_samples/竞品分析报告.md`）与架构重构评审（`ARCHITECTURE_REDESIGN.md` ADR-3），
> **VMC4 降级为 V2+ 可选增强支线**，主生成后端让位给**寄存器式 C/C++（RegC）**。
>
> 降级理由：
> 1. **资源聚焦** —— mj2x 人手有限，自研 VM（resident opcode 自变异 + opcode alias +
>    资源切片）是 ~7 周量级；寄存器 IR + RegC 在更低成本下命中大部分 c2j gap。
> 2. **性能** —— RegC 生成普通 C/C++ 可被 MSVC 优化，远好于 VM 解释执行。
> 3. **避免 LLVM 依赖** —— `DEPENDENCIES.md` 明确无 LLVM；RegC 不引入新依赖。
>
> **本文档的定位变更**：
> - §1（为什么加 VM）与 §2（整体架构）已重写，反映 RegC 主线 + VMC4 可选支线。
> - §3-§12（VMC4 字节码格式、VmBackend、Native Runtime、反调试、keygen、对照表等）
>   **作为 VMC4 支线的设计参考保留**，设计内容本身有价值，只是不优先实现。
> - VMC4 的实现排在 ROADMAP 的 R5+ 之后，视 RegC + RT 闭环后的实际抗分析需求启动。
> - 本文与 `ARCHITECTURE_REDESIGN.md` 冲突时，以 redesign 文档为准。
>
> 原始定位（已作废，保留作历史）：
> > 基于现有 CBackend（JNIC 翻译）的基础，向自定义 Native VM 执行路线演进。
> > 让 mj2x 不仅是"Java→C 翻译器"，更是"Java→VM 字节码→Native VM 解释器"的保护工具链。
> >
> > 参考：JavaShroud VBC4/NBVM 路线、masxinlingvonta LLVM IR 路线
> > 前置：现有 MIR + CBackend 体系（详见 ARCHITECTURE.md）

---

## 1. 为什么要加 VM 路线

### 1.1 现有 CBackend 的保护局限性

| 问题 | 说明 |
|------|------|
| **固定翻译模板** | 每个方法生成的 JNI C 代码结构高度雷同，`stack[]` + `sp` + `goto` 模式可被脚本化识别 |
| **原始语义直露** | 字段读写、方法调用、算术运算的 JNI 调用直接在 C 源码中可见 |
| **无差异化保护** | 所有方法用同一套模板，ABI 一致，一个逆向工具可通杀全部 |
| **无运行期认证** | 生成的 .dll 不校验自身完整性，可被替换或 patch |
| **无密钥材料** | 所有保护依赖"源码不可见"，一旦拿到 .c 文件就无后续防护 |

### 1.2 VM 路线解决的问题

| 能力 | VM 方案 | 效果 |
|------|---------|------|
| **协议实例化** | 每个构建产物有独立的 VM 字节码编码、密钥、布局 | 不存在跨样本通用反编译脚本 |
| **语义层模糊** | 原始 Java 操作被翻译为 VM opcode 序列，不直接对应 JNI 调用 | 分析面从"读 C 代码"升级为"逆向 VM 协议" |
| **差异化** | Per-method opcode alias、block split、常量池加密 | 不同方法用不同解码路径 |
| **运行期校验** | VM 执行前校验资源 HMAC + 入口 token + 状态绑定 | patch 或替换资源会被检测 |
| **密钥分层** | Build-time 注入密钥，运行时派生执行上下文 | 公开源码不能直接变成通用解包器 |

### 1.3 适用场景决策（重构后）

VMC4 不再是"与 CBackend 互补的并行主线"，而是"RegC 之外的**可选深度保护支线**"：

```
方法保护路线选择（重构后）
├─ 普通方法 → [L1-L5] RegC（寄存器式 C/C++）—— 主路线
│   适用：绝大多数方法。性能好，已消除 cstack 锚点，命中 c2j Gap A/C/D。
│
└─ 极高价值方法（license check / 核心算法 / 协议组装）→ VMC4 虚拟化（可选支线）
    适用：少数需要"resident opcode 自变异 + opcode alias + 资源切片"额外抗分析层的方法。
    代价：解释执行性能损耗 10-50x；实现复杂度高（~7 周）。
    时机：RegC + RT 闭环后，据 mjdbg 攻击反馈决定是否启动（ROADMAP R5+）。
```

**判断原则（重构后）**：RegC 是默认主线；VMC4 是 RegC 抗性不足时对少数高价值方法的加固。
两者不再是"互补并行"，而是"主线 + 可选增强"。

> 原始判断原则（已作废，保留作历史）：
> > 方法是否适合 VM 保护？
> > ├─ 纯计算 + 高频调用 → 不适合（性能损耗大），继续用 CBackend
> > ├─ 纯计算 + 低频调用 → 可考虑
> > ├─ 含敏感逻辑（license check / 算法核心 / 协议组装） → 强烈推荐
> > ├─ 含大量 JNI 互操作 → 需评估，VM 中 Call*Method 开销叠加
> > └─ 含异常处理 / synchronized → CBackend 已有已知限制，VM 同样不完美
> >
> > **判断原则**：VM 路线和 CBackend 是互补关系，不是替代关系。CBackend 处理"需要高性能的简单方法"，VM Backend 处理"高价值的关键方法"。

---

## 2. 整体架构（重构后：RegC 主线 + VMC4 可选支线）

```
                     ┌───────────────────────────────────────────────────┐
                     │                   Pipeline                         │
                     │                                                    │
   .class/.jar ──────▶│  InputLoader (复用) → CompilerIndex (复用)         │
                     │    │                                               │
                     │    ▼                                               │
                     │  Preprocessors                                     │
                     │    │                                               │
                     │    ▼                                               │
                     │  [L1] CodeConverter → 栈式 MIR (复用)               │
                     │    │                                               │
                     │    ▼                                               │
                     │  [L2] SSA 构造 (★新增)                             │
                     │    │                                               │
                     │    ▼                                               │
                     │  [L3] 寄存器 IR (★新增)                            │
                     │    │                                               │
                     │    ▼                                               │
                     │  [L4] IR 变换 pass (★新增: 常量折叠/DCE/MBA/CFF)    │
                     │    │                                               │
                     │    ▼                                               │
                     │  Backend (可插拔, Preprocessor 按方法分发)          │
                     │    │                                               │
                     │    ├── [L5] RegC (★ 主路线)                         │
                     │    │     寄存器 IR → 寄存器式 C/C++                  │
                     │    │     (无 cstack, 无 jvalue 联合, 自定义符号 helper)│
                     │    │                                               │
                     │    └── VmBackend (★ 可选支线, V2+/R5+)              │
                     │          寄存器 IR → VMC4 字节码                    │
                     │          + 资源封装 + 密钥注入 + stub + loader       │
                     │    │                                               │
                     │    ▼                                               │
                     │  Postprocessors                                    │
                     │    │                                               │
                     │    ▼                                               │
                     │  OutputWriter (复用)                                │
                     └───────┬───────────────────────────────────────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
        Hybrid JAR (.class          Native DLL (.dll/.so)
         + VMC4 resources           ├─ [RT] 运行时（crypto/kernel/antidebug/
         + key material               antijvmti/stringpool, ★ R3 重写）
             (仅 VMC4 支线)          ├─ VM 解释器核心（仅 VMC4 支线）
                                     └─ RegC 生成的 C/C++ 编译产物（主路线）
```

> **与 v0.1 架构的差异**：v0.1 是 CBackend + VmBackend 并行（无 SSA/寄存器 IR 中间层）。
> 重构后主路线是 L1→L2→L3→L4→RegC 五层；VMC4 接在 L4 之后作为可选支线（而非独立路径）。
> 这意味着 VMC4 也享受寄存器化的成果（无 cstack 锚点），比 v0.1 设计更强。

### 2.1 新增模块概览（重构后）

| 模块 | 位置 | 职责 | 路线 |
|------|------|------|------|
| **SsaBuilder** | `src/mj2x/mir/ssa/` | [L2] 栈式 MIR → SSA（reaching-def + dominance + φ） | 主线 R1 |
| **RegLowering** | `src/mj2x/mir/register_ir/` | [L3] SSA → 寄存器 IR（φ 消解 + 虚拟寄存器） | 主线 R1 |
| **PassPipeline** | `src/mj2x/mir/passes/` | [L4] 常量折叠/DCE/MBA/CFF（schema + Kahn planner） | 主线 R4 |
| **RegCBackend** | `src/mj2x/backend/regc/` | [L5] 寄存器 IR → 寄存器式 C/C++（主生成后端） | 主线 R2 |
| **VmBackend** | `src/mj2x/backend/vm/` | 寄存器 IR → VMC4 字节码 + 资源封装 | 可选支线 R5+ |
| **VmConfigPreprocessor** | `src/mj2x/preprocessor/vm_config.hpp` | 标记哪些方法走 VMC4 支线 | 可选支线 R5+ |
| **runtime/crypto** | `runtime/src/crypto/` | SHA256/HMAC/AES（纯 C） | 主线 RT R3 |
| **runtime/kernel** | `runtime/src/kernel/` | HKDF 派生 + HMAC 校验 + volatile wipe | 主线 RT R3 |
| **runtime/antidebug** | `runtime/src/antidebug/` | PEB/ptrace/sysctl + 完整性校验 | 主线 RT R3 |
| **runtime/antijvmti** | `runtime/src/antijvmti/` | env->functions 篡改检测 + agent 扫描 | 主线 RT R3 |
| **runtime/vm** | `runtime/src/vm/` | VMC4 解释器（仅 VMC4 支线需要） | 可选支线 R5+ |
| **keygen** | `runtime/keygen/` | 构建期密钥生成（xmake rule） | 可选支线 R5+ |

> **与 v0.1 的差异**：v0.1 只列 VmBackend + runtime/vm + keygen（VM 专用）。
> 重构后 crypto/kernel/antidebug/antijvmti 是**主线 RT**（RegC 也依赖它们做符号 helper、
> 字符串池、反 JVMTI）；只有 runtime/vm + keygen 是 VMC4 支线专用。

---

## 3. VMC4 字节码设计

### 3.1 定位

VMC4（Virtual Machine Codec v4）是 mj2x 的自定义 VM 字节码格式。它不是一个通用 VM，而是专门为**执行翻译后的 Java 方法语义**设计的轻量解释器。

### 3.2 设计原则

| 原则 | 含义 |
|------|------|
| **贴近 MIR** | VMC4 指令集直接从 MIRKind 映射过来，降低 CodeConverter 改造成本 |
| **~60 条 opcode** | 不需要覆盖全部 JVM 指令，只覆盖 MIR 中出现的高频指令 |
| **寄存器式+栈式混合** | 算术用虚拟寄存器，JNI 调用用栈传参 |
| **定长指令** | 每条指令 4 字节（opcode + 3 个操作数），简化解析 |
| **opcode alias** | 同一语义可用多个 opcode 编码，per-method 随机选择 |

### 3.3 指令集草案

```
VMC4 指令格式:
  [opcode: u8] [operand1: u8] [operand2: u8] [operand3: u8]

寄存器: R0-R15 (u8 索引)
立即数: 通过常量池间接引用 (operand = cp_index)

类别       │ opcode range │ 示例
───────────┼──────────────┼────────────────────
常量操作   │ 0x00-0x0F    │ LOAD_I32 cp_idx, Rd
局部变量   │ 0x10-0x1F    │ LOAD_LOCAL slot, Rd / STORE_LOCAL Rd, slot
算术运算   │ 0x20-0x3F    │ IADD Rd, Rs1, Rs2 / IMUL Rd, Rs1, Rs2
类型转换   │ 0x40-0x4F    │ I2L Rd, Rs / D2I Rd, Rs
栈操作     │ 0x50-0x5F    │ DUP / SWAP / POP
控制流     │ 0x60-0x6F    │ JMP label / BEQ Rs1, Rs2, label
字段访问   │ 0x70-0x7F    │ GETFIELD cp_idx, Rd / PUTFIELD Rd, Rs, cp_idx
方法调用   │ 0x80-0x8F    │ INVOKEVIRTUAL cp_idx / INVOKESTATIC cp_idx
对象操作   │ 0x90-0x9F    │ NEW cp_idx, Rd / CHECKCAST cp_idx / ARRAYLENGTH Rd, Rs
数组操作   │ 0xA0-0xAF    │ IALOAD Rd, Rs_arr, Rs_idx / IASTORE Rd, Rs_arr, Rs_idx
返回       │ 0xB0-0xBF    │ RETURN / IRETURN Rs / ARETURN Rs
扩展       │ 0xC0-0xFF    │ 预留：monitor、throw、多指令折叠
```

### 3.4 Opcode Alias 方案

每个方法在构建时从 alias 池中随机抽取替代 opcode：

```
IADD 的 alias 池:
  alias_0 = 0x20  (默认)
  alias_1 = 0xE1
  alias_2 = 0xA3
  alias_3 = 0x7C

每个方法在 VMC4 资源头中记录 alias_map:
  struct alias_entry { u8 original; u8 aliased; };
```

VM 解释器的 dispatch 表在运行时根据 alias_map 重建，不是硬编码的 switch-case。

### 3.5 常量池加密

```
VMC4 资源结构:
┌─────────────────────────────┐
│  Resource Header            │
│  ├─ magic: "VMC4"           │
│  ├─ method_id_hash: u64     │
│  ├─ entry_token: u64        │
│  ├─ alias_map_offset: u32   │
│  ├─ cp_offset: u32          │
│  ├─ code_offset: u32        │
│  └─ layout_digest: u32      │
├─────────────────────────────┤
│  Opcode Alias Map           │
│  (AES-CTR 加密)             │
├─────────────────────────────┤
│  Constant Pool              │
│  ├─ 每个 entry 独立加密      │
│  │  (XOR per-entry key)     │
│  └─ 字符串用 zstd 压缩      │
├─────────────────────────────┤
│  Code Section               │
│  ├─ 指令序列                 │
│  └─ block dispatch table    │
├─────────────────────────────┤
│  HMAC-SHA256 (完整性校验)    │
└─────────────────────────────┘
```

---

## 4. VmBackend 详细设计

### 4.1 输入输出

```
输入:  vector<Block*> + CompilerMethod
输出:  BackendResult { kind = VM_RESOURCE, data = VMC4 binary blob }

同时产生的副作用:
  - 修改 .class 方法体为 native stub（复用 injectIntoClass）
  - 生成运行时所需的密钥材料（写入 key material 文件）
```

### 4.2 编码流程

```
MIR Blocks
    │
    ▼
[Register Allocation]
    │  将 stack/sp 模型转换为虚拟寄存器 R0-R15
    │  需要做简单的 liveness 分析
    │  (MVP 可先用 stack 直译，后续加 regalloc)
    │
    ▼
[Instruction Selection]
    │  MIRKind → VMC4 opcode
    │  如果该方法配置了 alias，随机选择 alias 变体
    │
    ▼
[Constant Pool Building]
    │  收集所有常量值 → cp entries
    │  字符串常量 → zstd 压缩 → XOR per-entry key
    │  int/long/float/double 常量 → XOR obfuscated
    │
    ▼
[Resource Packaging]
    │  组装 header + alias_map + cp + code
    │  HMAC-SHA256 计算完整性校验值
    │  AES-CTR 加密 alias_map 和敏感段
    │
    ▼
[Key Material Generation]
    │  每个方法/每次构建生成:
    │    - method_id_hash (FNV1a-64 of class.method:desc)
    │    - cp_encryption_key (per-method, AES-128)
    │    - entry_token (随机 u64, 注册到 runtime)
    │    - build_root_key (整个构建共享)
    │
    ▼
VmBackendResult
```

### 4.3 Backend 接口适配

```cpp
// 现有 Backend 接口不变，新增 VmBackend 实现
class VmBackend : public Backend {
public:
    std::string_view name() const override { return "vm_native"; }

    BackendResult compileMethod(
        const std::string& className,
        CompilerMethod& method,
        const std::vector<Block*>& blocks
    ) override;

    void injectIntoClass(
        CompilerClass& clazz,
        CompilerMethod& method,
        const BackendResult& result
    ) override;

    void finalize(const CompilerIndex& index) override;

    // VmBackend 专有配置
    void setKeyOutputPath(const std::string& path);
    void setPerMethodAlias(bool enabled);     // 默认 true
    void setConstantEncryption(bool enabled); // 默认 true
};
```

---

## 5. Native Runtime 设计

### 5.1 模块划分

```
runtime/src/
├── vm/                          # VM 解释器核心（★ 新增）
│   ├── vm_core.h                # VM API：加载、执行、销毁
│   ├── vm_core.c                # opcode dispatch 主循环
│   ├── vm_insn.h                # 指令辅助函数
│   ├── vm_insn.c
│   ├── vm_symbol.h              # JVM 符号解析缓存
│   ├── vm_symbol.c
│   ├── vm_memory.h              # VM 堆/栈/寄存器管理
│   └── vm_memory.c
│
├── kernel/                      # 微内核（★ 新增）
│   ├── kernel.h                 # Init / Verify / Decrypt API
│   ├── kernel.c                 # 密钥校验、HMAC、解密
│   ├── kernel_secrets.inc       # 构建时自动生成的密钥(CI inject)
│
├── antidebug/                   # 反调试模块（★ 新增）
│   ├── antidebug.h
│   ├── antidebug.c              # PEB / ptrace / sysctl 探测
│   ├── antidump.c               # /proc/self/mem 防护 / PR_SET_DUMPABLE
│   └── integrity.c              # 自身代码段哈希校验
│
├── crypto/                      # 密码学原语（★ 新增）
│   ├── crypto.h
│   ├── aes.c                    # AES-128 软件实现（无 OpenSSL 依赖）
│   ├── sha256.c                 # SHA-256
│   └── fnv1a.c                  # FNV-1a 哈希
│
├── obf/                         # 运行期字符串/常量反混淆（已有骨架）
│   ├── rt_str_obf.h             # 已有，功能需补全
│   └── rt_str_obf.cpp
│
├── jni_wrapper.h                # 已有：JNI 类型包装
├── platform.h                   # 已有：平台检测宏
├── rt_insn.h                    # 已有（仅 nop 宏，需扩展）
├── rt_memory.h                  # 已有（空壳，需填充）
│
├── main.cpp                     # 已有（DLL 入口 + JNI_OnLoad）
└── jni_bridge.c                 # ★ 新增：JNI RegisterNatives 注册
```

### 5.2 VM 解释器核心循环

```c
// vm_core.h
typedef struct {
    uint8_t  opcode;
    uint8_t  operand[3];
} vm_insn_t;

typedef struct {
    uint64_t   registers[16];     // R0-R15
    int        sp;                 // 栈指针
    uint64_t   stack[256];         // 操作数栈
    uint8_t*   code_start;         // 当前代码段
    uint32_t   code_len;
    uint32_t   pc;                 // 程序计数器
    vm_cp_t*   cp;                 // 常量池
    vm_symbol_cache_t* symbols;    // JNI 符号缓存
    JNIEnv*    env;                // 当前线程 JNIEnv
    uint8_t    dispatch_table[256]; // opcode → dispatch index
    uint32_t   resident_key;       // 常驻 opcode 加密 key
    uint32_t   epoch;              // 执行轮次（用于重加密）
} vm_state_t;

// opcode dispatch 主循环
int vm_execute(vm_state_t* state);
```

```c
// vm_core.c (核心 dispatch)
int vm_execute(vm_state_t* s) {
    for (;;) {
        vm_insn_t insn;
        // 加载指令，同时解密 opcode (resident encryption)
        insn.opcode  = s->code_start[s->pc]     ^ resident_key_derive(s);
        insn.operand[0] = s->code_start[s->pc+1];
        insn.operand[1] = s->code_start[s->pc+2];
        insn.operand[2] = s->code_start[s->pc+3];

        // 通过 dispatch_table 间接跳转
        switch (s->dispatch_table[insn.opcode]) {
        case DISPATCH_IADD: {
            int idx = s->dispatch_table[insn.opcode];
            s->registers[insn.operand[0]] =
                s->registers[insn.operand[0]] + s->registers[insn.operand[1]];
            s->pc += 4;
            break;
        }
        case DISPATCH_JMP: {
            s->pc = resolve_block_target(s, insn.operand[0]);
            break;
        }
        case DISPATCH_INVOKEVIRTUAL: {
            // 从常量池加载类/方法引用 → JNI Call*Method
            vm_invoke_virtual(s, insn.operand[0]);
            s->pc += 4;
            break;
        }
        // ... ~60 种 dispatch
        case DISPATCH_RETURN:
            return 0;
        }
        // 每 N 条指令后 epoch++
        if (++s->epoch % RESIDENT_REKEY_INTERVAL == 0)
            vm_rewrap_opcodes(s);  // 重新加密 opcode
    }
}
```

### 5.3 微内核

```c
// kernel.h
// 构建时注入的密钥材料
typedef struct {
    uint8_t  aes_key[16];         // 常量池解密
    uint8_t  hmac_key[32];        // VMC4 资源 HMAC
    uint64_t entry_token;         // 入口 token
    uint32_t layout_digest;       // 布局摘要
    uint32_t build_id;            // 构建标识
} kernel_secrets_t;

// 运行期 API
int  kernel_init(JNIEnv* env, const kernel_secrets_t* secrets);
int  kernel_verify_resource(const uint8_t* data, uint32_t len,
                            const uint8_t* hmac_expected);
int  kernel_decrypt_cp(const uint8_t* encrypted, uint32_t len,
                       uint8_t* out, const uint8_t* key);
void kernel_wipe(uint8_t* buf, uint32_t len);  // 安全擦除
```

### 5.4 反调试模块

参考 JavaShroud 的 `js_antidebug.c`，但只实现 mj2x 需要的最小集：

| 技术 | 平台 | 实现 |
|------|------|------|
| PEB BeingDebugged | Windows | `__readgsqword(0x60)` → `BeingDebugged` |
| NtQueryInformationProcess | Windows | `ProcessDebugPort` 查询 |
| ptrace 检测 | Linux | `prctl(PR_SET_DUMPABLE, 0)` + `TracerPid` |
| 完整性校验 | 跨平台 | 加载后对 `.text` 段做 SHA-256 校验 |
| Anti-dump | Linux | `PR_SET_DUMPABLE` |

MVP 阶段只做 Windows PEB 检测 + 自身完整性校验，其他平台和高级技术后置。

### 5.5 构建期密钥生成

每个构建的密钥材料由独立的 `keygen` 工具生成：

```bash
# xmake rule: 在构建 native dll 前自动运行
xmake run mj2x-keygen --out runtime/src/kernel/kernel_secrets.inc
```

`kernel_secrets.inc` 格式：

```c
// 由 mj2x-keygen 自动生成，每次构建不同
#define BUILD_ID 0x7A3F9C1E
static const uint8_t KERNEL_AES_KEY[16] = { ... };
static const uint8_t KERNEL_HMAC_KEY[32] = { ... };
static const uint64_t KERNEL_ENTRY_TOKEN = 0x...;
static const uint32_t KERNEL_LAYOUT_DIGEST = 0x...;
```

密钥同时写入 JAR 资源的 `META-INF/javashroud-keys/`（名称可改），供 Java 侧 stub 在加载 native 库时做密钥对账。

---

## 6. 与现有 CBackend 的集成关系

### 6.1 Pipeline 支持双 Backend

```cpp
// Pipeline 中允许注册多个 Backend
// Preprocessor 决定每个方法用哪个 Backend
class VmMarkerPreprocessor : public Preprocessor {
public:
    void process(CompilerIndex& index) override {
        for (auto& [name, cls] : index.classes_) {
            for (auto& method : cls.methods) {
                // 根据注解 / 配置 / 方法签名决定
                if (shouldVmProtect(method)) {
                    method.setUserData("backend", "vm");
                }
            }
        }
    }
};

// Pipeline::compileMethods 中按标记分发:
for (auto& method : methods) {
    auto backendName = method.getUserData("backend");
    if (backendName == "vm") {
        vmBackend.compileMethod(clsName, method, blocks);
    } else {
        cBackend.compileMethod(clsName, method, blocks);
    }
}
```

### 6.2 Hybrid JAR 输出

```
output.jar/
├── com/example/MyClass.class        # 含 native stub 的 class
├── com/example/Helper.class         # 未处理的 class
└── META-INF/
    ├── MANIFEST.MF
    └── vmc4/
        ├── com.example.MyClass.method1.vmc4   # VMC4 资源
        ├── com.example.MyClass.method2.vmc4
        ├── index.registry                     # call gate 注册表
        └── build.key                          # 本次构建的密钥材料
```

运行时 class 加载流程：
1. JVM 加载 `MyClass.class`，遇到 `native` 方法
2. `System.loadLibrary("mj2x-runtime")` 加载 native DLL
3. DLL 的 `JNI_OnLoad` 解析 `index.registry`，注册 VMC4 资源的 entry token → 方法映射
4. `MyClass.method1()` 被调用 → JNI stub → VM 解释器根据 entry token 查找并加载 `.vmc4` 资源 → 解密 → 解析 → 执行

---

## 7. MVP 范围（V0: VM Backend 原型）

> ⚠️ **重构后注（2026-06-24）**：本节的"V0 ~7 周 MVP"是 VMC4 支线的实现计划，
> **不再是 mj2x 的整体 MVP**。mj2x 整体 MVP 现在是 ROADMAP 的 R0-R2（架构设计 +
> SSA/寄存器 IR + RegC）。VMC4 V0 排在 R5+ 之后启动。本节时间表保留作 VMC4 支线启动时的参考。


### 7.1 包含

| 组成部分 | 范围 | 预计工作量 |
|----------|------|-----------|
| **VmBackend** | 支持 MIR subset（常量/局部变量/算术/控制流/返回）→ VMC4 编码 | ~2 周 |
| **VMC4 格式** | Header + CP + Code 三段式，无 alias、无加密 | ~1 周 |
| **vm_core** | 约 30 条 opcode 的 C 解释器，stack 模型，JNI 回掉 | ~2 周 |
| **jni_bridge** | JNI_OnLoad + RegisterNatives + entry token 注册 | ~0.5 周 |
| **keygen** | 最小版本：每构建生成随机 token + HMAC key | ~0.5 周 |
| **E2E** | 一个纯计算 Java 方法：Java → VMC4 → VM execute → 结果正确 | ~1 周 |
| **合计** | | **~7 周** |

### 7.2 不包含（MVP 后）

- Opcode alias（所有方法用固定 opcode）
- 常量池加密（明文 CP）
- Resident opcode re-encryption
- 反调试模块（需要时再打开）
- 异常处理 / monitor / invokedynamic 虚拟化
- 多平台 native 构建（MVP 只做 Windows MSVC）
- kernel_secrets.inc 自动注入（MVP 手写固定密钥）

### 7.3 不做的权衡

```
性能:  VM 解释器比 CBackend 生成的 JNI C 代码慢 10-50x
       这是路线选择的固有代价，不是优化问题
       对于低频关键方法可以接受，高频方法继续用 CBackend

安全性: MVP 无加密无 alias，只验证"VM 能正确执行翻译后的方法"
        安全性是 V1/V2 的增量目标，MVP 只验证功能可行性

测试:  MVP 只做端到端正确性测试
       不测抗逆向、抗篡改、性能基线
```

### 7.4 与现有 dev.5 的关系

```
原始 dev.5:  bytecode → C++ 翻译 MVP
调整后:      dev.5 拆为两个子里程碑

dev.5a (C++ 翻译完善):
  完成现有 CBackend 的已知缺陷（JNI ID 缓存、JAR 支持等）
  时间: ~3 周

dev.5b (VM Backend MVP):
  VmBackend + vm_core 原型
  时间: ~7 周（与 dev.5a 可并行）
```

---

## 8. 风险

| 风险 | 等级 | 应对 |
|------|------|------|
| **VM 解释器性能不可接受** | 中 | MVP 后加简单 JIT 或选择性翻译回 C；高频方法用 CBackend |
| **JNI 回调叠加开销太大** | 中 | VM 中减少 Call*Method 频次，批量处理；或直接链接已知方法 |
| **VMC4 格式设计不合理** | 低 | MVP 用简单格式，V1 重构格式，不破坏接口即可 |
| **多后端 Pipeline 复杂度** | 低 | Backend 接口已抽象，Preprocessor 标记即可 |
| **构建期密钥注入增加 CI 复杂度** | 低 | MVP 用手写密钥，CI 集成是 V1 任务 |
| **团队无 VM 解释器经验** | 中 | 控制在 ~60 opcode + stack 模型，参考 lua/v3 等小型 VM 设计 |

---

## 9. 长期演进路线

> ⚠️ **重构后注（2026-06-24）**：下表的 V0-V4 是 **VMC4 支线**的演进，
> 不再是 mj2x 整体路线。mj2x 整体路线见 `ROADMAP.md` 的 R0-R5。
> VMC4 支线启动时机 = R5+（RegC + RT 闭环后，据 mjdbg 攻击反馈决定）。

```
V0 (MVP):  功能验证
  ├─ VmBackend + vm_core (~30 opcode)
  ├─ 明文 CP + 固定 opcode
  └─ 手动密钥注入

V1 (基础保护):  差异化
  ├─ Opcode alias per-method
  ├─ 常量池 AES 加密
  ├─ HMAC 完整性校验
  ├─ 构建期自动密钥生成
  └─ 反调试: PEB 检测

V2 (深度保护):  运行期动态化
  ├─ Resident opcode 重加密
  ├─ entry token + 状态绑定
  ├─ block 拆分/合并扰动
  ├─ 异常边 masking
  └─ 反调试: 全平台 (ptrace/sysctl/TracerPid)

V3 (体系化):  融合现有混淆工具链
  ├─ 与 morpher 的 obfuscate pass 联动
  │   (先混淆再翻译，或先翻译再混淆 VM)
  ├─ CFG 平坦化 → VM block dispatch
  ├─ 字符串加密在 VM 层处理
  └─ 方法间 dispatch seed 耦合

V4+ (未来):
  ├─ LLVM 后端 + OLLVM 移植
  ├─ PE 壳 + 导入表保护
  └─ 硬件绑定 (TPM / 设备指纹)
```

---

## 10. 与 JavaShroud 的对照

| 维度 | JavaShroud VBC4 | mj2x VMC4（计划） |
|------|----------------|-------------------|
| 构建期 | Kotlin + ASM | C++ + libjbytecode |
| MIR | 无（直接 ASM→VBC4） | 有（MIR 中间层，可复用现有 CodeConverter） |
| VM opcode 数量 | ~200 | ~60（MVP），~120（V2） |
| Register | 有（register IR） | MVP 用 stack，V1 加 register |
| Opcode alias | 有 | V1 加入 |
| Resident 加密 | 有 | V2 加入 |
| 反调试 | PEB/ptrace/sysctl/TracerPid | V1 PEB，V2 全平台 |
| 资源切片 | 有（VBC4S） | V2 考虑 |
| JNI 回掉 | 全量（所有 Java 互操作） | 同左 |
| 密钥注入 | Kotlin 构建脚本 + native_secrets.inc | xmake rule + kernel_secrets.inc |
| 桌面 GUI | Wails + Vue | mj2x 不提供（morpher-web 覆盖） |
| 开源协议 | GPLv3 | GPLv3 |

---

## 11. 文件清单与实现状态

```
mj2x/
├── doc/
│   └── NATIVE_VM_ROADMAP.md        ← 本文档（新增）
├── src/mj2x/
│   ├── backend/
│   │   ├── backend.hpp             (已有)
│   │   ├── backend_result.hpp      (已有)
│   │   ├── c_backend.hpp/cpp       (已有)
│   │   └── vm/                     (★ 新增目录)
│   │       ├── vm_backend.hpp      (VmBackend 类声明)
│   │       ├── vm_backend.cpp      (MIR → VMC4 编码)
│   │       ├── vm_encoder.hpp      (VMC4 字节码编码器)
│   │       ├── vm_encoder.cpp
│   │       ├── vm_packer.hpp       (资源封装 + HMAC)
│   │       └── vm_packer.cpp
│   ├── preprocessor/
│   │   ├── preprocessor.hpp        (已有)
│   │   ├── mark_all_preprocessor.hpp (已有)
│   │   └── vm_config_preprocessor.hpp (★ 新增：VM 标记)
│   └── core/
│       ├── pipeline.hpp/cpp        (已有，需扩展支持多 backend)
│       └── compiler_index.hpp/cpp  (已有，需加 backend 选择字段)
├── runtime/
│   ├── src/
│   │   ├── vm/                     (★ 新增目录)
│   │   │   ├── vm_core.h/c
│   │   │   ├── vm_symbol.h/c
│   │   │   └── vm_memory.h/c
│   │   ├── kernel/                 (★ 新增目录)
│   │   │   ├── kernel.h/c
│   │   │   └── kernel_secrets.inc  (自动生成)
│   │   ├── antidebug/              (★ 新增目录)
│   │   │   ├── antidebug.h/c
│   │   │   └── integrity.h/c
│   │   ├── crypto/                 (★ 新增目录)
│   │   │   ├── crypto.h
│   │   │   ├── aes.c
│   │   │   ├── sha256.c
│   │   │   └── fnv1a.c
│   │   ├── jni_bridge.c            (★ 新增：JNI 注册)
│   │   ├── main.cpp                (已有，需适配)
│   │   └── ... (已有文件)
│   └── keygen/                     (★ 新增目录)
│       ├── keygen_main.cpp         (密钥生成工具 CLI)
│       └── xmake.lua               (构建规则)
└── xmake.lua                       (已有，需加新 target)
```

---

## 12. 后续行动

### 近期（1-2 周）

1. **在现有代码上做技术验证**：手写一个极简 C VM（~10 opcode），手动编码一个小方法，验证 VM 解释器能通过 JNI 正确执行
2. **确认 MIR subset 足以覆盖目标方法**：选取 3-5 个真实 class，用 `--dump-mir` 查看生成的 MIR 节点类型分布
3. **决定 VMC4 指令集最后 60 条**：根据验证结果精确定义 MVP 指令范围

### 中期（3-4 周）

4. **实现 VmBackend**：MIR → VMC4，不加密，不 alias
5. **实现 vm_core MVP**：~30 opcode 的 C 解释器，跑通 E2E
6. **Pipeline 多 backend 支持**：Preprocessor 标记 + Pipeline 分发

### 远期（5-7 周）

7. **keygen 工具** + kernel_secrets.inc 自动生成
8. **Windows 反调试**（PEB 检测）
9. **E2E 测试闸门**：Java 编译 → mj2x VmBackend → VM 执行 → 结果验证

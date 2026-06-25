# mj2x — Test Strategy

> **v2（2026-06-24 架构重构后）**：测试分层对齐新的五层 IR（L1-L5）+ RT 运行时。
> v1 的测试目标（≥80 单元，仅针对 CBackend）调整为按层分质量门。
> 重构依据见 `ARCHITECTURE_REDESIGN.md`。

---

## 1. 测试分层（对齐 L1-L5 + RT）

### 1.1 现有基线（CBackend MVP，50 测试，保留）

CBackend MVP（`feat/mj2x/mvp-completion` 分支）的 50 测试是**重构期的 L1 + legacy 基线**，
全部保留，不删：

| 测试 | 层 | 用例 | 处置 |
|------|----|------|------|
| `core/compiler_index_test` | 基础设施 | 2 | 保留 |
| `core/io_test`, `input_loader_test`, `jar_io_test` | 基础设施 | 8 | 保留 |
| `core/e2e_test`（CBackend 版） | legacy CBackend | 3 | 随 CBackend 移 reference，标 legacy |
| `frontend/code_converter_test` | **L1** 栈式 MIR | 13 | 保留（L1 基线） |
| `backend/c_backend_test` | legacy CBackend | 11 | 随 CBackend 移 reference，改 `*_legacy_test` 或 `DISABLED_`，不删 |

### 1.2 各层测试目标（R1-R4 落地时补）

| 层 | 测试文件（计划） | 测试内容 | 落地波次 |
|----|----------------|---------|---------|
| **L1** 栈式 MIR | `frontend/code_converter_test`（已有）+ 补 INVOKEDYNAMIC/MULTIANEWARRAY 提取 | 字节码→MIR、CFG、L1 preprocessor | R1 补 |
| **L2** SSA | `mir/ssa_test`（★新增） | reaching-def、dominance、φ 插入、重命名正确性 | R1 |
| **L3** 寄存器 IR | `mir/register_ir_test`（★新增） | φ 消解、虚拟寄存器分配、**操作数无 stack slot**（硬断言） | R1 |
| **L4** 变换 pass | `mir/passes/*_test`（★新增） | 每个 pass（常量折叠/DCE/MBA/CFF）的语义保持 + 变换生效 | R4 |
| **L5** RegC | `backend/regc_test`（★新增） | RegIR→C 生成正确性、**生成 C 无 `cstack` 模式**（硬断言） | R2 |
| **RT** 运行时 | `runtime/*_test`（★新增，C 单测） | crypto/kernel/antidebug 正确性；antijvmti 在 mock 篡改下 fail-closed | R3 |

### 1.3 关键结构断言（防御有效性验证）

这些断言直接对应 `ARCHITECTURE_REDESIGN.md §4` 防御矩阵，是"防御是否生效"的客观检查：

| 断言 | 层 | 含义 | 对应 c2j 攻击 |
|------|----|------|--------------|
| RegIR 操作数无 stack slot | L3 | dump RegModule，grep 不到 stack/local index | cstack 正则失配 |
| 生成 C 无 `cstack[` 子串 | L5 | 对 RegC 产物做字符串断言 | CSTACK_RE 失配 |
| 生成 C 无 "Cannot invoke" 子串 | L5 | 对 RegC 产物做字符串断言 | throw_reason 兜底失配 |
| 生成 C 无全局 `cclasses[`/`cmethods[` 数组 | L5 | 符号走 helper，非全局表 | TABLE_RE/cache_table 失配 |
| antijvmti 在 mock 篡改 `env->functions` 时 fail-closed | RT | 单元测试注入假 vtable | c2j vtable 劫持反制 |

### 1.4 集成 / E2E 测试

| 测试 | 范围 | 方法 | 落地波次 |
|------|------|------|---------|
| Pipeline 空跑（mock backend） | pipeline → preproc → L1→L2→L3→L4 → backend → postproc → write | mock backend 验证调用链 | R1 |
| MIR → SSA → RegIR → RegC | 完整 IR 翻译路径 | 真实 bytecode → RegC C 代码 | R2 |
| InputLoader → Pipeline → OutputWriter | 完整读-改-写轮转 | .class/.jar roundtrip（已有） | 已有 |
| Java → mj2x → .c → cl → 运行 | 端到端行为对比 | JDK + MSVC，结果与原 Java 一致 | R2 |
| 受保护样本 + c2j agent | anti-JVMTI 有效性 | 注入 c2j agent，验证 fail-closed 或 trace 退化（**与 mjdbg 协作**） | R3 |

---

## 2. 测试组织

```
src/unittest/
├── main.cpp                                — gmock main（test::setup("mj2x")）
├── core/                                   — 基础设施测试（已有）
│   ├── compiler_index_test.cpp
│   ├── io_test.cpp
│   ├── input_loader_test.cpp
│   ├── jar_io_test.cpp
│   └── e2e_test.cpp                        — legacy CBackend E2E（移 reference 时标 legacy）
├── frontend/
│   └── code_converter_test.cpp             — [L1] 栈式 MIR（已有，R1 补 indy/multianewarray）
├── mir/                                    — ★ 新增（R1/R4）
│   ├── ssa_test.cpp                        — [L2] SSA
│   ├── register_ir_test.cpp                — [L3] 寄存器 IR
│   └── passes/                             — [L4] 变换 pass
│       ├── constfold_test.cpp
│       ├── dce_test.cpp
│       ├── mba_test.cpp
│       └── cff_test.cpp
└── backend/
    ├── regc_test.cpp                       — ★ [L5] RegC（R2）
    └── reference/
        └── c_backend_legacy_test.cpp       — legacy CBackend（从 c_backend_test.cpp 移入，标 legacy）
```

runtime 测试（R3）单独组织在 `runtime/test/`（C 单测，不一定用 gmock）。

---

## 3. 测试数据

### 3.1 内联构造（首选）

大多数单元测试用**内联构造的 ClassNode/MethodNode**，避免依赖外部文件：

```cpp
auto cn = std::make_unique<ClassNode>();
cn->version = V1_8;
cn->access = ACC_PUBLIC;
cn->name = "Test";
cn->superName = "java/lang/Object";

auto mn = std::make_unique<MethodNode>();
mn->access = ACC_PUBLIC | ACC_STATIC;
mn->name = "add";
mn->desc = "(II)I";
mn->instructions.add(new VarInsnNode(ILOAD, 0));
mn->instructions.add(new VarInsnNode(ILOAD, 1));
mn->instructions.add(new InsnNode(IADD));
mn->instructions.add(new InsnNode(IRETURN));
```

字符串/类型必须经共享 `testArena().intern()`（CBackend 测试曾因 bare `toRef` 指针与
libjbytecode Type 全局状态交互导致 flaky，已修）。

### 3.2 真实 class 文件

少数 E2E 测试用 `test_resource/` 下的真实 `.class`（与 libjbytecode 共用资源目录）。
输出一律走 `test::getOutPath()` 等 test_helper API，**绝不写 `test_resource/`**。

### 3.3 c2j 攻击验证样本（R3+）

R3 起，受保护样本需在 c2j agent 下验证。样本与 expected trace surface 文档化，
交 mjdbg 协作验证（见 ROADMAP §5 Shared Contracts）。

---

## 4. 测试运行

```bash
# 配置（一次性）
xmake f --with_mj2x=y --with_tests=y

# 全部
xmake build mj2x_unittest && xmake run mj2x_unittest

# gmock 过滤
xmake run mj2x_unittest --gtest_filter="RegC*"
xmake run mj2x_unittest --gtest_filter="SsaTest.*"

# runtime 单测（R3 起）
xmake build mj2x_runtime_test && xmake run mj2x_runtime_test
```

---

## 5. 质量门槛（按层）

| 层 | 单元测试门槛 | 结构断言门槛 | 落地波次 |
|----|------------|------------|---------|
| L1 栈式 MIR | ≥ 15（已有 13，R1 补） | — | 已有/R1 |
| L2 SSA | ≥ 10 | φ 插入/dominance 正确 | R1 |
| L3 寄存器 IR | ≥ 10 | **操作数无 stack slot（硬断言）** | R1 |
| L4 变换 pass | 每 pass ≥ 5 | 语义保持 + 变换生效 | R4 |
| L5 RegC | ≥ 15 | **生成 C 无 cstack/Cannot invoke/cclasses（硬断言）** | R2 |
| RT 运行时 | 每 module ≥ 5 | antijvmti mock 篡改 fail-closed | R3 |
| E2E | ≥ 3（R2）→ ≥ 6（R3 含 c2j agent） | 行为与原 Java 一致 | R2/R3 |

全局：编译警告 0，CI 必过，RTTI 禁用（用 `typed::cast` 不用 `dynamic_cast`）。

> 旧 v1 目标（≥80 单元 / ≥60% 覆盖，仅针对 CBackend）已被本按层目标取代。

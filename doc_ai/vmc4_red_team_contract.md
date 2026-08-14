# VMC4 红队契约（Phase 4）

> 本文是 mj2x 与 mjdbg 红队闭环在 VMC4 侧的交付物定义（`doc/roadmap.md:10-18` 红蓝闭环、
> `deobf-attack-doctrine.md` §5 样本契约）。mjdbg 对 VMC4 样本的分析结论必须能回到这里
> 定义的观测面与强度基线。
>
> 分工：mj2x 产出样本 + 声明 trace surface（CLI `--vmc4-trace-surface`）；mjdbg 产出
> weakness report 并按本文 §4 方法量化。强度归零的实例由 mj2x 调参重产。

## 1. 样本交付物

每个 VMC4 保护样本应随附：

| 交付物 | 来源 | 说明 |
|---|---|---|
| 保护后 jar/class + 生成的 `.c` | `--backend vmc4 ... --output` | 产物本身 |
| `trace-surface.txt` | `--vmc4-trace-surface <file>` | 每个 VmInstance 的 expected trace surface（dispatcher 形态、instanceSeed、aliasCount/bogusHandlers、opcode 字节集合、handlerOrder、alt-form 选择） |
| 构建参数 | CLI 命令行 | `--vmc4-seed`/`--vmc4-dispatcher`/`--vmc4-granularity`/`--vmc4-alias`/`--vmc4-bogus` |

`trace-surface.txt` 由 `VmInstance::describe()` 确定性生成：同 seed + 同输入 → 同内容，
红队据此核对「观测到的结构」与「声明结构」是否一致（不一致 = 发现未声明结构）。

## 2. expected trace surface（红队应观测到）

| 维度 | 观测目标 | 红队应报告 |
|---|---|---|
| dispatcher 形态 | 主循环结构：switch 跳表 / 函数指针表+间接调用 / `goto *`（GCC）/ if 链 / state 平坦化 | 与 trace-surface 声明的形态一致；跳表 vs 间接调用 vs 线性链的结构识别 |
| opcode 编码熵 | 字节流里 opcode 字节的分布：canonical + 别名混合，指令集大小 = 11 语义 × (1+aliasCount) 字节 | 枚举全部 opcode 字节；统计别名使用（同一语义多字节）；与 trace-surface 的字节集合比对 |
| handler 数量 | dispatch 结构中的 handler 数 = 10（计算/控制流）+ 0..4 bogus（不可达） | 区分可达 handler 与 bogus（bogus 的特征：不被表/case 引用） |
| handler 形态 | Unary/Compare 的 alt-form（`x^全1` vs `~x`、`!(a!=b)` vs `a==b`） | 报告实际形态；trace-surface 的 `[alt-form]` 标记应命中 |
| super-operator | SuperInc/SuperDec 出现（`+1`/`-1` 折叠，无 rhs operand） | 报告是否存在折叠指令 |
| 寄存器文件 | JNI wrapper 的 `regs[N]`、常量槽高水位、边拷贝合成块 | 寄存器数量与布局 |
| 多实例差异 | 不同类/方法（per-class/per-method）的字节集合/handler 顺序 | **跨实例分析复用性测试**（§4.1）的输入 |
| case 标签轮换（#762 升级档） | drift 开启时 `caseLabelPerms:` 段的每变体排列；dispatch case 常量 = `d*K + perm_d[canonical]` | 红队还原的「case 常量 ↔ 语义」表须按变体分组且依赖排列派生；单次分析结论跨周期失效（变义强度观测面） |

## 3. 强度归零基准

一个 VmInstance 的强度归零（记为 0）当且仅当满足以下任一：

1. **ISA 全恢复**：mjdbg 能从单个样本的 native 产物自动还原完整 ISA（语义 → 字节
   映射 → 指令格式），且还原脚本可复用于同 seed 的其他样本。
2. **跨实例可复用**：对方法 A 的 VM 分析结论（handler 语义、指令格式、寄存器布局）
   不经修改即可用于同样本的方法 B（粒度失效）。
3. **无 seed 敏感性**：改变 `--vmc4-seed` 后重产样本，旧分析工具仍直接可用（映射
   未真正随机化）。

强度归零 → mj2x 调参（换 dispatcher/提高 aliasCount/改 granularity/换 seed）重产，
红队复测，形成循环。

## 4. 红队量化方法

### 4.1 跨实例分析复用性测试

```
输入：一个保护样本（≥2 个 VmInstance：per-class 下至少 2 个类）
步骤：
  1. 对实例 A 完整还原 ISA（字节集合 + handler 语义）
  2. 用 A 的还原结论直接分析实例 B（不修改脚本）
  3. 判定：
     - 全部 opcode 字节能对映到 A 的语义 → 复用性 = 100%（强度归零）
     - 字节对映失败率 ≈ 1 - 1/|B 的字节集合| → 复用性 ≈ 0（实例独立成立）
报告：复用率 + 失败点列表
```

### 4.2 opcode 编码熵

对字节流统计：每个 opcode 字节的 Shannon 熵与别名使用率
（同语义多字节的出现比）。预期：canonical+别名混合使用 → 单字节频率不集中。

### 4.3 trace-surface 一致性核对

红队从产物观测到的结构与 `trace-surface.txt` 声明逐项比对：
- 声明了但未观测到 → 结构缺失（mj2x 缺陷）
- 观测到但未声明 → 未声明结构（报告，mj2x 补声明或删除）

## 5. mj2x 侧交付接口

- `mj2x-cli --backend vmc4 --vmc4-trace-surface <file> ...`：生成 trace surface 文件。
- `VmInstance::describe()`（`mj2x-core/src/mj2x/backend/vmc4/vmc4_instance.cpp`）：
  确定性文本，单测 P4-1 锁定内容与可复现性。
- 12 项 e2e（`vmc4_backend_e2e_test.cpp`）在别名+bogus+super-op+morphing 全开下
  验证语义保真——红队还原的 handler 语义必须与 e2e 断言的行为一致。

## 6. 待 mjdbg 落地的部分

> 状态：已排期（mjdbg dev_plan N-7，P1；2026-08-15）。ISA 恢复工具交付后
> 按本契约 §2 表逐维度对账，强度归零判定回填本文件与 mj2x dev_plan B 项。

- 对 VMC4 样本的自动化 ISA 还原工具（dispatcher 识别 + 字节集合枚举 + handler 语义标注）。
- weakness report 按 §2 表逐维度产出，并带 §4.1 复用率数值。
- 红队结论回填 mj2x `dev_plan.md`，驱动下一次可变性调参。

## 7. mj2x 侧自验闭环（2026-08-15，mjdbg 工具就绪前的自动化替代）

mjdbg 对抗工具未落地期间，mj2x 侧用 `Vmc4TraceSurfaceConsistencyTest` 自动化
执行 §4.1/§4.3 的核对方法，防止「声明的 trace surface 与产物漂移」与
「实例独立失效」在无人观测时悄然发生：

| 测试 | 核对内容（对应红队方法） |
|---|---|
| `DescribeFileRoundTripsToInstanceState` | 把 describe() 输出当作 trace-surface 文件解析，与 VmInstance 产物状态逐字节比对：每语义字节集合、alt-form 标记、handlerOrder 字节序列、alias/bogus/drift 字段、全集双射（§4.3 声明↔产物一致性） |
| `DescribeFileRoundTripsDriftInstance` | drift 开启实例：声明字段、每个变体 unmap 输出 canonical 全排列（双射）、跨变体轮转真实发生（§2 opcode 编码熵 / handler 数量的观测面支撑） |
| `CrossInstanceSemanticMappingIsUnreusable` | §4.1 低配量化：实例 A 的语义表对实例 B 字节的命中率——实测 **2.50%**（随机期望 ~5%，阈值 20%）——跨实例复用性 ≈ 0，实例独立成立 |

红队工具就绪后：§7 自验结果作为 §4 量化方法的前置基线；工具结论与 §7
数值不一致时按「发现未声明结构」处理（§4.3）。

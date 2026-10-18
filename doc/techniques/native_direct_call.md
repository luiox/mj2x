# native→native 直接 C 调用（Direct Native Call）

> 已转 native 的方法调用另一个已转 native 的方法时，跳过 JNI 边界，直接调对方
> 的 C 函数符号。

| 项 | 值 |
|---|---|
| 对标术语 | 无通用业界名；本仓内部称 direct call / native-to-native call |
| 作用层 | 发射端（`tryEmitDirectCall` / `InternalCallResolver`） |
| 开关 | `--regc-direct-calls`（RegC）；LLVM 侧**无门控** |
| 后端 | RegC ✅ / LLVM ✅（仅 INVOKESTATIC）/ StackC ❌ / VMC4 ❌ |
| 默认 | RegC 关；LLVM 开 |
| 状态 | ✅ 已落地（两端口径待统一） |

## 对抗什么

这一项**不是**混淆技术，是**性能 + 攻击面**技术。它对抗的是 JNI 边界本身：

- **性能**：每次 JNI 调用是 `FindMethod` 缓存查找 + `CallXxxMethodA` + 可能的
  Push/PopLocalFrame + ExceptionCheck，往返开销远大于一次 C 函数调用。
- **攻击面**：JNI 边界是可 hook 点。端点越少，红队在边界上插桩的机会越少。

## 机制

RegC 的调用发射链里，直调**排在最前**——先于 intrinsic、先于通用 JNI：

```text
tryEmitDirectCall  →  intrinsic  →  resolved  →  cached JNI 兜底
```

命中条件（全部满足）：

- 目标已成功转换（在 `convertedFunctions` 里）
- 分派确定（`isDeterministicDispatch`）
- 不在 invokedynamic 站点上
- 无 native 状态参数逃逸（StringBuilder 之类在寄存器里是 NULL，直调会破坏被调
  方 ABI）

排除：`<clinit>` 保持 JNI（类加载语义）；`<init>` 允许直调（INVOKESPECIAL 精确
分派 + 已转换构造器体）。

### 继承方法

调用点 owner 是 receiver 的静态类型，`convertedFunctions` 的键是**声明类**——
两者不一致时沿超类链（限编译输入，深度上限 32）找声明类后重查。

### 确定性虚调用是默认开的例外

`--regc-direct-calls` 默认关，但**确定性分派的虚调用**免门控直调：final 方法 /
final owner / 编译输入内无覆写时，resolved 路径命中 implClass 后本来就直调
（无 JVM 帧，栈观察语义与直调完全一致），运行时 `IsInstanceOf` 检查是纯冗余。
静态 / INVOKESPECIAL 调用仍要求显式开关。

这类调用点带 NULL receiver 守卫：JNI 调用点在 receiver 为 NULL 时抛 NPE，直调
必须显式复现（见生成的 `if ((rcv) == NULL)` 分支）。

## 为什么默认关：#814 的栈观察语义

C 直调**不产生 JVM 栈帧**。`getStackTrace`、`StackWalker`、反射等依赖栈帧的
语义会失真——受保护方法的中间帧从栈上看不见。开启仅用于性能优先且确认无栈观察
的产物。

这是本项最大的代价，也是它默认关的唯一理由。

## 两端口径不一致（待修）

| 后端 | 门控 | 覆盖面 |
|---|---|---|
| RegC | `--regc-direct-calls`，默认关 | 静态 / 虚 / special，含继承 |
| LLVM | **无门控，默认直调** | **仅 INVOKESTATIC** |

也就是说 LLVM 侧一直在承担 #814 的语义代价却没有开关，而 RegC 侧的覆盖面更广
却默认关着。拟改为统一门控 + 分档：

| 档 | 语义 |
|---|---|
| `off` | 全走 JNI（当前 RegC 默认） |
| `intra-class` | 仅同类内直调，栈失真范围可控 |
| `all` | 所有确定性分派直调（当前 `--regc-direct-calls`） |

## 代价

| 维度 | 影响 |
|---|---|
| 正确性风险 | **栈观察语义失真**（#814）——这是主要代价 |
| 覆盖面 | 子类覆写、indy、native 状态参数都会把调用点打回 JNI |
| 产物 | 需要前向声明（目标函数定义可能在调用点之后） |

前向声明有个细节：C11 6.7.1p6 禁止块作用域 `static` 函数声明（GCC 拒绝、MSVC
放行），故声明登记到 TU 头部注册表统一输出。另外无声明时 C 的隐式 `int()` 声明
与 REF 返回值冲突（MSVC C2040）。

## 与其他技术的关系

- 与 **调用目标间接化**方向相反：一个要合并符号（间接化），一个要暴露符号
  （直调）。两者可共存——直调命中时优先，未命中才走 dispatcher。
- 与 **SSA 方法内联**（`--regc-no-inline` 反向控制）是同族：内联是编译期消除
  调用，直调是运行期消除 JNI 边界。

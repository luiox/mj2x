# 调用目标间接化（Call Target Indirection / per-signature dispatcher）

> 调用点不再自带"我要调谁"，只带一个 `(站点表, 索引)`；目标身份字符串从调用点
> 移出，调用图收敛到单一 dispatcher 符号。

| 项 | 值 |
|---|---|
| 对标术语 | jvmtp `obfuscation.calls.*`；业界通称 call indirection / import table obfuscation |
| 作用层 | RegIR pass（置标记）→ 发射端（建表）→ 运行时（解析） |
| 开关 | `--regc-indirect-calls`（**不被 `--regc-obfuscate` 联动**） |
| 后端 | RegC ✅ / LLVM ✅ / StackC ❌ / VMC4 ❌ |
| 默认 | 关 |
| 状态 | ✅ 已落地 |

## 对抗什么

调用图是逆向的主干信息：`A.f()` 调了 `String.concat` 和 `Math.max`，靠扫调用点
的字符串引用就能建出来。即使方法体全部 native，调用边的身份仍然留在 native
产物里。

间接化之后，所有外部调用都指向同一个 `mj2x_regc_dispatch_i32` 之类的符号，
差异只在整数索引上——要还原调用边必须先解析"索引 → 站点表条目"。

## 机制

### 三段分工

| 段 | 做什么 |
|---|---|
| pass | 只置 `RegInstruction::indirectCall` 标记，不建表 |
| 发射端 | 按遍历序建站点表 + 发 dispatcher 调用 |
| 运行时 | 首次分派解析 owner 类 + jmethodID 并缓存进条目 |

**为什么 pass 不建表**：pass 看不到跨函数的"哪些方法已被转换"，而发射端本就按
解析结果分流（自有 intrinsic / 同模块直调 / resolved / cached JNI 兜底）。落到
dispatcher 的只有通用 JNI 兜底路径——固有快路径天然不受影响，不需要 pass 去猜
内外。这样也保住了可复现构建不变量（pass 不依赖全局信息）。

### 站点表

按 **dispatcher 后缀（返回形态）**分组：同签名共享一张表、组内独立索引。同一
`(owner, name, desc, invokeKind)` 只登记一条目，多个调用点共享索引——运行时也
只解析一次。

两个后端的表存放位置不同：

| 后端 | 表形态 |
|---|---|
| RegC | 函数级 `static mj2x_regc_call_site _mj2x_ct_N[]` |
| LLVM | 模块级 `internal global`（跨函数天然去重；`internal` linkage 不进符号表） |

### 运行时解析纪律（两条，缺一即崩）

1. **存进条目的 jclass 必须是 global ref。** `FindClass` / `GetObjectClass` 返回
   的是 local ref，只在当次 native 调用内有效；直接缓存会在下一次调用时悬垂。
2. **virtual / special 按 receiver 实际类解析，不按声明 owner。** 抽象/接口
   owner 上按声明类解析会拿到抽象条目，`CallXxxMethodA` 触发
   `AbstractMethodError`（HotSpot 17 实测 ACCESS_VIOLATION）。故另设
   per-receiver-class 槽，同类重复调用经 `IsSameObject` 命中。

槽位是**换占式写入、只增不删**：CAS 把旧类换出后不 `DeleteGlobalRef` 旧句柄——
`Delete→NULL→New` 窗口下其他线程可能正对旧句柄做 `IsSameObject`（use-after-free，
含双重 Delete）。泄漏上界 = 该调用点出现过的 distinct receiver 类数，只在子类
交替场景触发。

### invokeKind

`0` = virtual/interface，`1` = static，`2` = special。static 走
`CallStaticXxxMethodA`（解析类是声明 owner 的 global ref），另两者走
`CallXxxMethodA`（receiver 原样传入，JNI 按实际类虚分派）。

### 跳过的情况

自递归静态调用（`INVOKESTATIC` 且 owner == 当前函数 owner）不标记：它走 self-call
直调路径，间接化只会把 0 JNI 的自调用变成一次 FindClass + GetStaticMethodID。

## 产物形态

```c
// 变换前
r2 = _mj2x_regc_call_cached_int(&cache, rcv, a, b);

// 变换后
_mj2x_regc_va[0].i = a;
_mj2x_regc_va[1].i = b;
r2 = mj2x_regc_dispatch_i32(_mj2x_regc_current_context(),
                            _mj2x_ct_0, 3, rcv, _mj2x_regc_va);
```

站点表（RegC）：

```c
static mj2x_regc_call_site _mj2x_ct_0[] = {
  {0, 0, "java/util/List", "add",  "(Ljava/lang/Object;)Z", 0, 0, 0},
  {0, 0, "java/util/List", "size", "()I",                   0, 0, 0},
};
```

`jvalue` 数组是复用的：发射端本来就为 `CallXxxMethodA` 构造它，dispatcher 直接
吃现成的，不新增参数打包开销。

## 代价

| 维度 | 影响 |
|---|---|
| 性能 | 每次调用多一层分派；命中缓存后与原有 cached 帮手同开销（1 JNI） |
| 产物体积 | 多一张站点表（条目很小） |
| 覆盖面 | 只有通用 JNI 兜底路径落到 dispatcher；intrinsic 与同模块直调保持原形态 |

热路径开销可观，这是它不被 `--regc-obfuscate` 联动、必须显式开启的原因。

## 已知绕过面

- **站点表本身是可读的**：表里的 owner/name/desc 字符串仍能被 `strings` 扫到。
  这一项的收益在于**调用点不再自带身份**，静态分析须先解析 index 才能还原
  调用边——不是让字符串消失。要连字符串一起隐藏，是另一项（字符串 at-rest
  加密）的事。
- **动态 trace / JNI hook**：运行时 jmethodID 就在条目里。对抗的是静态。
- **单态调用点**：若某索引全程只被用到一次，调用边仍可唯一确定。

## 与其他技术的关系

- 排 **CFF 之后、立即数隐藏之前**；只置标记不改指令序列，与前后两者零交互。
- 与 **computed jumps** 是同族的下一项：本项打散"调用边"，computed jumps 打散
  "跳转边"（issue #7，未实现）。

# POD 类降级为 C struct（Object Scalar Replacement）

> 纯原生字段的 Java 类，在 native 侧降级为 C struct：字段访问变成 struct 成员
> 读写，不再走 `Get/SetXxxField`，Java 侧的类定义不再承载真实数据。

| 项 | 值 |
|---|---|
| 对标术语 | 业界通称 scalar replacement / object inlining；JVM 侧对应 escape analysis + 标量替换 |
| 作用层 | 分析（逃逸分析）→ 发射端 |
| 开关 | 拟 `--regc-pod-struct`（默认关） |
| 后端 | 拟 RegC 先试点 |
| 默认 | 关 |
| 状态 | 📋 **未实现** |

## 对抗什么

1. **性能**：字段访问是 `jfieldID` 缓存查找 + `Get/SetXxxField` 两次 JNI 往返
   （实例字段另加 null 检查）。struct 成员读写是零 JNI。
2. **可还原锚点**：`GetIntField(obj, "x")` 直接暴露字段名和类型。struct 成员是
   偏移，字段名从产物里消失。
3. **类定义可见性**：Java 侧的类不再承载真实数据，反编译出来是个空壳。

## 降级条件（必须全部满足）

| 条件 | 理由 |
|---|---|
| 字段全是 primitive（Z/B/C/S/I/J/F/D），**无引用字段** | 引用字段涉及 GC 与写屏障，struct 表示不了 |
| 不实现接口、无子类 | 分派必须能静态绑定到声明类 |
| 对象不返回、不存进数组或字段 | 逃逸即失效 |
| 不传给任何外部方法 | 外部方法按 Java 对象语义访问 |
| 不 `hashCode` / `equals` / `toString` | 这三个是身份语义，struct 没有 |
| 不 `synchronized` | 对象头监视器语义 |
| 不当 `Object` 用（不向上转型） | 同上 |

构造器要能识别成 struct 初始化：`NEW` + `INVOKESPECIAL <init>` 配对，且 `<init>`
体可静态分析。

## 现成底座：逃逸分析已有

不需要从零写。RegC 侧 `regc_intrinsics.cpp` 已经有一套完整的逃逸分析基建，是
StringBuilder / StringBuffer native lowering 用的：

- union-find 归并"根"（分配点 → 寄存器根）
- escape events 记录（参数传递、返回、存储即逃逸）
- use-after-escape 判定（逃逸后再用即整根回退）

LLVM 侧对应 `analyzeWrapperShadows` / `nativeSlots_`（包装类 shadow 寄存器 →
结构槽类型）。POD 降级复用这套判定即可。

## 一个必须回答的设计问题：隐藏到什么程度

| 方案 | 做法 | 风险 |
|---|---|---|
| (a) 字段挪进 native 内存，Java 侧类只剩空壳，getter 走 JNI 读 struct | 类定义保留，字段存储外移 | 反射仍能看到字段声明；序列化可能读到空值 |
| (b) 整个类从产物里删掉 | 类定义消失 | **风险大**：反射、序列化、其他未受保护的代码都会崩 |

**倾向先做 (a)**，试点跑通再谈 (b)。(b) 需要先把"这个类在编译输入之外没有被
引用"证明出来，这个证明在 JAR 边界上很难成立。

## 代价

| 维度 | 影响 |
|---|---|
| 覆盖面 | 条件极严，实际命中率可能很低（大多数类都有 `toString` 或向上转型） |
| 正确性风险 | 逃逸分析漏判 = 数据静默错乱，比崩溃更难查 |
| 反射 | 反射访问 struct 化对象拿到的是空壳，语义与未保护版本不一致 |

## 验收（拟）

- 开关默认关
- 单测：降级判定边界（每个条件各一条反例）
- E2E：真实编译 + 运行，字段读写语义与 JVM 基线一致；另加一条反射访问用例明确
  记录降级后的行为

## 与其他技术的关系

- 与 **native→native 直调**是同族的结构降级：一个消除字段访问的 JNI 边界，
  一个消除调用的 JNI 边界。
- 与**立即数隐藏**可叠：字段名消失 + 常量值消失。

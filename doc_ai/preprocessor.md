# mj2x L1 Preprocessor 设计

> 状态：R1b 实现 note，权威随代码演进
> 日期：2026-06-25
> 关系：本文档定义 `src/mj2x/preprocessor/` 下的字节码改写预处理。
> 上层依据：`architecture_redesign.md §5.2`（L1 占位/未完成项）。

---

## 1. 为什么需要 preprocessor

L1 `CodeConverter`（栈式 MIR 翻译）对几类 JVM 指令**无法直接翻译**，因为它们的语义
超出了"逐指令→栈式 IR"的 1:1 映射：

| 指令 | 为什么不能直译 | preprocessor 处理 |
|------|---------------|-------------------|
| `invokedynamic` | bootstrap 方法在运行时链接 call site，构建期无法静态决定目标；`MIRInvoke` 只能表达静态已知的 owner/name/desc | 展开为"调用 bootstrap → 取 MethodHandle → invokeWithArguments"的字节码序列（R1b 只做 STD_JAVA 路径） |
| `multianewarray` | 一次分配 N 维数组，需要 N 个长度操作数；`MIRNewArray` 只支持 1 维 | 展开为嵌套 `anewarray`/`newarray`（每维一次） |
| `ldc` Handle/Type | Handle/Type 不是普通常量，需要 `MethodHandles.Lookup` 反射构造；`MIRConstant` 只支持标量 | Handle → `Lookup.findVirtual/findStatic/...` 链；Type → `Class.forName`/`MethodType.fromMethodDescriptorString` |

这些 preprocessor **在 CodeConverter 之前运行**，直接改写 ASM 树层（`MethodNode::instructions`），
把"难翻译"的指令替换成"易翻译"的等价指令序列。改写后 `CodeConverter` 看到的全是它能直译的指令。

> 这是 native-obfuscator 的成熟做法（`IndyPreprocessor`/`LdcPreprocessor`/`MethodHandleUtils`），
> masxinlingvonta 也用 `InstructionExtractor` 做同类事。mj2x R1b 移植这套思路到 C++。

---

## 2. 接口

所有 preprocessor 实现 `Preprocessor`（`preprocessor/preprocessor.hpp`）：

```cpp
class Preprocessor {
public:
    virtual ~Preprocessor() = default;
    virtual void process(CompilerIndex& index) = 0;
};
```

`process` 遍历 `index.getClasses()` 的每个非库类的每个方法，扫 `method->instructions`，
遇到目标指令就用等价序列替换（`InsnList::insert`/`remove`/`set`）。

Pipeline 注册顺序：`LdcPreprocessor` → `InvokeDynamicPreprocessor` → `MultiANewArrayPreprocessor`
（Indy 的展开会产生新的 LDC Handle，故 Ldc 先跑；multianewarray 独立）。

---

## 3. LdcPreprocessor（R1b.3）

### 3.1 处理对象

`LdcInsnNode`，其 `cst` 是：
- `Handle`（`H_GETFIELD/H_GETSTATIC/.../H_NEWINVOKESPECIAL`）→ 展开
- `Type` 且 sort==METHOD（MethodType）→ 展开
- 其它（int/float/String/Class）→ 不动（L1 能直译）

### 3.2 Handle 展开

参考 native-obfuscator `MethodHandleUtils.generateMethodHandleLdcInsn`，按 tag 分派：

```
INVOKESTATIC native/magic/.../lookup/obfuscatorX.a   ; → MethodHandles.Lookup（当前类 lookup）
LDC <owner-class>
; per tag:
;   H_GETFIELD/H_GETSTATIC/H_PUTFIELD/H_PUTSTATIC → findGetter/findStaticGetter/findSetter/findStaticSetter(Class, name, type)
;   H_INVOKEVIRTUAL/H_INVOKEINTERFACE → findVirtual(Class, name, MethodType)
;   H_INVOKESTATIC → findStatic(...)
;   H_INVOKESPECIAL → findSpecial(Class, name, MethodType, callerClass)
;   H_NEWINVOKESPECIAL → findConstructor(Class, MethodType)
```

`name` 和 `type`（descriptor）从 Handle 取；MethodType 用 `MethodType.fromMethodDescriptorString(desc, classloader)`。

### 3.3 MethodType 展开

```
LDC <descriptor-string>
INVOKESTATIC native/magic/.../classloader/obfuscatorX.a   ; → 当前 classloader
INVOKESTATIC java/lang/invoke/MethodType.fromMethodDescriptorString
```

### 3.4 magic 调用

`native/magic/.../{lookup,classloader}/obfuscator<rand>.a` 是合成标记方法（不是真类）。
R1b 不需要它们真的存在 —— 在 L5 生成阶段（R2）识别这些标记并 inline 成 C helper
（`get_lookup(env, clazz)` / `get_classloader(env, clazz)`）。R1b 只保证字节码层等价。

> R1b 简化：先**不**注入 magic 调用，而是直接用 `MethodHandles.lookup()`（INVOKESTATIC）
> 和 `getClass().getClassLoader()`。这避开 magic 标记机制，让 L1/L5 阶段更简单。
> magic 化（每样本随机名）留到 R4 多样化 pass。

---

## 4. InvokeDynamicPreprocessor（R1b.4）

### 4.1 处理对象

`InvokeDynamicInsnNode`（bsm + bsmArgs + name + desc）。

### 4.2 STD_JAVA 路径（R1b 唯一实现）

参考 native-obfuscator `IndyPreprocessor` 的非 HOTSPOT 分支。生成（伪字节码）：

```
bootstrapStart:
   ; 装箱所有 dynamic-call 参数为 Object[]（category-2 手动 box）
   ; 加载 LOOKUP_LOCAL、invocation name (String)、MethodType（LdcPreprocessor 的展开）、
   ;   每个 BSM 参数（box int/long/...，递归处理 Object[]）
   ; INVOKESTATIC bsm(owner, name, type, ...bsmArgs)   ; 直接调 bootstrap 方法
   ; 结果 checkcast CallSite → getTarget() → MethodHandle
   goto invokeStart
bootstrapEnd:
bsmeStart:                       ; catch Throwable
   dup; instanceof BootstrapMethodError; ifne throwLabel;
   new BootstrapMethodError; dup; dup2_x1; pop2;
   invokespecial BootstrapMethodError.<init>(Throwable)
   throwLabel: athrow
invokeStart:
   ; 把 receiver + 已装箱 args 作为 Object[] 传给 MethodHandle.invokeWithArguments
   ; CHECKCAST / unbox 返回类型
tryCatch(bootstrapStart..bootstrapEnd -> bsmeStart, Throwable)
```

### 4.3 varargs BSM 归一

若 BSM descriptor 末参是 `Object...` 而 bsmArgs 数量与固定参数不匹配，补 `null` 或截断
（`StringConcatFactory.makeConcatWithConstants` 等场景）。

### 4.4 R1b 不做 HOTSPOT 路径

`linkCallSite`（`MethodHandleNatives.linkCallSite`）是 Hotspot 内部 fast-path，
依赖 JVM 私有 API。R1b 只做 STD_JAVA（通用、可移植）。HOTSPOT 路径推到 R5+（若 SnakeGame 需要）。

---

## 5. MultiANewArrayPreprocessor（R1b.5）

### 5.1 处理对象

`MultiANewArrayInsnNode`（desc = 数组类型，dims = 维数，栈顶有 dims 个 int 长度）。

### 5.2 展开

把 `multianewarray T d dims` 展开为嵌套分配（最外层 anewarray，内层循环 anewarray）。
但**循环展开在字节码层很重**。R1b 采用更简单的等价：

**方案 A（当前采用）**：保留 `multianewarray` 但在 L1 翻译时把它映射到
`MIRMultiNewArray`，后续 SSA/RegIR/RegC 通过 `_mj2x_regc_alloc_multi_array`
helper 执行。preprocessor **不改字节码**，只统计并提示该站点会走 helper 路径。

> 早期 R1b 最小实现只要求“不静默落 `MIRNop`”；当前 RegC 已有 multi-array
> helper 和 E2E 覆盖，所以这里不再是 fail-closed 边界。

**方案 B（保留备选）**：字节码层展开为 anewarray + 嵌套填充循环（参考
masxinlingvonta `InstructionExtractor`）。成本高，当前不需要。

---

## 6. R1b 验收

1. `LdcPreprocessor`：一个含 `ldc MethodType`/`ldc MethodHandle` 的方法，preprocess 后
   `CodeConverter` 不再遇到原始 ldc Handle/Type（被展开成方法调用序列）。
2. `InvokeDynamicPreprocessor`：一个含 `invokedynamic` 的方法（如 lambda / String concat），
   preprocess 后 `CodeConverter` 不再遇到 invokedynamic（被展开成 bootstrap+invoke 链）。
3. `MultiANewArrayPreprocessor`：multianewarray 在 L1 不再静默落 `MIRNop`，而是明确
   进入 `MIRMultiNewArray` / RegC helper 路径。
4. 现有 45/46 测试不回归。

R1b 用 javac 编译的小样本验证（`test_resource/preproc/`）。

---

## 7. 文件组织

```
src/mj2x/preprocessor/
├── preprocessor.hpp                  # 接口（已有）
├── mark_all_preprocessor.hpp         # 已有
├── ldc_preprocessor.{hpp,cpp}        # R1b.3
├── invokedynamic_preprocessor.{hpp,cpp}  # R1b.4
└── multianewarray_preprocessor.{hpp,cpp} # R1b.5（最小：fail-closed 标记）
```

## 8. 不做的事

- HOTSPOT linkCallSite fast-path（R5+，若 SnakeGame 触发）。
- magic 调用的随机化命名（R4 多样化 pass）。
- multianewarray 字节码层循环展开（方案 B，SnakeGame 用不到）。
- ConstantDynamic（`condy`，JDK11+）—— 极罕见，遇到再补。

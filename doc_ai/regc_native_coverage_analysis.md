# RegC native 覆盖率分析

> 状态：2026-08-09 更新。本文跟踪 RegC
> fail-closed 边界的收口进度与实测覆盖率。

## 实测基准

输入：JavaObfuscatorTest `TEST.jar`（未混淆编译产物，42 类 ~56 个用户方法）。
转换命令（release 版）：

```powershell
mj2x-cli --backend regc -i TEST.jar -o TEST-regc.jar --emit-cpp native.c --compile
```

产物运行（`java -noverify -jar TEST-regc.jar`）17 项测试全 PASS。

## 覆盖率（2026-08-09）

| 类别 | 状态 |
|---|---|
| 业务方法（非构造器）native 化 | **100%**（零 skip） |
| `<init>` 空构造器 | 骨架化（`super` + `invokestatic __regc_init` + return） |
| `<init>` 有逻辑构造器 | 保留字节码（反射方法数断言兼容，见 §3） |
| `<clinit>` | 搬运到 `__regc_clinit_<hash>`（注册时机保持在骨架头部） |
| 抽象方法 / 接口方法 | 不可 native，保留（JVM 语义） |
| mj2x runtime 类（`mj2x/cloader/Loader`、`mj2x/runtime/NativeLoader`、`mj2x/lambda/*` 及 SAM 同包 lambda 合成类） | 保留字节码（工具类自身） |

## 已收口的 fail-closed 边界（按时间）

| 边界 | 收口 |
|---|---|
| 反射组（`Class`/`reflect/*`/`Object.getClass`） | 引用管理（PushLocalFrame/PopLocalFrame）落地后放开 |
| IO 组（`InputStream`/`ByteArrayInputStream`） | 已放开（collect_args 数组参数修正后） |
| 类加载组（`ClassLoader.loadClass`/`getResourceAsStream`/`defineClass`） | 已放开（byte[] 参数修正后） |
| 并发组（`Thread`/`java.util.concurrent/*`） | thread-local env/ctx 修复后放开（2026-08-09） |
| System 组（`exit`/`setSecurityManager`/`getSecurityManager`） | thread-local 修复后放开（JDK17 语义一致） |
| 内部类 `$`（RegisterNatives 动态绑定） | 已放开 |
| invokedynamic 自定义 bootstrap | 阶段3 运行时 call-site helper（2026-08-09） |

## 仍未转 native 的合法情形

- 有逻辑构造器（局部变量表非空）：搬运注入的 `__regc_init_*` 会被
  `Class.getDeclaredMethods()` 反射计数（synthetic 亦可见），破坏精确方法数断言
  （如 TEST.jar Test 2.1 Counter）。业界（JNIC/j2cc/native-obfuscator）均不转
  构造器逻辑，属标准行为而非缺陷。
- 构造器含 try-catch / 局部变量注解 / 骨架段含分支或跨段 label 引用：搬运法保守边界。

## 未来方向

- 装箱+加宽复合适配的 invokedynamic（需读 instantiatedMethodType）。
- 构造器逻辑 native 化的替代方案（如仅注入 synthetic 且接受反射差异）。

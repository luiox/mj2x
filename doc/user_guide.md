# mj2x 用户指南

> mj2x 把 Java 字节码(`.class` / `.jar`)翻译为 C 源码,并将原方法标记为 `native`。
> 本文档讲怎么用。项目概览见 `../readme.md`;已实现能力清单见 `capabilities.md`。

---

## 1. 构建

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-cli       # CLI 工具
xmake build mj2x-core      # 核心静态库(被 CLI 依赖)
xmake build mj2x_unittest  # 单元测试
xmake build mj2x_runtime_test  # R3 runtime 测试
```

需要 JDK(`JAVA_HOME` 指向 JDK 安装目录,必须包含 `jni.h`)。

---

## 2. 可复现验证

发布前的 mj2x MVP 验证命令:

```powershell
xmake build -y mj2x-core
xmake build -y mj2x-cli
xmake build -y mj2x_unittest
xmake build -y mj2x_runtime_test
xmake run -y mj2x_unittest
xmake run -y mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
git diff --check HEAD
```

`product_sample_smoke.ps1` 会用 `mj2x/test_resource/mvp_jar/` 生成半真实 JAR,
分别验证 StackC/RegC 的 class 与 jar 输入,并比较连续两次生成的 `.class` /
`.jar` / `.c` SHA-256。生成物统一写到 `test/product_sample_hardening/mj2x/`。

能力到测试的追溯表见 [capabilities.md](../doc_ai/capabilities.md) 与 [test_strategy.md](../doc_ai/test_strategy.md)。

---

## 3. 基本用法

### 3.1 单 class 输入

```bash
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c
```

- `Hello_native.class`:原方法被标记为 `native`
- `Hello.c`:生成的 C 源码

### 3.2 JAR 输入

```bash
mj2x-cli -i myapp.jar -o myapp_native.jar --emit-c myapp.c
```

JAR 内多个 class 会被逐个处理,非 class 资源原样保留,entry 顺序保留。

### 3.3 选择后端

```bash
# StackC(默认,广覆盖正确性; cbackend 仍作为旧别名兼容)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend stackc

# RegC(保护导向,无栈)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend regc
```

后端对比见 [capabilities.md §2 / §3](../doc_ai/capabilities.md)。

### 3.4 只翻译带注解的方法

默认情况下,mj2x 会翻译所有可支持的非抽象、非 native 方法。若只想让部分方法走 native,可使用项目提供的 `mj2x.Native` 注解:

```java
import mj2x.Native;

public class Hello {
    @Native
    public static int protectedAdd(int a, int b) {
        return a + b;
    }

    public static int plainSub(int a, int b) {
        return a - b;
    }
}
```

先把注解源码放到 javac classpath:

```bash
javac -d build/mj2x-annotations mj2x/mj2x-annotations/src/main/java/mj2x/Native.java
javac -cp build/mj2x-annotations Hello.java
```

再用注解 descriptor 选择方法:

```bash
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --native-annotation Lmj2x/Native;
```

- `@mj2x.Native` 的 descriptor 是 `Lmj2x/Native;`
- 未加注解的方法保留原字节码
- `<init>` / `<clinit>` 不会被翻译
- `mj2x.Native` 使用 `RetentionPolicy.CLASS`,运行时不依赖该注解类出现在应用 classpath

### 3.5 调试:查看中间表示

```bash
mj2x-cli -i Hello.class -o /dev/null --dump-mir
```

---

## 4. 参数参考

| 参数 | 说明 |
|---|---|
| `-i, --input <.class/.jar>` | 输入文件 |
| `-o, --output <.class/.jar>` | 输出文件(方法被标记为 native) |
| `--emit-c <file>` | 输出的 C 源码路径 |
| `--emit-cpp <file>` | 输出 RegC C++17 源码路径；必须以 `.cpp` 结尾并配合 `--backend regc` |
| `--emit-llvm <file>` | 输出 LLVM textual IR 路径；必须以 `.ll` 结尾并配合 `--backend llvm` |
| `--config <file>` | 从 JSON 配置读取默认参数,命令行显式参数覆盖配置 |
| `--backend <stackc\|regc\|llvm>` | 选择后端(默认 `stackc`; `cbackend` / `c` 是兼容旧脚本的别名)。`llvm` 仅转换静态的 `int` / `long` / `float` / `double` / `void` 纯计算与控制流方法，其他方法保持原字节码 |
| `--native-annotation <desc>` | 只翻译带目标方法注解的方法,如 `Lmj2x/Native;` |
| `--compile` | 在 `--emit-c`、`--emit-cpp` 或 `--emit-llvm` 后自动编译 native library |
| `--c-compiler <msvc\|gcc\|clang\|zig>` | 覆盖自动探测到的 native 编译器；C++17 分别调用 `cl`、`g++`、`clang++` 或 `zig c++` |
| `--llvm-tool <clang\|llc>` | LLVM IR 的编译路径。`clang` 直接生成动态库，`llc` 先生成 object 再由 clang 链接；仅与 `--backend llvm --compile` 同用 |
| `--generate-cmake <dir>` | 生成独立 CMake native 工程；未指定 source output 时写入 `<dir>/native.c` |
| `--dump-mir` | 打印中间表示(调试用) |
| `--help` | 打印帮助 |

生成 CMake 工程示例:

```bash
mj2x-cli -i Hello.class -o Hello_native.class --generate-cmake native_project
cmake -S native_project -B native_project/build
cmake --build native_project/build --config Release
```

产品级 smoke 会验证 StackC/RegC 对 class 和 jar 输入的重复生成稳定性：

```powershell
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild
```

该 smoke 从 `mj2x/test_resource/mvp_jar/` 编译一个半真实 JAR，连续运行
StackC/RegC 两次，并比较输出 class/jar 与 C 源的 SHA-256。

---

## 5. 编译生成的 C 为 DLL / SO

### 5.1 CLI 自动编译

```bash
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --compile
```

不需要保留生成的 C 源码时可使用 `pipe` 模式：

```bash
mj2x-cli -i Hello.class -o Hello_native.class --compile --compile-mode pipe --c-compiler clang
```

`pipe` 模式不能与 `--emit-c`、`--emit-cpp`、`--emit-llvm`、`--generate-cmake` 或 `--generate-xmake` 同用。GCC、
Clang 和 Zig (`zig cc`) 直接从 stdin 读取源码，不产生 `.c` 文件。MSVC 的 `cl`/`c1` 不支持 stdin，并会拒绝
Windows 命名管道路径，因此 Windows/MSVC 使用受控临时 `.c` fallback；CLI 在编译成功和失败
返回前都会清理该文件，但这不是严格的 no-write 模式。需要严格不落盘时显式选择 Clang。

输出文件位于 `--output` 同目录:

- Windows:`Hello_native.dll`
- Linux:`Hello_native.so`
- macOS:`Hello_native.dylib`

默认编译器探测顺序:

- Windows:MSVC `cl`,再尝试 `clang` / `gcc` / `zig`
- Linux:`gcc`,再尝试 `clang` / `zig`
- macOS:`clang`,再尝试 `gcc` / `zig`

可用 `--c-compiler` 覆盖:

```bash
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --compile --c-compiler clang
```

Zig 使用其 C frontend，不要求单独安装系统 GCC/Clang：

```bash
mj2x-cli -i Hello.class -o Hello_native.class --compile --compile-mode pipe --c-compiler zig
```

自动编译需要:

- `JAVA_HOME` 指向 JDK,且包含 `include/jni.h`
- Windows 使用 MSVC 时,mj2x 会在环境未初始化时自动查找并调用 Visual Studio 2022 的 `vcvarsall.bat`;如果找不到,再从 Developer Command Prompt 或已运行 `vcvars` 的 shell 中执行

### 5.2 xmake 一键构建

生成并保留独立工程，同时立即构建：

```bash
mj2x-cli -i Hello.class -o Hello_native.class --generate-xmake native_project --compile
```

不需要保留工程时可将 xmake 作为构建驱动：

```bash
mj2x-cli -i Hello.class -o Hello_native.class --compile --c-compiler xmake
```

第二种形式在输出目录创建唯一临时工程，构建完成或失败后都会清理。两种形式都顺序执行
xmake configure/build，验证唯一 release 共享库后原子复制到 `--output` 同目录；JAR 输出随后嵌入
该库。xmake 是落地工程模式，不能与 `--compile-mode pipe` 同用。显式 direct 编译器（如
`--c-compiler clang`）也不能和 `--generate-xmake ... --compile` 同时指定。

### 5.3 RegC C++17

`--emit-cpp` 将 RegC C99 JNI 函数表调用转换为 C++ JNI 成员调用，并为 JNI export 保持
C linkage。RegC CRT 源码始终以 C 编译，通过稳定的 C ABI 链接：

```bash
mj2x-cli -i Hello.class -o Hello_native.class --backend regc \
  --emit-cpp Hello.cpp --compile --c-compiler zig
```

`--generate-cmake` 和 `--generate-xmake` 会保留 `.cpp` 文件，并随 RegC 工程复制
`mj2x-crt` source pack。

### 5.4 LLVM IR（opt-in）

`--backend llvm --emit-llvm` 直接输出 textual LLVM IR，不链接 LLVM SDK。当前只接受静态
`int` / `long` / `float` / `double` / `void` 方法中的纯数值计算、比较、分支、循环和 switch；
对象、JNI 调用、字段/数组访问、异常、monitor、整数除法/取余和引用类型会 fail closed，保留原
Java 字节码。

```bash
mj2x-cli -i Hello.class -o Hello_native.class --backend llvm \
  --emit-llvm Hello.ll --compile --llvm-tool clang
```

`--llvm-tool llc` 先调用 `llc -filetype=obj`，再由 clang 链接。两种路径都要求相应工具在
`PATH` 中。LLVM IR 当前不支持 `--compile-mode pipe`、`--generate-cmake` 或
`--generate-xmake`。

### 5.5 生成 CMake 工程

`--generate-cmake <dir>` 会把生成的 `.c` 和最小 CMake 工程写到指定目录。
核心是把生成的 `.c` 连同 JNI 头编译为 shared library。

### 5.6 手动编译(Windows / cl)

```bash
javac -h . Hello.java
cl /LD /I"%JAVA_HOME%\include" /I"%JAVA_HOME%\include\win32" Hello.c /FeHello.dll
```

### 5.7 手动编译(Linux / gcc)

```bash
javac -h . Hello.java
gcc -shared -fPIC -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" Hello.c -o libnative.so
```

---

## 6. 运行

```bash
# Windows
java -Djava.library.path=. Hello

# Linux
java -Djava.library.path=. Hello
```

---

## 7. 后端选择指南

| 场景 | 推荐后端 |
|---|---|
| 需要最大正确性覆盖、不关心抗分析 | StackC |
| 保护导向,需要无栈输出抗 c2j | RegC |
| 需要除零 trap 符合 JVM 语义 | RegC + R3 runtime |
| 代码大量使用 `Math.*` / `Integer.parseInt` / 包装类拆箱 / `Random.next*` | RegC + R3 runtime intrinsic |

后端的详细支持矩阵和限制:
- StackC:`doc/stackc_backend_readme.md`
- RegC:`doc/regc_backend_readme.md`

### 7.1 RegC intrinsic 支持范围

RegC 会在生成 C 时识别部分 JDK 调用,把这些调用降到 native runtime symbol,
避免在匹配调用点发 `_mj2x_regc_call_*` JNI 派发:

- `java.lang.Math`: `abs` / `min` / `max` / `sin` / `cos` / `tan` /
  `sqrt` / `pow` / `floor` / `ceil` / `round` / `random`
- `Integer.parseInt(String)`,包括字符串字面量参数
- 非逃逸包装类窄链路: `Integer/Long/Short/Byte/Float/Double/Boolean/Character.valueOf`
  后接对应 `xxxValue`
- 非逃逸 `java.util.Random`: `new Random(seed)` 后接 `nextInt` /
  `nextInt(bound)` / `nextLong` / `nextBoolean` / `nextFloat` / `nextDouble`

这些 intrinsic 是保护导向的窄场景优化,不是完整 JDK 对象模型。包装类或
`Random` 对象一旦作为返回值、字段/数组元素、未知 Java 调用参数或跨复杂 CFG
边界逃逸,RegC 当前会 fail-closed,不会生成语义不可靠的 jobject。

---

## 8. 测试真实混淆 class

```bash
jar xf ref_samples/SnakeGame-obf-grunt.jar b.class
mj2x-cli -i b.class -o b_out.class --emit-c b_out.c
```

> 已知:`SnakeGame-obf-grunt.jar` 中 4/14 的 class 加载时抛 `readInt exceeds buffer`(libjbytecode 边界 bug,非 mj2x 问题)。

---

## 9. 常见问题

**Q: 生成的 DLL 加载报 `UnsatisfiedLinkError`?**
检查:JNI 头是否正确生成(`javac -h`)、DLL 是否在 `java.library.path`、方法签名是否匹配。

**Q: `--compile` 报 `JAVA_HOME is not set`?**
设置 `JAVA_HOME` 到 JDK 根目录,不是 JRE 目录。mj2x 会读取 `$JAVA_HOME/include` 和平台子目录(`win32` / `linux` / `darwin`)。

**Q: Windows 上 `cl` 存在但 `--compile` 报找不到 `vcvarsall.bat`?**
mj2x 会优先使用当前 shell 的 MSVC 环境;如果 `INCLUDE` 尚未配置,会尝试在 Visual Studio 2022 的 Community / Professional / Enterprise / BuildTools 路径下查找 `VC/Auxiliary/Build/vcvarsall.bat`。若安装位置不在这些默认目录,从 Visual Studio Developer Command Prompt 运行,或先执行对应的 `vcvars64.bat`。

**Q: JAR 改写后签名失效?**
当前 MVP 不处理签名。`META-INF/*.SF` / `*.RSA` / `*.DSA` / `*.EC` 会触发 fail-closed。需要先去签名再处理。

**Q: StackC 遇到 `invokedynamic` 怎么处理?**
StackC 会通过 runtime MethodHandle 桥执行 javac 生成的 string concat / lambda。RegC 仍会对未展开的 `invokedynamic` fail-closed。

**Q: RegC 输出里有 `cstack` / `sp`?**
不应该有。如果有,是 bug —— RegC 的结构断言禁止 stack-machine anchor。

# mj2x 用户指南

> mj2x 把 Java 字节码(`.class` / `.jar`)翻译为 C 源码,并将原方法标记为 `native`。
> 本文档讲怎么用。项目概览见 `../readme.md`。

---

## 1. 构建

```bash
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-cli       # CLI 工具
xmake build mj2x-core      # 核心静态库(被 CLI 依赖)
xmake build mj2x_unittest  # 单元测试
xmake build mj2x_rt_test  # C 运行时测试
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

JAR + `--compile` 的整体模式（改写全部类 + 编译 DLL 并内嵌 + NativeLoader 自动加载）：

```bash
mj2x-cli -i myapp.jar -o myapp_native.jar --emit-c myapp.c --compile
java -cp myapp_native.jar app.Main   # 运行改写后的完整程序
```

生成的 DLL 内嵌在 `META-INF/mj2x/native/`，类加载时自动释放加载，无需改运行命令。
**JNI 符号转义说明**：生成代码里 native 方法 JNI 符号的 `$` 按 JNI 规范转义为 `_00024`（JDK 1.2+ 约定；早于该约定的 `_24` 写法会让 HotSpot 找不到内类/带 `$` 描述符的 wrapper）。外部脚本若按旧符号名定位导出函数，需要同步该约定。
整程序语义对比（多类 jar 三后端一致）由 `jar_whole_program_e2e_test` 锁定。

### 3.3 选择后端

```bash
# StackC(默认,广覆盖正确性; cbackend 仍作为旧别名兼容)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend stackc

# RegC(保护导向,无栈)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend regc
```

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

### 3.4b 按类名选择:glob include/exclude

真实 JAR 通常混合自有代码与第三方库,只想转自己的类时用类级 glob 过滤器:

```bash
# 只编译 com/acme 下的类(含子包)
mj2x-cli -i myapp.jar -o myapp_native.jar --emit-c myapp.c --native-include 'com/acme/**'

# 白名单 + 黑名单(exclude 优先)
mj2x-cli -i myapp.jar -o myapp_native.jar --emit-c myapp.c \
    --native-include 'com/acme/**' --native-exclude 'com/acme/test/**,com/third/**'
```

- glob 语义:`*` 不跨段、`**` 跨段、`?` 单字符;`/` 与 `.` 等价,jvmtp `filter.*` 的 `com/example/**` 形态可直接粘贴
- `--native-include` / `--native-exclude` 均可重复,单次取值内也支持逗号分隔
- 过滤器只收窄**类集合**,集合内的方法级选择(注解 / 前缀 / 默认全标记)照常生效
- 被剔除的类原样保留在输出 JAR 里(不标记 native、不改写字节码),只是不生成对应 C 代码
- 空片段(如 `a,,b`、纯空白)按 fail-closed 拒绝,防止笔误静默丢模式

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
| `--emit-llvm <file>` | 输出 LLVM textual IR 路径；必须以 `.ll` 结尾并配合 `--backend llvm`（v2.2 起默认开启；`with_mj2x_llvm=n` 构建时不暴露） |
| `--config <file>` | 从 JSON 配置读取默认参数，命令行显式参数覆盖配置。**全量 45+ 参数可配置**（参数注册表单源，见 [config_file.md](config_file.md) 键表） |
| `--dump-config-schema[=<file>]` | 输出配置 JSON Schema（draft-07；裸=stdout，内联 `=` 指定文件）——GUI 配置工具 / VS Code / ajv 直接消费 |
| `--dump-config-template[=<file>]` | 输出全量键 + 默认值 + 注释的配置模板（JSONC，删除注释即得合法配置）；文件目标仅内联 `=` 形态 |
| `--intrinsic-config <file>` | 用户自定义 intrinsic 配置（JSON，三后端通用；详见 [user_intrinsic.md](user_intrinsic.md)） |
| `--intrinsic-src <file>` | 追加用户 intrinsic 实现源文件（可重复，与配置内 `sources` 叠加） |
| `--backend <stackc\|regc\|llvm\|vmc4>` | 选择后端(默认 `stackc`; `cbackend` / `c` 是兼容旧脚本的别名)。`llvm` 转换标量计算/控制流 + 对象/数组/实例与外部调用/异常表/monitor/ldc（窄类型签名、引用类型与 JNI 边界均已支持；round-2 起 invokedynamic 运行时解析 / intrinsic 扩面 / 保护特性接入，剩余面 fail-closed，见 §5.4）；`vmc4` 为 VM 虚拟化后端（v2.2 可用版，保护导向） |
| `--native-annotation <desc>` | 只翻译带目标方法注解的方法,如 `Lmj2x/Native;` |
| `--native-method-prefix <prefix>` | 只翻译 `owner.name` 以指定前缀开头的方法(可重复;与 `--native-annotation` 互斥) |
| `--native-include <glob>` | 类名 glob 白名单:只编译命中的类(可重复/逗号分隔)。`*` 不跨段、`**` 跨段、`?` 单字符;`/` 与 `.` 等价。默认空 = 全部类;与注解/前缀选择正交,见 §3.4b |
| `--native-exclude <glob>` | 类名 glob 黑名单:命中的类一律不编译,**优先于** `--native-include`(可重复/逗号分隔) |
| `--compile` | 在 `--emit-c`、`--emit-cpp` 或 `--emit-llvm` 后自动编译 native library |
| `--c-compiler <auto\|msvc\|gcc\|clang\|zig\|xmake>` | 覆盖自动探测到的 native 编译器；C++17 分别调用 `cl`、`g++`、`clang++` 或 `zig c++`；`xmake` 走构建驱动（工程式编译）；`auto` 回到自动探测（默认） |
| `--compile-mode <file\|pipe>` | native 编译输入模式（默认 file；pipe 经 stdin 直传不落源文件） |
| `--emit-native-dir <dir>` | RegC 多文件输出目录（mj2x_regc_api.h + runtime.c + 每类 m_<Class>.c + entry.c） |
| `--stackc-exception-check <full\|lite>` | StackC pending 异常检查密度（默认 full；lite 仅保留 handler 边界检查） |
| `--llvm-tool <clang\|llc>` | LLVM IR 的编译路径。`clang` 直接生成动态库，`llc` 先生成 object 再由 clang 链接；仅与 `--backend llvm --compile` 同用 |
| `--generate-cmake <dir>` | 生成独立 CMake native 工程；未指定 source output 时写入 `<dir>/native.c` |
| `--generate-xmake <dir>` | 生成独立 xmake native 工程（推荐；CLI 内嵌 mj2x-rt 源，配合 `--compile` 一键出 DLL/SO） |
| `--native-opt-level <0\|1\|2\|3\|s>` | 生成 native 工程的优化级别（默认 `2`）。MSVC 映射 `/Od\|/O1\|/O2\|/Ox\|/Os`，GCC/Clang 与 LLVM 侧映射 `-O0..-O3\|-Os`，xmake 工程映射 `set_optimize`。**级别越低越能保住源码级形态**：`--backend regc --regc-obfuscate` 的 CFF dispatcher 在 `-O2` 下会被优化器消解（RegC 已用 volatile state 兜住，见 §CFF 边界），降档可进一步确保形态留存；`s` 面向体积 |
| `--native-timeout <秒>` | native 编译外部进程（编译器 / link / MSVC 环境探针 / xmake / pack hook）的墙钟超时，默认 `900` 秒；`0` = 不限时。**每次外呼独立计时**；超时即整树终止（Windows 先 `taskkill /T /F` 收割进程树再收尸，Linux 杀进程组）并以非零退出，日志记 `timed out after <n>s`。编译器卡死/挂起的环境把默认 900 调小可快速失败；对标 jvmtp `compiler.timeout` |
| `--strings-encrypt` | 字符串常量 at-rest 加密（StackC/RegC）：明文字符串序列加密后落入产物（密钥由明文 fnv1a64 折叠派生），运行时解密使用，静态分析看不到明文。默认关闭 |
| `--lang <zh_CN\|en_US>` | 消息语言（默认 `zh_CN`；`MJ2X_LANG` 环境变量同效；`--lang=<lang>` 或 `--lang <lang> <args...>` 形态，悬空值 fail-closed） |
| `--dump-mir` | 打印中间表示(调试用) |
| `--quiet` | 日志只保留警告与错误（stderr） |
| `--verbose` | 日志提升到 debug 级别（stderr） |
| `--help` | 打印帮助 |

### 4.0.1 后端专属参数

RegC：

| 参数 | 说明 |
|---|---|
| `--regc-no-inline` | 关闭 SSA 方法内联（**默认开启**：静态调用 + 无异常区 + 大小(≤48 指令)/深度(≤2)/递归门控的语义等价内联，消除 native 方法间 JNI 往返；benchmark 对照用） |
| `--regc-trace-calls` | 生成运行时调用日志（`[CALL]`/`[DEEP]`，stderr，排查用） |
| `--regc-direct-calls <档>` | native-to-native 直接 C 调用的**档位**（默认 `off`）。直调不产生 JVM 栈帧，`Throwable.getStackTrace` 等语义失真（issue #814），故按**栈失真范围**分档：`off` 只放行确定性分派的虚调用；`intra-class` 加同类内调用（含自递归）；`all` 放行全部确定性分派。原布尔开关语义等价于 `all` |
| | 实测（jvm-obf-tester，7 次取中位数，合计）：`off` 114.5ms → `intra-class` 108.6ms → `all` 99.6ms；`all` 档反超未混淆 JVM 基线（104.9ms） |
| `--regc-safe-env` | 开启 safe-env：JNI 函数表快照 + 方法入口恢复，阻断 agent 的 JNI hook（issue #748） |
| `--regc-safe-env-debug` | safe-env 调试观察模式（检出 hook 只告警不 fail，隐含开启 `--regc-safe-env`） |
| `--regc-comment-level <none\|summary\|detailed>` | 生成注释级别（默认 summary） |
| `--obf-scope-include <glob>` / `--obf-scope-exclude <glob>` | **混淆作用域**（可重复、可逗号分隔）：按类级 glob 收窄「哪些类受 RegIR 变换影响」。真实工程通常只想保护少数核心类——性能、产物体积、可调试性都要求收窄作用域。模式语义与 `--native-include/exclude` 一致（`*` 不跨段、`**` 跨段、exclude 优先） |
| | 显式给 include 即隐含开启混淆，不必再开 `--regc-obfuscate`；单独给 exclude 不隐含开启（它只做收窄）。两者都为空时零行为变化 |
| `--regc-obfuscate` | 开启基础混淆变换（MBA + opaque 谓词 + CFF 平坦化）。**默认全部关闭**：RegC 定位高性能 native 生成器，默认产物语义直译零混淆 |
| `--regc-cff-dead <n>` | CFF 恒假死块注入密度（默认 0 关闭；N = 每 N 个 dispatcher 跳转插 1 死块；死块条件 `(state*state+1)==0` 模 2³² 无解恒假、依赖运行时 state 不可被编译器证明，内容夹 `mj2x_regc_barrier` 防 DCE）。隐含开启 CFF（issue #829） |
| `--regc-cff-state-encrypt` | CFF 派生随机 state 码（splitmix64(seed,index) 替代顺序 1,2,3）+ dispatcher 入口状态异或。隐含开启 CFF（issue #829） |
| `--regc-mba-deep` | Add 深展开（De Morgan/进位恒等链 + barrier 防折叠 + per-site 按 seed 选变体）。隐含开启 MBA（issue #829） |
| `--regc-opaque-zero` | opaque 恒真谓词变体（demorgan `~(~x)==x` / shift-pair / or-dec，零 UB 位运算恒等）。隐含开启 opaque（issue #829） |
| `--regc-hide-constants` | 立即数隐藏：整型立即数 C 改写为 `E ^ const_key(site)`（E 编译期物化、密钥运行时经独立 TU 派生，LTO 下不可折叠）。**不被 `--regc-obfuscate` 联动**——每立即数多一次运行时调用，是混淆家族里最重的一项（对标 jvmtp `obfuscation.constants`） |
| `--regc-indirect-calls` | 调用目标间接化：调用点改走 per-signature dispatcher，只带 `(站点表, 组内索引)`，目标身份（owner/name/desc 字符串）移出调用点——调用图收敛到单一 dispatcher 符号（对标 jvmtp `obfuscation.calls.*`）。**不被 `--regc-obfuscate` 联动**——每次调用多一层分派，热路径开销可观。只有通用 JNI 兜底路径会落到 dispatcher：自有 intrinsic（StringBuilder/String 等）、JDK 纯函数 intrinsic（如 `Math.max`）与同模块直调快路径各自保持原有形态 |
| `--regc-computed-jumps` | computed jumps / 跳转目标间接化（**默认关**）：switch 的 `case key -> 目标块` 映射从代码挪到数据表（key 按站点密钥异或存放），分派改走运行时解析出的块索引，产物里不再有 `case <字面量>: goto block_bX` 这种直接可读的映射 |
| | 实测代价（jvm-obf-tester，5 次中位数）：ControlFlow probe 0.374ms → 0.477ms（+27%），合计 +8.9%。属混淆增益，按需开 | POD 类降级为 C struct（**默认关**）：不逃逸的纯原生字段实例改为 C 结构体，字段访问零 JNI，字段名从产物里消失。降级后 Java 侧对象不存在，反射/序列化访问它会读到空壳——这是主要代价 |
| | 实测（PodBench 样本，字段访问密集）：关 611ms → 开 **103ms**（5.9x），结果与 JVM 基线一致 |
| `--regc-transform-seed <n>` | 混淆变换种子（state 码/变体选择，默认 0；同 seed 同产物）。立即数隐藏的 site 为函数内局部计数器，不掺 seed（site 必物化在产物里，掺 seed 不增加分析者成本） |

VMC4：

| 参数 | 说明 |
|---|---|
| `--vmc4-dispatcher <switch_table\|funcptr_table\|computed_goto\|if_nest\|state_dispatcher>` | 解释器分派形态（默认 switch_table） |
| `--vmc4-seed <n>` | 编译期随机化种子 |
| `--vmc4-granularity <class\|method\|program>` | 实例粒度（默认 class；method 每方法独立 VM；program 全程序共享一个 VM，opcode 空间全局统一、体积最优） |
| `--vmc4-alias <n>` / `--vmc4-bogus <n>` | 别名/伪 handler 数量 |
| `--vmc4-drift <0..4>` | 执行期 dispatch 变体数（默认 0 关闭；issue #762 低风险档） |
| `--vmc4-drift-period <bytes>` | drift 变体切换周期（默认 16） |
| `--vmc4-encrypt-program` | 程序 blob at-rest 加密：编码期加密 opcode/字节流数据段（密钥自主 seed 派生），解释器加载期整体解密；产物 C 源与二进制中不出现明文程序流。默认关闭 |
| `--vmc4-lazy-decrypt` | 程序 blob 分块懒解密（要求 `--vmc4-encrypt-program`）：按 64 字节指令对齐块独立加密，解释器首次取码到某块才原地解密，未执行分支保持密文（内存 dump 只能拿到实际执行过的块）。与 `--vmc4-block-lazy-decrypt` 互斥 |
| `--vmc4-block-lazy-decrypt` | 程序 blob 块粒度链式懒解密（要求 `--vmc4-encrypt-program`）：按 CFG 块独立加密，块密钥链式派生（首块 prevKey = seed），JNI_OnLoad 不解密、解释器首达才解密并置缓存位。与 `--vmc4-lazy-decrypt` 互斥（同开 fail-closed） |
| `--vmc4-shuffle-blocks` | 编码期块乱序：方法内 VM 块物理布局按 seed 确定性打乱（Sattolo 单环，恒无不动点），块 id 空间一并打乱，块邻接不再镜像 CFG；语义经间接跳转表重映射保持不变。开启时间接跳转表以 xorshift32 keystream 加密落盘、JNI_OnLoad 一次性原地解密。与 drift / encrypt-program 正交可叠 |
| `--vmc4-super-ops` | super-operator 融合（issue #9）：相邻指令折叠成复合指令——`Binary(x, ±1)` 折成 SuperInc/SuperDec（去掉 rhs 操作数）；同一基本块内相邻的 `t = a op1 b` + `dest = t op2 c`（t 仅此一处消费）折成一条 FusedBinary，中间值不再以独立寄存器存在。融合后 VM 指令与 JVM 字节码不再 1:1。与 `--vmc4-fragment` 可组合 |
| `--vmc4-fragment` | 指令碎片化（issue #9）：每条计算指令（Binary/Unary/Compare/Convert）的寄存器操作数先经专用临时槽中转一次，一条指令拆成「Copy + 原指令」多段，每段单独看是无意义的中间搬运。纯编码期变换（不新增 opcode）。**代价：指令数约 2~3 倍，解释执行开销同步上升**，面向高保护诉求按需开启。与 `--vmc4-super-ops` 可组合 |

两项指令级变换的机制、代价与绕过面见 [doc/techniques/vm_super_ops.md](techniques/vm_super_ops.md)。
| `--vmc4-trace-surface <file>` | 导出 VMC4 期望 trace surface（mjdbg 红队契约的实例描述清单）到文件；仅 `--backend vmc4` 有效 |

通用（RegC/VMC4）：

| 参数 | 说明 |
|---|---|
| `--client-id <n>` | per-client 唯一派生（默认 0 = 不变，产物与历史逐字节一致；非 0 时所有后端 seed 经 `splitmix64(baseSeed ^ clientId)` 派生——每个客户端得到唯一 ISA/变体/state 码产物；同 clientId 同 seed 同产物，可复现构建不受影响）。竞品借鉴（#759/#760）低成本项：分发时为每个客户独立构建 |
| `--runtime-protection <none\|log\|delay\|fake\|terminate>` | 反侦测检查点注入（方法入口 `mj2x_rt_checkpoint_run`） |
| `--runtime-protection-step <n>` / `--runtime-protection-delay <ms>` | 检查点步长/延迟参数 |
| `--native-lib-key <hex>` | native lib 加密存储（默认关闭 = 明文嵌入）：嵌入前 XOR 加密（xorshift32 密钥流），资源入口加 `.enc` 后缀，运行时 NativeLoader 解密加载；静态 `jar tf` 提取不可直接加载（sepacker 加壳 DLL 走同一形态） |
| `--native-lib-pack-hook <cmd>` | 嵌入前外部处理钩子（sepacker DLL 加壳等，默认关闭）：`{in}` 替换为编译产物路径，hook 应就地改写该文件，非零退出码视为失败；顺序：hook → 可选加密 → 嵌入 |

### 4.1 日志

mj2x 的日志统一走 **libca log 门面 + SimpleLogBackend**（stderr 彩色输出，
格式 `[时间] [级别] [模块] 消息 (文件:行)`），模块标签包括 `cli` / `compile` /
`pipeline` / `regc` / `stackc` / `vmc4` / `llvm` / `preprocess` / `io`。

- 默认级别 `Info`：进度与统计（Pipeline 阶段、Compiling 方法、写出的产物）。
- `--quiet`：只显示 `Warn`/`Error`（如后端 fail-closed 跳过、参数校验失败）。
- `--verbose`：显示 `Debug` 细节（如逐方法编译过程）。
- 库使用者可调 `mj2x::initLogging(level)` / `mj2x::setLogLevel(level)`
  （`mj2x/logging.hpp`）自行初始化；未初始化时日志宏静默丢弃。

`--dump-mir` 的 MIR 文本、`--emit-*` 的产物与编译器子进程的透传输出仍走
stdout/原通道，不属于日志。

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

`--generate-cmake` 和 `--generate-xmake` 会保留 `.cpp` 文件，并按 RegC 分发面
（common+regc）复制 `mj2x-rt` source pack 到工程 `src/mj2x-rt/`，同时注入
后端宏 `MJ2X_RT_BACKEND_REGC=1`。

### 5.4 LLVM IR（v2.2 转正；二次扩面补齐对象/数组/调用/异常/monitor/ldc，round-2 补齐 invokedynamic 运行时解析 / intrinsic 扩面对齐 RegC / 保护特性接入）

> v2.1 曾按发布范围默认关闭（`with_mj2x_llvm=n`，CLI fail-closed）；**v2.2 起默认
> 开启**。显式关闭时 `--backend llvm` / `--emit-llvm` / `--llvm-tool` 不可用。

`--backend llvm --emit-llvm` 直接输出 textual LLVM IR，不链接 LLVM SDK。除基础款的
纯数值计算（含移位，移位量按 Java 语义取低 5/6 位；整数除/取余，除零经 JNI ThrowNew
抛 `ArithmeticException`，`INT_MIN / -1` 按 Java 语义不溢出）、比较、分支、循环、
switch、浮点→整数转换（`llvm.fptosi.sat`，NaN→0 / 溢出饱和）与**编译索引内的静态
方法互调**（同类/跨类、直接递归/互递归均可；目标必须可转换且非 library 类，否则整链
保持原字节码）外，二次扩面新增：

- **窄类型签名**：`Z/B/S/C` 参数/返回（JNI 边界 i8/i16 与 i32 栈位宽互转）；
- **对象**：`NEW` / `CHECKCAST` / `INSTANCEOF` / 静态与实例字段读写（JNI
  Get/SetField），引用类型 `REF` 全程以 opaque `ptr` 传递；
- **数组**：`newarray`/`anewarray`/`multianewarray`、`arraylength`、全部
  `XALOAD`/`XASTORE`（`boolean[]` 组件按 `byte` 处理）；
- **调用**：实例方法（`this` 经 JNI 包装器透传）、`invokevirtual` /
  `invokespecial` / `invokeinterface` 经 `CallXxxMethodA` 分派，外部方法
  （编译索引外，如 Java API）同路径；
- **异常**：`athrow` + 异常表 `try/catch/finally` 分发（含 finally 重抛、整数除零
  在 try 块内按异常表处理）；
- **monitor**：`monitorenter` / `monitorexit`（synchronized 方法/块）；
- **常量**：`ldc` String（`NewStringUTF`）/ Class（`FindClass`）；
- **intrinsic（round-2 扩面对齐 RegC）**：`array.length` / `instanceof:<desc>` /
  `monitor.enter` / `monitor.exit`；`java.lang.Math` 纯函数面（abs/min/max
  I/J/F/D、sin/cos/tan/sqrt/floor/ceil/pow、round、random）直调 mj2x-rt runtime
  符号；包装类 `valueOf`/`xxxValue` 非逃逸切片（`%mj2x_int` 等单字段 shadow
  结构）；`java.util.Random` 状态机切片（非逃逸 `new Random(seed)` + nextXxx）；
  `StringBuilder` / `StringBuffer` 状态机切片（init/append/range·capacity/toString
  直调 crt，toString 经 `mj2x_string_builder_to_jstring` 返回真 jstring）。String
  类方法面保持 JNI 分派，留待整类下沉。
- **invokedynamic（round-2 运行时解析）**：StringConcatFactory / LambdaMetafactory
  站点由 pipeline 级预处理静态展开；残留自定义 bootstrap 站点以 `indy:<idx>`
  发射为模块级全局 cache + 运行时 bootstrap 解析（`MethodHandles.lookup` /
  `MethodType` / `privateLookupIn` → bsm 调用 → `CallSite.getTarget` →
  `invokeWithArguments`，参数装箱/返回拆箱，REF 结果直接透传）。与 RegC 对齐
  （RegC 的 emitIndyBootstrapFunction 提供同功能；原文"超出 RegC"失实，订正）。
- **保护特性（round-2）**：`--runtime-protection` 每方法入口注入
  `mj2x_rt_checkpoint` 检查点（crt `mj2x_rt_protect.c`）；`--regc-obfuscate` 及
  MBA / opaque / CFF 变换选项支持 llvm（RegIR 级同源变换，mbaDeep Barrier 指令
  LLVM 子集不支持时回退未变换形态并告警）。

仍不支持（fail closed，保留原字节码）：`ldc` MethodHandle（引用常量，行为与
RegC/StackC 一致）、`MethodType` 之外的引用常量、`jsr/ret` 等遗留指令。

```bash
mj2x-cli -i Hello.class -o Hello_native.class --backend llvm \
  --emit-llvm Hello.ll --compile --llvm-tool clang
```

`--llvm-tool llc` 先调用 `llc -filetype=obj`，再由 clang 链接。两种路径都要求相应工具在
`PATH` 中。`--compile-mode pipe`（对象/数组/调用/异常/monitor/ldc 综合 E2E）与
`--generate-cmake` / `--generate-xmake`（LLVM 路径）均已支持。

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
| 需要 VM 虚拟化 + 编译期/执行期随机化 | VMC4（保护导向，性能非关键） |
| 需要除零 trap 符合 JVM 语义 | RegC + C 运行时 |
| 代码大量使用 `Math.*` / `Integer.parseInt` / 包装类拆箱 / `Random.next*` | RegC + C 运行时 intrinsic |

后端的详细支持矩阵和限制:
- StackC:`doc/stackc_backend_readme.md`
- RegC:`doc/regc_backend_readme.md`
- VMC4：参数面见 §4.0.1，能力边界见 §5.4 与 §7

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

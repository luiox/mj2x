# mj2x 性能评估规范

> 规范文档:规定 mj2x native 转换后的性能评估方法学,避免"假全量 native"导致的数据失真。本文档来源于一次实际教训——曾因未验证覆盖率,得出"RegC 比 StackC 快 39 倍"的错误结论,实际是 RegC 把热点方法 fail-closed 给 JVM 跑了。

## 1. 核心原则

**性能数据真实性 = 覆盖率已验证 + 基准公平 + 多次取样。** 三者缺一不可。

1. **覆盖率优先**:任何性能数据,必须先用 javap 验证被测方法**确实转成了 native**(ACC_NATIVE + 无 Code 属性),否则数据失真。
2. **公平对比**:对比不同后端时,被测方法的 native/Java 状态必须一致,否则测的是"JVM 帮谁跑得多"。
3. **排除 JIT 干扰**:预热后计时,多次取中位数。

## 2. 强制前置:覆盖率审计(每份性能报告必做)

**转换后、跑基准前**,必须执行覆盖率审计。不可仅依赖 mj2x 的转换日志:显式 skip 与 fail-closed(SSA/lowering/emit 失败)均会输出 `[RegCBackend] Skipping <method>: <reason>` 日志,但 fail-closed 仍保留字节码,需结合 javap 确认。

### 2.1 审计方法

```bash
# 对每个被测 class,javap 检查每个方法的 native 状态
javap -p -c -cp <native.jar> <fully.qualified.Class>
```

判定规则:
- `native` 方法 + **无 `Code:` 属性** = 真正 native ✅
- 有 `Code:` 属性(含字节码反汇编)= **保留 Java** ⚠️
- `<init>` / `static {}`(clinit)保留字节码 = **预期行为**(MarkAll 硬限制,非 bug)

### 2.2 统计"真正 native"的方法数

对被测基准涉及的每个方法,逐一确认。**重点检查静默 fail-closed**:

```bash
# 静默 fail-closed 既不在显式 skip 日志,也保留字节码,但不会被标记
# 只能靠 javap 发现:方法有 Code 属性且不是 init/clinit
javap -p -c -cp <native.jar> <Class> | grep -B1 "    Code:"
```

凡是"有字节码、不是构造器/clinit"的方法,都是未转 native(fail-closed 或显式 skip)。哪些不转、为什么,见 `regc_codegen.md` §转换边界。

### 2.3 报告必须包含覆盖率表

每份性能报告必须附覆盖率:

```
被测方法            | native 状态 | 备注
<method>           | native/java | (fail-closed 原因,若有)
```

**未附覆盖率的性能数据不予采信。**

## 3. 基准设计规范

### 3.1 公平对比的前提

对比 StackC vs RegC 时,**确保被测方法在两个后端都是真 native**。否则对比无效。

已知会破坏公平性的情况:
- 被测方法含 invokedynamic/lambda → RegC fail-closed
- 被测方法命中 high-risk 黑名单(反射/安全/IO/Thread API)→ RegC 显式 skip
- 被测方法是内部类或大异常表(tryCatchBlocks>4)→ RegC 显式 skip

> 注:浮点比较(dcmpg/dcmpl/fcmpg/fcmpl/lcmp)已实现,不再是 fail-closed;含浮点比较的方法两后端均可转 native。

**规避方法**:优先选择两后端都能转的纯计算方法(整数运算、循环、整数比较、数组、对象字段)。见第 4 节的标准基准套件。

### 3.2 基准结构

- **被测代码**(目标方法):转 native,在计时区内
- **驱动代码**(调用被测方法):**保持 Java**,用反射或直接调用,不在计时热点路径

驱动必须保持 Java 的原因:若驱动也转 native,其 native 化状态在不同后端不同(如 RegC 把含 invokedynamic 的 main 保留 Java),会引入额外变量。

### 3.3 计时与取样

- 预热:被测方法在计时前先跑若干次(触发 JIT,排除解释执行开销)。典型预热 3 次。
- 计时:`System.currentTimeMillis()` 包裹被测段。
- 取样:**至少 5 次**,报告中位数 + 范围。单次数据不可信(JVM 抖动、GC、JIT 重编译)。
- 校验:被测方法若含正确性校验(如 count 累加),必须通过,证明功能等价。

### 3.4 测量粒度

绝对耗时 <5ms 的方法,测量误差主导,**无参考价值**。要么增大工作量(更多迭代),要么标注"无参考价值"。

### 3.5 冷启动成本模型警示(#887 优化一教训,2026-10)

把"首次调用成本 × 调用次数"当作循环总成本是**错误模型**。JDK 反射
(`Method.invoke` 的 accessor/LambdaForm 链)首次调用 ~8.4µs,但同一 `Method`
对象复用时,链条装配完成后稳态即落到亚 µs 量级——**0.37µs/次**是转换产物
(invoke 机器+JNI+native 通道)的稳态实测,纯 JDK 反射稳态独立复测更低
(0.12-0.2µs/次);3000 次调用实为 ~0.4-13ms,而非 25ms。

#887 优化一(Reflection 探针)实测链:`invoke 去虚拟化`切片
(runtime 直调 helper + 生成端 target 表 + 发射拦截)语义零变化
(21 项检查 fixture,jdk21/jdk25 与未混淆参考逐字节一致)后,
同会话对照 9 轮中位数:**探针中位数不动**(jdk21 38.9→40.5ms,
jdk25 43.0→44.0ms,CV 内)。评审复测补注:同日另一次 AFTER 轮
Reflection 探针反降 9.9%/3.6%,且同轮非 reflection 探针漂移远大于该增量
(Inner classes −35.5%、Visitor −23~−30%)——机间抖动大于切片效应,
增量不可归因,回退裁决不变。微基准显示直调快路稳态 0.55µs/次
**慢于**转换产物 invoke 稳态 0.37µs/次——切片已回退(revert patch 存档于
工作底稿,未入库)。

探针真实成本构成(独立冷 JVM 拆解,SecretBox 探针路径):

| 阶段 | 耗时量级 | 可优化性 |
|---|---|---|
| Class.forName 首触(运行时 bootstrap:DLL 提取/加载/RegisterNatives/cloader 定义) | 数十 ms(隔离冷跑 ~80ms,harness 内显著衰减) | **mj2x 侧可控**,后续优化主杠杆 |
| getAnnotation(JDK 注解解析,元注解递归) | ~19-20ms | JDK 内部,不可绕 |
| Field.get 首次 | ~2.5ms | JDK 内部 |
| Method.invoke 循环(3000 次) | ~1ms(暖)/~13ms(冷) | 已证伪为瓶颈 |

**推论**:归因排序必须用"harness 形状的循环成本",不得用"单次冷调用成本 ×
次数"外推;优化前置判断以同会话 BEFORE/AFTER 9 轮中位数为最终裁决。

### 3.6 runtime bootstrap 成本分解与主导项优化(#1183 实测,2026-10)

RegC 产物 Class.forName 首触 bootstrap(NativeLoader.load 的提取/加载/注册/
Loader 类定义)的四段分解。探针:`mj2x/benchmark/bootstrap_probe/`
(BootstrapProbe.java 逐段计时 + bootstrap_probe.ps1 双 JDK 协议),每种模式
独立冷 JVM、段内 System.nanoTime、预热 3 + 计量 9 取中位数。被测产物:tester
jar(34 转换类/111 native 方法/180KB DLL,--regc-direct-calls)。

**BEFORE 分解表**(优化前行为):

| 段 | jdk21 | jdk25 | 说明 |
|---|---|---|---|
| 1 提取(资源读+临时文件+写出) | 70.8ms | 74.3ms | **主导项(~98%)** |
| 2 加载(System.load:LoadLibrary+重定位+CRT+JNI_OnLoad+Loader 定义) | 16.1ms | 8.2ms | 次要 |
| 3 逐类注册(registerNativesForClass) | 2.9ms/13类 | 2.7ms/13类 | **线性 ~0.25ms/类**,总量小 |
| 4 Loader 类定义(=段2−load_predef) | ≤4ms(噪) | ~1.8ms(噪) | 小 |
| 总和锚点 full(端到端首类 clinit) | 86.5ms | 77.1ms | 分段和≈锚点 |

注:段 3 数字为 reg_13 探针「前 13 类各自首触 clinit 内注册」的窗口口径;
全产物 34 类注册总量 ~8.5ms,分散在各类首触点,不聚集于单一首类。

**提取段根因**(阶段探针,双 JDK 一致):`File.createTempFile` 内部首次触达
`SecureRandom`(TempDirectory 静态字段初始化)在冷 JVM 上耗时 **~52-59ms**,
是提取段乃至整个 bootstrap 的真主导项;其余为 classpath jar 首开 ~5-10ms
(真实流程已在 Main 类加载时支付)+ 180KB 读写 ~1.5ms。

**优化(#1183,语义零变化)**:生成 NativeLoader 弃用 `File.createTempFile`,
改发射 `newTempFile` 私有助手——ThreadLocalRandom 随机名 + `createNewFile`
原子独占创建(32 次重试;防碰撞/防抢占语义与 createTempFile 一致;
ThreadLocalRandom 首触 ~0.3ms;createNewFile 磁盘成本冷态 ~13ms 新旧两路径
共有,不计入收益口径)。失败路径异常类型差异(记录不消除):新路径耗尽重试
抛 `UnsatisfiedLinkError`,JDK `createTempFile` 抛 `IOException`。同会话前后对照:

| 指标 | jdk21 BEFORE→AFTER | jdk25 BEFORE→AFTER |
|---|---|---|
| 提取段(现行加载器复刻) | —→24.4ms | —→22.5ms |
| bootstrap 端到端 full | **86.5→20.0ms(-77%)** | **77.1→29.2ms(-62%)** |

**已证伪/裁决不动项**(度量裁决,同 #887 教训):RegisterNatives 逐类注册线性
且 13 类总量 ~3ms,分批/延迟无收益;System.load 段 6-16ms 属 JVM/OS 侧加载
成本,mj2x 侧无杠杆;「DLL 跨运行缓存复用」上限仅再省提取段残余 ~20ms,代价
是解密 DLL 以可预测路径持久化落盘(防护姿态变化),**记录不实施**,留产品
决策。

方法论注记:段 1 探针(extract 模式)为驱动侧对生成字节码的逐调用复刻,含
jar 首开 ~10ms,略高于产物内真实提取成本;full 锚点为最终裁决口径。原始 CSV
不入库。发射确定性验证口径(N1):同输入 jar 两次独立转换,生成
`mj2x/runtime/NativeLoader.class` 字节逐一致(1732B,sha256 两次相同),且产物
对 `File.createTempFile` 引用为 0。门禁回归对拍基线以库内
`test/jvm-obf-tester/gate-baseline/regc-perf-baseline.json` 为准(N2),不引
未入库的本地基线数字。

## 4. 标准基准套件(推荐)

基于 `NativeBench`(整数运算,两后端都真 native),已验证三方法在 StackC/RegC 都完整 lowering:

| 基准 | 内容 | 两后端 native | 适用场景 |
|---|---|---|---|
| `fib(32)` | 整数递归 | ✅ 均转 | 测 native 调用栈 + 递归开销 |
| `loopSum(50M)` | 整数循环累加 | ✅ 均转 | 测纯循环 + 字段访问开销 |
| `arraySum(100K)` | 整数数组求和 | ✅ 均转 | 测数组访问(绝对值小,慎用) |

**已知不适合直接用于性能对比的样本**:
- `JavaObfuscatorTest/test.jar` 的 Calc 基准(历史):曾因 runAdd 的 `cmp.nan_*` 未实现而 RegC fail-closed,数据失真。**该 bug 已修复**,runAdd 现转 native;但其它方法(反射/lambda/内部类)仍 fail-closed/skip,使用前仍须做覆盖率审计。

## 5. 实测数据示例(参考基准)

环境:Windows x64,MSVC 2022,JDK 21。5 次中位数,被测方法均经 javap 验证为真 native:

| 方法 | JVM | StackC | RegC | RegC vs StackC |
|---|---|---|---|---|
| fib(32) 递归 | 6ms | 9560ms | 108ms | RegC 快 89x |
| loopSum(50M) 纯循环 | 11ms | 312ms | 291ms | 持平 |

**解读**:
- 递归场景 RegC 优势巨大(StackC 每次递归重建栈机+跨 JNI 边界;RegC 编译成 C 函数递归)
- 纯循环场景两者接近(无 native→native 调用开销,差异在 JNI 字段访问)
- **结论必须限定在被测场景**:不可外推到"RegC 整体快 N 倍"

## 6. 反模式(禁止)

1. **禁止**不做覆盖率审计就报性能数据。
2. **禁止**用含 fail-closed 方法的基准对比后端(数据失真)。典型:`test.jar` 的 Calc 基准直接对比 StackC vs RegC。
3. **禁止**单次取样。至少 5 次中位数。
4. **禁止**把特定场景的结论外推到整体(如"fib 快 89x → RegC 整体快 89x")。
5. **禁止**用绝对耗时 <5ms 的数据下结论。

## 7. 报告模板

```markdown
## 性能报告:<基准名>

### 覆盖率审计(必填)
| 被测方法 | native 状态 | 备注 |
|---|---|---|

### 环境
- 平台 / 编译器 / JDK / mj2x 版本

### 数据(5 次中位数)
| 方法 | JVM | StackC | RegC |

### 结论(限定被测场景)
...

### 复现
命令 + 步骤
```

## 8. 关联

- RegC 转换边界（哪些不转 native、为什么）:`regc_codegen.md` §转换边界
- 能力声明:`capabilities.md`
- 测试策略:`test_strategy.md`

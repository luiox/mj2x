# indy 原生派发（invokedynamic native dispatch）设计

> 状态：sprint 波次一设计稿（2026-09-17，agent D）。
> 波次一交付：本文档 + 管线层静态判定原型（`InvokeDynamicPreprocessor`）+ 判定单测。
> 波次二交付（2026-09-17，agent F）：本文 §6 提案 **P1/P2/P3 已在 RegC 面落地**（见各条
> 「波次二落地」标注）；P4 保持提案状态。StackC/VMC4/LLVM 面（P5-P10）未动。
> 竞品锚点：jvmtp 1.6.4 indy 纯 native dispatch（~84ns vs ~660ns，~8x）+
> `indy.crack-adapters`（识别他方 MutableCallSite adapter 形状直派，1.8x~9x），
> 见根 `doc_ai/research/draft-jvmtp-jvmtidev.md` §2/§6.1(P2)。

---

## 1. 现状

### 1.1 管线层 indy 处理全链路（pre-SSA，pipeline 级默认注册）

`InvokeDynamicPreprocessor`（`mj2x/mj2x-core/src/mj2x/preprocessor/invokedynamic_preprocessor.cpp`）
在 SSA 之前把可静态解析的 indy 改写成 RegC/StackC 已能 lower 的普通字节码：

| 站点类别 | 识别函数 | 展开函数 | 形态 |
|---|---|---|---|
| 字符串拼接 | `isStringConcatSite`（:224，认 `java/lang/invoke/StringConcatFactory` 的 `makeConcat` / `makeConcatWithConstants`） | `expandConcatSite`（:126，改写为 `new StringBuilder` + append 链 + toString） | 阶段1 |
| lambda / 方法引用 | `isLambdaMetafactorySite`（:457，认 `java/lang/invoke/LambdaMetafactory` 的 `metafactory` / `altMetafactory`） | `expandLambdaSite`（:1014，为每站点合成实现类（public SAM 落 `mj2x/lambda/Lambda$N`，包私有/protected SAM 落 SAM 同包 `Lambda$N`，#1182），站点改写为 `new` + `<init>`；前置判定已抽取为 `lambdaExpansionPrecondition` :580，与 §5 判定原型共用） | 阶段2a–2f |

lambda 展开的静态前置谓词 `lambdaExpansionPrecondition`（:580，全部来自 classfile 静态可见内容）：

- impl handle tag ∈ {`H_INVOKESTATIC`, `H_INVOKEVIRTUAL`, `H_INVOKEINTERFACE`, `H_INVOKESPECIAL`（展开期提权）, `H_NEWINVOKESPECIAL`}（:612–:621；`H_GETFIELD`/`H_PUTFIELD` 等字段句柄 **不在** 展开支持面内，进入 §2 B2/B3 直派判定）；
- `parseAltMetafactoryTail`（:468）：altMetafactory 的 serializable / markers / bridges 解析，未知 flag 位 fail-closed；
- `canEmitSam`（:531）+ `canAdaptValue`（:364）：主 SAM 与各 bridge 描述符逐位可适配——同型、CHECKCAST 收窄、拆箱、装箱（目标限 Object/对应包装类）、基本类型加宽（阶段2f：i2l/i2f/i2d/l2f/l2d/f2d）、void 返回丢弃（POP/POP2）。

**不支持形态（fail-closed，indy 原样保留）**：基本类型收窄、装箱+加宽复合、字段句柄 impl、未知 alt flag、非 LMF/concat 的一切自定义 bootstrap。

### 1.2 残留 indy 的阶段3 helper 调用路径与开销构成

未被展开的 indy 站点（自定义 bootstrap / 不支持形态，含 record 的 `ObjectMethods.bootstrap`）走运行时解析：

1. **SSA**：`ssa_builder.cpp:1113 lowerInvokeDynamic` 把站点登记进 `SSAFunction::indySites`
   （`{name, desc, bsm, bsmArgs}`，`ssa_function.hpp:76`），生成 `Intrinsic("indy:<index>")`，
   实参为 SSA 值，effect = externalCall（不可 DCE/重排）。
2. **RegIR**：`RegFunction::indySites` 值拷贝透传（`reg_function.hpp:27`），Intrinsic 名不变。
3. **RegC 每调用发射**（`regc_emitter.cpp:770 emitInvokeDynamic`）：
   - `NewObjectArray` + 逐参装箱（`_mj2x_regc_box_*`，每参 1–2 个 JNI）+ `SetObjectArrayElement`；
   - 调文件级 helper `_mj2x_regc_indy_bsm_<i>(owner)` 取 target；
   - **每次调用现场 `GetMethodID(MethodHandle.invokeWithArguments)`（未缓存）**；
   - `CallObjectMethod(invokeWithArguments)` + `ExceptionCheck` + 结果拆箱（`_mj2x_regc_unbox_*`）。
4. **RegC finalize helper**（`regc_backend.cpp:1099 emitIndyBootstrapFunction`）：每站点一个
   static 函数；static jobject 缓存 `CallSite.getTarget()` 结果（命中 = `NewLocalRef` 一次直达；
   miss 时完整重建 `MethodHandles.lookup` → `privateLookupIn(owner)`（失败回退 public）→
   `MethodType.fromMethodDescriptorString` → bsmArgs 重建（String/Class/MethodType/Handle/装箱
   原语/varargs）→ 调 bsm → `getTarget` → 全局引用缓存）。

**稳态（target 已缓存）每次调用的开销构成（RegC）**：

| 步骤 | JNI 次数（N 参） | 备注 |
|---|---|---|
| class_literal(Object/MethodHandle) 查找 | ~2 | context TLS 查表 |
| NewObjectArray + 逐参装箱/写数组 | 2N+1 | 装箱分配 |
| helper 缓存命中 | 1 | NewLocalRef |
| **GetMethodID(invokeWithArguments)** | 1+ | **每次调用都做，方法名查表** |
| **CallObjectMethod(invokeWithArguments)** | 1 | **主体开销：MethodHandle 通用调用机器**（asList 装箱 + asType 适配 + Generic invoker，无 LambdaForm 特化——这是 jvmtp 对比里 ~660ns 的主要来源） |
| 结果拆箱 | 0–1 | 基本类型返回时 |

jvmtp ~84ns 的本质：构建期/首调用期把 target **破解**为 `(declaring class, jmethodID, refKind)`
直接缓存，后续调用按原生 marshalling 直派（0–1 个 JNI），完全绕开 MethodHandle 机器与 varargs 数组。

**其余后端现状**：

- **StackC**（`stackc_mir_invoke.cpp:190 emitInvokeDynamic`）：**每次调用完整重建整条 bootstrap 链**
  （FindClass / GetMethodID / lookup / privateLookupIn / MethodType / bsmArgs / 调 bsm / getTarget /
  invokeWithArguments 全部 per-call，零缓存），仅有「OBJECT 返回+单对象参」快路径省 varargs 数组——
  残留 indy 的每调用成本高于 RegC。
- **VMC4**：`indy:<idx>` intrinsic 未实现（`vm_encoder.cpp:1246` fail-closed）→ 含残留 indy 的方法
  **整方法不转 VM**，保留 Java 字节码。VMC4 的问题不是派发慢，而是**覆盖率缺口**。
- **LLVM**：`indy:<idx>` 模块级全局缓存（NewLocalRef + NewGlobalRef 稳定化）+ 运行时 bootstrap 解析，
  与 RegC 同构（`doc_ai/capabilities.md` §1 LLVM 节）。

**record（`java/lang/runtime/ObjectMethods.bootstrap`）现状**：管线层不识别 → 落入阶段3 helper
（RegC/LLVM）或整方法不转（VMC4）→ record 的 equals/hashCode/toString 每次调用走
invokeWithArguments。这是静态可判定直派的典型目标（§3）。

---

## 2. fast-path 判定树

判定所需信息**全部来自 classfile 静态可见内容**（bsm owner/name/desc、bsmArgs 常量、indy
name/desc、编译输入类集合），不依赖运行时探测。顶层分支：

```
indy site（pre-SSA 遇到的每个 InvokeDynamicInsnNode）
├── A. bsm ∈ StringConcatFactory.makeConcat[WithConstants]
│      → T0  StaticExpanded（阶段1，现状；展开后即普通字节码，无 indy 派发问题）
├── B. bsm ∈ LambdaMetafactory.metafactory|altMetafactory
│   ├── B1. impl tag ∈ {STATIC, VIRTUAL, INTERFACE, SPECIAL, NEWINVOKESPECIAL}
│   │       且主 SAM + 全部 bridge 逐位可适配（canEmitSam 子集）且 alt 尾解析通过
│   │       → T0  StaticExpanded（阶段2a–2f，现状）
│   ├── B2. impl tag ∈ {GETSTATIC, GETFIELD} 且 captured++sam 布局与字段类型精确一致（getter）
│   │       → F1  DirectFieldAccess（新直派形态；波次二发射字段读）
│   ├── B3. impl tag ∈ {PUTSTATIC, PUTFIELD} 且 sam=(字段类型)V、captured 布局一致（setter）
│   │       → F1  DirectFieldAccess（波次二发射字段写）
│   └── B4. 其余（收窄 / 装箱+加宽 / 未知 flag / 不支持 tag / 字段句柄但布局不符）
│           → T2  RuntimeHelper（现状阶段3；reason 记录具体原因）
├── C. bsm = java/lang/runtime/ObjectMethods.bootstrap（record）
│   ├── C1. bsmArgs 形态合法（[0]=Class、[1]=String、[2]=MethodType）
│   │       → F2  RecordObjectMethods（target 全部可静态命名：record 访问器 /
│   │           equals/hashCode/toString 语义由 recordClass 静态描述符决定）
│   └── C2. 形态不合法 → T2  RuntimeHelper（malformed reason）
└── D. 其他自定义 bootstrap
    ├── D1. bsm owner 是编译输入内的非 library 类
    │       → T2  RuntimeHelper + 标记 staticEvalCandidate
    │       （波次二可对 bsm 方法体做受控 DSL 静态求值：findStatic/findVirtual + insertArguments/
    │         asType/permute 常量折叠 → 证明结果为 ConstantCallSite 时升级 F3 StaticDirect）
    └── D2. bsm owner 在库/JDK 内（或找不到）
            → T2  RuntimeHelper + 标记 runtimeCrackCandidate
            （首次调用真实执行 bootstrap（语义保持），对返回的 target 做
            Lookup.revealDirect 破解：direct handle → 缓存 (jclass, jmethodID, refKind)
            升级直派；破解失败永久走 invokeWithArguments）
```

F1/B2/B3 的**严格精确子集**（原型实现，宁窄勿宽）：

| impl tag | 必要条件 |
|---|---|
| `H_GETSTATIC`（读静态字段） | sam 参数为空、captured 为空、`samRet desc == 字段 desc` |
| `H_GETFIELD`（读实例字段） | captured 恰 1 个且为引用类型（bound receiver）、sam 参数为空、`samRet desc == 字段 desc` |
| `H_PUTSTATIC`（写静态字段） | sam 参数恰 1 个且 desc == 字段 desc、captured 为空、sam 返回 void |
| `H_PUTFIELD`（写实例字段） | captured 恰 1 个引用类型（receiver）+ sam 参数恰 1 个且 desc == 字段 desc、sam 返回 void |

不满足精确一致（含任何加宽/装箱差异）一律回 T2——F1 的发射（波次二）不含适配代码，判定即承诺。

判定与展开的**一致性**：B1 的谓词与 `expandLambdaSite` 前置检查共用同一函数（波次一原型已抽取
`lambdaExpansionPrecondition`），保证「判定 = 展开」零漂移；判定结果只是标记，不改写行为。

---

## 3. 缓存结构设计

### 3.1 call-site 静态形态 → native 入口的映射缓存

**放置**：发射侧 TU 级 static 表，与现有 `_mj2x_regc_indy_bsm_<i>` 同一编号空间（finalize 期
发射，全局站点编号 i 由后端统一分配）。形态标记本身由管线层判定并随站点元数据
（`InvokeDynamicSite`，§6 接口 P1）透传到后端；后端只发射、不再判定。

```c
/* 每直派站点一组 TU 级 static 缓存（发射形态，波次二落地） */
static jclass    _mj2x_indy_direct_cls_<i>;     /* declaring class（global ref） */
static jmethodID _mj2x_indy_direct_mid_<i>;     /* 直派方法 ID */
static jobject   _mj2x_indy_direct_recv_<i>;    /* bound receiver global ref（无则 NULL） */
static u8        _mj2x_indy_direct_state_<i>;   /* 0=CRACK 1=READY 2=HELPER */
```

**键（build time）**：`(className, methodName, siteOrdinal, indyName, indyDesc,
fnv1a64(bsmOwner + bsmName + bsmArgs 值序列))` → 全局站点编号 i。内容哈希使同形站点可去重共享
（与字符串池同思路；波次二可选优化，首版按站点一表一项）。

**失效语义**：

- **T0/F1/F2（build-time 判定形态）**：JVMS §5.4.3.6：indy 链接每类至多一次；返回
  `ConstantCallSite` 时链接永久。F2 的 ObjectMethods 恒返回 ConstantCallSite（JDK 实现）；
  F1 无 bootstrap 参与。**现状阶段3 target 缓存已隐含「target 永不变」假设**
  （`regc_backend.cpp:1111–1118`：static jobject 缓存永不失效）——直派缓存语义等同现状，
  不引入新风险。
- **bootstrap 返回 MutableCallSite 的场景**：build-time 判定**不承诺**（D1 静态求值必须证明
  ConstantCallSite 才升级 F3）；D2 运行期破解仅当 `revealDirect` 成功且能确认目标为
  ConstantCallSite 才永久缓存，否则保持 invokeWithArguments。注意 jvmtp 的 crack-adapters
  恰恰在 MutableCallSite 上直派（他方混淆器从不 retarget，实测安全）——若未来跟进，必须做成
  opt-in 实验开关并明示语义边界。
- **类卸载**：global ref 持有 declaring class 会阻止卸载（与 #780 resolved dispatch 双槽缓存的
  既有权衡一致）；`jmethodID` 在类存活期有效。首版接受与现状相同的权衡，不做弱引用周期校验。

**与 crack-adapters 的对照与建议**：jvmtp 用**形状启发式**（impl class 形状匹配
insertArguments/asCollector/asType 的 MutableCallSite）识别**他方混淆器**产物，收益 1.8x~9x，
但正确性靠形状枚举（1.6.6 还修过对部分形状的 crash）。我们的立场：

1. **自己管线可判定的 site 走 build-time 判定**（F1/F2/D1），语义可证明，是默认路径；
2. 他方 adapter 形状识别 = 启发式 + 语义风险，**建议不进默认路径**；先做 D2 的
   `revealDirect` 运行期破解——那是 JVM 保证语义的「通用破解」，天然覆盖他方 direct 形态，
   且失败即回退、无 crash 面。crack-adapters 式形状匹配留作 wave 3+ 的 opt-in 实验开关
   （`--indy-crack-adapters`），若做，形状清单可直接复用 jvmtp 已公开的三类
   （insertArguments / asCollector / asType 前缀 adapter）作为 mjdbg 反混淆形状清单的对偶。

### 3.2 运行期降级阶梯（与 §4 衔接）

```
state=CRACK（首调用）→ 走既有 _mj2x_regc_indy_bsm_<i> 拿 target（与现状逐字节同路径）
  ├─ revealDirect 成功 + 校验通过 → 写 cls/mid/recv，state=READY（此后直派）
  └─ 失败 → state=HELPER（永久走 helper，即现状行为）
```

---

## 4. 与阶段3 helper 的回退关系

1. **build-time 回退**：判定为 T2 的站点后端只发既有 helper 调用（与现状字节级一致）——判定
   失败不影响任何现有行为。
2. **发射形态**：直派桩是单分支守卫：

   ```c
   if (_mj2x_indy_direct_state_<i> == 1 /* READY */) { /* 直派： marshalled Call*/ }
   else { /* 走既有 helper + invokeWithArguments（现状路径） */ }
   ```

   首调用在 helper 路径内完成 CRACK→READY 升级；**任何失败都单调降级为现状路径**，不存在
   「快路失败后语义分叉」的状态。
3. **运行期签名校验**：READY 写入前校验 `jmethodID != NULL`、参数 slot 布局与站点描述符一致
   （build-time 已判定的形态运行期只做非空断言）；不符即 state=HELPER。
4. **异常传播**：直派与 invokeWithArguments 调的是**同一个 target 方法**，方法内抛出的异常对象
   与传播路径一致（JNI ExceptionCheck → 既有异常分发）。两处需显式处理的差异：
   - invokeWithArguments 对参数不匹配抛 WrongMethodTypeException/ClassCastException（包装层），
     直派是裸调用——build-time 签名统一检查排除该差异（判定即承诺适配已在判定内完成或不存在）；
   - null receiver：invokeWithArguments 抛 NPE；直派桩必须显式 NPE trap（复用既有
     instance-invoke null trap 基础设施）。
5. **bootstrap 副作用**：D2/现状路径 bootstrap 真实执行一次（首调用），语义与现状完全一致；
   D1 静态求值若跳过 bsm 执行属可观测语义变化（bsm 可能记数/校验）→ 仅在显式
   `--indy-native-dispatch=static`（opt-in）下允许，默认 D1 也走「真实执行 + 破解」。

---

## 5. 波次一原型（管线层，已实现）

`invokedynamic_preprocessor.{hpp,cpp}` 新增（不影响字节码输出，对现有行为零回归）：

- `enum class IndyDispatchForm`（`RuntimeHelper / StaticExpanded / DirectFieldAccess /
  RecordObjectMethods`）+ `IndyDispatchDecision`（form / reason / 站点定位 className+methodName+
  ordinal / indyName+indyDesc）+ `IndyDispatchReport`（站点列表 + 按 form 计数）。
- 公开纯函数 `classifyIndySite(CompilerIndex&, const InvokeDynamicInsnNode*)`：§2 判定树的
  可执行版本，只读，产出 reason（稳定字面量，单测断言）。B1 谓词与 `expandLambdaSite` 共用
  `lambdaExpansionPrecondition`，保证判定=展开零漂移。
- `process(CompilerIndex&, IndyDispatchReport*)` 重载：与既有流程同一代码路径，仅旁路收集判定；
  既有 `process(CompilerIndex&)` 行为不变（既有单测全部继续成立）。

波次一**不**新增字节码形态、不改写任何站点行为；DirectFieldAccess / RecordObjectMethods 的
展开与发射在波次二按 §6 提案落地。

---

## 6. 三后端接入面（接口需求提案——波次二主线程合流的输入）

> **波次二落地状态（2026-09-17）**：P1/P2/P3 已在 RegC 面实施（P4 未做）。载荷结构
> 落在 `mj2x/mir/indy_dispatch.hpp`（`IndyDispatchForm` + `IndyDirectField` +
> `IndyRecordMethods` + `IndyDispatchPayload`）；判定核心抽为站点数据级
> `classifyCore`（`classifyIndySite` 与新增站点级入口 `classifyIndySiteForm` 共用，
> 判定单源不变）。与提案的**两处边界细化**（均为宁窄勿宽、fail-closed）：
>
> 1. **F1 载荷 engagement**：`DirectFieldAccess` 站点仅当 **indy 调用描述符本身就是
>    字段操作形态**（读 = 参数空/仅 receiver 且返回==字段 desc；写 = 值布局且返回
>    void；receiver 描述符精确等于声明类）才填 `directField` 载荷、发直派桩。波次一
>    精确一致子集（captured++sam lambda 布局，站点=创建函数接口实例）保持
>    form=DirectFieldAccess 候选标记（reason 逐字不变）但载荷不 engage——发射走
>    helper，零回归。原因：lambda 捕获形态的站点结果是 lambda 对象而非字段值，
>    「站点即字段访问」只对描述符即形态的站点语义可证。
> 2. **F2 访问器匹配**：`RecordObjectMethods` 站点仅当 indy 名与 bsmArgs[3..] 中
>    **唯一**访问器句柄名字一致、tag 受支持（STATIC/VIRTUAL/INTERFACE）、返回与参数
>    布局逐位一致、且名字**不是** ObjectMethods 保留名（equals/hashCode/toString——
>    多组件聚合语义不可归约为单访问器调用）才 engage `recordMethods.accessor`；
>    其余（javac record 全部三件套站点）走 helper，零回归。

以下全部为**提案**原文（保留作合流输入）。逐条给出签名、语义、理由。

### 6.0 共同前置（MIR 层数据结构）

- **P1 `InvokeDynamicSite` 增加派发形态载荷** ✅ 波次二已落地（RegC 链路）
  - 落地形态：`IndyDispatchForm`/载荷提升到 `mj2x/mir/indy_dispatch.hpp`
    （`IndyDirectField{isStatic,isWrite,owner,name,fieldDesc}`、
    `IndyRecordMethods{recordClass,names,methodsDesc,accessor}`——accessor 为判定内
    匹配出的可直派访问器三元组，比提案草稿多此一项，发射端零判定所需）；
    `SSAFunction::InvokeDynamicSite` 与 `RegFunction::InvokeDynamicSite` 增加
    `form + optional<DirectField> + optional<RecordMethods>`（值拷贝透传，
    `reg_lowering.cpp` 复制；SSA 内联合并随站点整体 move，天然携带）。
  - 传递管道：判定发生在 **SSA 构建期**（`ssa_builder.cpp lowerInvokeDynamic` 对每个
    存留站点调 `classifyIndySiteForm`——与 `classifyIndySite` 共用 `classifyCore`，
    每站点一次判定、发射端零判定逻辑）；不依赖 CompilerIndex 的站点级入口使 MIR
    构建处可直接复用判定（D1/D2 理由细分仅存在于报告面）。
  - 签名：`SSAFunction::InvokeDynamicSite` 与 `RegFunction::InvokeDynamicSite`
    （`mj2x/mj2x-core/src/mj2x/mir/ssa/ssa_function.hpp:76`、`mir/register_ir/reg_function.hpp:27`）增加：

    ```cpp
    /// 静态派发形态（InvokeDynamicPreprocessor::classifyIndySite 判定，值拷贝透传）。
    IndyDispatchForm form = IndyDispatchForm::RuntimeHelper;
    /// form==DirectFieldAccess 时的载荷：字段读/写的精确目标。
    struct DirectField { bool isStatic; std::string owner, name, fieldDesc; bool isWrite; };
    /// form==RecordObjectMethods 时的载荷：record 类 + 方法名集 + 方法描述符集。
    struct RecordMethods { std::string recordClass; std::string names; std::string methodsDesc; };
    std::optional<DirectField>   directField;
    std::optional<RecordMethods> recordMethods;
    ```

  - 理由：形态判定发生在管线层（只有管线层看得到 classfile 与编译输入类集合）；后端按
    form 查表发射，不做任何判定逻辑，避免三后端三份判定漂移。`IndyDispatchForm` 枚举定义
    需从 preprocessor 头提升到 MIR 可见位置（或 MIR 自带同值枚举 + 边界映射，取主线程偏好）。
  - 波次一原型已提供判定入口与单测锁定，字段落地即接上。

### 6.1 RegC（regc_emitter / regc_backend）

- **P2 直派发射分支** ✅ 波次二已落地
  - 落地形态：`emitIndyDirectCall`（`regc_emitter.cpp`，`emitInvokeDynamic` 入口按
    `site.form` 分派）；F1 → `emitIndyDirectFieldCall`（Get/Put[Static]<Type>Field，
    JNI 变体按字段描述符选择），F2 → `emitIndyDirectRecordCall`（Call*MethodA 直调
    访问器，jvalue marshalling 复用 cached-call 面的 tag/cast 工具）。现状 helper
    主体逐字保留为 `emitIndyHelperCall`——RuntimeHelper 站点与其余一切未 engage
    形态的发射文本与波次二之前逐字节一致（零回归锚，测试锁定）。
  - 回退语义：单分支守卫 `if (_mj2x_indy_direct_state_<i> == 1 ||
    _mj2x_indy_direct_resolve_<i>()) { 快路 } else { 既有 helper 路径 }`；实例访问
    receiver NULL 显式 `_mj2x_regc_throw_null`（NPE 语义复现），字段/调用异常经
    ExceptionCheck 进既有 cleanup 分发。
- **P3 站点缓存表 + finalize 发射** ✅ 波次二已落地
  - 落地形态：`emitIndyDirectCache`（`regc_backend.cpp`，finalize 期、密文缓冲定义
    之后方法体之前发射）。TU 级 static：`_mj2x_indy_direct_cls_<i>` +
    `_mj2x_indy_direct_fid_<i>`（F1）/`_mj2x_indy_direct_mid_<i>`（F2）+
    `_mj2x_indy_direct_state_<i>`（0=未解析 1=READY 2=HELPER）+ 惰性解析函数
    `_mj2x_indy_direct_resolve_<i>`。「build-time 即 READY」指判定面（无 CRACK/
    bootstrap 重放态）；jclass/jfieldID/jmethodID 是运行期句柄，首调用解析一次，
    失败 ExceptionClear + DeleteGlobalRef + state=HELPER 永久回退（§4 单调降级）。
    判定失败/载荷未 engage 的站点根本不发缓存表。owner/name/desc 字面量在
    `--strings-encrypt` 开启时走 `RegCCachePool::encryptedLiteralBuffer`（直派桩
    方法体内零字面量）。§3.1 的 `_mj2x_indy_direct_recv_<i>`（bound receiver 全局
    引用）未落地：receiver 一律取自站点实参（等价且无 stale 面）。
- **P4（可选，wave 2 后半）D2 运行期破解升级** ❌ 波次二未做（保持提案状态）
  - 签名：`_mj2x_regc_indy_bsm_<i>` helper 内 target 取得后追加 crack 段：
    `Lookup.revealDirect(target)` → `MethodHandleInfo{getDeclaringClass, getName,
    getReferenceKind, getMethodType}` → 写 §3.1 缓存并置 READY。
  - 语义：与 §4 降级阶梯一致；revealDirect 对非 direct handle 抛 IllegalArgumentException → 清
    异常、保持 helper。
  - 理由：对一切自定义 bootstrap（含他方 direct 形态 site）在语义保证下吃掉 invokeWithArguments
    的每调用开销，是 8x 差距的主收割点。

### 6.2 StackC（stackc_mir_invoke）

- **P5 `emitInvokeDynamic` 增加 F1/F2 分支**
  - 签名：`emitInvokeDynamic`（`stackc_mir_invoke.cpp:190`）入口按 `MIRInvokeDynamic` 携带的
    form 分派（MIR 节点需透传 P1 载荷）。
  - 语义：F1 → 直接 Get/PutField JNI（经既有 `classExprForDescriptor` + 懒缓存 jfieldID）；
    F2 getter → 既有 cached invoke 路径调访问器。
  - 理由：StackC 现状是零缓存每调用全量重建 bootstrap（§1.2），是三后端中最慢路径；F1/F2
    直派同时消掉重建与 invokeWithArguments。
- **P6（独立小步，建议先行）bootstrap 结果缓存**
  - 签名：`emitInvokeDynamic` 尾部把 `getTarget()` 结果缓存进 TU 级 `static jobject`
    （对齐 RegC :1111 形态；`trackLocalRef` 换 global ref）。
  - 理由：不依赖任何判定、纯现状语义收紧，一行状态位即可让残留 indy 稳态从「全量重建」降到
    「NewLocalRef + invokeWithArguments」，是零风险先手棋。

### 6.3 VMC4（vm_encoder）

- **P7（推荐）编码前改写式接入**
  - 签名：管线层新 preprocessor 步骤（或在 InvokeDynamicPreprocessor 内追加 pass）：
    对 `form ∈ {DirectFieldAccess, RecordObjectMethods}` 的站点**改写字节码**——F1 改写为
    getfield/putfield/getstatic/putstatic，F2 getter 改写为 invokevirtual 访问器（语义等价由
    §2 判定保证），改写后 form 清为 StaticExpanded。
  - 语义：VM 编码面零改动（`indy:<idx>` 仍 fail-closed）；改写产生的普通调用走 VMC4 既有
    per-callsite JNI helper。
  - 理由：VMC4 的缺口是覆盖率而非速度（§1.2）；改写式让含 record/字段句柄 site 的方法首次
    可进 VM，且 `-Xverify:all` round-trip 天然守门改写正确性（与阶段1/2 同标准）。
- **P8（备选）VM 内直派 opcode**：`VmMethodRef` 增加 indyDirect 载荷 + 解释器直调
  jmethodID。仅当 P7 改写被否（如需保护「站点曾是 indy」这一信息本身）才考虑；默认不推荐
  （多一份 ISA 面 + per-build 多样性维护成本）。
- **P9 `--vmc4-encrypt-program` 组合**：P7 改写发生在编码前 → 加密面无任何变化（改写后的
  调用与其他普通调用一样进密文 blob）；直派缓存表为 native 侧数据不进 blob。D2 破解升级在
  VMC4 不可用（VM 内无 crack 桩），VMC4 只消费 build-time 可判定形态——明确写入行为边界。

### 6.4 LLVM（llvm_ir_backend，波次二顺带对齐）

- **P10**：`indy:<idx>` 模块级缓存路径（capabilities §1 LLVM 节）按 `site.form` 分派：F1/F2
  直接发射 `Call*MethodA` / `Get*Field`（复用既有 class/method 缓存池），跳过运行时 bootstrap
  解析。理由：与 RegC 同构收益，边际成本低。

---

## 7. 安全与兼容

- **签名校验**：build-time（§2 判定树的类型统一检查：F1 精确一致子集、F2 形态合法性、B1
  canEmitSam 子集）+ 运行期（READY 写入前 jmethodID 非空；§4 降级阶梯单调回退）。
- **异常传播**：同 target 方法 → 异常对象与分发路径一致；包装层差异由 build-time 校验排除；
  null receiver NPE trap 复用既有基础设施；bootstrap 自身抛出的异常在 CRACK 首调用经既有
  helper 路径传播（与现状一致，不被 NPE 掩盖——`regc_emitter.cpp:845–846` 既有守卫保持）。
- **`--strings-encrypt` 交互**：直派桩中的 owner/name/desc 字符串一律走密文池
  （P3/P5 显式要求，模板即 `emitIndyBsmArg` :1033 现状处理）；判定 reason 只存在于编译期日志，
  不落产物。
- **#760 加密 / `--vmc4-encrypt-program` 组合**：见 P9——改写式方案下加密面零变化；native
  侧缓存表（cls/mid/ref）与既有 jmethodID/jclass 缓存同密级（不加密），无新增明文。
- **`-Xverify:all`**：改写式（P7）与阶段1/2 同守门标准；直派桩（P2）为纯 native 侧，不触碰
  字节码验证。
- **反射/栈观察语义**：直派经 JNI `Call*MethodA`，产生真实 JVM 帧，`getStackTrace` /
  `StackWalker` 语义与 invokeWithArguments 相同（后者也产生帧）；与 `--regc-direct-calls`
  的栈语义讨论（capabilities §3 CLI 节）无冲突——indy 直派不裁剪任何帧。
- **fail-closed 纪律**：一切判定失败 → T2 RuntimeHelper（现状路径）；一切运行期升级失败 →
  state=HELPER。不存在静默降级之外的第三态；reason 全程可观测（编译期日志 + 单测锁定）。

## 8. 验证与验收锚点

- 波次一：`mj2x/unittest/src/preprocessor/indy_dispatch_classify_test.cpp`（判定矩阵，见 §5）；
  既有 `invokedynamic_preprocessor_test.cpp` 全绿 = 零回归证明。
- 波次二（RegC 发射落地，2026-09-17）：同文件新增 F1/F2 载荷 engagement 断言
  （直派形态 engage / lambda 捕获与保留名不 engage）；
  `mj2x/unittest/src/backend/regc_backend_test.cpp` 新增发射级锚——F1 读/写直派桩
  （守卫 + GetStatic/SetIntField + NPE trap）、F2 访问器直调（CallIntMethodA）、
  finalize 直派缓存表（TU 级 static + 解析函数 + READY/HELPER 状态位）、
  RuntimeHelper 站点发射文本逐字节一致且零直派缓存、`--strings-encrypt` 组合下
  解析函数字面量密文化。按 `performance_evaluation_spec.md` 的覆盖率审计与
  microbench 对比（残留 helper vs F1/F2 直派）留待发射合流后执行。
- 后续波次：三后端各一条 exec-level E2E（record site + 字段句柄 site）随 P5-P10
  消费合流补齐。

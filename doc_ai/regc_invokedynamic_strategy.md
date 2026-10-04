# RegC invokedynamic 策略

> 状态：2026-08-09 更新。invokedynamic 支持分阶段实施，本文记录每阶段的
> 覆盖范围、机制与 fail-closed 边界。支撑 `dev_plan.md` R6.1。

## 分阶段策略

| 阶段 | 覆盖 | 机制 | 状态 |
|---|---|---|---|
| 阶段1 | `StringConcatFactory.makeConcat[WithConstants]` | pre-SSA 展开为 `new StringBuilder`/`append` 序列 | ✅ |
| 阶段2 | `LambdaMetafactory.metafactory`/`altMetafactory` | pre-SSA 为每站点合成实现类（public SAM 落 `mj2x/lambda/*`；包私有/private 嵌套 SAM 落 SAM 同包（javac protected 落盘即 public，谓词不触发），#1182；default 包 SAM 落 default 包裸名。生成器侧修复：已发布产物遇包私有 SAM 不自动治愈，需重转换），SAM `invokestatic` 转 impl，含捕获/方法引用/装箱/桥接/序列化等 2a-2f | ✅ |
| 阶段3 | 任意自定义 bootstrap | SSA 层登记站点 + RegIR 透传 + finalize 生成文件级 `_mj2x_regc_indy_bsm_<i>` 运行时 call-site helper | ✅（2026-08-09） |

## 阶段3 机制

- **SSA**：`INVOKE_DYNAMIC` 节点（非 LMF/concat）登记到 `SSAFunction::indySites`
  （name/desc/bsm Handle/bsmArgs），生成 `Intrinsic("indy:<index>")` 指令，参数为
  调用实参的 SSA 值。不再整方法 fail-closed。
- **RegIR**：`RegFunction::indySites` 值拷贝透传，Intrinsic name 不变。
- **emitter**：方法体内收集参数装箱 `Object[]`，调用 `_mj2x_regc_indy_bsm_<i>
  (owner)`，`invokeWithArguments` 调用后拆箱回目标寄存器。
- **finalize**：为每站点生成 bootstrap 函数：
  - `MethodHandles.lookup()` → `privateLookupIn(owner, lookup)`（失败回退 public）；
  - `MethodType.fromMethodDescriptorString(desc, owner.getClassLoader())`；
  - bsm 静态参数重建：`String`（NewStringUTF）、`Class`（含 primitive 包装类
    `TYPE`）、`MethodType`、`Handle`（`Lookup.findVirtual/findStatic/findSpecial/
    findConstructor/findGetter/...`）、装箱原语、varargs `Object[]`；
  - `CallStaticObjectMethod(bsm)` → `CallSite.getTarget()` → **全局引用缓存**
    （每站点 static jobject，二次调用直达）。
- **owner-class**：trampoline 把 `clazz`（静态）或 `GetObjectClass(thisObject)`
  （实例）存入 thread_local `_mj2x_regc_owner_class`。

## fail-closed 边界

- 装箱+加宽复合适配（引用类型先拆到 instantiated 包装类再加宽到 impl 基本类型）：
  仍 fail-closed。
- 基本类型收窄：非法形态，fail-closed。

## 验证

- 自定义 bootstrap 用例（ASM 生成 invokedynamic 字节码）：
  - `(I)I` 无 bsmArgs：`INDY_RESULT=6` PASS；
  - `(String,I)String` + Handle bsmArg：`INDY2_RESULT=hi:42` PASS。
- 回归：TEST.jar 17 项全 PASS、snake 稳定运行。

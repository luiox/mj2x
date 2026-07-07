# mj2x 能力索引（Capability Map）

> mj2x 产品线内部能力地图。共享能力以 mbase `doc_ai/capability_map.md` 为准。
> 本文件只列 mj2x 独有能力和对外依赖。

---

## 独有能力

| 能力域 | 权威来源 | 说明 |
|---|---|---|
| 字节码转 native（RegC / CBackend） | `mj2x/` 各 backend | 栈式 MIR → SSA → 寄存器 IR → C/native |
| VMC4 自研 VM | `mj2x/vm/`（可选支线，R5+） | opcode 自变异 + 资源切片 |
| native 运行时 | `mj2x/runtime/` | crypto / antidebug / antijvmti / stringpool |
| 方法选择与 CLI native 编译 | `mj2x/mj2x-cli/` | 注解筛选、C 编译器探测、JNI include 推导 |
| 用户注解包 | `mj2x/mj2x-annotations/` | `@mj2x.Native` 方法标记源码 |
| 受保护样本生成 | `mj2x/` 保护管线 | 供 mjdbg 攻击的受保护产物 |

## 对外依赖（共享能力走 mbase）

| 能力 | 权威来源 |
|---|---|
| i32/u32/Result 等基础类型 | `ca::core`（third_party/libca） |
| 字节码解析/修改 | `libjbytecode`（libs/，mbase 维护） |
| JAR/class 文件 IO | mbase ResourceHelper / JarUtil |
| UTF-8 字符串 | `ca::str` |

## 入口文档

- 架构：`mj2x/doc/architecture.md` / `mj2x/doc/architecture_redesign.md`
- 路线图：`mj2x/doc/roadmap.md`
- 测试策略：`mj2x/doc_ai/test_strategy.md`
- 开发计划：`mj2x/dev_plan.md`

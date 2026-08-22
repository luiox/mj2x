# R3 保护模块整合设计(mj2x-runtime → mj2x-rt)

> 状态:**探测移植已实施**(分支 `feat/mj2x-rt-protect-probes`);mj2x-runtime 整树
> 删除待后续 commit。背景见 dev_plan 2026-08-22 条目与 #894 运行时/模板分离。

## 现状

`mj2x-runtime`(C++ 静态库)自产品线早期预研而来,从未被生成工程消费。
`mj2x-rt/src/common/mj2x_rt_protect.{h,c}` 是其检测/响应概念的 **C99 独立重实现**
(rt_protect.h:4 自述),已随 `--runtime-protection` 全链路使用。

两套系统对照:

| 能力 | rt_protect(C99,在用) | mj2x-runtime(C++,搁浅) |
|---|---|---|
| 用户态调试器 | IsDebuggerPresent / CheckRemoteDebuggerPresent | 同左 + PEB NtGlobalFlag 堆标志 |
| 硬件断点 | 无 | x64 调试寄存器 Dr0-7(GetThreadContext) |
| 软件断点 | 无 | 代码段 0xCC/int3 字节扫描 |
| Tracer | Linux TracerPid | 同左 |
| 可疑映射 | maps 可写可执行段 | 同左(实现细节更细) |
| agent/JDWP | cmdline 扫描(-javaagent 等) | antijvmti:JVMTI 模块枚举 + capability 查询 |
| 响应层 | Log/Delay/Fake/Terminate + 概率触发 | enforcement:CheckpointState + 策略 + fail-closed 聚合 |
| 加密面 | 无 | sha256/HKDF/kernel + stringpool 解码 |

## 方案:探测目录移植,C ABI 不变

**不缝合两套系统**。将 C++ 层独有探测逐项移植为 C99 并入
`mj2x_rt_protect.c`(保持自包含、无 JNI 依赖、随包分发),完成后删除
mj2x-runtime 整树。

### 移植清单(实施状态)

1. **PEB NtGlobalFlag**(Windows):✅ 已移植——PEB.BeingDebugged(+2) 与
   NtGlobalFlag(x64 +0xBC / x86 +0x68,mask 0x70),MSVC 内在函数读段基址。
2. **NtQueryInformationProcess 三元组**:✅ 已移植——DebugPort(7)/
   DebugObjectHandle(30,容忍 STATUS_PORT_NOT_SET)/DebugFlags(31),动态加载。
3. **硬件断点**:✅ 已移植——CONTEXT_DEBUG_REGISTERS 取 Dr0-3/Dr7,
   判定"任一地址非零 && Dr7 低 8 位有使能位"(地址残留不误报)。
4. **Linux agent 库映射**:✅ 已移植——maps 中路径含 "agent" 的 .so 映射,
   补运行期 attach 注入面(启动型由 cmdline 覆盖)。
5. **软断点自检**:**有意缓议**——对自身序言扫 0xCC 在优化代码下有把立即数/
   偏移字节误判的风险,而本探测的失败代价是 TERMINATE 杀掉正常进程,不对称。
   待有可信的自校验方案(如构建期记录函数哈希)再启用。
6. enforcement 策略层:C 侧概率触发模型已覆盖 CheckpointState 语义,不再移植。

### 接口影响

无。`mj2x_rt_checkpoint_run` 签名不变;新探测并入内部 probe 集,命中走既有
force/概率判定与 action 分发。可选加 `mj2x_rt_probe_mask(uint32_t)` 允许调用方
关停特定探针(误报逃生门)。

### Tier-2:stringpool/crypto(独立特性,另行排期)

sha256/HKDF + stringpool 解码支撑"常量池加密"发射器特性(stackc `_mj2x_ln`
内联 UTF-16 数组的加密版):构建期 HKDF 派生密钥加密字符串池,运行期惰性解码。
这是发射器+运行时联动的新特性,不属于本次移植;移植完成后再评估是否从旧树抢救
实现参考。

## 实施步骤

1. 新建分支,按清单 1-4 逐项移植,每项独立 commit + 单测
   (探测逻辑注入测试:伪造 PEB/maps 文本/上下文,沿用 anti_test.cpp 的用例骨架)。
2. 删除 `mj2x/mj2x-runtime/` 整树与 `mj2x_runtime_test` 目标;
   intrinsic 四件套由 `mj2x-rt/src/regc/mj2x_regc_intrinsic.c` 接管(45/49 符号
   已覆盖,余 4 个符号核实无消费后弃)。
3. regc/vmc4 e2e 的 `compileRuntime("intrinsic/*.cpp")` 清单移除。
4. capabilities.md 删 runtime core 行、readme/regc_intrinsic.md 结构收敛;
   dev_plan 记录验证数据。

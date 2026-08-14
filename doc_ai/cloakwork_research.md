# Cloakwork 研究笔记（2026-08-15）：对 mj2x 代码生成的借鉴评估

> 来源：[ck0i/Cloakwork](https://github.com/ck0i/Cloakwork)（header-only C++20 Windows
> 混淆库，MIT，`cloakwork.h` ~6800 行）。本文记录对其机制的研究结论、与 morpher 的
> 对照、以及可吸收项与风险契约。待办登记见 issue #829。

## 一、Cloakwork 机制清单

| 机制 | 原理 | 强度 | 局限 |
|---|---|---|---|
| 编译期随机 `CW_RANDOM_CT` | `consteval fnv1a(__FILE__/__LINE__/__COUNTER__/__TIME__)` → 每处展开唯一编译期常量 | 每站点独立密钥/结构 | `__COUNTER__` 是 MSVC/GCC 扩展，重排源码会变 |
| `CW_STR` 字符串 | 参数化 Feistel 密码，8 种轮函数变体由 4 个模板 key 推异；`constinit` 落 .rdata；size_pad 防长度特征 | 每站点结构唯一 | 解密后即明文；模板元编程形态 |
| `CW_INT`/MBA | 随机 key 编码 `(add_mba(v,k1))^k2`；Deep MBA = De Morgan/进位恒等展开 + `volatile` 防折叠 | 内存值永不明文 | 模板类，只 C++ |
| `CW_FLAT_*`/`CW_PROTECT` | switch 状态机；**state 码经 derive_state(id,seed) 伪随机映射**；死块注入；状态异或加密；opaque-零缠绕防 DCE；CW_NOINLINE 防 LTCG | IDA 呈复杂状态机 | 每处膨胀大 |
| `CW_IMPORT`/`CW_SYSCALL` | PEB walk + 导出表 FNV-1a hash 解析；syscall number 提取 + 间接 syscall thunk | 无导入表 | Windows PE 专用 |
| `meta_func` 元变形 | VirtualAlloc 生成随机 junk + `mov rax,target;jmp rax` thunk，每 1000 次调用重建 | 运行时形态变化 | 自修改代码页，JVM 禁止 |
| anti_debug/anti_vm | PEB/NtGlobalFlag/硬件断点/rdtsc/CPUID/沙箱 DLL/父进程；crash-if-detected | 面广 | Windows 原生 |
| ret-gadget/hook/完整性 | ntdll ret gadget 覆写返回地址；CRC32 完整性；E9/FF25 等 hook 字节检测 | 对抗 hook | Windows 原生 |

## 二、与 morpher 对照

| Cloakwork | morpher 对应物 | 差距 |
|---|---|---|
| 编译期随机模板 key | 后端 IR 一次性发射，无 per-site 随机化 | **无 per-site 编译期随机**（有 seed 体系，确定性） |
| Feistel 多态字符串 | runtime stringpool（HKDF-SHA256 密钥流 XOR） | **加密强度已更强**；缺"每站点多形态代码" |
| MBA volatile 深度展开 | applyRegTransformPasses MBA（纯位恒等） | 深度/防折叠弱；无 per-site 变体 |
| CFF + 死块 + 状态加密 + opaque-零 | RegC CFF（**顺序 state 码 1,2,3..**，纯函数才平坦化） | 缺死块/状态异或/随机 state 映射/零缠绕 |
| 不透明谓词（4 恒等） | opaquePredicates（Or/And(Eq,1)） | 形态少，缺 gauss/demorgan 变体 |
| PEB/导出表/syscall | rt_protect（Linux 为主）+ safe-env + antijvmti | Windows 侧互补；策略层更强（enforcement） |
| 元变形 thunk | 无 | 不适用（JVM 禁止自修改） |

## 三、结论

1. **有帮助，集中在"代码生成形态"层**（纯 IR/发射层改动，零 Windows 依赖）：
   - RegC CFF 增强：死块注入 + 状态异或 + 派生随机 state 码（替代顺序 1,2,3）
   - MBA 深度变体 + volatile 防折叠 + per-site 变体选择
   - opaque-零恒等变体扩展（gauss/demorgan/consec_product）
2. **不需要新 cpp 后端**：现有 `--emit-cpp` 只是 C99→C++17 交叉编译方言
   （`#if __cplusplus` + thread_local/JNIEnv 直参），**不是模板元编程混淆层**。
   Cloakwork 式模板混淆价值主要吸收进 C 输出层；cpp 维持"同一源码交叉编译"定位。
3. **思路借鉴**：编译期随机状态映射、死块/状态加密 CFF、VMC4"每样本种子→splitmix
   opcode 映射 + 死 opcode"、hash 驱动无明文解析（仅 Windows 产物可选）。

## 四、优先级（issue #829 待办）

- **P0**：① RegC CFF 增死块 + 状态异或 + 派生随机 state 码；② MBA 增 volatile
  深度变体与 per-site 变体选择；③ opaque-零恒等变体扩展
- **P1**：VMC4 opcode 每样本种子随机映射 + 死 opcode；后端支持方法级常量种子参数
  （同 seed 同产物，`--seed random` 才非确定）
- **P2**：Windows 面 optional：crt 补 FNV-1a 无明文模块解析（仅产物跑 Windows 时）

## 五、风险契约（吸收前必须遵守）

- **可复现构建**：任何"每次编译不同"的随机化必须**显式种子控制 + 默认不变**，
  否则撞碎 mj2x 的 repeatability/fail-closed 测试基线（capabilities 明确有
  重复生成稳定性契约）。
- **fail-closed**：死块/opaque-零可能被 C 编译器优化掉或报 unreachable，需
  per-pass 开关；深 volatile MBA 显著增大二进制（O0 开销），默认关闭或强度档位控制。
- **不适用项**（JVM/Windows 语境无意义或有害）：CW_IMPORT/syscall、ret-gadget
  spoof、hook 字节检测、PEB/CPUID/沙箱 DLL、meta_func 自修改 thunk。

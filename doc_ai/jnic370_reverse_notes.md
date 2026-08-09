# JNIC 3.7.0 逆向笔记（jnic37_native.dll 分析）

> 本文档记录对 JNIC 3.7.0 生成的 native DLL 的逆向取证结果，重点对比 3.5.1 生成模型，
> 提炼**性能相关更新点**与混淆机制，供后续性能评估参考。
>
> 素材：
> - `C:\Users\Canrad\AppData\Local\Temp\opencode\jnic37_native.dll`（7558656 B，PE64，MSVC C）
> - IDA 9.0 数据库：`C:\Users\Canrad\AppData\Local\Temp\opencode\jnic37_native.dll.i64`（56 MB）
> - 分析手段：`idat.exe -A` 批处理 + IDAPython（无 GUI）；ida-pro-mcp 插件随 idat 自动加载

## 1. DLL 结构总览

| 项 | 值 |
|---|---|
| 大小 | 7558656 B（.text 6.56 MB / .rdata ~400 KB / .data 128 KB） |
| 编译 | MSVC C（无 C++ RTTI），x64 |
| 导入 | **仅 KERNEL32 / msvcrt / SHELL32**（`CommandLineToArgvW`）——**零 JNI import** |
| 函数数 | 2440（约 2345 个匿名 `sub_`，无符号表） |
| 导出 | 94 个 `Java_dev_jnic_aN__00024jnicLoader`（JUMPOUT stub 壳）+ `JNI_OnLoad` |
| JNI_OnLoad | 单函数 **553 KB**（0x18056e85c-0x1805f598d），集中注册入口 |
| 壳 | 无加壳；`.rdata` 压缩（含加密常量池，运行时解密） |

关键事实：**DLL 不导入任何 JNI 函数**。所有 JNI 调用经 `env->functions` 函数表
**间接调用**（`call qword ptr [reg+0xNN]`），IDA 无法静态关联名字——这是 3.7.0 与
3.5.1（直接 `(*env)->Fn(env, ...)` 源码）编译形态的本质区别。

## 2. JNI 函数表偏移实证

调用点证据（反汇编上下文验证语义）：

| 偏移 | 槽位 | 语义 | 证据 |
|---|---|---|---|
| 0x030 | 6 | FindClass | `call [rax+30h]` + rdx=类名字符串 |
| 0x0A8 | 21 | NewGlobalRef | FindClass 后 `call [rax+0A8h]`，结果存全局 |
| 0x110 | 34 | CallObjectMethod | `mov rbx,[rax+110h]` 缓存指针再调用 |
| 0x6B0 | 214 | RegisterNatives | `(env, table, count=1)` + 紧跟 ExceptionCheck |
| **0x720** | 228 | **ExceptionCheck** | 1 参、jboolean 返回（`test al,al; jnz 异常路径`） |

注意：**ExceptionCheck 偏移为 0x720 而非标准 JNINativeInterface 的 0x718**——
3.7.0 编译时使用扩展/自研 jni.h（ExceptionCheck 前多一个槽）。后续若精确反混淆
需按自定义表重映射。

## 3. JNI 调用统计（全量扫描 42033 个间接调用点）

```
JNI 函数                    调用点   含调用的函数数
ExceptionCheck  (0x720)    15694     696
CallObjectMethod(0x110)     3013      97
ExceptionClear  (0x088)     3615     201
ExceptionOccurred(0x078)    3343     208
NewGlobalRef    (0x0A8)     1685     403
DeleteLocalRef  (0x0B8)     1448     115
PushLocalFrame  (0x098)     1340     367
SetCharArrayRegion(0x570)    824     193
SetByteArrayRegion(0x568)    778     104
Throw           (0x068)      657     196
GetCharArrayRegion(0x500)    538      68
IsInstanceOf    (0x100)      449     103
FindClass       (0x030)      163      11
RegisterNatives (0x6B0)        1       1
NewString       (0x408)       32       2
```

要点：
- **ExceptionCheck 只有 15694 / 42033 = 37.3% 的 JNI 调用点带检查**（3.5.1 为 100% 逐点检查）
- **RegisterNatives 全 DLL 仅 1 次**（3.5.1 每类一次，TEST.jar 30 次）
- **NewString 仅 32 次**（3.5.1 每次字符串字面量都 NewString）→ 字符串常量池化
- 数组 Region 批量调用大量（Set*/Get*ArrayRegion 合计 ~3800 次）→ 数组访问走批量接口

## 4. 注册机制（集中式）

- 全部类在 `JNI_OnLoad` 流程中**一次性 RegisterNatives**（0x6B0 仅 1 调用点）
- 注册数据（类名/方法名/描述符/函数指针表）加密存储在 `.rdata` 压缩区，
  **运行时解密**（反编译显示 `qword_180729658` 全局解密缓冲区 + dword 异或校验：
  `*(_DWORD*)(buf+offset) ^ 0xED5BBF5D` 等）
- 94 个 `Java_dev_jnic_aN__00024jnicLoader` 导出是**混淆 stub**（`JUMPOUT` 尾跳），
  不是真实注册入口
- `dev/jnic/dHQvOm/JNICLoader` 类名硬编码在 DLL（`asc_1806F93FC`）

## 5. 类缓存（全局变量惰性缓存）

3.5.1 的 `cached_c_N` 结构 + `atomic_flag` 自旋锁被替换为**全局变量数组 + 惰性 null 检查**：

```c
// 每个类一个全局缓存槽（反编译还原形态）
if (!qword_180729700) {
    // 解密类名字符串 -> 调用 FindClass（函数表+0x168 或 +0x30）
    qword_180729700 = FindClass(env, v564);       // FindClass 调用
    if (ExceptionCheck(env)) { /* 分发/返回 */ }
    qword_180729700 = NewGlobalRef(env, qword_180729700);  // 升级全局引用
}
```

- 163 个 FindClass 调用点 → 163 个全局缓存槽（0x180729658..0x18072D8A0 区间）
- **无锁**：初始化假定在单线程（OnLoad/首次使用）完成；无 memory_order 语义
- 缓存槽直接以 `cs:qword_18072xxxx` 全局地址访问（3.5.1 是每次调用缓存函数 `c_N_(env)`）

## 6. 方法体翻译模式（混淆版栈机）

反编译一个 51 KB 巨型方法体（sub_180001021）还原的生成模型：

1. **栈机保留**：`STACK[0x1200]` 等巨型栈帧数组（= 3.5.1 的 stack_N/jvalue 槽位，
   改为数组 + 巨帧）
2. **常量加密**：整型/浮点常量写成随机 64 位（`STACK[0x1258] = 0x6F270AB9552F6001LL`），
   运行时解密
3. **控制流扁平化**：`switch(v73)` + 魔法数（`case 0u:`/`-501621571`/`-882691371`）分发，
   直链 if/goto 被打散
4. **MBA 整数混淆**：`4LL * (a62 != 0) + 1068`、`4LL * (v70 << 32 != 0) + 600`
5. **函数指针缓存**：调用前 `mov rax,[env]; mov rbx,[rax+0x110]`（缓存 CallObjectMethod
   指针到寄存器/局部），间接调用
6. **巨型签名**：扁平化函数被编译成 60+ 参数（`local variable allocation has failed`），
   hexrays 还原度低

3.5.1 的逐指令直译（`stack0.i = (unsigned int) stack0.i + stack1.i`）在 3.7.0 中
被"加密常量 + 扁平化 + MBA"组合替代——**混淆优先于可读性**。

## 7. 性能更新点（3.5.1 → 3.7.0）

| # | 更新点 | 3.5.1 | 3.7.0 | 性能影响 |
|---|---|---|---|---|
| 1 | **选择性异常检查** | 每个 JNI 调用后必查 ExceptionCheck | 仅 37.3% 调用点检查（15694/42033） | 省 60%+ 检查开销 |
| 2 | **函数指针缓存** | `(*env)->Fn` 每次取表 | 调用点前 `mov reg,[rax+off]` 缓存指针复用 | 省间接寻址 |
| 3 | **集中注册** | 每类一次 RegisterNatives（30 次） | 全 DLL 1 次 | 启动注册成本摊薄 |
| 4 | **字符串常量池** | 每次 NewString 重建 | 全局池化（NewString 32 次） | 省字符串构造 |
| 5 | **数组批量接口** | 逐元素 Get/SetObjectArrayElement | Set*/Get*ArrayRegion 批量（~3800 次） | 省边界检查/调用开销 |
| 6 | **无锁类缓存** | atomic_flag 自旋锁每类缓存函数 | 全局槽惰性初始化（OnLoad 单线程） | 省锁开销 |
| 7 | **集中初始化** | FindClass 散落各缓存函数 | 163 个 FindClass 集中在初始化期 | 首调用延迟摊薄 |
| — | 混淆成本（负优化） | 纯直译 C | 扁平化 + MBA + 常量加密 | 代码膨胀 ~10x，I-cache 恶化 |

## 8. 混淆机制汇总

- **控制流扁平化**：switch + 魔法数分发（JNI_OnLoad 与巨型方法体均有）
- **常量加密**：立即数异或/随机化，运行时解密（含注册表 `.rdata` 压缩区）
- **整数 MBA**：`4*N+1068` 形态的运算替换
- **符号剥离**：方法体全部 `sub_` 匿名，仅保留 94 个 loader 导出壳名
- **反 agent 字符串**：DLL 内残留 `isNativeMethodPrefixSupported`（`-javaagent` 检测相关）
- **运行时解密**：jar 内 3.7.0 的 JNICLoader 解密出本 DLL；DLL 内注册数据再次解密

## 9. 结论

3.7.0 相对 3.5.1 的核心演进：**性能优化（选择性异常检查、函数指针缓存、字符串池、
批量数组、集中注册）+ 混淆加固（扁平化/MBA/常量加密/符号剥离）**。其中性能项
（尤其 #1 选择性异常检查与 #2 函数指针缓存）是 3.5.1 直译模型的直接升级方向；
混淆项与性能互斥（代码膨胀、I-cache 压力），可作为"混淆开销"的对照组。

## 10. 附录：分析脚本

均位于 `C:\Users\Canrad\AppData\Local\Temp\opencode\ida_out\`，经
`idat.exe -A -S<script> <dll|i64>` 运行：
- `dump_funcs_ida.py` / `funcs.txt`：函数清单
- `jni_indirect_ida.py` / `onload_stats.txt`：JNI 间接调用统计
- `hotspot_ctx_ida.py` / `hotspot_ctx.txt`：关键偏移调用点上下文
- `reg_caller_decomp.txt` / `body_decomp.txt` / `loader_decomp.txt`：反编译产物

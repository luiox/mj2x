# native 库打包/加密

> 状态：XOR 加密存储形态已实现（2026-08-15）；强加密/密钥下沉 native 层
> 属 sepacker 对接范围（roadmap 主线第 6 项）。目标混淆器的解密逻辑逆向
> 记录见 `jnic370_reverse_notes.md`。

## 打包形态（已实现）

- `--compile` / `--generate-xmake` 生成的 native 库经 `jar uf` 嵌入
  `META-INF/mj2x/native/<name>.dll`（Windows）/ `.so`（Linux）/ `.dylib`（macOS）。
- 受保护类 `<clinit>` 注入：
  1. `mj2x.runtime.NativeLoader.load("META-INF/mj2x/native/<lib>", ".dll")` —— 解出到
     临时目录并 `System.load`；
  2. `mj2x.cloader.Loader.registerNativesForClass(classId, ThisClass.class)` ——
     RegisterNatives 动态绑定（JNI_OnLoad 入口统一按 classId 分发表）。

## 加密存储形态（已实现，--native-lib-key）

- `--native-lib-key <hex>`：嵌入前对 native 库做 XOR 加密（xorshift32 密钥流，
  `mj2x/core/native_lib_crypto.hpp`，与 NativeLoader 字节码内算法逐字节一致），
  资源入口加 `.enc` 后缀（`<name>.dll.enc`）。
- 运行时 `NativeLoader.load` 检测 `.enc` 后缀 → 读流逐字节解密 → 写临时文件 →
  `System.load`。静态 `jar tf` 提取得到的是密文 blob，不可直接加载。
- 强度说明：非密码学（密钥藏于 NativeLoader 字节码常量），用途是抬高静态
  提取/取证门槛；密钥下沉 native 层、强加密（AES/密钥流）属 sepacker 对接
  范围——sepacker 加壳 DLL 走同一 `.enc` 存储形态（`kNativeLibEncryptedSuffix`
  约定）。

## 平台资源命名

`x64-windows.dll` / `x64-linux.so` / `x64-macos.dylib` / arm 变体（参考
native-obfuscator 的 `LoaderUnpack` 分发：`os`/`arch` 前缀 + `System.load`）。
加密形态下后缀叠加：`<name>.dll.enc`。

## 加密方案参考（业界）

| 方案 | 机制 | 备注 |
|---|---|---|
| 明文嵌入 | JAR 内直接放 dll | 可被 `jar tf` + 提取，弱 |
| 熵编码/异或 | 加载时解码到内存/临时文件 | ClassGuard 的 AES 类加载器思路；mj2x 已实现 XOR 档（--native-lib-key） |
| 全加密 + 内存加载 | 解密后 `DefineClass`/内存加载 | JNIC 字符串加密（chacha20 密钥流）同思路 |
| 运行时壳 | 解密 key 藏在 native 层 | 与幻影盾 Ed25519 校验链可组合；sepacker 对接点 |

## 解密算法的获取路径

目标混淆器的解密逻辑通过 IDA/动态插桩逆向（参考 `jnic370_reverse_notes.md` 的
frida + patch 流程），解码算法在 `native_library_packaging.md` 的对应小节补全。

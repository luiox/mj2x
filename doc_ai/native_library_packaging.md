# native 库打包/加密

> 状态：草稿（2026-08-09 更新）。知识储备：本文整理 native 库在 JAR 内的
> 打包、加载与可选加密方案。具体解密算法待实现时按目标混淆器逆向补充。

## 打包形态（已实现）

- `--compile` / `--generate-xmake` 生成的 native 库经 `jar uf` 嵌入
  `META-INF/mj2x/native/<name>.dll`（Windows）/ `.so`（Linux）/ `.dylib`（macOS）。
- 受保护类 `<clinit>` 注入：
  1. `mj2x.runtime.NativeLoader.load("META-INF/mj2x/native/<lib>", ".dll")` —— 解出到
     临时目录并 `System.load`；
  2. `mj2x.cloader.Loader.registerNativesForClass(classId, ThisClass.class)` ——
     RegisterNatives 动态绑定（JNI_OnLoad 入口统一按 classId 分发表）。

## 平台资源命名

`x64-windows.dll` / `x64-linux.so` / `x64-macos.dylib` / arm 变体（参考
native-obfuscator 的 `LoaderUnpack` 分发：`os`/`arch` 前缀 + `System.load`）。

## 加密方案参考（业界）

| 方案 | 机制 | 备注 |
|---|---|---|
| 明文嵌入 | JAR 内直接放 dll | 可被 `jar tf` + 提取，弱 |
| 熵编码/异或 | 加载时解码到内存/临时文件 | ClassGuard 的 AES 类加载器思路 |
| 全加密 + 内存加载 | 解密后 `DefineClass`/内存加载 | JNIC 字符串加密（chacha20 密钥流）同思路 |
| 运行时壳 | 解密 key 藏在 native 层 | 与幻影盾 Ed25519 校验链可组合 |

## 解密算法的获取路径

目标混淆器的解密逻辑通过 IDA/动态插桩逆向（参考 `jnic370_reverse_notes.md` 的
frida + patch 流程），解码算法在 `native_library_packaging.md` 的对应小节补全。

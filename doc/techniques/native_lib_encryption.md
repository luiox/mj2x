# native 库 at-rest 加密（Native Library Encryption）

> 原生库嵌入 JAR 前先 XOR 加密，资源入口加 `.enc` 后缀；运行时 NativeLoader
> 解密后加载。直接解压 JAR 拿到的库无法加载。

| 项 | 值 |
|---|---|
| 对标术语 | 无通用业界名；属 packer 族 |
| 作用层 | 打包期（加密）+ 运行时（NativeLoader 解密） |
| 开关 | `--native-lib-key <hex>` / `--native-lib-pack-hook` |
| 后端 | 与后端无关（产物打包期） |
| 默认 | 关（明文嵌入） |
| 状态 | ✅ 已落地 |

## 对抗什么

默认产物把 native 库以明文资源嵌进 JAR（`META-INF/mj2x/native/`）。解压 JAR 就能
拿到可直接 `dlopen` / 反汇编的 `.dll` / `.so`——所有前面的变换成果都以明文摆在
那里。

这一项防的是"直接把库抠出来分析"这条最短路径。

## 机制

- **加密**：`--native-lib-key` 给密钥，嵌入前按 xorshift32 密钥流做 XOR 加密，
  资源入口加 `.enc` 后缀。
- **解密**：运行时 `NativeLoader` 检测 `.enc` 后缀 → 解密 → `System.load`。

同步有一个 pack hook：`--native-lib-pack-hook` 是嵌入前的外部处理钩子（sepacker
DLL 加壳对接），`{in}` 占位符替换为编译产物路径，hook 应就地改写该文件；非零
退出码视为失败。

**顺序**：hook → 可选加密 → 嵌入。

## 代价

| 维度 | 影响 |
|---|---|
| 性能 | 加载时一次解密（一次性） |
| 兼容性 | 依赖 `NativeLoader` 在类初始化前完成加载（clinit 前插） |
| 防护强度 | XOR 流加密是**强度最低的一档**，只防"随手解压"，不防认真分析 |

## 已知绕过面

- **密钥在产物里**：XOR 密钥必然物化在产物中（否则运行时无从解密）。
- **解密后就是明文**：解密到临时文件后 `System.load`，dump 该文件即拿到明文库。
- **NativeLoader 字节码是可读的**：解密算法完全可见。

要真正的强度得走外部加壳（`--native-lib-pack-hook` 挂 sepacker 之类），本项只
是内置的轻量档。

## 与其他技术的关系

- 与 **VM 程序加密**定位相同（at-rest），但保护对象不同：本项保护整个 native
  库，后者保护 VM 的指令 blob 且支持块级懒解密。
- 与 **字符串加密**同属 at-rest 族。

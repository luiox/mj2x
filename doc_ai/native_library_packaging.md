# Native 库打包/加密技术（草稿）

> 状态：**草稿**。本文档记录 native 库（dll/so/dylib）打包进 jar 的各种技术方案，作为后续实现的知识储备。mj2x 当前用最简单的明文资源方案（§1），其余方案（压缩/加密）尚未实现，待 R6 主线（覆盖率提升）完成后补充实现与细节。
>
> 关联：`dev_plan.md` R6.2；参考实现见 `ref_samples/native-obfuscator`、`morpher-legacy`、`JavaObfuscatorTest-main/sample/JNIC-3.7.0.jar`。

## 0. 共同骨架

所有方案共享同一个加载骨架（与 mj2x StackC/RegC 现有 `NativeLoader` 一致）：

```
<clinit>:  NativeLoader.load(resourcePath)        // 或直接 inline
             → getResourceAsStream(resourcePath)
             → [解压/解密] → 临时文件
             → System.load(tempFile.getAbsolutePath())
             → JNI_OnLoad 里 RegisterNatives 绑定（StackC）
             → 或标准 Java_* 符号自动解析（RegC）
```

`init()` 空方法常作为触发器（类加载即触发 `<clinit>` → load），JNIC 用此模式。

各方案差异只在「资源如何存储」与「读取时如何还原」两环。

## 1. 方案 A：明文资源（mj2x 现状）

- **存储**：dll/so/dylib 直接作为 jar 内资源，路径如 `META-INF/mj2x/native/<libname>.dll`。
- **加载**：`getResourceAsStream` → 写临时文件 → `System.load`。
- **实现**：mj2x `mj2x-runtime` 的 `NativeLoader.load(resourcePath, tempSuffix)`（`NativeLoaderPostprocessor` 注入 `<clinit>` 调用）。
- **优点**：实现最简，无额外依赖。
- **缺点**：体积大（dll 明文，可被 unzip 直接取出）；无反逆向。
- **mj2x 状态**：已实现，StackC/RegC 通用。

## 2. 方案 B：压缩归档（减小体积）

- **存储**：把 native 库（或整个 native 目录）用 Deflater 压缩成单数据块，存为 jar 资源。
- **加载**：`getResourceAsStream` → Inflater 解压 → 临时文件 → `System.load`。
- **参考实现**：`morpher-legacy/morpher-base/src/main/java/com/github/luiox/morpher/jar/internal/CompressDataTool.java`。
  - 特点：**带去重排序**的压缩。把数据切成 chunk，按 SipHash24 计算的 sort key 排序后压缩，使含大量重复数据的大目录/多文件压缩率显著提升（用于多平台 native 库共存场景：win/linux/mac × x64/arm 的多个库去重）。
  - 格式：`H2A1` 头 + varlong size + chunk 序列（每个 chunk: id 列表 + sort key + 数据）。
  - 配套 `extract(fromFile, toDir, fileName, targetName)` 解压还原。
- **native-obfuscator 的变体**：`LoaderUnpack.java`（`ref_samples/native-obfuscator/.../compiletime/LoaderUnpack.java`）——按平台（os.name/os.arch）选 `/ <platformTypeName>-<osTypeName>` 资源，读到临时文件后 load。
- **优点**：体积显著减小（尤其多平台库）；实现中等。
- **缺点**：仍无加密，解压后是明文 dll。
- **mj2x 适用**：当需要支持多平台 native 库打包时，借鉴 CompressDataTool 的去重排序。

## 3. 方案 C：加密 + 解密流（反逆向，JNIC 采用）

- **存储**：dll 加密后作为 jar 资源（或拆散到多个合成类里）。
- **加载**：自定义 `InputStream` 子类，在 `read()` 里流式解密 → 写临时文件 → `System.load`。
- **参考实现**：JNIC 的 `dev.jnic.<pkg>.JNICLoader`（见 `JavaObfuscatorTest-main/sample/JNIC-3.7.0.jar`）。
  - `JNICLoader extends InputStream`，字段含 `ByteBuffer z`（64 字节直接缓冲，LITTLE_ENDIAN）、`DataInputStream b`、辅助类 `S`/`t`/`s`（实现流式解密状态机）、`byte[] l`（1 字节读缓冲）。
  - `<clinit>`：按平台（os.name/os.arch → win/linux/macos × x64/arm64）选资源 → `JNICLoader` 解密流 → 临时文件 → `System.load`。带 `ByteBuffer.putInt(<verification int>)` 验证码机制（防篡改）。
  - `init()` 空方法触发类加载。
  - 解密算法：javac 把解密逻辑编译成复杂的字节码状态机（多 boolean 标志 + DataInputStream），具体算法需逆向（草稿不展开，实现时从样本逆向）。
- **dump 工具**：`JNIC-Virtualization`（`C:\workspace\JNIC-Virtualization`）。原理：用 ASM patch `JNICLoader.<clinit>`，把 `File.createTempFile` 替换成固定路径 `new File("jnic_nativeLibrary")`、移除 `System.load`，运行后解密的明文 dll 落到固定文件。证明 JNIC 是真 AOT 机器码（不是运行时解释），只是加密存储。
- **优点**：强反逆向（静态看不到明文 dll，需动态 dump）；体积可控（可结合压缩）。
- **缺点**：实现复杂（解密算法、密钥管理、防 dump）；性能开销小（解密一次性）。
- **mj2x 适用**：作为保护强度增强项，需独立设计密钥派生与解密流。参考 JNIC 的 `extends InputStream` 流式解密模式。

## 4. 方案 D：xor 头部变种（轻量混淆）

- **存储**：zip 包，前 N 字节（如 1000）做 `xor 1`（或固定 key 异或），破坏 zip magic header 使常规 unzip 无法识别。
- **加载**：读资源 → 前 N 字节 xor 还原 → 当作 zip 解析 → 取出 dll → 临时文件 → `System.load`。
- **特点**：native-obfuscator 的某些变种/同类工具采用。极轻量，只能挡最基础的静态提取。
- **mj2x 适用**：保护强度低，除非作为最低成本的可选项，一般不优先。

## 5. 方案对比

| 方案 | 体积 | 反逆向强度 | 实现复杂度 | 代表 |
|---|---|---|---|---|
| A 明文资源 | 大 | 无 | 低 | **mj2x 现状** |
| B 压缩归档 | 小 | 无 | 中 | CompressDataTool |
| C 加密解密流 | 可控 | 强 | 高 | JNIC |
| D xor 头部 | 中 | 极弱 | 低 | native-obfuscator 变种 |

## 6. mj2x 落地建议（草稿，待 R6 主线后细化）

1. **短期**：保持方案 A（明文），确保功能正确。
2. **中期（多平台支持时）**：引入方案 B 的去重压缩（借鉴 CompressDataTool），解决多平台 native 库体积问题。
3. **长期（保护强度增强）**：实现方案 C（加密解密流），需设计：
   - 密钥派生（从类名/常量池/运行时环境派生，避免硬编码）。
   - 流式解密 `InputStream`（参考 JNIC `JNICLoader`）。
   - 防篡改（验证码/checksum，参考 JNIC `ByteBuffer.putInt`）。
   - `<clinit>` 注入逻辑（扩展现有 `NativeLoaderPostprocessor`）。
4. **实现位置**：`mj2x-core/src/mj2x/postprocessor/`（打包策略 postprocessor）+ `mj2x-runtime/src/`（解包运行时）。

## 7. 待补充（实现时填）

- [ ] 方案 C 的具体解密算法（从 JNIC 样本逆向 `JNICLoader.read(byte[],int,int)` 的状态机）。
- [ ] 方案 B 在 mj2x 的集成接口（postprocessor 签名、资源命名约定）。
- [ ] 多平台 native 库的命名/选择约定（os.name/os.arch → 资源名映射，参考 LoaderUnpack）。
- [ ] 与 NativeLoader/StackC RegisterNatives 机制的集成点（加载顺序：解包必须在 RegisterNatives 之前）。

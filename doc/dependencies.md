# mj2x — Dependency Analysis & Version Lock

> 遵循 morpher-native third_party 依赖策略 (DEPENDENCY_POLICY.md)
> 所有版本必须固定 commit/tag，不可浮动。

---

## 1. 依赖清单

### 1.1 构建时依赖 (P0)

| 依赖 | 类型 | 版本 | 锁定方式 | 用途 |
|------|------|------|---------|------|
| **libjbytecode** | 内部 (同仓库) | `main` HEAD | 同仓库分支 | class 文件读写核心 |
| **libjstr** | 内部 (同仓库) | `main` HEAD | 同仓库分支 | 字符串工具 |
| **libca_core** | submodule | commit 0c901e41 | `VERSIONS.lock.md` | 基础类型 (u8, i32) |
| **libtyped** | 内部 (同仓库) | `main` HEAD | 同仓库分支 | RTTI 替代 |
| **libcrypto** | 内部 (同仓库) | `main` HEAD | 同仓库分支 | SHA-1 (可选) |

### 1.2 运行时依赖 (P0)

| 依赖 | 类型 | 版本 | 用途 |
|------|------|------|------|
| **JNI 头文件** | 系统/JDK | JDK 17+ | JNI 函数签名和类型定义 |
| **C 编译器** | 系统 | MSVC/GCC/Clang | 编译生成的 .c 文件 |

### 1.3 测试依赖 (P1)

| 依赖 | 类型 | 版本 | 锁定方式 | 用途 |
|------|------|------|---------|------|
| **gmock** | submodule | v1.17.x (commit 52eb8108) | `VERSIONS.lock.md` | 单元测试 |
| **Google Benchmark** | submodule | v1.9.4 (可选) | `VERSIONS.lock.md` | 性能基准 |

### 1.4 无新增外部依赖

MVP (C 后端) 不需要 LLVM 或其他第三方库。所有依赖均为 morpher-native 现有内部库。

### 1.5 R3 runtime crypto status

R3 runtime core currently carries an in-repo minimal implementation under
`mj2x/runtime/src/crypto/`: SHA-256, HMAC-SHA256, AES-128 block encryption,
AES-128 CTR, and constant-time comparison.  This keeps the first RegC/runtime
samples dependency-free and testable while the loader ABI is still moving.

The implementation is covered by public SHA/HMAC/AES/HKDF known-answer vectors
in `mj2x_runtime_test`.  It is not a claim of hardened side-channel resistance.
Before mj2x claims production-grade cryptographic hardening, this module should
be reviewed or replaced with a locked, audited lightweight library.

| Replacement candidate | Shape | Evaluation point |
|------|------|---------|
| **tiny-AES-c + local SHA/HMAC** | single-file C AES plus current SHA/HMAC | small; AES table timing still needs review |
| **Mbed TLS subset** | trimmed C library | complete primitive set; more integration work |
| **BearSSL subset** | constant-time-oriented C library | stronger side-channel posture; lower-level API |

**Decision point**: before R3 is advertised as hardened rather than functional,
lock a reviewed choice here and in `third_party/VERSIONS.lock.md`.

---

## 2. 依赖版本确认

### 2.1 libjbytecode API 依赖分析

mj2x-core 依赖的 libjbytecode API:

| libjbytecode 组件 | mj2x 用途 | 风险 |
|--------------------|-----------|------|
| `ClassReader` | 解析 .class 文件 | 低 — API 稳定 |
| `ClassWriter` | 写回修改后的 class | 低 — API 稳定 |
| `ClassNode` / `MethodNode` / `FieldNode` | Tree API 操作 | 低 — API 稳定 |
| `InsnList` / `AbstractInsnNode` 及 16 子类 | 指令遍历和分析 | 低 — API 稳定 |
| `TryCatchBlockNode` / `LabelNode` | 异常处理分析 | 低 — API 稳定 |
| `Type` | 类型描述符解析 | 低 — API 稳定 |
| `Opcodes` | 指令常量 | 低 — 不变 |
| `Analyzer<V>` / `Frame<V>` | 帧分析 (MVP 可选) | 中 — 可能需调整 |
| `typed::cast<T>` | 无 RTTI 向下转型 | 低 — API 稳定 |

**结论**: 无 API 断裂风险，可直接依赖 `main` 分支。

### 2.2 JDK 版本要求

- **构建时**: JDK 17+ (获取 jni.h)
- **运行时**: JRE 17+ (加载生成的 native 库)
- **JNI 头文件路径**: 通过 `JAVA_HOME` 环境变量发现

---

## 3. JNI 头文件集成

C 后端需要 `jni.h`，但 mj2x 本身不直接链接 JDK。策略：

```lua
-- 通过 xmake 的 find_package 或 JAVA_HOME 定位 jni.h
if is_plat("windows") then
    -- jni.h 在 %JAVA_HOME%/include/ 和 include/win32/
    local jdk_include = os.getenv("JAVA_HOME") .. "/include"
    add_includedirs(jdk_include)
    add_includedirs(jdk_include .. "/win32")
end
```

---

## 4. 版本变更策略

1. 初始开发阶段：对接 `libjbytecode` `main` 分支 HEAD
2. 如果 libjbytecode API 发生不兼容变更：
   - 在 mj2x 的 `xmake.lua` 中锁定 libjbytecode 到特定 commit
   - 或适配新 API
3. 进入稳定期后：将版本记录提交到 `third_party/VERSIONS.lock.md`

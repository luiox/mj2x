# mj2x 日志规范（libca log 门面）

> 本文记录 mj2x（及 mjt-obf）统一日志接入的设计与约定。持久约定，改动需同步本文。

## 选型

- **门面**：`libca/log`（`third_party/libca` submodule 的 log 模块，`ca::log` 命名空间）。
  用户宏 `CA_LOG_*`（default target）/ `CA_LOGT_*`（指定 target），fmt 风格格式化，
  编译期裁剪由 `CA_COMPILE_LOG_LEVEL` 控制（默认 Info，低于该级别的宏调用在编译期移除）。
- **后端**：`SimpleLogBackend`（零依赖同步，stderr 彩色，格式
  `[时间] [级别] [target] 消息 (file:line)`）。spdlog 后端（`with_spdlog=y`）存在但
  当前无消费方——CLI/混淆器工具同步直写足够，且避免 spdlog 的 windows.h 污染
  （wingdi.h 会把 `VOID`/`ERROR` 等宏化）。
- **挂载**：`third_party/libca.lua` 已 `includes("libca/libca/log")`；消费方
  `add_deps("libca_log")`。

## Windows 宏冲突（重要）

`windows.h`（wingdi.h）会 `#define ERROR 0`。libca log 的级别枚举因此把
`Error` 命名为 **`Error_`**（`Level::Error_`；`to_string`/`from_string` 仍输出
"Error"）。业务代码只用宏（`CA_LOGT_ERROR`），不直接引用枚举值；必须直接引用时
用 `Level::Error_`。mjt-obf 的 `LogHandle` 转发层已按此适配。

## 初始化

```cpp
mj2x::initLogging();                        // 默认 Info，stderr 彩色
mj2x::initLogging(ca::log::Level::Debug);   // 或显式级别
mj2x::setLogLevel(ca::log::Level::Warn);    // 运行期调整（--quiet/--verbose）
```

`initLogging` 注册的 target：`default`、`cli`、`compile`、`pipeline`、`regc`、
`stackc`、`vmc4`、`llvm`、`preprocess`、`io`（共享同一 backend，各自持有级别）。
**未初始化时宏调用被门面静默丢弃**（`LoggerRegistry::get` 返回 nullptr）——库使用方
必须自行初始化（如 unittest 的 `SetUp` 调 `mj2x::initLogging`）。

## 级别语义

| 级别 | 用途 |
|---|---|
| Error | 失败、参数校验错误、fail-closed 原因 |
| Warn | 降级/回退（如后端 Skipping 方法）、可恢复问题 |
| Info | 进度与统计（Pipeline 阶段、Compiling 方法、写出产物） |
| Debug | 细节（逐方法编译过程等），`--verbose` 开启 |
| Trace | 预留（当前无使用方） |

## 通道约定

- 日志一律 stderr（SimpleLogBackend 默认）。
- **不属于日志的输出保留原通道**：CLI usage 帮助（stdout）、`--dump-mir` 的 MIR
  文本（stdout）、编译器子进程透传输出（原样转发）、生成的 native 代码内的
  runtime trace（`regc_runtime_template` 的 `[CALL]/[DEEP]`，属于生成产物）。

## mjt-obf 参考

mjt-obf 保留历史调用点形态：`LogUtil::get("PassName")` 返回 `LogHandle`
（`mjt-obf/src/mjt/obf/log_util.hpp`），`log.info(...)` 等逐一转发到
`ca::log::detail::log_with_source`。新代码可直接用 `CA_LOGT_*` 宏；调用点零改动的
迁移层仅在历史代码需要时保留。

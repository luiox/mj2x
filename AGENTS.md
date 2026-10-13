# AGENTS.md

mj2x 是独立的 Java 字节码 → native 编译器仓：把 `.class`/`.jar` 翻译为 C/C++/LLVM IR
并将原方法标记 `native`，经 JNI 保持 Java 运行时互操作。自 morpher monorepo 拆出
（拆仓来历与 vendor 清单见 [MIGRATION.md](MIGRATION.md)）。

**仓库私有，不开源**（所有者裁决）：商业保护产品线，建仓必须 `--private`；
不得公开、不得加公开镜像 remote；对外只走产品发布物。

## 命令

```sh
xmake f --with_tests=y        # 开测试（gtest 走 xrepo 自动拉取）
xmake build <target>          # 一次一个 target
xmake run <target> --gtest_brief=1
```

目标：`mj2x-cli`（产品）、`mj2x-core`（库）、`mj2x-rt`（C 运行时，随生成工程分发）、
`mj2x_unittest` / `mj2x_integration_test`（需 `--with_tests=y`）、
`mj2x_fake_zig` / `mj2x_fake_xmake`（测试桩 exe）、`mj2x_rt_test`（运行时自测）。

构建开关：`--with_tests=y`、`--with_mj2x_llvm=n`（LLVM IR 后端，默认开）、
`--with_mj2x_backend=all|stackc|regc|vmc4|llvm`（#886 单后端编译裁剪）、
`--with_asan=y`（不与 gtest target 组合）。全量回归用默认配置（backend=all）。

外部依赖全部走 luiox-repo 中心包仓直连包（libca / libjstr / libjbytecode /
libjzip / libgarena / libjdk_finder + xrepo 的 gtest / nlohmann_json），不引入
仓内副本、不做 phony 垫片；各包嵌套的 libca 必须用 `add_requireconfs` 钉回同一
`LUIOX_LIBCA_VERSION`。仓内自带件只有两处 vendor：`test_helper/`（测试基建）与
`mtest_set/src/`（manifest + 生成器头文件子集），来历见 MIGRATION.md。

## 约定

- **Doxygen**：`///` 行注释；`/** */` 禁止。
- **No RTTI**：`dynamic_cast`/`typeid` 禁止，用 `ca::core::tag_cast::cast<T>` 家族。
- **Types**：`i32`/`u32`/`usize`/`Result<T,E>`（libca）。
- **Strings**：`Utf8String` 家族；API 借用视图用 `std::string_view`，不新增
  `std::string` 存储（字节边界除外）。
- **Includes**：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`（如
  `"mj2x-cli/src/..."`、`"manifest/native_fixture_manifest.hpp"`），禁 `../`。
- **测试输出**：`<project_root>/test/`（test-helper API），基线在 `test_resource/`。
- **构建门**：delete-then-rebuild——内容哈希增量会跳过"回到旧状态"的文件，
  信测试结果前先 `rm <exe> && xmake -r <target>`；构建与验证不并成一条命令。
- **三后端纪律**：StackC/RegC/VMC4 三后端改动须 emit 形单测 + 每后端至少一条
  exec 级 E2E（`StackCBackendE2E` / `RegCBackendE2E` / `Vmc4BackendE2E` 矩阵）；
  跨三后端改动三面都要验。

## 加依赖前的判断

新增依赖必须说明它服务于哪个在用的消费方；优先用 luiox-repo 已有包，包不存在的
库先在上游发版再消费，不为省事往仓里拷源码副本。vendor 目录（test_helper、
mtest_set/src）只做等价同步，不做功能性新增。

## E2E 依赖

后端 E2E 需要：JDK（`JAVA_HOME` 或 PATH，javac/java）、MSVC（vcvars64 + cl）、
clang（LLVM/RegC intrinsic 直编，PATH 探测）。缺哪样，对应用例 skip（不算失败，
但报数时要如实列出）。

## 提交

- 不提交测试产物与临时文件（`test/`、`build/`、`.xmake/`）；提交前 `git status`。
- 提交信息用约定式前缀并带模块作用域：`feat(core): …`、`fix(cli): …`、
  `chore(build): …`、`docs: …`。

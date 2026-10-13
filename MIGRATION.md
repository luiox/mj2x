# MIGRATION — 自 morpher monorepo 拆出记录

本文记录 mj2x 独立仓的拆出来历、vendor 清单与后续待办，供溯源与接手。

## 拆出来源

- 源仓:`morpher`(D:\WorkSpace\morpher)@ HEAD `0afa97db84b6a7c989752e45c75888a87522db51`
  (分支 `codex/libjbc-foundation-fixes`,拆仓日 2026-10-13)。
- 旧 hash 回查:filter-repo 的 `commit-map` 在本仓 `.git/filter-repo/commit-map`
  (旧 monorepo hash → 新仓 hash 全映射);monorepo 侧 issue 号(#NNN)仍指向
  morpher 仓,本仓不可直接引用。

## filter-repo 命令(两遍,均 --force)

```sh
git clone --no-local <morpher> mj2x && cd mj2x
git checkout codex/libjbc-foundation-fixes
python -m git_filter_repo --force --path mj2x/ --path morpher-native/mj2x/ \
    --path-rename 'morpher-native/mj2x/:mj2x/'
python -m git_filter_repo --force --path-rename 'mj2x/:'
```

拆出基线验证(通过):567 提交、367 文件、根化后 `HEAD^{tree}` =
`ee0e7fb885b201e7863ab553cc7700b4f3fd90a5`,与 monorepo `HEAD:mj2x` 树哈希相等
(内容逐字节未动)。历史保留 filter-repo 原样,未 amend;monorepo 的 v2.x 系列
tag(v2.0 … v2.2)随历史重写保留。分支收敛为单 `main`。

## 依赖接线

monorepo 时期 mj2x 消费仓内 `libs/` target 与根 xmake 垫片;独立仓改走
[luiox-repo](https://github.com/luiox/luiox-repo) 中心包仓直连包(接线基准照抄
mjt 独立仓先例):

| 包 | 版本 | 替代的 monorepo 面 |
|---|---|---|
| libca(全模块超集,含 test/resources/i18n) | 0.0.15 | `libca_*` phony 垫片(xmake/libca.lua) |
| libjbytecode | 0.0.2 | 仓内 `libs/libjbytecode`(聚合 + 五子库) |
| libjstr / libjzip / libgarena | 0.0.2 | 仓内 `libs/libjstr` / `libs/libjzip` / `libs/libgarena` |
| libjdk_finder | 0.0.1 | 仓内 `libs/libjdk_finder` |
| gtest / nlohmann_json | xrepo | 同 monorepo |

全部嵌套 libca 用 `add_requireconfs` 钉回同一 `LUIOX_LIBCA_VERSION`(D33 同族
防线,见根 xmake.lua 注释);`xmake/libca_rules.lua` 自 mjt 等价拷贝
(resources/i18n 构建规则,包安装产物不带规则)。

源码零改动部分:各子模块 xmake(`mj2x-core`/`mj2x-cli`/`mj2x-rt`/`unittest`)
只改依赖来源(phony 垫片 `add_deps` → 包 `add_packages`),目标定义、文件清单、
后端裁剪逻辑原样;源码硬依赖(`<jbytecode/...>`、`<libca/...>`、`<jzip/...>`、
`<jdk_finder/...>`、`<nlohmann/json.hpp>`)include 形态与包安装布局天然一致。

## vendor 清单(仓内自带件)

| 目录 | 内容 | 来源 | 说明 |
|---|---|---|---|
| `test_helper/` | test_helper.hpp/.cpp、jdk_fixture.hpp、jdk_fixture_gtest.hpp、README.md(4+1 文件) | morpher @ 拆仓 HEAD | 取 morpher 版(mj2x 测试针对它写;mjt 版同代码、注释旧)。`xmake.lua` 用 mjt 式包消费(`add_packages("libjdk_finder", "libca")`);未带 `qprotect_sample/`(mbase 专属) |
| `mtest_set/src/` | manifest 3 头(core_fixture_registry / native_fixture_manifest / semantic_fixture_manifest)+ gen 10 头(sample_builder + arith/array/const_load/control_flow/static_value/wave1-4) | morpher @ 拆仓 HEAD | mj2x E2E 实际消费的 header-only 子集;保持 `mtest_set/src/` 路径使 `$(projectdir)/mtest_set/src` include 不变。`.java` 样本源、C++ 生成器 exe、unittest 等未带(E2E 全走 inline 生成器产 .class bytes) |

## 仓内路径修复(拆出后)

测试代码原有 `current_path() / "mj2x" / "mj2x-rt"` 形态的 monorepo 相对路径,
拆出后改为仓根直取(11 处:backend e2e ×4、cli_test、io_test、
vmc4_conversion_test 的 kFixtureSrc、perf regression);CLI 生产代码
`findCrtRoot` 的 monorepo 布局候选(`cwd/mj2x/mj2x-rt`)保留——同一 exe 在
monorepo 检出内仍可用,`MJ2X_CRT_DIR` 可显式覆盖。

行尾契约恢复:`.gitattributes` 原在 monorepo 根未随拆迁移,缺失时全局
`core.autocrlf=true` 把文本检出成 CRLF,native 工程生成物(LF)与 golden
基线的逐字节比对失败(gitignore/xmake.lua/README golden),已按主仓原样恢复。

已知文案性残留(不咬合任何基线,暂不改):samples/protected/llvm 测试生成的
报告头 `Generated from \`mj2x/test_resource/...\`` 仍带 monorepo 前缀,属
来源注记文字。

## 后续待办

- **mtest_set 包化**:上游把 manifest/生成器头发版进 luiox-repo 后,删仓内
  `mtest_set/src` vendor 改包消费。
- **CI**:独立仓流水线待建(Windows MSVC + 三后端矩阵 + JDK/clang 依赖面)。
- **远程托管**:git remote 由仓库所有者后定(拆出仓未配 remote)。
- **跨仓引用残留**:README 引用的 DLL 加壳需求文档在 morpher 主仓
  `doc_ai/dll_packer_requirements.md`(未随拆迁移);`benchmark/` 依赖的
  jvm-obf-tester 外部样本仓为环境依赖,非仓内文件。
- **tag 语义**:随历史保留的 v2.x tag 是 monorepo 全线发版标记,在独立仓语境下
  语义需重新约定(或清理)。

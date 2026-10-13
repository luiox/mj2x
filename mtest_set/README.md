# mtest_set（vendored 子集）

本目录是 morpher monorepo `mtest_set/`（JVM 测试样本生成系统）中 **mj2x E2E 实际
消费的 header-only 子集**，自拆仓基线（见根 [MIGRATION.md](../MIGRATION.md)）等价
拷贝，保持 `mtest_set/src/` 路径使 `$(projectdir)/mtest_set/src` include 不变。

- `src/manifest/` — native/semantic fixture manifest + core registry（mj2x 的
  StackC native 转换矩阵与三后端语义矩阵消费）
- `src/gen/` — 样本生成器头（inline，经 libjbytecode 包在内存构造 .class bytes，
  不依赖 JDK / javac）

未携带的部分：`.java` 样本源、C++ 生成器 exe（`mtest_set_gen`）、sample CLI、
mtest_set 自身 unittest、bench——mj2x 测试全走 inline 生成器，无需这些。

同步纪律：只做与上游的等价同步，不做功能性新增；上游把 manifest/生成器头发版进
luiox-repo 后本目录整体退场改包消费。

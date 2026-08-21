-- mj2x - Morpher Java Bytecode to X
-- 聚合入口：各子模块（core/cli/runtime/unittest）拆到独立目录，对齐
-- libs/libjbytecode/<module>/ 的子库模式。每个子模块自带 xmake.lua。
includes("mj2x-core")
includes("mj2x-cli")
includes("mj2x-rt")
includes("mj2x-runtime")
includes("unittest")

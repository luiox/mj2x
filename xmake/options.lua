-- 集中定义 mj2x 独立仓的构建开关。
--
-- 命令行用下划线形式：xmake f --with_tests=y 等。
-- 根 xmake.lua 在其他 includes 之前 `includes("xmake/options.lua")`，
-- 之后用 has_config("with_xxx") 判断分支。

-- 启用所有 *_unittest / *_integration_test target。gtest 仅在开启时才被
-- add_requires。monorepo 时期的 --with_mj2x 总开关已随拆仓退场（整仓即
-- mj2x），后端面开关保留。
option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable mj2x_unittest / mj2x_integration_test targets (pulls gtest via xrepo).")

-- mj2x LLVM IR 后端开关。v2.1 曾按发布范围关闭（覆盖面窄：仅静态纯计算/控制流，
-- 对象/JNI/异常/monitor/内存操作全 fail-closed）；v2.2 转正默认开启（基础款）。
-- 需要收紧发布面时可 `xmake f --with_mj2x_llvm=n` 关闭（CLI fail-closed）。
option("with_mj2x_llvm")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the mj2x LLVM IR backend (scalar/control-flow slice; default on since v2.2).")

-- mj2x 单后端编译选择器（morpher issue #886）。all=全后端（默认，行为不变）；
-- stackc|regc|vmc4|llvm=仅编入该后端：被排除后端的源码不参与编译、CLI 注册/
-- 分发面以 MJ2X_DISABLE_*_BACKEND 编译期裁剪、mj2x-rt 嵌入按分发面收缩。
-- llvm 选择器仍编译 regc intrinsic registry（JDK intrinsic 符号实现）。
-- 需要全量 mj2x-core 的测试 target（mj2x_unittest / mj2x_integration_test）
-- 在单后端构建下自动禁用；全量回归请在默认配置执行。
option("with_mj2x_backend")
    set_default("all")
    set_showmenu(true)
    set_values("all", "stackc", "regc", "vmc4", "llvm")
    -- 非法值的干净报错在 mj2x-core/xmake.lua 的文件级校验（set_values 在
    -- xmake 3.0.9 上不拦 CLI 传值——monorepo 评审 T3-3 勘实）。
    set_description("mj2x backend set: all | stackc | regc | vmc4 | llvm (single-backend build, #886).")

-- AddressSanitizer。MSVC 下 /fsanitize=address 会改变 STL 的 ABI（annotate_vector
-- 等注解），因此**必须在全局作用域开启**，让所有 target——包括被依赖的静态库
-- ——用同一套插桩，否则链接期报 LNK2038 不匹配。关优化 + 保留调试信息，否则
-- 报告里的调用栈不可读。
option("with_asan")
    set_default(false)
    set_showmenu(true)
    set_description("Build with AddressSanitizer (global; do not combine with gtest targets).")

-- mj2x unittest 子模块
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析
target("mj2x_fake_zig")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)
    add_files("src/cli/fake_zig.cpp")
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

target("mj2x_fake_xmake")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)
    add_files("src/cli/fake_xmake.cpp")
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

-- mj2x-cli-companion：纯构建依赖桩（phony 无产物、无链接副作用）。
-- unittest 经 test_helper 的 getBuildTargetPath("mj2x-cli") 定位伴生 CLI，该定位
-- 优先取测试 exe 自身所在的活动 buildir；若 mj2x-cli 不随 unittest 构建，自定义
-- buildir（xmake f -o build_xxx）下会命中缺失或陈旧的默认 buildir 产物。用 phony
-- 把 mj2x-cli 挂进依赖图，保证"构建 mj2x_unittest"必然在同一 buildir 内新建 CLI；
-- 不直接 add_deps("mj2x-cli") 是避免二进制依赖的链接面耦合（CLI 与测试无符号关系）。
target("mj2x-cli-companion")
    set_kind("phony")
    set_group("mj2x/test")
    set_default(false)
    add_deps("mj2x-cli")

target("mj2x_unittest")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)

    -- mj2x-cli-companion：伴生 CLI 必须在活动 buildir 内新建，防陈旧产物（见上）。
    add_deps("mj2x-core", "test_helper", "mj2x_fake_zig", "mj2x_fake_xmake", "libca_process",
             "mj2x-cli-companion")
    add_packages("gtest")
    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（Linux CI 与 Windows 同一口径）。
    -- mj2x-core → libjbytecode 聚合库（实现分散在子库），须逐个补齐；
    -- test_helper 自身依赖 libca_test/libjdk_finder，一并补入。
    add_links("mj2x-core", "libjbytecode_util", "libjbytecode_commons", "libjbytecode_analysis",
              "libjbytecode_tree", "libjbytecode_core", "libgarena",
              "libjstr", "libjzip", "libca_log", "libca_process",
              "test_helper", "libca_test", "libjdk_finder",
              "libca_zip", "libca_str", "libca_crypto", "libca_core")
    -- 注：libca_resources 为 headeronly target（无 .lib 产物），只入 add_deps 不入 add_links。
    add_files("src/main.cpp")
    add_files("src/**/*_test.cpp")
    remove_files("src/integration/**.cpp")
    add_includedirs("src")
    -- 与 mj2x-cli 对齐：LLVM 后端显式关闭时（with_mj2x_llvm=n），LLVM 用例跳过。
    if not has_config("with_mj2x_llvm") then
        add_defines("MJ2X_DISABLE_LLVM_BACKEND")
    end
    -- mtest_set 的 manifest（header-only）供 native 转换 E2E 消费样本，与 mbase 同模式。
    add_includedirs("$(projectdir)/mtest_set/src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

target("mj2x_integration_test")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)

    add_deps("mj2x-cli", "test_helper", "libjbytecode")
    add_packages("gtest")
    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（mj2x-cli 为 binary 依赖，只定构建顺序，
    -- 不参与链接，故此处闭包只覆盖 test_helper + libjbytecode 族）。
    add_links("libjbytecode_util", "libjbytecode_commons", "libjbytecode_analysis",
              "libjbytecode_tree", "libjbytecode_core", "libgarena", "libjstr",
              "test_helper", "libca_test", "libjdk_finder",
              "libca_str", "libca_crypto", "libca_core")
    add_files("src/integration/main.cpp")
    add_files("src/integration/*_test.cpp")
    add_includedirs("src")
    add_includedirs("$(projectdir)/mtest_set/src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

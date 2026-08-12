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

target("mj2x_unittest")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)

    add_deps("mj2x-core", "test_helper", "mj2x_fake_zig", "mj2x_fake_xmake", "libca_process")
    add_packages("gtest")
    add_files("src/main.cpp")
    add_files("src/**/*_test.cpp")
    remove_files("src/integration/**.cpp")
    add_includedirs("src")
    -- 与 mj2x-cli 对齐：LLVM 后端关闭时（with_mj2x_llvm=n），cli_test 的 LLVM 用例跳过。
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
    add_files("src/integration/main.cpp")
    add_files("src/integration/*_test.cpp")
    add_includedirs("src")
    add_includedirs("$(projectdir)/mtest_set/src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

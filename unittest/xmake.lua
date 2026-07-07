-- mj2x unittest 子模块
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析
target("mj2x_unittest")
    set_kind("binary")
    set_group("mj2x/test")
    set_default(false)

    add_deps("mj2x-core", "test_helper")
    add_packages("gtest")
    add_files("src/main.cpp")
    add_files("src/**/*_test.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

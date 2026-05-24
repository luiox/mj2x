-- mj2x - Morpher Java Bytecode to X
-- Phase 1: 项目骨架
--
target("mj2x-core")
    set_kind("static")
    set_group("mj2x")

    add_deps("libjbytecode", "libjstr")

    add_files("src/mj2x/**.cpp")
    add_headerfiles("src/mj2x/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    add_deps("mj2x-core")

    add_files("src/mj2x-cli/*.cpp")
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("mj2x_unittest")
    set_kind("binary")
    set_group("mj2x/test")

    add_deps("mj2x-core", "gmock")
    add_files("src/unittest/main.cpp")
    add_files("src/unittest/**/*_test.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

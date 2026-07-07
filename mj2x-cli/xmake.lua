-- mj2x-cli 子模块：命令行入口
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析
target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    add_deps("mj2x-core")

    add_files("src/*.cpp")
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

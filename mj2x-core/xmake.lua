-- mj2x-core 子模块：字节码翻译核心库
-- 对齐 libs/libjbytecode/<module>/ 的子库模式：
--   include 根 = src（即 mj2x-core/src），源码 include "mj2x/xxx.hpp" 零改动
target("mj2x-core")
    set_kind("static")
    set_group("mj2x")
    set_policy("build.merge_archive", true)

    add_deps("libtyped", "libjbytecode", "libjstr", "libjzip", "libca_log")

    add_files("src/mj2x/**.cpp")
    add_headerfiles("src/mj2x/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

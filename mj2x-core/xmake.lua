-- mj2x-core 子模块：字节码翻译核心库
-- 对齐 libs/libjbytecode/<module>/ 的子库模式：
--   include 根 = src（即 mj2x-core/src），源码 include "mj2x/xxx.hpp" 零改动
target("mj2x-core")
    set_kind("static")
    set_group("mj2x")
    set_policy("build.merge_archive", true)

    add_deps("libca_core", "libjbytecode", "libjstr", "libjzip", "libca_log", "libca_resources")

    add_files("src/mj2x/**.cpp")
    add_headerfiles("src/mj2x/**.hpp")
    add_includedirs("src", {public = true})
    -- 代码生成模板(#894):resources/codegen/*.c.in 经 resources.embed 嵌入,
    -- 生成头自注册;消费方经 resources::bundle("codegen") 读取。
    add_files("resources/all.resources", {rules = "libca.resources.embed"})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

-- mj2x-core 子模块：字节码翻译核心库
-- 对齐 libs/libjbytecode/<module>/ 的子库模式：
--   include 根 = src（即 mj2x-core/src），源码 include "mj2x/xxx.hpp" 零改动
target("mj2x-core")
    set_kind("static")
    set_group("mj2x")
    set_policy("build.merge_archive", true)

    add_deps("libca_core", "libjbytecode", "libjstr", "libjzip", "libca_log", "libca_resources")

    -- #886 单后端编译裁剪：with_mj2x_backend=all（默认）保持全量收编，行为
    -- 与历史一致；单后端构建只编入所选后端目录，被排除后端不进归档（CLI
    -- 分发面以 MJ2X_DISABLE_*_BACKEND 同步裁剪，见 mj2x-cli/xmake.lua）。
    local mj2x_backend = tostring(get_config("with_mj2x_backend") or "all")
    if mj2x_backend == "" then mj2x_backend = "all" end
    if mj2x_backend == "all" then
        add_files("src/mj2x/**.cpp")
    else
        -- 非后端目录与 backend/ 根共享文件全量；后端目录按选择器收编。
        add_files("src/mj2x/*.cpp")
        for _, dir in ipairs({"codegen", "core", "frontend", "io", "mir",
                              "postprocessor", "preprocessor"}) do
            add_files("src/mj2x/" .. dir .. "/**.cpp")
        end
        add_files("src/mj2x/backend/*.cpp")
        local backend_dir = {stackc = "stackc", regc = "regc", vmc4 = "vmc4", llvm = "llvmir"}
        add_files("src/mj2x/backend/" .. backend_dir[mj2x_backend] .. "/**.cpp")
        -- LLVM 依赖 regc 的 JDK intrinsic 注册表面（符号实现）：registry 与
        -- matcher 两文件（matcher 自包含，仅依赖自身头 + jbytecode）；
        -- RegCBackend 本体不收编，CLI 的 regc 面照常裁剪。
        if mj2x_backend == "llvm" then
            add_files("src/mj2x/backend/regc/regc_intrinsic_registry.cpp")
            add_files("src/mj2x/backend/regc/regc_intrinsic_matcher.cpp")
        end
    end
    add_headerfiles("src/mj2x/**.hpp")
    add_includedirs("src", {public = true})
    -- 代码生成模板(#894):resources/codegen/*.c.in 经 resources.embed 嵌入,
    -- 生成头自注册;消费方经 resources::bundle("codegen") 读取。
    add_files("resources/all.resources", {rules = "libca.resources.embed"})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

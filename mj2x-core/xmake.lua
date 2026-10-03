-- mj2x-core 子模块：字节码翻译核心库
-- 对齐 libs/libjbytecode/<module>/ 的子库模式：
--   include 根 = src（即 mj2x-core/src），源码 include "mj2x/xxx.hpp" 零改动

-- #886 后端选择器合法值校验：必须在文件顶 local function 里做——target 描述
-- 块的子沙箱无 raise/assert（实测 3.0.9 连 error/os.exit 都没有），而文件级
-- 函数环境与 mjdbg 各线先例一致可用 raise（评审 T3-3）。
local kMj2xBackendDirs = {stackc = "stackc", regc = "regc", vmc4 = "vmc4", llvm = "llvmir"}
-- 本仓 xmake 构建的工程脚本沙箱剥掉了全部报错原语（raise/assert/error/
-- os.exit 在文件作用域、target 描述块、on_load 三层实测全 nil，评审 T3-3
-- 勘实）；可用的失败面只有「print 明确原因 + 命名 nil 索引强制中止」——
-- 错误行自带诊断字段名，conf 在中止趟不落盘。
local function mj2x_backend_check()
    local backend = tostring(get_config("with_mj2x_backend") or "all")
    if backend == "" then backend = "all" end
    if backend ~= "all" and kMj2xBackendDirs[backend] == nil then
        print("error: with_mj2x_backend='" .. backend .. "' is invalid (allowed: all | stackc | regc | vmc4 | llvm)")
        -- 命名 local 携带诊断信息进中止错误行（本沙箱无 raise/assert 可用）
        local MJ2X_INVALID_WITH_MJ2X_BACKEND_VALUE = nil
        local _ = MJ2X_INVALID_WITH_MJ2X_BACKEND_VALUE.abort
    end
    return backend
end

target("mj2x-core")
    set_kind("static")
    set_group("mj2x")
    set_policy("build.merge_archive", true)

    add_deps("libca_core", "libjbytecode", "libjstr", "libjzip", "libca_log", "libca_resources")

    -- #886 单后端编译裁剪：with_mj2x_backend=all（默认）保持全量收编，行为
    -- 与历史一致；单后端构建只编入所选后端目录，被排除后端不进归档（CLI
    -- 分发面以 MJ2X_DISABLE_*_BACKEND 同步裁剪，见 mj2x-cli/xmake.lua）。
    local mj2x_backend = mj2x_backend_check()
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
        add_files("src/mj2x/backend/" .. kMj2xBackendDirs[mj2x_backend] .. "/**.cpp")
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

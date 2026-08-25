-- mj2x-cli 子模块：命令行入口
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析

target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    add_deps("mj2x-core", "libjdk_finder", "libca_process", "libca_opt", "libi18n", "libresources")
    add_packages("nlohmann_json")

    -- v2.2 起 LLVM IR 后端默认开启（with_mj2x_llvm=y，见 xmake/options.lua）。
    -- 显式关闭时 CLI 不暴露 --backend llvm / --emit-llvm / --llvm-tool（fail-closed）。
    if not has_config("with_mj2x_llvm") then
        add_defines("MJ2X_DISABLE_LLVM_BACKEND")
    end

    add_files("src/*.cpp")
    -- mj2x-rt 源包嵌入(#891):common/regc/templates 三挂载,生成头自注册,
    -- 生成头 resources_mj2x_cli.generated.hpp 由消费 TU 直接 include。
    add_files("../mj2x-rt/rt.resources", {rules = "resources.embed"})
    -- CLI 文案单源表(issue #888):zh_CN 唯一事实源,--lang/MJ2X_LANG 切换。
    add_files("translations/*.lang", {rules = "i18n.embed-lang"})
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        -- ComSpec 等 env 读取走 std::getenv（与 unittest 目标同豁免）
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

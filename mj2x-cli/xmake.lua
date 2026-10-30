-- mj2x-cli 子模块：命令行入口
-- include 根 = src；mj2x/ 头通过 add_deps("mj2x-core") 透传的 public includedir 解析

-- #886 单后端编译选择器读取（与 mj2x-core / unittest 同口径）。
local mj2x_backend = tostring(get_config("with_mj2x_backend") or "all")
if mj2x_backend == "" then mj2x_backend = "all" end

target("mj2x-cli")
    set_kind("binary")
    set_group("mj2x")

    -- #886 互斥校验：llvm 选择器要求 with_mj2x_llvm 保持开启（两开关同管
    -- llvm 面）。本仓 xmake 构建的脚本沙箱剥掉全部报错原语（描述作用域/
    -- on_load 实测 raise/assert/error/os.exit 全 nil，评审 T3-3 勘实）——
    -- 失败面用「print 明确原因 + 命名 nil 索引强制中止」，错误行自带诊断名。
    on_load(function (target)
        if tostring(get_config("with_mj2x_backend") or "all") == "llvm"
           and not has_config("with_mj2x_llvm") then
            print("error: with_mj2x_backend=llvm conflicts with with_mj2x_llvm=n (re-enable with_mj2x_llvm or pick another backend)")
            local MJ2X_LLVM_BACKEND_CONFLICT = nil
            local _ = MJ2X_LLVM_BACKEND_CONFLICT.abort
        end
    end)

    -- 独立仓接线：libca/libjdk_finder 走 luiox-repo 直连包（原 phony 垫片退场）。
    add_deps("mj2x-core")
    add_packages("libca", "libjdk_finder", "nlohmann_json")

    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（Linux CI 与 Windows 同一口径）。
    -- 批 6e：mj2x-core 已零 libjbytecode 符号引用，链接闭包随之摘除。
    -- 裸 libca_*/libjdk_finder 库名落到包 libdir 解析（add_packages 传导 libpath）。
    -- libca 0.0.16 起模块间出现跨模块符号引用（fs→core/str 等，"fs 读 stdio
    -- + SHA-3 重写"）：模块库全量入闭包且 str/crypto/core 垫后——仅靠
    -- add_packages 传导不保证 str/core 落在传导 fs 之后，GNU ld 单遍解析下
    -- mj2x-cli 链接期 undefined reference（Linux CI 实测）。
    mj2x_binary_link_closure("mj2x-core", "libjbc", "libjstr",
              "libjzip", "libjdk_finder",
              "libca_fs", "libca_json", "libca_net", "libca_http",
              "libca_io", "libca_thread", "libca_env",
              "libca_log", "libca_process", "libca_opt", "libca_zip",
              "libca_str", "libca_crypto", "libca_core")
    -- 注：libca_resources/libca_i18n 为 headeronly target（无 .lib 产物），
    -- 只入 add_deps 不入 add_links。

    -- v2.2 起 LLVM IR 后端默认开启（with_mj2x_llvm=y，见 xmake/options.lua）。
    -- 显式关闭、或被 #886 单后端选择器排除时，CLI 不暴露 llvm 后端面（fail-closed）。
    local llvm_enabled = has_config("with_mj2x_llvm") and
                         (mj2x_backend == "all" or mj2x_backend == "llvm")
    if not llvm_enabled then
        add_defines("MJ2X_DISABLE_LLVM_BACKEND")
    end
    -- #886 单后端裁剪面：被排除后端在注册/分发侧编译期消失（不是运行时空转）。
    if mj2x_backend ~= "all" then
        if mj2x_backend ~= "stackc" then add_defines("MJ2X_DISABLE_STACKC_BACKEND") end
        if mj2x_backend ~= "regc" then add_defines("MJ2X_DISABLE_REGC_BACKEND") end
        if mj2x_backend ~= "vmc4" then add_defines("MJ2X_DISABLE_VMC4_BACKEND") end
    end

    add_files("src/*.cpp")
    -- mj2x-rt 源包嵌入(#891):common/regc/templates 三挂载,生成头自注册,
    -- 生成头 resources_mj2x_cli.generated.hpp 由消费 TU 直接 include。
    -- #886: 单后端构建按 embedded_rt.hpp 的分发面收缩嵌入挂载（stackc-only
    -- 产物不含 regc CRT 等），经共享 rule 的 target 侧可选过滤声明；默认
    -- （all）不设值，全量嵌入行为与历史一致。
    add_files("../mj2x-rt/rt.resources", {rules = "libca.resources.embed"})
    if mj2x_backend ~= "all" then
        local rt_mounts = {
            stackc = "stackc,templates",
            regc   = "common,regc,templates",
            vmc4   = "common,vmc4,templates",
            llvm   = "common,regc,llvmir,templates",
        }
        set_values("resources.embed.only", rt_mounts[mj2x_backend])
    end
    -- CLI 文案单源表(issue #888):zh_CN 唯一事实源,--lang/MJ2X_LANG 切换。
    add_files("translations/*.lang", {rules = "libca.i18n.embed-lang"})
    add_includedirs("src")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
        -- ComSpec 等 env 读取走 std::getenv（与 unittest 目标同豁免）
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end

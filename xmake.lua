-- mj2x - Bytecode to Native Compiler（独立仓根构建入口）
--
-- 自 morpher monorepo 拆出（见 MIGRATION.md）：本仓即整条 mj2x 产品线，
-- 外部库全部走 luiox-repo 中心包仓直连包（接线基准照抄 mjt 独立仓先例），
-- 仓内自带 test_helper 与 mtest_set 头文件子集（vendor 清单见 MIGRATION.md）。
--
-- 各子模块（core/cli/rt/unittest）沿用 monorepo 时期的目录拆分，每个子模块
-- 自带 xmake.lua，只改依赖来源（包消费），目标定义保持原样。

-- 测试类 target 保 assert（R8 评审 N1 处方：测试 target 去 NDEBUG）。
--
-- 背景：release 模式下 xmake 内置 mode.release rule 会向每个 target 注入
-- -DNDEBUG——注意不是 defines 而是 cxflags（xmake rules/mode/xmake.lua 的
-- on_config 里 target:add("cxflags", "-DNDEBUG")，v3.0.9 实查），标准 C
-- assert 因此整体剥除：mj2x_rt_test 在 release 下 "all passed, exit 0"
-- 实际零向量验证（绿验空转，#1058 评审实锤）。产品 target 的 NDEBUG 必须保留
-- （release 优化语义不动），只有测试类 target 在这里摘掉它。
--
-- 为什么挂 on_config 而不是 remove_defines：NDEBUG 不在 defines 列表里，
-- remove_defines 无效；且 target:remove("cxflags", ...) 不是注册 API
-- （v3.0.9 的 target remove API 只覆盖 files 类，实测 raise unknown api）。
-- xmake 派发顺序为「各 rule on_config → target 自身 on_config → 各 rule
-- after_config」，故 target 级 on_config 晚于 mode.release 注入，在此
-- get + set 过滤即可摘掉刚注入的值（target:get 只取 target 自身值，不混入
-- dep/package 来源，set 不会产生重复/串源）。
-- 覆盖面：release/releasedbg/minsizerel/profile 四种模式 xmake 均注入 NDEBUG，
-- 一并移除（debug 等其余模式本来就不注入，过滤为无害空操作）。
-- 注（R15 评审 N1）：本仓根 xmake.lua 只挂 mode.debug/mode.release 两规则，
-- 四态中实际可达的仅 release，其余分支为防御性空转（releasedbg 实测产品库
-- 亦无 NDEBUG），保留以防后续根规则扩展。
function mj2x_test_keep_asserts()
    on_config(function (target)
        if is_mode("release", "releasedbg", "minsizerel", "profile") then
            for _, name in ipairs({"cxflags", "cflags", "cxxflags"}) do
                local values = table.wrap(target:get(name))
                if #values > 0 then
                    local filtered = {}
                    for _, value in ipairs(values) do
                        if value ~= "-DNDEBUG" then
                            table.insert(filtered, value)
                        end
                    end
                    target:set(name, table.unpack(filtered))
                end
            end
            -- 兜底：部分 xmake 版本走 defines 注入（值不带 -D 前缀）。
            local defines = table.wrap(target:get("defines"))
            if #defines > 0 then
                local filtered = {}
                for _, value in ipairs(defines) do
                    if value ~= "NDEBUG" then
                        table.insert(filtered, value)
                    end
                end
                target:set("defines", table.unpack(filtered))
            end
        end
    end)
end

add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

set_project("mj2x")
-- 版本号即发版 tag（v2.3 …），与 git-cliff changelog 同源；发版见 AGENTS.md。
set_version("2.3")
set_xmakever("2.9.3")
set_allowedplats("windows", "linux", "macosx")
set_defaultplat("windows")
set_defaultarchs("x64")

set_languages("c99", "cxx17")
add_rules("mode.debug", "mode.release")
set_warnings("all")

-- 全线静态 CRT（/MT，debug 档自动映射 /MTd）：CLI 以 subprocess 拉起 javac/
-- clang，测试 exe 以 jdk_fixture 拉起 java——与宿主进程混链旧 CRT 是
-- LNK2038/AV 根因族，静态链接后不再依赖外部 CRT 解析。xrepo 包按项目
-- runtime 自动以 MT 口味重装。Linux/macOS 不受此设置影响。
if is_plat("windows") then
    set_runtimes("MT")
end

-- 日志门面编译期级别=Trace：CA_LOG_DEBUG/TRACE 一律编入，可见性由 Logger
-- 运行期级别与各调用点既有门控负责。
add_defines("CA_COMPILE_LOG_LEVEL=0")

-- 测试/工具 binary 的静态库链接闭包（add_links + linux 闭包组）。
--
-- 背景：xmake 3.x 不把静态 target 依赖传进 binary 链接，须显式给全闭包；
-- GNU ld 单遍从左到右扫描归档，闭包列表必须消费库在前、被依赖库在后；
-- MSVC link.exe 对库顺序不敏感。声明序即闭包序（xmake 3.0.9 实证）。
-- 包依赖（libca/libjbytecode 族）的 libpath/liba 由 add_packages 传导，
-- 闭包里的裸库名落到包 libdir 上解析（monorepo 时期同名 phony 垫片的
-- 等价物）。
function mj2x_binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

-- 禁用 RTTI：dynamic_cast 替换为 tag_cast/cast（libca 约定）。
if is_plat("windows") then
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    -- 关掉 xmake 默认追加的 /EHsc：与 /EHa 同传会触发 MSVC D9025。
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

if is_plat("windows") then
    set_toolchains("msvc")
end

if is_host("windows") then
    set_plat("windows")
    set_toolchains("msvc")
end

-- 构建开关：必须最先 include，后续用 has_config("with_xxx") 判断。
-- 命令行用下划线形式：xmake f --with_tests=y 等。
includes("xmake/options.lua")

-- AddressSanitizer：MSVC 下必须在全局作用域开启，让所有 target（含静态库）
-- 用同一套插桩，否则链接期 LNK2038。不要与 gtest targets 组合。
if has_config("with_asan") then
    set_optimize("none")
    set_symbols("debug")
    if is_plat("windows") then
        add_cxflags("/fsanitize=address", {tools = "cl"})
        add_defines("_DISABLE_VECTOR_ANNOTATION", "_DISABLE_STRING_ANNOTATION")
        add_ldflags("/INCREMENTAL:NO", {tools = "link"})
    else
        add_cxflags("-fsanitize=address", "-fno-omit-frame-pointer")
        add_ldflags("-fsanitize=address")
    end
end

-- gtest：仅在启用测试时通过 xrepo 拉取。
if has_config("with_tests") then
    add_requires("gtest")
end

add_requires("nlohmann_json 3.12.0")

-- 中心包仓直连包（无仓内副本、无 phony 垫片，消费点直接 add_packages）。
add_repositories("luiox-repo C:/sw/luiox-repo")
-- libarchive 本地覆盖仓（同 morpher/mjt）：官方 xmake-repo 包只在 windows 传
-- -DPOSIX_REGEX_LIB=NONE，linux/macOS 沙箱构建走系统正则探测分支会失败；
-- 覆盖包对所有平台传 NONE（mjbase 静态库闭包含 libarchive 符号，消费方须
-- 同版本同配置实例满足 final link）。
add_repositories("mj2x-repo xmake/repo")
-- libca 版本/模块双单源：顶层与全部嵌套路径一律归并到同一 (版本, 模块集)。
-- 家族包内声明的 libca 依赖无版本约束且是小模块超集（core,str,log,crypto,zip），
-- 放任自解析会多出实例；消费方目标同名包去重后落到无 json 的实例时，
-- 链接期 LNK2019 ca::json（morpher D33 同族根因）。升级 libca 只改下面两行。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
-- test 模块必须显式点名：test_helper 调用 ca::test::top_project_path()，
-- 缺模块时链接期 undefined reference；包 on_load 按依赖闭包展开排序链接项。
-- 嵌套 libca 通配钉定（morpher 终局口径）：一条 **.libca 覆盖全树任意深度，
-- 新包接入不再逐路径补条目；override 让包定义内已声明的配置也归并。
add_requireconfs("**.libca", {override = true, version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
-- 基础库统一超集升级（2026-10-29，随 mjbase 0.0.3 接入）：libjbc 0.4.15→0.4.23
-- （上游定义已自带 /utf-8 on_load 修正，项目内联定义整体退役）、
-- libjstr/libjzip/libgarena 0.0.2→0.0.3、libjdk_finder 0.0.2→0.1.0（snake_case
-- 破坏性 API）；libjbytecode 退役（libjbc 迁移完成后零代码消费方）。
-- 新增 mjbase 0.0.3（hierarchy/LCA 公共父类求解 + 帧查询面，帧换装底座）及其
-- 依赖闭包 libjstd/libjhasm/libjclassfile；libarchive/zlib 随 mjbase 链接闭包。
add_requires("libjbc 0.4.23")
add_requires("libjstr 0.0.4")
add_requires("libjzip 0.0.4")
add_requires("libgarena 0.0.4")
add_requires("libjclassfile 0.0.2")
add_requires("libjstd 0.0.1")
add_requires("libjhasm 0.3.2")
add_requires("libjdk_finder 0.1.0")
-- libjbytecode 0.0.2：unittest 测试夹具面仍在消费（jbytecode 树 API 构造
-- MethodNode/InsnNode 夹具）；产品面（core/cli/rt）已零消费。弃用注记在案，
-- 夹具迁 libjbc MethodEditor 后退役。
add_requires("libjbytecode 0.0.2")
add_requires("mjbase 0.0.3")
add_requires("zlib")
add_requires("libarchive 3.8.8", {configs = {openssl3 = false}})
-- i18n/resources 构建规则内嵌（.lang/.resources add_files 规则，包安装产物
-- 不带规则；来源 luiox/libca @ 1ab29dea，libca 升级若规则变更需同步）。
includes("xmake/libca_rules.lua")

includes("mj2x-core")
includes("mj2x-cli")
includes("mj2x-rt")
includes("test_helper")
includes("unittest")

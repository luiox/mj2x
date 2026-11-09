-- libjbc 包定义（项目内联定义，源自 luiox-repo 同名定义 + mjbase/mjt 同型）
--
-- 为什么项目内联（#44 曾整体退役本文件，本次随帧换装接线恢复）：
-- mjbase 依赖闭包的 libjhasm 0.3.3 安装沙箱自带其仓内内联 libjbc 词条，
-- 条目上限 0.4.22（libjhasm main 已收编 0.4.24 条目但未发版）；libjhasm
-- 0.3.3 自身根 require 又要 libjbc >=0.4.23——沙箱内自相矛盾，外部传什么
-- 版本其内层词条不识别即按裸 ref clone（CI 实证 fatal: Remote branch
-- >=0.4.23/0.4.24 not found，与 luiox-repo 词条是否注册无关）。mjbase
-- 上游的 shipped 解法：消费方带内联 libjbc 定义承载 0.4.22 条目，
-- libjhasm.libjbc 边显式钉 0.4.22（mjbase root xmake.lua 同款 requireconf），
-- 内层词条识别该版本即按词条 commit 锁正常安装。0.4.22→0.4.24 无 API 差
-- （0.4.23 纯文档+测试面，0.4.24 纯 libca 依赖接线），mjbase 闭包内
-- libjbc 0.4.22 与顶层同版本单实例。
-- 待 libjhasm 发含 0.4.24 内联词条的新版本后：闭包三处钉定（见 root
-- xmake.lua mjbase.libjbc/libjhasm.libjbc）翻 0.4.24，本文件再整体退役。
--
-- 其余纪律同 mjt/mjbase 同名文件：上游 on_load 的 /utf-8 修正 luiox-repo
-- 定义已自带（0.4.16+），本文件保留；libca 依赖直接钉本仓单源 (版本, 模块集)
-- （项目内联包定义不吃 requireconfs（实测），裸声明会浮动到仓库最新版，
-- 与项目实例形成双版本 libca——D33 同族隐患）。版本/模块常量取 root
-- xmake.lua 全局（本文件在常量定义之后 include，作用域可见）。
--
-- 机制：add_urls 指向 git 仓库 + add_versions 第二参数锁 commit。xmake 用
-- 系统 git 克隆并 checkout 到锁定 commit，随后在本包沙箱内独立构建安装
-- （不进消费工程的 target 图）。
--
-- 发版/升级：在 libjbc 仓库定版后，于此文件 add_versions 追加新版本条目
-- （既有条目只追加不修改）；消费项目自行择机升级。
--
-- 链接说明：整包单库（links = libjbc）。libjbc 工程自身 xmake.lua 已声明
-- libca 依赖（modules = core,str,log,crypto,zip），包沙箱内自解析；下方
-- add_deps 是给消费方的传递声明（另一回事，不可省）。libca 版本/模块取
-- root xmake.lua 的 LUIOX_LIBCA_VERSION/LUIOX_LIBCA_MODULES 家族统一超集
-- （同一消费图内各包的 libca (版本, 模块集) 必须完全一致——实例身份 =
-- (版本, configs 哈希)，任何偏差都会多出一个 libca 实例：链接闭包多条
-- -libpath，同名符号双归档，解析次序脆弱）。
--
-- 许可：MIT（源仓 v0.4.12 起 LICENSE 落地）。

package("libjbc")
    set_homepage("https://github.com/luiox/libjbc")
    set_description("node-first JVM bytecode library: lazy classfile DOM with fidelity write-back (git direct package)")
    set_license("MIT")

    add_urls("https://github.com/luiox/libjbc.git")

    -- 语义版本名 + 锁定 commit。版本条目只追加、不修改。
    -- 0.1.0 = 首个独立仓基线（core/ir/analysis/type 四层，MIT 预期未定版）。
    add_versions("0.1.0", "78e8ad8ffd7e8c610e8af1123371c46ee3988318") -- v0.1.0 基线  2026-10-14
    -- 0.4.12 = typed 属性面 13 种 + CP 压缩 + 成员级增删/adopt + 子程序 strict 帧；MIT 定版。
    add_versions("0.4.12", "106cf8302255d18c84b247888724c29bd764a084") -- 重指 GCC 修正版（tag v0.4.12 同 commit；原 a7b12f0 MSVC-only） -- v0.4.12 快照  2026-10-15
    -- 0.4.14 = Nest/Record 属性 typed 建模 + 嵌套属性表 CP 重映射 + nest/record 语料。
    add_versions("0.4.14", "8a00e45613254ba7811b6f196e45afc98c435a9f") -- v0.4.14 快照  2026-10-15
    -- 0.4.15 = Module 族属性 typed 建模 + 压缩封闭集收编；首版测试树完整入库（此前 15+2 文件被忽略规则吞）。
    add_versions("0.4.15", "9f1a13ec3df96f3493703ee3c6fbb7c5186187bf") -- v0.4.15 快照  2026-10-16
    -- 0.4.16 = 类构建挂载面（typed 属性合成 attach 家族）+ BSM 语义 intern 家族 + docs 全量归位。
    add_versions("0.4.16", "edce61d83be812b933731142b811d82c347b3fc0") -- v0.4.16 发版 main 头  2026-10-17
    -- 0.4.17 = consumer 模块移除（协议面归上层仓单份持有）。
    add_versions("0.4.17", "2a44a3749d4cbca04a6221c37b78a56d4620d254") -- v0.4.17 发版（tag 同 commit）  2026-10-20
    -- 0.4.18 = MethodEditor（从零建方法门面，PR #9）+ MethodParameters 接收者行放行（PR #11，main 合入头）。
    add_versions("0.4.18", "ada334a12ec875ca9769bd5d4cb1f7e8494d2ebd") -- PR #11 合入 main 头  2026-10-26
    -- 0.4.19 = CodeDom/CodeEditor move-range 指令区间整体搬移原语（PR #12）+ self-version 对齐（PR #13，main 合入头）。
    add_versions("0.4.19", "81173a0f724f9d0369fedc1d5f95ccd14fe984a4") -- PR #13 合入 main 头  2026-10-28
    -- 0.4.20 = cp_resolve CP 读侧语义解析原语三件：resolveMemberRef / resolveHandleCp / resolveClassName（PR #14）+ 版本对齐（PR #15，main 合入头）。
    add_versions("0.4.20", "16ca1076ea9e9bfc7aaed2debab9b15a248d0e56") -- PR #15 合入 main 头  2026-10-28
    -- 0.4.21 = 域引擎基础档处理器种子改待并参与者（不再抢先单跑），fake-try 假阳性 Underflow 消除（PR #16）+ 版本对齐（PR #17，main 合入头）。
    add_versions("0.4.21", "4a782744447a862593218412c64b2fb9a7ea9967") -- PR #17 合入 main 头  2026-10-28
    -- 0.4.22 = 精确档异常边发射域放宽到范围内可达指令（JVM 保守口径，PR #18）+ 版本对齐（PR #19，main 合入头）。
    add_versions("0.4.22", "a408506203aaba7f74cdf588885187d3a49b0bd0") -- PR #19 合入 main 头  2026-10-28
    -- 0.4.23 = libjbc 仓瘦身（其开发计划与 ergonomics 文档撤离，design.md 现状化）+ class_build_attrs_test 类构建挂载面契约收编（PR #20，main 合入头）；纯文档+测试面。
    add_versions("0.4.23", "4228cf3650b2409a106511a3cf8ca7275ed5ed04") -- PR #21 合入 main 头  2026-10-29
    -- 0.4.24 = 依赖统一（PR #22）：libca 0.0.13 → 0.0.16（modules 超集不变；单测 505 例 A/B 与 0.0.13 基线逐例一致）。
    add_versions("0.4.24", "f14fc9ab0ac675ba27552111ee07f6c9c9f1d763") -- PR #22 合入 main 头  2026-10-30

    -- 嵌套 libca 直接钉到本仓单源版本：项目内联包定义不吃 deps.lua 的
    -- **.libca / libjbc.libca requireconfs（实测），裸声明会解析到仓库最新版
    -- （0.0.15），与项目实例（LUIOX_LIBCA_VERSION）形成双版本 libca——D33
    -- 同族隐患。版本/模块常量取 root xmake.lua 全局（本文件在常量定义之后
    -- include，作用域可见）；configs 必须与项目级 add_requires 完全一致，
    -- 否则同一消费图内多出一份 libca 实例（链接闭包多条 -libpath）。
    add_deps("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})

    on_load(function (package)
        package:add("links", "libjbc")
        -- 语言标准传导：公开头使用 C++17（同 libjstr/libjclassfile 包口径）。
        if package:is_plat("windows") then
            package:add("cxxflags", "/std:c++17")
            -- 公开头含 UTF-8 中文注释，消费方编译期（含本包 on_test 探针）必须
            -- 按 UTF-8 读源，GBK ACP 下否则注释吞行致语法错（见文件头说明）。
            package:add("cxxflags", "/utf-8")
        else
            package:add("cxxflags", "-std=c++17")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        -- 包安装只装库本体：unittest/demo 均为开发态目标。
        import("package.tools.xmake").install(package, {with_tests = false})
        -- v0.1.0 布局归位：工程 headerfiles 模式 src/jbc/(**.hpp) 的括号组把
        -- jbc/ 前缀剥掉了，沙箱产物是 include/{jbc.hpp,core/,ir/,...} 平铺
        -- 布局，公开头的 #include "jbc/..." 解不到。这里整体搬回 include/jbc/
        -- 下；include/jbc 已存在（上游未来修正了布局）则跳过。附带摘掉误入
        -- 安装面的 test_helper（开发态测试基建，非交付物）。
        local includedir = path.join(package:installdir(), "include")
        if not os.isdir(path.join(includedir, "jbc")) then
            local staged = path.join(includedir, "jbc")
            os.mkdir(staged)
            for _, entry in ipairs({"jbc.hpp", "analysis", "core", "ir", "type"}) do
                os.mv(path.join(includedir, entry), path.join(staged, entry))
            end
        end
        os.tryrm(path.join(package:installdir(), "lib", "test_helper.lib"))
        os.tryrm(path.join(package:installdir(), "lib", "libtest_helper.a"))
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("jbc/jbc.hpp", {configs = {languages = "cxx17"}}))
        -- 链接级自检：get_lib_version 强制解析 libjbc + libca 的符号链
        -- （缺任一环即在此暴露）。
        assert(package:check_cxxsnippets({test = [[
            #include "jbc/jbc.hpp"
            int main(int argc, char** argv) {
                return jbc::get_lib_version() != nullptr && argc >= 0 ? 0 : 1;
            }
        ]]}, {configs = {languages = "cxx17"}}))
    end)
package_end()

#pragma once

// test_helper：morpher 测试基建门面。
//
// 通用的多项目测试布局机制（.project_root_file 扫描 / 资源定位 / 输出根约定）
// 已下沉到 libca/test（#912/#913），本头文件保持 morpher 侧历史 API 形态
// （namespace test::、camelCase），内部委托 ca::test 实现——既有测试 main
// 无需任何改动。
//
// morpher 私有件（不下沉）：
// - asm-test 特例定位（libjbytecode/asm-test 目录布局胶水）
// - 构建产物路径（build/artifacts 布局属产品构建知识）
// - jdk_fixture.hpp（架构在闭源 libjdk_finder 之上，另见该文件）
//
// 使用约定不变：测试 target 须 set_rundir("$(projectdir)")，
// main 里先 test::setup("<子项目名>")。

#include <cstdint>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "libca/test/test.hpp"

#include "test_helper_dom_sync.hpp"

namespace test {

inline void setup(const std::string& subproject)
{
    ca::test::setup(subproject);
}

inline std::filesystem::path getTopProjectPath()
{
    return ca::test::top_project_path();
}
inline std::filesystem::path getCurrentProjectPath()
{
    return ca::test::current_project_path();
}

inline bool                  hasResource(const std::string& filename);
inline std::filesystem::path getResourcePath(const std::string& filename);
inline std::vector<uint8_t>  getResource(const std::string& filename);

bool                  hasAsmTestResource(const std::string& filename);
std::filesystem::path getAsmTestResourcePath(const std::string& filename);
std::vector<uint8_t>  getAsmTestResource(const std::string& filename);

/// 跨项目定位：按 .project_root_file 名字（如 "mtest_set" / "mj2x"）取项目根 / 资源。
/// 统一样本供给的底层原语——任何子项目按名字拿另一子项目的 test_resource 文件，
/// 无需硬编码相对路径。未注册名字回落 <top>/<name>。
inline std::filesystem::path getProjectPath(const std::string& projectName)
{
    return ca::test::project_path(projectName);
}
inline std::filesystem::path getProjectResourcePath(const std::string& projectName,
                                                    const std::string& relPath)
{
    return ca::test::project_resource_path(projectName, relPath);
}
inline bool hasProjectResource(const std::string& projectName, const std::string& relPath)
{
    return ca::test::has_project_resource(projectName, relPath);
}
inline std::vector<uint8_t> getProjectResource(const std::string& projectName,
                                               const std::string& relPath)
{
    return ca::test::project_resource(projectName, relPath);
}

inline std::filesystem::path getDemoOutPath(const std::string& filename)
{
    return ca::test::demo_out_path(filename);
}
inline std::filesystem::path getOutPath(const std::string& filename)
{
    return ca::test::out_path(filename);
}
inline std::filesystem::path getTempOutPath(const std::string& filename)
{
    return ca::test::temp_out_path(filename);
}

std::string           getBuildModeName();

/// 定位同 buildir 的伴生构建产物（如 mj2x-cli）。优先按测试可执行文件自身位置
/// 推导活动 buildir——xmake binary 目标同树同目录输出，自定义 `-o <dir>` 配置下
/// 依然正确；目标未随测试同树构建（文件不存在）时回退默认 buildir 布局
/// <top>/build/<plat>/<arch>/<mode>/。@p targetName 可带 .dll 后缀定位 shared
/// 目标产物（如 "mjt-obf-runtime-native.dll"）。
std::filesystem::path getBuildTargetPath(const std::string& targetName);
/// 定位同 buildir 的 artifacts 层伴生产物
/// <buildir>/artifacts/<plat>/<arch>/<mode>/<line>/<name>。活动 buildir 优先
/// （同 getBuildTargetPath，exe 在 binary/artifacts 层均可上溯定位），产物不存在
/// 时回退默认 buildir 布局 <top>/build/artifacts/...。
std::filesystem::path getArtifactTargetPath(const std::string& line, const std::string& targetName);

// ---- 当前项目资源：委托 ca::test ----

inline std::filesystem::path getResourcePath(const std::string& filename)
{
    return ca::test::resource_path(filename);
}
inline bool hasResource(const std::string& filename)
{
    return ca::test::has_resource(filename);
}
inline std::vector<uint8_t> getResource(const std::string& filename)
{
    return ca::test::resource(filename);
}

// ---- 批 6d DOM 权威 pass 的测试三明治 ----
//
// 树载体退役后，夹具建模 = mj2x::test::classFromTree（树 → DOM + 快照对齐），
// 树断言 = mj2x::test::treeFromDom 回投影。三明治退化为「DOM 权威执行 pass +
// 快照对齐」：缺 dom 的夹具类直接报错（迁移漏网的裸 CompilerClass 夹具）。

/// DOM 权威 pre/postprocessor 的测试适配：执行 pass 后把 methods/fields 元数据
/// 快照与 DOM 对齐（markedForCompile/backendResult 等管线旗标按 name+desc 锚定
/// 迁移）。指令/常量池断言经 treeFromDom 回投影现查。
template <typename Pass>
void runWithDomSync(Pass&& pass, mj2x::CompilerIndex& index)
{
    for (auto& entry : index.getClasses()) {
        if (entry.second.dom == nullptr) {
            throw std::runtime_error("runWithDomSync: class without dom (fixture must go "
                                     "through test::classFromTree): "
                                     + entry.first);
        }
    }
    pass.process(index);
    mj2x::refreshSnapshots(index);
}

/// 带 report 收集参数的版本（InvokeDynamicPreprocessor::process(index, report*)）。
template <typename Pass, typename Report>
void runWithDomSync(Pass&& pass, mj2x::CompilerIndex& index, Report* report)
{
    for (auto& entry : index.getClasses()) {
        if (entry.second.dom == nullptr) {
            throw std::runtime_error("runWithDomSync: class without dom (fixture must go "
                                     "through test::classFromTree): "
                                     + entry.first);
        }
    }
    pass.process(index, report);
    mj2x::refreshSnapshots(index);
}

// ---- 批 5 backend 注入（injectIntoClass 走 DOM）的测试适配 ----

/// 注入前守卫：批 6d 后树载体退役，夹具经 classFromTree 落 DOM，本函数退化为
/// 缺守卫（dom 已在则零操作；裸 CompilerClass 即迁移漏网，报错暴露）。
inline void ensureDomFor(mj2x::CompilerClass& cls, const std::string& name)
{
    if (cls.dom != nullptr) return;
    throw std::runtime_error("ensureDomFor: class without dom (fixture must go through "
                             "test::classFromTree): "
                             + name);
}

/// 注入后刷新：树槽已随批 6d 退役，本函数退化为快照对齐（methods/fields 元数据
/// 与 DOM 对齐）。指令/成员断言一律经 treeFromDom 回投影现查或读快照字段。
inline void refreshTreeFromDom(mj2x::CompilerClass& cls, const std::string& /*name*/)
{
    mj2x::refreshMethodSnapshots(cls);
}

}   // namespace test

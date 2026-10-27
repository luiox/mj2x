#include "pipeline.hpp"
#include "mj2x/io/input_loader.hpp"
#include "mj2x/core/class_dom_sync.hpp"
#include "mj2x/io/output_writer.hpp"

#include "libca/log/log_macros.hpp"

#include <cstdio>
#include <string>
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {

Pipeline::Pipeline(PipelineConfig config)
    : config_(std::move(config))
{}

Pipeline::~Pipeline() = default;

void Pipeline::registerBackend(std::unique_ptr<Backend> backend)
{
    backends_.push_back(std::move(backend));
}

void Pipeline::registerPreprocessor(std::unique_ptr<Preprocessor> preproc)
{
    preprocs_.push_back(std::move(preproc));
}

void Pipeline::registerPostprocessor(std::unique_ptr<Postprocessor> postproc)
{
    postprocs_.push_back(std::move(postproc));
}

int Pipeline::run()
{
    CA_LOGT_INFO("pipeline", "Pipeline start (input={}, output={})\n",
                 config_.inputPath.c_str(), config_.outputPath.c_str());

    try {
        loadInput();
    }
    catch (const std::exception& e) {
        CA_LOGT_ERROR("pipeline", "load failed - {}\n", e.what());
        return 1;
    }

    // 中段（预处理器/编译/后处理器）与 IO 同级防护：CLI main 不兜异常，
    // 任何 preprocess/compile/inject 路径抛出直接 std::terminate。
    try {
        runPreprocessors();      // 批 4：preprocessor 直接改 DOM（改写权威）
        syncDomToTree(*index_);  // DOM → 树刷新：backend 树读（stackc hidden 编译等）的兼容投影
        compileMethods();        // 读 DOM；批 5 起 inject 也改 DOM（dom_inject 门面）
        runPostprocessors();     // 批 4：postprocessor 直接改 DOM（inject 后无需再树→DOM）
        // 批 6：output 改读 DOM（writeClass 直出 + 帧垫片），编译后 DOM→tree
        // 同步点摘除；树权威的生成类在 output 内经 ensureDomFromTree 落位。
    }
    catch (const std::exception& e) {
        CA_LOGT_ERROR("pipeline", "compile stage failed - {}\n", e.what());
        return 1;
    }

    try {
        writeOutput();
    }
    catch (const std::exception& e) {
        CA_LOGT_ERROR("pipeline", "write failed - {}\n", e.what());
        return 1;
    }

    CA_LOGT_INFO("pipeline", "Pipeline done\n");
    return 0;
}

void Pipeline::loadInput()
{
    InputLoader loader;
    index_ = loader.load(config_.inputPath);
}

void Pipeline::runPreprocessors()
{
    // 批 4 双载体拆段：树载体 pass（invokedynamic，批 4b 收尾）先跑，树改动
    // 经 syncTreeToDom 落 DOM（顺带为 lambda 合成类补 DOM 载体）；DOM 载体
    // pass 随后在 DOM 权威上改写。无序依赖：树/DOM pass 的改写产物互不可见
    //（indy 展开产 LDC String/Class，不被 LdcPreprocessor 二次处理；反向亦然）。
    for (auto& p : preprocs_) {
        if (!p->editsDom()) p->process(*index_);
    }
    syncTreeToDom(*index_);
    for (auto& p : preprocs_) {
        if (p->editsDom()) p->process(*index_);
    }
}

void Pipeline::compileMethods()
{
    if (backends_.empty()) return;

    // beforeCompile（issue #1193）：任何方法指令被清空前的唯一整输入时机，
    // 后端在此建立条目序无关的解析层（如 RegC 内联缓存）。
    for (auto& backend : backends_) backend->beforeCompile(*index_);

    CodeConverter converter(index_->arena());

    for (auto& [name, cls] : index_->getClasses()) {
        if (cls.isLibrary) continue;

        // 批 3b：读路径主入口走 DOM（批 2 同步产物）。同步保序 tree→DOM，
        // 再按 name+desc（类内唯一）锚定下标，双保险防御序漂移。
        if (cls.dom == nullptr) {
            throw std::runtime_error("pipeline: DOM not synced for " + name);
        }

        for (auto& method : cls.methods) {
            if (!method.markedForCompile) continue;

            // 批 5：inject 走 DOM（dom_inject 会话字节往返提交会替换 cls.dom），
            // dom 指针每方法重取；domIndex 同因重锚。
            if (cls.dom == nullptr) {
                throw std::runtime_error("pipeline: DOM consumed for " + name);
            }
            auto* dom = cls.dom.get();

            auto mnName = method.methodNode->name.to_std_string();
            auto mnDesc = method.methodNode->desc.to_std_string();
            CA_LOGT_INFO("pipeline", "  Compiling {}.{}{}\n", name.c_str(), mnName.c_str(), mnDesc.c_str());

            ca::usize domIndex = dom->methods.size();
            for (ca::usize i = 0; i < dom->methods.size(); ++i) {
                const auto& dm = dom->methods[i];
                if (dom->cp.utf8Equals(dm.nameCp, mnName) && dom->cp.utf8Equals(dm.descCp, mnDesc)) {
                    domIndex = i;
                    break;
                }
            }
            if (domIndex == dom->methods.size()) {
                throw std::runtime_error("pipeline: method not in DOM: " + name + "." + mnName);
            }

            auto blocks = converter.convert(*dom, domIndex);

            if (config_.dumpMIR) {
                printf("--- MIR dump ---\n%s\n", CodeConverter::dumpMIR(blocks).c_str());
            }

            for (auto& backend : backends_) {
                auto result = backend->compileMethod(name, method, blocks, index_.get());
                method.backendResult = result;
                backend->injectIntoClass(cls, method, result);
            }
        }
    }

    for (auto& backend : backends_) backend->finalize(*index_);
}

void Pipeline::runPostprocessors()
{
    for (auto& p : postprocs_) p->process(*index_);
}

void Pipeline::writeOutput()
{
    OutputWriter writer;
    writer.write(config_.outputPath, *index_);
}

}   // namespace mj2x

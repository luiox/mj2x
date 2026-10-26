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
        runPreprocessors();
        syncTreeToDom(*index_);   // 批 2：预处理树改动 → DOM（批 3 起供读方消费）
        compileMethods();
        syncTreeToDom(*index_);   // 批 2：backend 注入 → DOM
        runPostprocessors();
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
    for (auto& p : preprocs_) p->process(*index_);
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
        auto* dom = cls.dom.get();

        for (auto& method : cls.methods) {
            if (!method.markedForCompile) continue;

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

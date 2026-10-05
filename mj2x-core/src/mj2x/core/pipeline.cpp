#include "pipeline.hpp"
#include "mj2x/io/input_loader.hpp"
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
        compileMethods();
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

        for (auto& method : cls.methods) {
            if (!method.markedForCompile) continue;

            auto mnName = method.methodNode->name.to_std_string();
            auto mnDesc = method.methodNode->desc.to_std_string();
            CA_LOGT_INFO("pipeline", "  Compiling {}.{}{}\n", name.c_str(), mnName.c_str(), mnDesc.c_str());

            auto blocks = converter.convert(*method.methodNode);

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

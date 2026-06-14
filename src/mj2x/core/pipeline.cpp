#include "pipeline.hpp"
#include "mj2x/io/input_loader.hpp"
#include "mj2x/io/output_writer.hpp"

#include <cstdio>
#include <string>
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"

namespace {

std::string toStr(ca::str::Utf8StringRef ref) {
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

} // anonymous namespace

namespace mj2x {

Pipeline::Pipeline(PipelineConfig config)
    : config_(std::move(config))
{}

Pipeline::~Pipeline() = default;

void Pipeline::registerBackend(std::unique_ptr<Backend> backend) {
    backends_.push_back(std::move(backend));
}

void Pipeline::registerPreprocessor(std::unique_ptr<Preprocessor> preproc) {
    preprocs_.push_back(std::move(preproc));
}

void Pipeline::registerPostprocessor(std::unique_ptr<Postprocessor> postproc) {
    postprocs_.push_back(std::move(postproc));
}

int Pipeline::run() {
    printf("[mj2x] Pipeline start (input=%s, output=%s)\n",
           config_.inputPath.c_str(), config_.outputPath.c_str());

    try {
        loadInput();
    } catch (const std::exception& e) {
        printf("[mj2x] ERROR: load failed - %s\n", e.what());
        return 1;
    }

    runPreprocessors();
    compileMethods();
    runPostprocessors();

    try {
        writeOutput();
    } catch (const std::exception& e) {
        printf("[mj2x] ERROR: write failed - %s\n", e.what());
        return 1;
    }

    printf("[mj2x] Pipeline done\n");
    return 0;
}

void Pipeline::loadInput() {
    InputLoader loader;
    index_ = loader.load(config_.inputPath);
}

void Pipeline::runPreprocessors() {
    for (auto& p : preprocs_)
        p->process(*index_);
}

void Pipeline::compileMethods() {
    if (backends_.empty())
        return;

    CodeConverter converter;

    for (auto& [name, cls] : index_->getClasses()) {
        if (cls.isLibrary) continue;

        for (auto& method : cls.methods) {
            if (!method.markedForCompile) continue;

            auto mnName = toStr(method.methodNode->name);
            auto mnDesc = toStr(method.methodNode->desc);
            printf("  Compiling %s.%s%s\n",
                   name.c_str(), mnName.c_str(), mnDesc.c_str());

            auto blocks = converter.convert(*method.methodNode);

            if (config_.dumpMIR) {
                printf("--- MIR dump ---\n%s\n",
                       CodeConverter::dumpMIR(blocks).c_str());
            }

            for (auto& backend : backends_) {
                auto result = backend->compileMethod(name, method, blocks);
                method.backendResult = result;
                backend->injectIntoClass(cls, method, result);
            }
        }
    }

    for (auto& backend : backends_)
        backend->finalize(*index_);
}

void Pipeline::runPostprocessors() {
    for (auto& p : postprocs_)
        p->process(*index_);
}

void Pipeline::writeOutput() {
    OutputWriter writer;
    writer.write(config_.outputPath, *index_);
}

} // namespace mj2x


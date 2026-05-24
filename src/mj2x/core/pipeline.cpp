#include "pipeline.hpp"
#include "mj2x/io/input_loader.hpp"
#include "mj2x/io/output_writer.hpp"

#include <cstdio>

namespace mj2x {

Pipeline::Pipeline(PipelineConfig config)
    : config_(std::move(config))
{}

Pipeline::~Pipeline() = default;

int Pipeline::run() {
    printf("[mj2x] Pipeline start (input=%s, output=%s)\n",
           config_.inputPath.c_str(), config_.outputPath.c_str());

    try {
        loadInput();
    } catch (const std::exception& e) {
        printf("[mj2x] ERROR: load failed - %s\n", e.what());
        return 1;
    }

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

void Pipeline::writeOutput() {
    OutputWriter writer;
    writer.write(config_.outputPath, *index_);
}

} // namespace mj2x

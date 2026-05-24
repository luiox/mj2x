#pragma once

#include "compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

struct PipelineConfig {
    std::string inputPath;
    std::string outputPath;
    bool dumpIR = false;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    int run();

private:
    void loadInput();
    void writeOutput();

    PipelineConfig config_;
    std::unique_ptr<CompilerIndex> index_;
};

} // namespace mj2x

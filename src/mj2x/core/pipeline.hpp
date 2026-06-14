#pragma once

#include "compiler_index.hpp"
#include "mj2x/backend/backend.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/preprocessor/preprocessor.hpp"
#include "mj2x/postprocessor/postprocessor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mj2x {

struct PipelineConfig {
    std::string inputPath;
    std::string outputPath;
    bool dumpIR = false;
    bool dumpMIR = false;
};

class Pipeline {
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    void registerBackend(std::unique_ptr<Backend> backend);
    void registerPreprocessor(std::unique_ptr<Preprocessor> preproc);
    void registerPostprocessor(std::unique_ptr<Postprocessor> postproc);

    int run();

private:
    void loadInput();
    void runPreprocessors();
    void compileMethods();
    void runPostprocessors();
    void writeOutput();

    PipelineConfig config_;
    std::unique_ptr<CompilerIndex> index_;
    std::vector<std::unique_ptr<Backend>> backends_;
    std::vector<std::unique_ptr<Preprocessor>> preprocs_;
    std::vector<std::unique_ptr<Postprocessor>> postprocs_;
};

} // namespace mj2x


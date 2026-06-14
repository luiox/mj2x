#include "mj2x/mj2x.hpp"
#include "mj2x/backend/c_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"

#include <cstdio>
#include <string>

static void printUsage() {
    printf("Usage: mj2x-cli -i <input.class> -o <output.class> [options]\n");
    printf("Options:\n");
    printf("  --emit-c <file>    Output C source to file\n");
    printf("  --dump-mir         Dump MIR intermediate representation\n");
    printf("  --help             Print help\n");
}

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    std::string cOutputPath;
    bool dumpMIR = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--emit-c" && i + 1 < argc) cOutputPath = argv[++i];
        else if (arg == "--dump-mir") dumpMIR = true;
        else if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else {
            printUsage();
            return 1;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        printUsage();
        return 1;
    }

    mj2x::PipelineConfig config;
    config.inputPath = inputPath;
    config.outputPath = outputPath;
    config.dumpMIR = dumpMIR;

    mj2x::Pipeline pipeline(std::move(config));

    if (!cOutputPath.empty()) {
        auto cBackend = std::make_unique<mj2x::CBackend>();
        cBackend->setOutputPath(cOutputPath);
        pipeline.registerBackend(std::move(cBackend));
    }

    pipeline.registerPreprocessor(
        std::make_unique<mj2x::MarkAllMethodsPreprocessor>());

    return pipeline.run();
}


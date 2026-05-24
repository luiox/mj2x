#include "mj2x/mj2x.hpp"

#include <cstdio>
#include <string>

static void printUsage() {
    printf("Usage: mj2x-cli -i <input.class> -o <output.class>\n");
    printf("       mj2x-cli --help\n");
}

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
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

    mj2x::Pipeline pipeline(std::move(config));
    return pipeline.run();
}

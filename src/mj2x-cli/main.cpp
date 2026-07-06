#include "mj2x/mj2x.hpp"
#include "mj2x/backend/c_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/multianewarray_preprocessor.hpp"

#include <cstdio>
#include <string>

static void printUsage() {
    printf("Usage: mj2x-cli -i <input.class> -o <output.class> [options]\n");
    printf("Options:\n");
    printf("  --emit-c <file>            Output C source to file\n");
    printf("  --backend <cbackend|regc>  Select C emitter when --emit-c is used (default: cbackend)\n");
    printf("  --dump-mir                 Dump MIR intermediate representation\n");
    printf("  --help                     Print help\n");
}

enum class CEmitterBackend {
    CBackend,
    RegC,
};

static bool parseBackendName(const std::string& name, CEmitterBackend& backend) {
    if (name == "cbackend" || name == "c") {
        backend = CEmitterBackend::CBackend;
        return true;
    }
    if (name == "regc") {
        backend = CEmitterBackend::RegC;
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    std::string cOutputPath;
    CEmitterBackend cBackendKind = CEmitterBackend::CBackend;
    bool backendSelected = false;
    bool dumpMIR = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--emit-c" && i + 1 < argc) cOutputPath = argv[++i];
        else if (arg == "--backend" && i + 1 < argc) {
            backendSelected = true;
            if (!parseBackendName(argv[++i], cBackendKind)) {
                fprintf(stderr, "Unknown --backend value. Expected cbackend or regc.\n");
                printUsage();
                return 1;
            }
        }
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

    if (backendSelected && cOutputPath.empty()) {
        fprintf(stderr, "--backend requires --emit-c because it selects the C emitter.\n");
        printUsage();
        return 1;
    }

    mj2x::PipelineConfig config;
    config.inputPath = inputPath;
    config.outputPath = outputPath;
    config.dumpMIR = dumpMIR;

    mj2x::Pipeline pipeline(std::move(config));

    if (!cOutputPath.empty()) {
        if (cBackendKind == CEmitterBackend::RegC) {
            auto regcBackend = std::make_unique<mj2x::RegCBackend>();
            regcBackend->setOutputPath(cOutputPath);
            pipeline.registerBackend(std::move(regcBackend));
        } else {
            auto cBackend = std::make_unique<mj2x::CBackend>();
            cBackend->setOutputPath(cOutputPath);
            pipeline.registerBackend(std::move(cBackend));
        }
    }

    // Bytecode-shaping passes run before MarkAll so every selected method sees
    // the same normalized L1 input regardless of the chosen native backend.
    pipeline.registerPreprocessor(std::make_unique<mj2x::LdcPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::InvokeDynamicPreprocessor>());
    pipeline.registerPreprocessor(std::make_unique<mj2x::MultiANewArrayPreprocessor>());
    pipeline.registerPreprocessor(
        std::make_unique<mj2x::MarkAllMethodsPreprocessor>());

    return pipeline.run();
}


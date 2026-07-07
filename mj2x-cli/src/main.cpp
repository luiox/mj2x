#include "mj2x/mj2x.hpp"
#include "mj2x/backend/cbackend/c_backend.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "mj2x/preprocessor/multianewarray_preprocessor.hpp"
#include "mj2x/preprocessor/native_annotation_preprocessor.hpp"
#include "native_compiler.hpp"

#include <cstdio>
#include <string>

static void printUsage() {
    printf("Usage: mj2x-cli -i <input.class|input.jar> -o <output.class|output.jar> [options]\n");
    printf("Options:\n");
    printf("  --emit-c <file>            Output C source to file\n");
    printf("  --backend <cbackend|regc>  Select C emitter when --emit-c is used (default: cbackend)\n");
    printf("  --native-annotation <desc> Only compile methods annotated with JVM descriptor desc\n");
    printf("  --compile                  Compile emitted C into a native library beside --output\n");
    printf("  --c-compiler <name>        Override C compiler detection (msvc, gcc, clang)\n");
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

static bool isNativeAnnotationDescriptor(const std::string& descriptor) {
    return descriptor.size() >= 3 && descriptor.front() == 'L' &&
           descriptor.back() == ';' && descriptor.find('.') == std::string::npos;
}

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputPath;
    std::string cOutputPath;
    std::string nativeAnnotation;
    CEmitterBackend cBackendKind = CEmitterBackend::CBackend;
    mj2x::cli::CCompilerKind cCompilerKind = mj2x::cli::CCompilerKind::Auto;
    bool backendSelected = false;
    bool compileNative = false;
    bool compilerSelected = false;
    bool dumpMIR = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--emit-c" && i + 1 < argc) cOutputPath = argv[++i];
        else if (arg == "--native-annotation" && i + 1 < argc) nativeAnnotation = argv[++i];
        else if (arg == "--compile") compileNative = true;
        else if (arg == "--c-compiler" && i + 1 < argc) {
            compilerSelected = true;
            if (!mj2x::cli::parseCompilerName(argv[++i], cCompilerKind)) {
                fprintf(stderr, "Unknown --c-compiler value. Expected msvc, gcc, or clang.\n");
                printUsage();
                return 1;
            }
        }
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

    if (!nativeAnnotation.empty() && !isNativeAnnotationDescriptor(nativeAnnotation)) {
        fprintf(stderr,
                "--native-annotation must be a JVM annotation descriptor like Lcom/example/Native;.\n");
        printUsage();
        return 1;
    }

    if (backendSelected && cOutputPath.empty()) {
        fprintf(stderr, "--backend requires --emit-c because it selects the C emitter.\n");
        printUsage();
        return 1;
    }

    if (compileNative && cOutputPath.empty()) {
        fprintf(stderr, "--compile requires --emit-c because it compiles emitted C source.\n");
        printUsage();
        return 1;
    }

    if (compilerSelected && !compileNative) {
        fprintf(stderr, "--c-compiler requires --compile.\n");
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
    if (!nativeAnnotation.empty()) {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::NativeAnnotationPreprocessor>(nativeAnnotation));
    } else {
        pipeline.registerPreprocessor(
            std::make_unique<mj2x::MarkAllMethodsPreprocessor>());
    }

    int rc = pipeline.run();
    if (rc != 0)
        return rc;

    if (compileNative)
        return mj2x::cli::compileCSource(cOutputPath, outputPath, cCompilerKind);

    return 0;
}


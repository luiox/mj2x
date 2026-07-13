/**
 * @file regc_backend.hpp
 * @brief Native backend facade for the Register-C pipeline.
 *
 * @details
 * RegCBackend adapts the stackless RegC emitter to the shared mj2x Backend
 * interface.  It lowers one method through SSA and RegIR, emits the register
 * helper function, then wraps that helper in a JNI-exported native method and
 * marks the original Java method as native.  The current helper ABI is emitted
 * inline into the generated translation unit so RegC samples are executable
 * while the longer-term R3 runtime symbol/context boundary is still being
 * hardened.
 */
#pragma once

#include "mj2x/backend/backend.hpp"

#include <memory>
#include <string>

namespace mj2x {

/**
 * @brief Backend that emits JNI-native C using the SSA -> RegIR -> RegC path.
 *
 * @details
 * The backend owns no bytecode conversion logic itself.  The shared Pipeline
 * still supplies L1 MIR blocks; RegCBackend builds SSA and RegIR from those
 * blocks, delegates stackless C generation to RegCEmitter, and appends a JNI
 * wrapper with javah-compatible symbol naming.  Methods that cannot be lowered
 * fail closed and are not injected as native methods; this prevents partially
 * translated bytecode from being replaced by a broken native stub.
 */
class RegCBackend : public Backend
{
public:
    RegCBackend();
    ~RegCBackend() override;

    std::string_view name() const override { return "regc_native"; }

    BackendResult compileMethod(const std::string& className, CompilerMethod& method,
                                const std::vector<Block*>& blocks) override;

    void injectIntoClass(CompilerClass& clazz, CompilerMethod& method,
                         const BackendResult& result) override;

    void finalize(const CompilerIndex& index) override;

    /**
     * @brief Set a `.c` output path written during finalize.
     *
     * @details
     * Pipeline and E2E tests use this to compile the generated translation unit
     * into a native library.  When empty, finalize still records
     * `generatedSource()` but does not write a file.
     */
    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }

    /**
     * @brief Full generated C translation unit after finalize.
     *
     * @details
     * The source includes helper declarations/definitions, one RegC helper per
     * successfully lowered method, and the JNI wrappers used to bind those
     * helpers back to the protected Java class.
     */
    const std::string& generatedSource() const { return sourceCode_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string cOutputPath_;
    std::string sourceCode_;
};

}   // namespace mj2x

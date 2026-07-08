#pragma once

#include "mj2x/backend/backend.hpp"

#include <memory>
#include <string>

namespace mj2x {

class CBackend : public Backend {
public:
    CBackend();
    ~CBackend() override;

    std::string_view name() const override { return "c_source"; }

    BackendResult compileMethod(
        const std::string& className,
        CompilerMethod& method,
        const std::vector<Block*>& blocks
    ) override;

    void injectIntoClass(
        CompilerClass& clazz,
        CompilerMethod& method,
        const BackendResult& result
    ) override;

    void finalize(const CompilerIndex& index) override;

    void setOutputPath(const std::string& cOutputPath) { cOutputPath_ = cOutputPath; }
    void setJarNativeLoaderResourcePath(
        const std::string& resourcePath,
        const std::string& tempSuffix);

    // Access generated code for testing
    const std::string& generatedHeader() const { return headerCode_; }
    const std::string& generatedSource() const { return sourceCode_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string cOutputPath_;
    std::string headerCode_;
    std::string sourceCode_;
};

} // namespace mj2x


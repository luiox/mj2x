#pragma once

#include "backend_result.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/frontend/block.hpp"

#include <string_view>

namespace mj2x {

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string_view name() const = 0;

    virtual BackendResult compileMethod(
        const std::string& className,
        CompilerMethod& method,
        const std::vector<Block*>& blocks
    ) = 0;

    virtual void injectIntoClass(
        CompilerClass& clazz,
        CompilerMethod& method,
        const BackendResult& result
    ) = 0;

    virtual void finalize(const CompilerIndex& index) = 0;
};

} // namespace mj2x


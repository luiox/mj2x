#pragma once

#include "mir_node.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mj2x {

struct ExceptionHandler {
    int startLabel = 0;
    int endLabel = 0;
    int handlerLabel = 0;
    std::string catchType; // empty = finally
};

struct Block {
    int id = 0;
    std::vector<std::unique_ptr<MIRNode>> nodes;
    std::vector<Block*> successors;
    std::vector<Block*> predecessors;
    ExceptionHandler* exceptionHandler = nullptr;

    explicit Block(int id_) : id(id_) {}
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
};

} // namespace mj2x

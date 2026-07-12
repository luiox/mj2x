/**
 * @file reg_dump.hpp
 * @brief Text dump for Register IR tests and review.
 */
#pragma once

#include "mj2x/mir/register_ir/reg_function.hpp"

#include <string>

namespace mj2x {

/// @brief Dump a RegFunction without JVM stack-machine anchor names.
std::string dumpRegIR(const RegFunction& fn);

}   // namespace mj2x

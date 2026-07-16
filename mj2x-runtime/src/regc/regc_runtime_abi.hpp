/// @file regc_runtime_abi.hpp
/// @brief Compatibility include for the distributable RegC runtime ABI.
///
/// RegC generated code and all runtime ABI implementations now ship from
/// `mj2x-crt`.  This forwarding header preserves source compatibility for
/// in-repository consumers while keeping `mj2x-crt` as the sole ABI owner.
#pragma once

#include <regc/mj2x_regc_crt.h>

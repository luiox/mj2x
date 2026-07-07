#include "intrinsic/intrinsic_wrapper.hpp"

#include <cstddef>

namespace {

// Integer cache mirrors java.lang.Integer.IntegerCache: a pre-populated table
// covering [-128, 127].  The cache is process-wide and read-only after first
// use, so unsynchronized access from generated code is safe.
constexpr std::int32_t kIntCacheLow = -128;
constexpr std::int32_t kIntCacheHigh = 127;
constexpr std::int32_t kIntCacheSize = kIntCacheHigh - kIntCacheLow + 1;

struct IntegerCache {
    mj2x_int slots[static_cast<std::size_t>(kIntCacheSize)];
    IntegerCache() {
        for (std::int32_t i = 0; i < kIntCacheSize; ++i) {
            slots[static_cast<std::size_t>(i)].value = kIntCacheLow + i;
        }
    }
};

const IntegerCache& integerCache() {
    static const IntegerCache cache;
    return cache;
}

// Boolean preallocated TRUE/FALSE instances mirror java.lang.Boolean.TRUE and
// Boolean.FALSE so equality checks against the boxed result stay stable.
struct BooleanCache {
    mj2x_bool falseSlot;
    mj2x_bool trueSlot;
    BooleanCache() : falseSlot{0}, trueSlot{1} {}
};

const BooleanCache& booleanCache() {
    static const BooleanCache cache;
    return cache;
}

}  // namespace

extern "C" {

mj2x_int mj2x_int_value_of(std::int32_t value) {
    if (value >= kIntCacheLow && value <= kIntCacheHigh) {
        return integerCache().slots[static_cast<std::size_t>(value - kIntCacheLow)];
    }
    return mj2x_int{value};
}

std::int32_t mj2x_int_int_value(mj2x_int box) {
    return box.value;
}

mj2x_long mj2x_long_value_of(std::int64_t value) {
    return mj2x_long{value};
}

std::int64_t mj2x_long_long_value(mj2x_long box) {
    return box.value;
}

mj2x_short mj2x_short_value_of(std::int16_t value) {
    return mj2x_short{value};
}

std::int16_t mj2x_short_short_value(mj2x_short box) {
    return box.value;
}

mj2x_byte mj2x_byte_value_of(std::int8_t value) {
    return mj2x_byte{value};
}

std::int8_t mj2x_byte_byte_value(mj2x_byte box) {
    return box.value;
}

mj2x_float mj2x_float_value_of(float value) {
    return mj2x_float{value};
}

float mj2x_float_float_value(mj2x_float box) {
    return box.value;
}

mj2x_double mj2x_double_value_of(double value) {
    return mj2x_double{value};
}

double mj2x_double_double_value(mj2x_double box) {
    return box.value;
}

mj2x_bool mj2x_bool_value_of(std::int8_t value) {
    return value != 0 ? booleanCache().trueSlot : booleanCache().falseSlot;
}

std::int8_t mj2x_bool_bool_value(mj2x_bool box) {
    return box.value;
}

mj2x_char mj2x_char_value_of(std::uint16_t value) {
    return mj2x_char{value};
}

std::uint16_t mj2x_char_char_value(mj2x_char box) {
    return box.value;
}

}  // extern "C"

#include "intrinsic/intrinsic_wrapper.hpp"
#include "regc/regc_runtime_abi.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

#ifdef MJ2X_RUNTIME_WITH_JNI
#include <jni.h>
#endif

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

std::int32_t _mj2x_intrinsic_int_compare(std::int32_t lhs, std::int32_t rhs) {
    return (lhs > rhs) - (lhs < rhs);
}

std::int32_t _mj2x_intrinsic_long_compare(std::int64_t lhs, std::int64_t rhs) {
    return (lhs > rhs) - (lhs < rhs);
}

std::int32_t _mj2x_intrinsic_byte_compare(std::int8_t lhs, std::int8_t rhs) {
    return static_cast<std::int32_t>(lhs) - static_cast<std::int32_t>(rhs);
}

std::int32_t _mj2x_intrinsic_short_compare(std::int16_t lhs, std::int16_t rhs) {
    return static_cast<std::int32_t>(lhs) - static_cast<std::int32_t>(rhs);
}

std::int32_t _mj2x_intrinsic_float_compare(float lhs, float rhs) {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    std::uint32_t lhsBits;
    std::uint32_t rhsBits;
    std::memcpy(&lhsBits, &lhs, sizeof(lhsBits));
    std::memcpy(&rhsBits, &rhs, sizeof(rhsBits));
    if ((lhsBits & 0x7fffffffU) > 0x7f800000U) lhsBits = 0x7fc00000U;
    if ((rhsBits & 0x7fffffffU) > 0x7f800000U) rhsBits = 0x7fc00000U;
    if (lhsBits == rhsBits) return 0;
    return static_cast<std::int32_t>(lhsBits) < static_cast<std::int32_t>(rhsBits) ? -1 : 1;
}

std::int32_t _mj2x_intrinsic_double_compare(double lhs, double rhs) {
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    std::uint64_t lhsBits;
    std::uint64_t rhsBits;
    std::memcpy(&lhsBits, &lhs, sizeof(lhsBits));
    std::memcpy(&rhsBits, &rhs, sizeof(rhsBits));
    if ((lhsBits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) {
        lhsBits = 0x7ff8000000000000ULL;
    }
    if ((rhsBits & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL) {
        rhsBits = 0x7ff8000000000000ULL;
    }
    if (lhsBits == rhsBits) return 0;
    return static_cast<std::int64_t>(lhsBits) < static_cast<std::int64_t>(rhsBits) ? -1 : 1;
}

std::int32_t _mj2x_intrinsic_bool_compare(std::int8_t lhs, std::int8_t rhs) {
    return ((lhs != 0) > (rhs != 0)) - ((lhs != 0) < (rhs != 0));
}

std::int32_t _mj2x_intrinsic_char_compare(std::uint16_t lhs, std::uint16_t rhs) {
    return static_cast<std::int32_t>(lhs) - static_cast<std::int32_t>(rhs);
}

void* _mj2x_intrinsic_int_to_string(mj2x_regc_context* context, std::int32_t value) {
#ifdef MJ2X_RUNTIME_WITH_JNI
    auto* env = context == nullptr ? nullptr : static_cast<JNIEnv*>(context->env);
    if (env == nullptr) return nullptr;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    return env->NewStringUTF(buffer);
#else
    (void)context;
    (void)value;
    return nullptr;
#endif
}

void* _mj2x_intrinsic_long_to_string(mj2x_regc_context* context, std::int64_t value) {
#ifdef MJ2X_RUNTIME_WITH_JNI
    auto* env = context == nullptr ? nullptr : static_cast<JNIEnv*>(context->env);
    if (env == nullptr) return nullptr;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return env->NewStringUTF(buffer);
#else
    (void)context;
    (void)value;
    return nullptr;
#endif
}

void* _mj2x_intrinsic_bool_to_string(mj2x_regc_context* context, std::int8_t value) {
#ifdef MJ2X_RUNTIME_WITH_JNI
    auto* env = context == nullptr ? nullptr : static_cast<JNIEnv*>(context->env);
    if (env == nullptr) return nullptr;
    return env->NewStringUTF(value ? "true" : "false");
#else
    (void)context;
    (void)value;
    return nullptr;
#endif
}

void* _mj2x_intrinsic_char_to_string(mj2x_regc_context* context, std::uint16_t value) {
#ifdef MJ2X_RUNTIME_WITH_JNI
    auto* env = context == nullptr ? nullptr : static_cast<JNIEnv*>(context->env);
    if (env == nullptr) return nullptr;
    char buffer[4];
    std::int32_t length = 0;
    if (value == 0) {
        buffer[0] = static_cast<char>(0xC0);
        buffer[1] = static_cast<char>(0x80);
        length = 2;
    } else if (value <= 0x7F) {
        buffer[0] = static_cast<char>(value);
        length = 1;
    } else if (value <= 0x7FF) {
        buffer[0] = static_cast<char>(0xC0 | (value >> 6));
        buffer[1] = static_cast<char>(0x80 | (value & 0x3F));
        length = 2;
    } else {
        buffer[0] = static_cast<char>(0xE0 | (value >> 12));
        buffer[1] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        buffer[2] = static_cast<char>(0x80 | (value & 0x3F));
        length = 3;
    }
    buffer[length] = '\0';
    return env->NewStringUTF(buffer);
#else
    (void)context;
    (void)value;
    return nullptr;
#endif
}

}  // extern "C"

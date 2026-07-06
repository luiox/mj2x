#include "regc/regc_runtime_abi.hpp"

#include <limits>

namespace {

constexpr const char* kArithmeticException = "java/lang/ArithmeticException";
constexpr const char* kDivideByZero = "/ by zero";

void throwException(mj2x_regc_context* context, const char* className, const char* message) {
    if (context == nullptr || context->throwException == nullptr) return;
    context->throwException(context->env, className, message);
}

}  // namespace

extern "C" {

void mj2x_regc_init_context(mj2x_regc_context* context,
                            void* env,
                            mj2x_regc_throw_callback throwExceptionCallback) {
    if (context == nullptr) return;
    context->env = env;
    context->throwException = throwExceptionCallback;
}

void mj2x_regc_set_env(mj2x_regc_context* context, void* env) {
    if (context == nullptr) return;
    context->env = env;
}

void* mj2x_regc_env(const mj2x_regc_context* context) {
    return context == nullptr ? nullptr : context->env;
}

void mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message) {
    throwException(context, kArithmeticException, message == nullptr ? kDivideByZero : message);
}

std::int32_t mj2x_regc_div_i32(mj2x_regc_context* context, std::int32_t lhs, std::int32_t rhs) {
    // JVM integer division traps before evaluating the native divide; keeping
    // this branch in runtime avoids repeating UB-prone checks in generated C.
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return lhs / rhs;
}

std::int64_t mj2x_regc_div_i64(mj2x_regc_context* context, std::int64_t lhs, std::int64_t rhs) {
    // C++ would overflow on INT64_MIN / -1; the JVM specifies wraparound to the
    // same minimum value, so handle that edge before the native divide.
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return lhs / rhs;
}

std::int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, std::int32_t lhs, std::int32_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
        return 0;
    }
    return lhs % rhs;
}

std::int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, std::int64_t lhs, std::int64_t rhs) {
    if (rhs == 0) {
        mj2x_regc_throw_arithmetic(context, kDivideByZero);
        return 0;
    }
    if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
        return 0;
    }
    return lhs % rhs;
}

void mj2x_regc_barrier(mj2x_regc_context* context, const char* reason) {
    // The barrier is an ABI placeholder.  Later R3 policies can add checks here
    // without changing already-generated RegC call sites.
    (void)context;
    (void)reason;
}

}  // extern "C"

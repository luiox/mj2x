#include "regc/regc_runtime_abi.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <gmock/gmock.h>

namespace {

struct ThrownException {
    int count = 0;
    std::string className;
    std::string message;
};

void recordThrow(void* env, const char* className, const char* message) {
    auto* thrown = static_cast<ThrownException*>(env);
    if (thrown == nullptr) return;
    thrown->count++;
    thrown->className = className == nullptr ? "" : className;
    thrown->message = message == nullptr ? "" : message;
}

}  // namespace

TEST(RegCRuntimeAbiTest, InitializesAndUpdatesOpaqueEnvironment) {
    ThrownException left;
    ThrownException right;
    mj2x_regc_context context{};

    mj2x_regc_init_context(&context, &left, recordThrow);
    EXPECT_EQ(mj2x_regc_env(&context), &left);

    mj2x_regc_set_env(&context, &right);
    EXPECT_EQ(mj2x_regc_env(&context), &right);
}

TEST(RegCRuntimeAbiTest, DivRemFollowJvmOverflowRules) {
    mj2x_regc_context context{};
    mj2x_regc_init_context(&context, nullptr, recordThrow);

    EXPECT_EQ(mj2x_regc_div_i32(&context, 9, 3), 3);
    EXPECT_EQ(mj2x_regc_rem_i32(&context, 10, 4), 2);
    EXPECT_EQ(mj2x_regc_div_i64(&context, 12, 3), 4);
    EXPECT_EQ(mj2x_regc_rem_i64(&context, 13, 5), 3);
    EXPECT_EQ(mj2x_regc_div_i32(&context, std::numeric_limits<std::int32_t>::min(), -1),
              std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(mj2x_regc_rem_i64(&context, std::numeric_limits<std::int64_t>::min(), -1), 0);
}

TEST(RegCRuntimeAbiTest, DivideByZeroReportsArithmeticException) {
    ThrownException thrown;
    mj2x_regc_context context{};
    mj2x_regc_init_context(&context, &thrown, recordThrow);

    EXPECT_EQ(mj2x_regc_div_i32(&context, 1, 0), 0);
    EXPECT_EQ(thrown.count, 1);
    EXPECT_EQ(thrown.className, "java/lang/ArithmeticException");
    EXPECT_EQ(thrown.message, "/ by zero");

    EXPECT_EQ(mj2x_regc_rem_i64(&context, 1, 0), 0);
    EXPECT_EQ(thrown.count, 2);
}

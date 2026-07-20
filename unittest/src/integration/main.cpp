// integration/main.cpp - mj2x 真实 native/JVM 集成测试入口。

#include <gmock/gmock.h>

#include "test_helper.hpp"

int main(int argc, char** argv)
{
    test::setup("mj2x");
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}

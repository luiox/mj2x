#include <gmock/gmock.h>
#include "test_helper.hpp"

int main(int argc, char** argv) {
    test::setup("mj2x");
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}


#include "regc/mj2x_regc_crt.h"

#include <cassert>
#include <thread>
#include <vector>

extern "C" void test_string_buffer_concurrency(void)
{
    mj2x_regc_context context;
    mj2x_regc_init_context(&context, nullptr, nullptr);

    mj2x_string_buffer* buffer = nullptr;
    assert(mj2x_string_buffer_init(&context, &buffer) == MJ2X_REGC_STATUS_OK);
    assert(buffer != nullptr);

    constexpr int            threadCount      = 8;
    constexpr int            appendsPerThread = 2000;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&] {
            for (int i = 0; i < appendsPerThread; ++i) {
                assert(mj2x_string_buffer_append_char(&context, buffer, 'x') ==
                       MJ2X_REGC_STATUS_OK);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    int32_t length = 0;
    assert(mj2x_string_buffer_length(&context, buffer, &length) == MJ2X_REGC_STATUS_OK);
    assert(length == threadCount * appendsPerThread);
    mj2x_string_buffer_destroy(buffer);
}

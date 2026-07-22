#include "regc/mj2x_regc_crt.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct thrown_exception
{
    int         count;
    const char* class_name;
    const char* message;
} thrown_exception;

void test_string_buffer_concurrency(void);
void test_regc_intrinsic_c99_abi(void);

static void record_throw(void* env, const char* class_name, const char* message)
{
    thrown_exception* thrown = (thrown_exception*)env;
    if (thrown == NULL) return;
    thrown->count += 1;
    thrown->class_name = class_name;
    thrown->message    = message;
}

static void test_div_rem_and_status_bridge(void)
{
    thrown_exception  thrown = {0};
    mj2x_regc_context context;
    mj2x_regc_init_context(&context, &thrown, record_throw);

    assert(mj2x_regc_div_i32(&context, 9, 3) == 3);
    assert(mj2x_regc_rem_i64(&context, 13, 5) == 3);
    assert(mj2x_regc_div_i32(&context, INT32_MIN, -1) == INT32_MIN);
    assert(mj2x_regc_rem_i64(&context, INT64_MIN, -1) == 0);
    assert(mj2x_regc_div_i32(&context, 1, 0) == 0);
    assert(thrown.count == 1);
    assert(strcmp(thrown.class_name, "java/lang/ArithmeticException") == 0);
    assert(strcmp(thrown.message, "/ by zero") == 0);

    mj2x_regc_report_status(&context, MJ2X_REGC_STATUS_UNSUPPORTED, "test operation");
    assert(thrown.count == 2);
    assert(strcmp(thrown.class_name, "java/lang/UnsupportedOperationException") == 0);
    assert(strcmp(thrown.message, "test operation") == 0);
}

static void test_compare_intrinsics(void)
{
    assert(_mj2x_intrinsic_int_compare(1, 2) == -1);
    assert(_mj2x_intrinsic_long_compare(INT64_MAX, INT64_MIN) == 1);
    assert(_mj2x_intrinsic_byte_compare(INT8_MIN, INT8_MAX) == -255);
    assert(_mj2x_intrinsic_short_compare(300, -20) == 320);
    assert(_mj2x_intrinsic_float_compare(-0.0f, 0.0f) == -1);
    assert(_mj2x_intrinsic_double_compare(0.0, -0.0) == 1);
    assert(_mj2x_intrinsic_bool_compare(42, 1) == 0);
    assert(_mj2x_intrinsic_char_compare((uint16_t)'B', (uint16_t)'A') == 1);
}

static void test_string_builder_utf16_and_errors(void)
{
    const uint16_t       prefix[] = {(uint16_t)'A', 0x4E2D, 0xD83D, 0xDE00};
    thrown_exception     thrown   = {0};
    mj2x_regc_context    context;
    mj2x_string_builder* builder = NULL;

    mj2x_regc_init_context(&context, &thrown, record_throw);
    assert(mj2x_string_builder_init(&context, &builder) == MJ2X_REGC_STATUS_OK);
    assert(builder != NULL);
    assert(mj2x_string_builder_append_utf16(&context, builder, prefix, 4) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_append_i32(&context, builder, -42) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_append_bool(&context, builder, 1) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_utf16_length(builder) == 11);
    int32_t  capacity = 0;
    int32_t  length   = 0;
    uint16_t value    = 0;
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 16);
    assert(mj2x_string_builder_ensure_capacity(&context, builder, 17) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 34);
    assert(mj2x_string_builder_set_length(&context, builder, 20) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_length(&context, builder, &length) == MJ2X_REGC_STATUS_OK);
    assert(length == 20);
    assert(mj2x_string_builder_char_at(&context, builder, 11, &value) == MJ2X_REGC_STATUS_OK);
    assert(value == 0);
    assert(mj2x_string_builder_set_char_at(&context, builder, 11, (uint16_t)'X') ==
           MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_trim_to_size(&context, builder) == MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_capacity(&context, builder, &capacity) == MJ2X_REGC_STATUS_OK);
    assert(capacity == 20);
    assert(mj2x_string_builder_append_code_point(&context, builder, 0x1F600) ==
           MJ2X_REGC_STATUS_OK);
    assert(mj2x_string_builder_utf16_length(builder) == 22);

    assert(mj2x_string_builder_append_utf16(&context, NULL, prefix, 1) ==
           MJ2X_REGC_STATUS_INVALID_STATE);
    assert(thrown.count == 1);
    assert(strcmp(thrown.class_name, "java/lang/IllegalStateException") == 0);

    assert(mj2x_string_builder_append_utf16(&context, builder, prefix, SIZE_MAX) ==
           MJ2X_REGC_STATUS_OUT_OF_MEMORY);
    assert(thrown.count == 2);
    assert(strcmp(thrown.class_name, "java/lang/OutOfMemoryError") == 0);

    mj2x_string_builder_destroy(builder);
}

int main(void)
{
    test_div_rem_and_status_bridge();
    test_compare_intrinsics();
    test_regc_intrinsic_c99_abi();
    test_string_builder_utf16_and_errors();
    test_string_buffer_concurrency();
    return 0;
}

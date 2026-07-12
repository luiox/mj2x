#include "intrinsic/intrinsic_string.hpp"

#include <limits>

namespace {

void setStatus(mj2x_string_status* out, mj2x_string_status status)
{
    if (out != nullptr) *out = status;
}

bool isValidView(mj2x_utf16_view view)
{
    return view.units != nullptr || view.length == 0;
}

}   // namespace

extern "C" {

mj2x_utf16_view mj2x_utf16_view_of(const std::uint16_t* units, std::size_t length)
{
    return mj2x_utf16_view{units, length};
}

std::int32_t mj2x_string_length(mj2x_utf16_view view, mj2x_string_status* status)
{
    if (!isValidView(view)) {
        setStatus(status, MJ2X_STRING_UNSUPPORTED_NULL);
        return 0;
    }
    if (view.length > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        setStatus(status, MJ2X_STRING_UNSUPPORTED_BOUNDS);
        return 0;
    }
    setStatus(status, MJ2X_STRING_OK);
    return static_cast<std::int32_t>(view.length);
}

std::uint16_t mj2x_string_char_at(mj2x_utf16_view view, std::size_t index,
                                  mj2x_string_status* status)
{
    if (!isValidView(view)) {
        setStatus(status, MJ2X_STRING_UNSUPPORTED_NULL);
        return 0;
    }
    if (index >= view.length) {
        setStatus(status, MJ2X_STRING_UNSUPPORTED_BOUNDS);
        return 0;
    }
    setStatus(status, MJ2X_STRING_OK);
    return view.units[index];
}

mj2x_string_status mj2x_stringbuilder_unsupported_status()
{
    return MJ2X_STRING_UNSUPPORTED_MUTABLE_BUILDER;
}

}   // extern "C"

/// @file runtime_entry.cpp
/// @brief Minimal exported symbol for the mj2x runtime shared library.

extern "C" int mj2x_runtime_version()
{
    return 3;
}

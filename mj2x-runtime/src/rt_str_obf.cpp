#include "rt_str_obf.h"
#include <cstdlib>
#include <cstring>

char* c_str_obf(char* a, char* b, const size_t len)
{
    volatile char* c = (char*)std::malloc(len);
    memcpy((char*)c, b, len);
    memcpy(b, (char*)c, len);
    for (size_t i = 0; i < len; i++) {
        a[i] ^= b[i];
    }
    std::free((void*)c);
    return a;
}

unsigned short* j_str_deobf(unsigned short* a, unsigned short* b, const size_t len)
{
    volatile unsigned short* c = (unsigned short*)std::malloc(len * sizeof(unsigned short));
    memcpy((unsigned short*)c, b, len);
    memcpy(b, (unsigned short*)c, len);
    for (size_t i = 0; i < len; i++) {
        a[i] ^= b[i];
    }
    std::free((void*)c);
    return a;
}

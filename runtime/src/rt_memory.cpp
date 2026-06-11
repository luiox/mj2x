#include "rt_memory.h"


inline jlong rt_memory_read(jint size)
{
	return (jlong)malloc(size);
}

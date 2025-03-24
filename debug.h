#pragma once

#include <stdio.h>

#ifdef DEBUG
#define fdebug(stream, ...)                                                                        \
    do {                                                                                           \
        fprintf(stream, "[%s:%s():%d]\t", __FILE__, __func__, __LINE__);                           \
        fprintf(stream, __VA_ARGS__);                                                              \
        fprintf(stream, "\n");                                                                     \
    } while (0);
#else
#define fdebug(stream, ...) ((void) 0)
#endif

#ifdef ERROR
#define ferror(stream, ...)                                                                        \
    do {                                                                                           \
        fprintf(stream, "[%s:%s():%d]\t", __FILE__, __func__, __LINE__);                           \
        fprintf(stream, __VA_ARGS__);                                                              \
        fprintf(stream, "\n");                                                                     \
    } while (0);
#else
#define ferror(stream, ...) ((void) 0)
#endif

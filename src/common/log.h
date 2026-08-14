#pragma once
#include <cstdio>

#define KRG_LOG(...)                        \
    do {                                    \
        std::fprintf(stderr, __VA_ARGS__);  \
        std::fputc('\n', stderr);           \
        std::fflush(stderr);                \
    } while (0)

#pragma once

#include <cstdio>
#include <cstring>

inline size_t strnlen_s(const char* value, size_t limit) { return strnlen(value, limit); }
inline int strcpy_s(char* destination, size_t size, const char* source) {
    if (!destination || !source || size == 0) return 1;
    size_t length = std::strlen(source);
    if (length >= size) { destination[0] = '\0'; return 1; }
    std::memcpy(destination, source, length + 1);
    return 0;
}
template <size_t N, typename... Args>
int sprintf_s(char (&destination)[N], const char* format, Args... args) {
    return std::snprintf(destination, N, format, args...);
}

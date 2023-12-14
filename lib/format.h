#pragma once

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

static std::string format_string(const char* fmt, ...)
{
    std::vector<char> buf;
    va_list args1, args2;
    int len, ret;

    va_start(args1, fmt);
    len = vsnprintf(NULL, 0, fmt, args1);
    assert(len >= 0);
    va_end(args1);

    buf.resize(len + 1);
    va_start(args2, fmt);
    ret = vsnprintf(buf.data(), buf.capacity(), fmt, args2);
    assert(len == ret);
    va_end(args2);

    return std::string(buf.data(), len);
}

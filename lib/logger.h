// See LICENSE for license details.

#pragma once

#include <cstdarg>

struct logger
{
    enum L { Ltrace, Ldebug, Linfo, Lwarn, Lerror, Lpanic };

    static const char* level_names[6];

    static L level;
    
    static void output(const char*prefix, const char* fmt, va_list ap);
    static void log(L level, const char* fmt, ...);
    static void set_level(L level);
};

#undef Trace
#define Trace(fmt, ...) \
if (logger::L::Ltrace >= logger::level) { logger::log(logger::L::Ltrace, fmt, ## __VA_ARGS__); }

#undef Debug
#define Debug(fmt, ...) \
if (logger::L::Ldebug >= logger::level) { logger::log(logger::L::Ldebug, fmt, ## __VA_ARGS__); }

#undef Info
#define Info(fmt, ...) \
logger::log(logger::L::Linfo, fmt, ## __VA_ARGS__);

#undef Warn
#define Warn(fmt, ...) \
logger::log(logger::L::Lwarn, fmt, ## __VA_ARGS__);

#undef Error
#define Error(fmt, ...) \
logger::log(logger::L::Lerror, fmt, ## __VA_ARGS__);

#undef Panic
#define Panic(fmt, ...) \
logger::log(logger::L::Lpanic, fmt, ## __VA_ARGS__);

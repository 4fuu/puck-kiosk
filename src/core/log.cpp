#include "core/log.h"

#include <cstdarg>
#include <cstdio>

namespace pk::log {
namespace {

void emit(const char* level, const char* format, std::va_list args) {
    std::fprintf(stderr, "[puck-kiosk] %-5s ", level);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
}

}  // namespace

void info(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    emit("info", format, args);
    va_end(args);
}

void warn(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    emit("warn", format, args);
    va_end(args);
}

void error(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    emit("error", format, args);
    va_end(args);
}

}  // namespace pk::log

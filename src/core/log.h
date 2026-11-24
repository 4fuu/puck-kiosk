#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define PK_LOG_PRINTF_FORMAT(format_index, argument_index) \
    __attribute__((format(printf, format_index, argument_index)))
#else
#define PK_LOG_PRINTF_FORMAT(format_index, argument_index)
#endif

namespace pk::log {

void info(const char* format, ...) PK_LOG_PRINTF_FORMAT(1, 2);
void warn(const char* format, ...) PK_LOG_PRINTF_FORMAT(1, 2);
void error(const char* format, ...) PK_LOG_PRINTF_FORMAT(1, 2);

}  // namespace pk::log

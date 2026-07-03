#pragma once
#ifndef RT_LOG_H
#define RT_LOG_H

#include <cstdio>
#include <cstdarg>
#include <string>

// Simple file logger that writes to rt2_log.txt in the working directory.
// Usage: RT_LOG("message: %d", value);
// The log file is created on first use and flushed after each write.

namespace RTLog {

inline FILE* getLogFile()
{
    static FILE* fp = nullptr;
    if (!fp)
    {
        fp = fopen("rt2_log.txt", "w");
        if (fp)
            fprintf(fp, "=== RT2 Log ===\n");
    }
    return fp;
}

inline void log(const char* fmt, ...)
{
    FILE* fp = getLogFile();
    if (!fp) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);
}

} // namespace RTLog

#define RT_LOG(...) RTLog::log(__VA_ARGS__)

#endif // RT_LOG_H
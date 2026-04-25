#pragma once

// Define VOICE_LOG to enable file logging (e.g. add to project preprocessor
// definitions or uncomment the line below).
// #define VOICE_LOG

#ifdef VOICE_LOG
#include <Windows.h>
#include <cstdio>

inline void dbglog(const char* msg) {
    static CRITICAL_SECTION cs;
    static bool cs_init = false;
    static FILE* f = nullptr;

    if (!cs_init) {
        InitializeCriticalSection(&cs);
        cs_init = true;
        char path[64];
        sprintf_s(path, "C:\\voice_dll_%lu.txt", GetCurrentProcessId());
        f = fopen(path, "a");
    }

    EnterCriticalSection(&cs);
    if (f) { fprintf(f, "%s\n", msg); fflush(f); }
    LeaveCriticalSection(&cs);
}

inline void dbgloghr(const char* tag, unsigned long hr) {
    char buf[128];
    sprintf_s(buf, "%s hr=0x%08X", tag, hr);
    dbglog(buf);
}

#else
inline void dbglog(const char*) {}
inline void dbgloghr(const char*, unsigned long) {}
#endif

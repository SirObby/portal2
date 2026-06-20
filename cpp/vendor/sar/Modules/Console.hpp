// Simplified replacement for SAR's Modules/Console.hpp.
//
// The real SourceAutoRecord Console module wraps the engine's ICvar/
// IVEngineClient logging system plus a thread-marshalling Scheduler so
// console output is safe to call from any thread and routes through the
// in-game console UI. Our plugin only needs to print diagnostic messages
// to stdout/stderr (visible in the terminal Portal 2 was launched from,
// which is sufficient for a headless RL training setup) so we provide a
// minimal drop-in with the same `console->Print`/`DevWarning`/`DevMsg`
// call shape used by the vendored Entity.cpp/Interface.cpp, without
// pulling in SAR's engine-console-printing plumbing.

#pragma once

#include <cstdarg>
#include <cstdio>

class Console {
public:
    void Print(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stdout, fmt, args);
        va_end(args);
        std::fflush(stdout);
    }

    void DevMsg(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stdout, fmt, args);
        va_end(args);
        std::fflush(stdout);
    }

    void DevWarning(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fflush(stderr);
    }

    void Warning(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
        std::fflush(stderr);
    }
};

extern Console *console;

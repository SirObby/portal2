#include "Offsets.hpp"

#undef OFFSET_DEFAULT
#undef OFFSET_EMPTY
#undef OFFSET_WINDOWS
#undef OFFSET_LINUX

#undef SIGSCAN_DEFAULT
#undef SIGSCAN_EMPTY
#undef SIGSCAN_WINDOWS
#undef SIGSCAN_LINUX

#define OFFSET_DEFAULT(name, win, linux) int name;
#define OFFSET_EMPTY(name) int name;
#define OFFSET_WINDOWS(name, off)
#define OFFSET_LINUX(name, off)

#define SIGSCAN_DEFAULT(name, win, linux) const char *name;
#define SIGSCAN_EMPTY(name) const char *name;
#define SIGSCAN_WINDOWS(name, sig)
#define SIGSCAN_LINUX(name, sig)

namespace Offsets {
    #include "Offsets/Default.hpp"
}

// --- Runtime assignment pass ---
//
// The declaration pass above (and the one inside Offsets.hpp at parse
// time) only reserves storage for each offset -- it never assigns the
// actual win/linux values, since OFFSET_DEFAULT there expands to a bare
// declaration. Offsets.hpp *also* defines the assignment-style macros
// (guarded by #ifdef _WIN32/#else, so the right platform's value is
// picked at compile time) immediately after its own declaration pass,
// but never invokes them as executable statements -- in upstream
// SourceAutoRecord, that invocation happens inside Game::LoadOffsets()
// (see Games/Portal2.cpp), which we don't vendor since it's part of the
// multi-game abstraction we don't need. This function is the minimal
// replacement: it re-includes the same offsets table a second time,
// this time with Offsets.hpp's own (already #ifdef-platform-correct)
// assignment macros active, which actually populates every Offsets::*
// global. Must be called once, early, before any Offsets::* value is
// read (see Plugin.cpp Load()).
//
// Without this call, every Offsets::* value silently reads as 0 --
// which is exactly the bug that caused early crashes: e.g.
// `think + Offsets::UTIL_PlayerByIndex` degenerated to `think + 0`,
// so Memory::Read() resolved a function address from the wrong bytes
// entirely, producing a plausible-looking but garbage pointer instead
// of an obvious null.
void Offsets::LoadOffsets() {
#undef OFFSET_DEFAULT
#undef OFFSET_EMPTY
#undef OFFSET_WINDOWS
#undef OFFSET_LINUX
#undef SIGSCAN_DEFAULT
#undef SIGSCAN_EMPTY
#undef SIGSCAN_WINDOWS
#undef SIGSCAN_LINUX

#ifdef _WIN32
#define OFFSET_DEFAULT(name, win, linux) name = win;
#define OFFSET_EMPTY(name) name = 0;
#define OFFSET_WINDOWS(name, off) name = off;
#define OFFSET_LINUX(name, off)

#define SIGSCAN_DEFAULT(name, win, linux) name = win;
#define SIGSCAN_EMPTY(name) name = "";
#define SIGSCAN_WINDOWS(name, sig) name = sig;
#define SIGSCAN_LINUX(name, sig)
#else
#define OFFSET_DEFAULT(name, win, linux) name = linux;
#define OFFSET_EMPTY(name) name = 0;
#define OFFSET_WINDOWS(name, off)
#define OFFSET_LINUX(name, off) name = off;

#define SIGSCAN_DEFAULT(name, win, linux) name = linux;
#define SIGSCAN_EMPTY(name) name = "";
#define SIGSCAN_WINDOWS(name, sig)
#define SIGSCAN_LINUX(name, sig) name = sig;
#endif

    using namespace Offsets;
    #include "Offsets/Default.hpp"
}

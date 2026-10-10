#ifndef wheat_pattern_hooks_h
#define wheat_pattern_hooks_h
/* Interface between the patched Lua string library and the mod sandbox. */
#include "lua.h"

#if defined(_MSC_VER)
#  define WHEAT_NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#  define WHEAT_NORETURN __attribute__((noreturn))
#else
#  define WHEAT_NORETURN
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* Shared counter of matcher work done during the current sandbox call. Pattern work is
   charged to the SAME pool as VM instructions, so many cheap-ish calls add up. */
lua_Unsigned *wheat_pattern_counter(lua_State *L);
/* How many more steps ONE pattern call may take right now: min(per-call cap, what is
   left in the pool). */
lua_Unsigned wheat_pattern_allowance(lua_State *L);
/* Called when the step limit is hit. Trips the sandbox budget and raises a Lua error. */
WHEAT_NORETURN void wheat_pattern_exhausted(lua_State *L);
/* Replacement for luaopen_string (same library, step-limited matcher). */
int wheat_luaopen_string(lua_State *L);
#ifdef __cplusplus
}
#endif
#endif

// Lua's own tarball does not ship lua.hpp (only the distro packages add it).
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

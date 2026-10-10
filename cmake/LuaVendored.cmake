# Builds Lua 5.4.8 from source, MINIMALLY: the io, os, debug and package libraries
# (and the lua/luac front ends) are never even compiled. lstrlib.c is excluded too:
# the sandbox uses its own patched copy (third_party/lua_strlib).
include(FetchContent)
FetchContent_Declare(lua_src
  URL      https://github.com/lua/lua/archive/refs/tags/v5.4.8.tar.gz
  URL_HASH SHA256=d85b70a65f43c5d2254944d58d625e822c8e2e10d9c6a3bd9b5b657e46376a19
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(lua_src)

set(_names
  lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser
  lstate lstring ltable ltm lundump lvm lzio
  lauxlib lbaselib lcorolib ltablib lmathlib lutf8lib)
set(_srcs "")
foreach(n ${_names})
  list(APPEND _srcs ${lua_src_SOURCE_DIR}/${n}.c)
endforeach()

add_library(lua54_vendored STATIC ${_srcs})
set_target_properties(lua54_vendored PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C)
target_include_directories(lua54_vendored PUBLIC
  ${lua_src_SOURCE_DIR} ${CMAKE_CURRENT_LIST_DIR}/../third_party/lua_vendor)
set(LUA_TARGET lua54_vendored)
message(STATUS "Using vendored Lua 5.4.8 (minimal build)")

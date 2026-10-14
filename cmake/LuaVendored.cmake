include(FetchContent)
FetchContent_Declare(luasrc
  URL      https://github.com/lua/lua/archive/refs/tags/v5.4.8.tar.gz
  URL_HASH SHA256=d85b70a65f43c5d2254944d58d625e822c8e2e10d9c6a3bd9b5b657e46376a19
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(luasrc)

set(luaFiles
  lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser
  lstate lstring ltable ltm lundump lvm lzio
  lauxlib lbaselib lcorolib ltablib lmathlib lutf8lib)
set(luaSources "")
foreach(file ${luaFiles})
  list(APPEND luaSources ${luasrc_SOURCE_DIR}/${file}.c)
endforeach()

add_library(lua54Vendored STATIC ${luaSources})
set_target_properties(lua54Vendored PROPERTIES C_STANDARD 99 LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
target_include_directories(lua54Vendored PUBLIC
  ${luasrc_SOURCE_DIR} ${CMAKE_CURRENT_LIST_DIR}/../third_party/lua_vendor)
set(luaTarget lua54Vendored)
message(STATUS "Using vendored Lua 5.4.8 (minimal build)")

cmake_minimum_required(VERSION 3.20)
project(modloader_sandbox C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This code targets Lua 5.4 EXACTLY (5.5 changed parts of the C API).
#   Arch/CachyOS: sudo pacman -S lua54 pkgconf     Debian/Ubuntu: apt install liblua5.4-dev pkg-config
#   Windows:      vcpkg install lua  (5.4.x)
find_package(PkgConfig QUIET)
set(LUA_TARGET_OK FALSE)
if(PkgConfig_FOUND)
  foreach(mod lua5.4 lua54 lua-5.4)
    pkg_check_modules(LUA54 QUIET IMPORTED_TARGET ${mod})
    if(LUA54_FOUND)
      set(LUA_TARGET PkgConfig::LUA54)
      set(LUA_TARGET_OK TRUE)
      message(STATUS "Using Lua 5.4 via pkg-config (${mod})")
      break()
    endif()
  endforeach()
endif()
if(NOT LUA_TARGET_OK)
  find_package(Lua 5.4 EXACT REQUIRED)
  add_library(lua54_fallback INTERFACE)
  target_include_directories(lua54_fallback INTERFACE ${LUA_INCLUDE_DIR})
  target_link_libraries(lua54_fallback INTERFACE ${LUA_LIBRARIES})
  set(LUA_TARGET lua54_fallback)
endif()

set(STRLIB_DIR third_party/lua_strlib)
set_source_files_properties(${STRLIB_DIR}/lstrlib_patched.c PROPERTIES
  COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS;_XOPEN_SOURCE=600;_LARGEFILE_SOURCE;_FILE_OFFSET_BITS=64"
  LANGUAGE C)
add_library(lua_sandbox STATIC src/lua_sandbox.cpp ${STRLIB_DIR}/lstrlib_patched.c)
target_include_directories(lua_sandbox PUBLIC include PRIVATE ${STRLIB_DIR})
target_link_libraries(lua_sandbox PUBLIC ${LUA_TARGET})
if(MSVC)
  target_compile_options(lua_sandbox PRIVATE /W4)
else()
  target_compile_options(lua_sandbox PRIVATE -Wall -Wextra)
endif()

add_executable(sandbox_tests tests/sandbox_tests.cpp)
target_link_libraries(sandbox_tests PRIVATE lua_sandbox)

enable_testing()
add_test(NAME sandbox COMMAND sandbox_tests)

add_executable(gate_tests tests/gate_tests.cpp)
target_include_directories(gate_tests PRIVATE include)
add_test(NAME gate COMMAND gate_tests)

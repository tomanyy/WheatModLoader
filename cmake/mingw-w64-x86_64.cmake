# Cross-compile for 64-bit Windows (what CS2 under Proton runs) from Linux.
#   Arch/CachyOS: sudo pacman -S mingw-w64-gcc
#   Debian/Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

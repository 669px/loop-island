# Cross-compile from Linux with the distribution's MinGW-w64 GCC
# (Debian/Ubuntu: g++-mingw-w64-x86-64-posix, Fedora: mingw64-gcc-c++, Arch: mingw-w64-gcc).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_p x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${_p}-gcc)
set(CMAKE_CXX_COMPILER ${_p}-g++)
set(CMAKE_RC_COMPILER ${_p}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${_p})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

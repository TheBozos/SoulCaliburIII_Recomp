# Cross build for Windows from Linux with a mingw-w64 GCC targeting the UCRT, the compiler MSYS2 UCRT64 and TMB's kit use:
#   MINGW_ROOT=<folder with bin/x86_64-w64-mingw32-g++> cmake -S . -B <build> -G Ninja --toolchain cmake/mingw-cross.cmake
#       -DSC3_BOZO=ON -DSC3_HOST_TOOLS=<native bozo-externals and bozo-recomp> -DBOZO_FFMPEG_ROOT=<FFmpeg> -DBOZO_BUILD_TOOLS=OFF ...
# Clang (zig c++, llvm-mingw) does not work: on Win64 __builtin_longjmp sets rbp to the frame address __builtin_setjmp stored,
# not to the frame pointer, and BozoEngine's stops depend on it.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(sc3_mingw "$ENV{MINGW_ROOT}/bin/x86_64-w64-mingw32")
set(CMAKE_C_COMPILER "${sc3_mingw}-gcc")
set(CMAKE_CXX_COMPILER "${sc3_mingw}-g++")
set(CMAKE_RC_COMPILER "${sc3_mingw}-windres")
set(CMAKE_AR "${sc3_mingw}-ar")
set(CMAKE_RANLIB "${sc3_mingw}-ranlib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

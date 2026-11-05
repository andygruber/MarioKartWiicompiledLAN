# CMake toolchain file for cross-compiling the Windows build on Linux with the Linux-hosted
# llvm-mingw in Dockerfile.windows (installed at /opt/llvm-mingw).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER /opt/llvm-mingw/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER /opt/llvm-mingw/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER /opt/llvm-mingw/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /opt/llvm-mingw/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Case-corrected aliases for mixed-case Windows header names (see mingw-case-headers.sh).
set(MKW_MINGW_CASE_INCLUDE_DIR "/work/.release-home/mingw-case-include")
set(CMAKE_C_FLAGS_INIT "-idirafter ${MKW_MINGW_CASE_INCLUDE_DIR}")
set(CMAKE_CXX_FLAGS_INIT "-idirafter ${MKW_MINGW_CASE_INCLUDE_DIR}")

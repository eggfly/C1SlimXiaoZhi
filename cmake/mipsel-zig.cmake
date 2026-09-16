# CMake toolchain: C1 Slim / MP-D261 device target.
#
# Produces ELF32 MIPS little-endian, o32 ABI, MIPS32r2, hard float (double
# precision, 32-bit FPRs), statically linked. Those are exactly the ELF
# attributes of the factory `mpenMain` binary; see docs/PLAN.md.
#
# Requires zig 0.14+ on PATH. The wrappers in cmake/bin pin the target so that
# CMake, opus and mbedtls all agree on it.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

get_filename_component(_c1xz_bin "${CMAKE_CURRENT_LIST_DIR}/bin" ABSOLUTE)

set(CMAKE_C_COMPILER   "${_c1xz_bin}/zig-cc")
set(CMAKE_CXX_COMPILER "${_c1xz_bin}/zig-cxx")
set(CMAKE_AR           "${_c1xz_bin}/zig-ar"     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${_c1xz_bin}/zig-ranlib" CACHE FILEPATH "" FORCE)

# zig cc understands the GNU driver interface.
set(CMAKE_C_COMPILER_ID   GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Static everywhere. Nothing on the device resolves our libraries for us.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# -Oz keeps the binary small; the device has ~50 MiB of RAM and a read-only
# 400 MB rootfs. Section GC drops the parts of opus/mbedtls we never call.
set(CMAKE_C_FLAGS_INIT   "-ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# zig cc cannot run the produced binaries on the build host.
set(CMAKE_CROSSCOMPILING ON)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

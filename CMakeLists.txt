cmake_minimum_required(VERSION 3.20)
project(c1xiaozhi C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

# DEVICE: the real C1 Slim target (MIPS, tinyalsa, e-paper, evdev).
# HOST:   native build used by the unit tests; audio and display are stubs.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "mipsel")
    set(C1XZ_DEVICE ON)
else()
    set(C1XZ_DEVICE OFF)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS ${TP}/opus/CMakeLists.txt)
    message(FATAL_ERROR "third_party is empty. Run tools/fetch-deps.sh first.")
endif()

# ---------------------------------------------------------------- third party

# Fixed point: the XBurst1 core has an FPU but SILK's fixed-point path is the
# one that gets the MIPS-specific optimisations in silk/mips and celt/mips.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API ON CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/opus opus EXCLUDE_FROM_ALL)

set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/mbedtls mbedtls EXCLUDE_FROM_ALL)

add_library(cjson STATIC ${TP}/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC ${TP}/cJSON)

if(C1XZ_DEVICE)
    add_library(tinyalsa STATIC
        ${TP}/tinyalsa/src/pcm.c
        ${TP}/tinyalsa/src/pcm_hw.c
        ${TP}/tinyalsa/src/pcm_plugin.c
        ${TP}/tinyalsa/src/mixer.c
        ${TP}/tinyalsa/src/mixer_hw.c
        ${TP}/tinyalsa/src/mixer_plugin.c
        ${TP}/tinyalsa/src/limits.c
        ${TP}/tinyalsa/src/snd_card_plugin.c)
    target_include_directories(tinyalsa PUBLIC ${TP}/tinyalsa/include)
    # tinyalsa ships its own copy of the ALSA uapi headers, so no kernel
    # headers are needed on the build host.
    target_compile_definitions(tinyalsa PRIVATE _GNU_SOURCE)
endif()

# ------------------------------------------------------------------- CA bundle

# Compiled in rather than read from the device: the C1 Slim rootfs has no
# trust store, and the factory HTTPS client does not verify certificates at all.
if(NOT DEFINED C1XZ_CA_BUNDLE)
    set(C1XZ_CA_BUNDLE ${TP}/cacert.pem)
endif()
if(NOT EXISTS ${C1XZ_CA_BUNDLE})
    message(FATAL_ERROR "CA bundle not found at ${C1XZ_CA_BUNDLE}. Run tools/fetch-deps.sh.")
endif()
file(READ ${C1XZ_CA_BUNDLE} C1XZ_CA_PEM)
string(LENGTH "${C1XZ_CA_PEM}" C1XZ_CA_PEM_LEN)
# A raw string literal keeps the PEM readable and needs no escaping; the
# delimiter cannot occur inside base64 or the PEM armour.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ca_bundle.cc.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle.cc @ONLY)
message(STATUS "CA bundle: ${C1XZ_CA_BUNDLE} (${C1XZ_CA_PEM_LEN} bytes)")

# --------------------------------------------------------------------- sources

set(CORE_SRC
    src/platform/log.cc
    src/platform/event_loop.cc
    src/settings.cc
    src/system_info.cc
    src/net/tls_socket.cc
    src/net/http_client.cc
    src/net/websocket_client.cc
    src/net/mqtt_client.cc
    src/net/udp_socket.cc
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle.cc
    src/protocols/protocol.cc
    src/protocols/websocket_protocol.cc
    src/protocols/mqtt_protocol.cc
    src/audio/opus_codec.cc
    src/audio/audio_service.cc
    src/audio/ogg_demuxer.cc
    src/device_state_machine.cc
    src/mcp_server.cc
    src/ota.cc
    src/application.cc
    src/display/display.cc
    src/display/gfx.cc
    src/display/font.cc
    src/boards/board.cc)

if(C1XZ_DEVICE)
    list(APPEND CORE_SRC
        src/audio/tinyalsa_audio_codec.cc
        src/display/epaper_display.cc
        src/boards/c1_slim/c1_slim_board.cc
        src/boards/c1_slim/input_evdev.cc
        src/boards/c1_slim/launcher_lease.cc
        src/boards/c1_slim/wifi_status.cc
        src/boards/c1_slim/battery_sysfs.cc)
else()
    list(APPEND CORE_SRC
        src/audio/null_audio_codec.cc
        src/display/console_display.cc
        src/boards/host/host_board.cc)
endif()

add_library(c1xz_core STATIC ${CORE_SRC})
target_include_directories(c1xz_core PUBLIC src assets)
target_link_libraries(c1xz_core PUBLIC
    opus cjson MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
if(C1XZ_DEVICE)
    target_link_libraries(c1xz_core PUBLIC tinyalsa)
    target_compile_definitions(c1xz_core PUBLIC C1XZ_DEVICE=1)
else()
    target_compile_definitions(c1xz_core PUBLIC C1XZ_DEVICE=0)
endif()
find_package(Threads REQUIRED)
target_link_libraries(c1xz_core PUBLIC Threads::Threads)

if(NOT DEFINED C1XZ_VERSION)
    set(C1XZ_VERSION "0.1.0")
endif()
target_compile_definitions(c1xz_core PUBLIC C1XZ_VERSION="${C1XZ_VERSION}")
# Left empty by default so builds are reproducible; pass -DC1XZ_BUILD_TIME=...
# to stamp a release.
if(NOT DEFINED C1XZ_BUILD_TIME)
    set(C1XZ_BUILD_TIME "")
endif()
target_compile_definitions(c1xz_core PUBLIC C1XZ_BUILD_TIME="${C1XZ_BUILD_TIME}")

add_executable(c1xiaozhi src/main.cc)
target_link_libraries(c1xiaozhi PRIVATE c1xz_core)
if(C1XZ_DEVICE)
    target_link_options(c1xiaozhi PRIVATE -static -Wl,--gc-sections -s)
endif()

# ----------------------------------------------------------------------- tests

if(NOT C1XZ_DEVICE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()

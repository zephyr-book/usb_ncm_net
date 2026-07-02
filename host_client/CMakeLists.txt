# =============================================================================
# usb_ncm_net -- host EDHOC + OSCORE client (macOS/Linux).
#
# The EDHOC Initiator + OSCORE client counterpart of the ZBook firmware, built
# from the SAME libraries the device runs -- libedhoc (EDHOC, cipher suite 0 /
# X25519) + uoscore-uedhoc's OSCORE half -- so interop is guaranteed at the
# encoder level. Sources come from the west-managed deps/ tree; crypto (PSA:
# AES-CCM / SHA-256 / HKDF / RNG) is provided by Homebrew mbedtls@3. CoAP over
# UDP uses cantcoap + the sample socket helpers, adapted from
# uoscore-uedhoc/samples/linux_edhoc_oscore/initiator_client.
#
# libedhoc and uoscore-uedhoc BOTH ship a flat <edhoc.h> and each of their own
# sources #include "edhoc.h" expecting their own; they are therefore compiled as
# separate static libraries, each with its own include order (mirrors the
# device's per-target include fix). Only the executable's client.cpp pulls in
# libedhoc's <edhoc.h> + uoscore's <oscore.h> (distinct names, no clash).
#
# Build (from this dir):  cmake -S . -B build && cmake --build build
#   ./build/edhoc_oscore_client            # default host 192.0.2.1
# Regenerate creds first with `just keys` (writes edhoc_creds_client.h here).
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(edhoc_oscore_client LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-ffunction-sections -fdata-sections -Wno-unused)

get_filename_component(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/modules ABSOLUTE)
set(LIBEDHOC   ${DEPS}/lib/libedhoc)
set(COMPACT    ${DEPS}/lib/compact25519)
set(UOSCORE    ${DEPS}/lib/uoscore-uedhoc)
set(ZCBOR      ${DEPS}/lib/zcbor)
set(CANTCOAP   ${UOSCORE}/externals/cantcoap)
set(SAMPLE_COMMON ${UOSCORE}/samples/common)

# --- Host crypto: Homebrew mbedtls@3 (PSA) -----------------------------------
set(MBEDTLS_PREFIX "/opt/homebrew/opt/mbedtls@3" CACHE PATH "mbedtls@3 install prefix")
find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED HINTS ${MBEDTLS_PREFIX}/lib)

# --- zcbor (shared by libedhoc's CBOR backend and uoscore) -------------------
add_library(zcbor STATIC
  ${ZCBOR}/src/zcbor_common.c ${ZCBOR}/src/zcbor_decode.c ${ZCBOR}/src/zcbor_encode.c)
target_include_directories(zcbor PUBLIC ${ZCBOR}/include)
target_compile_definitions(zcbor PUBLIC ZCBOR_CANONICAL)

# --- libedhoc (EDHOC engine, suite 0 helper, compact25519 X25519) ------------
file(GLOB LIBEDHOC_SRC ${LIBEDHOC}/library/*.c ${LIBEDHOC}/backends/cbor/src/*.c)
file(GLOB COMPACT_SRC ${COMPACT}/src/*.c ${COMPACT}/src/c25519/*.c)
add_library(libedhoc STATIC
  ${LIBEDHOC_SRC} ${LIBEDHOC}/helpers/src/edhoc_cipher_suite_0.c ${COMPACT_SRC})
target_include_directories(libedhoc PUBLIC
  ${LIBEDHOC}/include ${LIBEDHOC}/helpers/include
  ${LIBEDHOC}/backends/cbor/include ${LIBEDHOC}/backends/memory/include
  ${LIBEDHOC}/backends/log/include ${COMPACT}/src
  ${CMAKE_CURRENT_SOURCE_DIR}          # edhoc_config.h (non-Zephyr build config)
  ${MBEDTLS_PREFIX}/include)
target_link_libraries(libedhoc PUBLIC zcbor)

# --- uoscore-uedhoc OSCORE half (its own inc/ first for its flat edhoc.h) ----
file(GLOB UOSCORE_SRC ${UOSCORE}/src/oscore/*.c ${UOSCORE}/src/cbor/oscore_*.c)
add_library(uoscore STATIC
  ${UOSCORE_SRC}
  ${UOSCORE}/src/common/byte_array.c ${UOSCORE}/src/common/crypto_wrapper.c
  ${UOSCORE}/src/common/memcpy_s.c ${UOSCORE}/src/common/print_util.c)
target_include_directories(uoscore PUBLIC ${UOSCORE}/inc ${MBEDTLS_PREFIX}/include)
target_compile_definitions(uoscore PUBLIC MBEDTLS)
target_link_libraries(uoscore PUBLIC zcbor)

# --- The client executable ---------------------------------------------------
# libedhoc's headers listed first so client.cpp's <edhoc.h> is libedhoc's.
add_executable(edhoc_oscore_client
  src/client.cpp ${CANTCOAP}/cantcoap.cpp
  ${SAMPLE_COMMON}/sock.c ${SAMPLE_COMMON}/sock_ipv4.c ${SAMPLE_COMMON}/sock_ipv6.c)
target_include_directories(edhoc_oscore_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CANTCOAP} ${SAMPLE_COMMON})
target_compile_definitions(edhoc_oscore_client PRIVATE LINUX_SOCKETS)
target_link_libraries(edhoc_oscore_client PRIVATE libedhoc uoscore zcbor ${MBEDCRYPTO_LIB})

if(APPLE)
  target_link_options(edhoc_oscore_client PRIVATE -Wl,-dead_strip)
else()
  target_link_options(edhoc_oscore_client PRIVATE -Wl,--gc-sections)
endif()

cmake_minimum_required(VERSION 3.20.0)

# The board (zbook/rp2350b/m33) comes from the `zbook` Zephyr module declared in
# west.yml — no BOARD_ROOT here. Select it at build time with `-b zbook/rp2350b/m33`.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_ncm_net LANGUAGES C)

include_directories(./include)

file(GLOB_RECURSE SRC ${CMAKE_SOURCE_DIR} "src/*.c")
target_sources(app PRIVATE ${SRC})

# The CoAP service "coap_server" (src/coap_server.c) collects its resources into a
# per-service iterable section named coap_resource_<service>. Register it for both
# linker backends so COAP_SERVICE_DEFINE() finds the list start/end symbols.
zephyr_linker_sources(DATA_SECTIONS sections-ram.ld)
zephyr_iterable_section(
  NAME coap_resource_coap_server
  GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})

# EDHOC engine = libedhoc (oscore build only, CONFIG_LIBEDHOC_ENABLE). The
# Zephyr module compiles libedhoc's core + CBOR backend, but the cipher-suite
# crypto helper is meant to be compiled by the application (it is not part of the
# module library). We use cipher suite 0 (X25519): its helper does ECDH via the
# compact25519 library (no mbedTLS ECP, no point decompression) and AES-CCM /
# SHA-256 / HKDF via PSA.
if(CONFIG_LIBEDHOC_ENABLE)
  set(COMPACT25519_DIR ${ZEPHYR_LIBEDHOC_MODULE_DIR}/../compact25519)
  file(GLOB COMPACT25519_SRC
    ${COMPACT25519_DIR}/src/*.c
    ${COMPACT25519_DIR}/src/c25519/*.c)
  target_sources(app PRIVATE
    ${ZEPHYR_LIBEDHOC_MODULE_DIR}/helpers/src/edhoc_cipher_suite_0.c
    ${COMPACT25519_SRC})
  # Prepend libedhoc's public + helper headers (so <edhoc.h> in src/edhoc_oscore.c
  # resolves to libedhoc, not uoscore-uedhoc's inc/edhoc.h, which is also on the
  # global path and is still linked for OSCORE) and compact25519's src/ (so the
  # suite-0 helper finds <compact_x25519.h> / <c25519/c25519.h>).
  target_include_directories(app BEFORE PRIVATE
    ${ZEPHYR_LIBEDHOC_MODULE_DIR}/include
    ${ZEPHYR_LIBEDHOC_MODULE_DIR}/helpers/include
    ${COMPACT25519_DIR}/src)
  # PSA_KEY_HANDLE_INIT was removed from this PSA; the suite-0 helper still uses
  # it as a null key-id initialiser. Map it to the current spelling.
  target_compile_definitions(app PRIVATE PSA_KEY_HANDLE_INIT=PSA_KEY_ID_NULL)

  # Same flat-edhoc.h clash, other direction: uoscore-uedhoc's own sources
  # #include "edhoc.h" expecting *its* inc/edhoc.h (which defines WEAK etc.), but
  # libedhoc's include/edhoc.h is also on the global include path. Prepend
  # uoscore's inc/ to its module libraries so they resolve to uoscore's header.
  foreach(uoscore_tgt uoscore_uedhoc_common uoscore)
    if(TARGET ${uoscore_tgt})
      target_include_directories(${uoscore_tgt} BEFORE PRIVATE
        ${ZEPHYR_UOSCORE_UEDHOC_MODULE_DIR}/inc)
    endif()
  endforeach()
endif()

# The RP2350 Pico-SDK RNG state lives in an ".uninitialized_data.*" section.
# Zephyr's template places a ".uninitialized_data (NOLOAD)" section in the middle
# of the RAM data region, before usbd_context; once the RNG fills it, that NOLOAD
# gap misaligns the startup .data copy and corrupts usbd_context (USB init fails
# with -EPERM). Claim those inputs into .data (RWDATA, earlier in link order) so
# the gap never forms. See sections-rwdata.ld.
zephyr_linker_sources(RWDATA sections-rwdata.ld)

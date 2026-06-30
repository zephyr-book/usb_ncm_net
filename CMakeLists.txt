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

# The RP2350 Pico-SDK RNG state lives in an ".uninitialized_data.*" section.
# Zephyr's template places a ".uninitialized_data (NOLOAD)" section in the middle
# of the RAM data region, before usbd_context; once the RNG fills it, that NOLOAD
# gap misaligns the startup .data copy and corrupts usbd_context (USB init fails
# with -EPERM). Claim those inputs into .data (RWDATA, earlier in link order) so
# the gap never forms. See sections-rwdata.ld.
zephyr_linker_sources(RWDATA sections-rwdata.ld)

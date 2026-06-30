/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The RP2350 Pico-SDK RNG (entropy driver, used here for CoAP) keeps its state
 * in an ".uninitialized_data.*" section via __uninitialized_ram(). Zephyr's
 * linker template defines a ".uninitialized_data (NOLOAD)" output section in the
 * middle of the RAM data region, *before* the device/usbd_context iterable
 * sections. When the Pico RNG populates it, that NOLOAD gap (no flash bytes)
 * misaligns the contiguous startup .data copy and corrupts everything after it,
 * including usbd_context -- so USB init fails (usbd_add_descriptor() -> -EPERM).
 *
 * Claim those input sections here, inside the .data output section (RWDATA),
 * which precedes the template's NOLOAD section in link order. The RNG state then
 * becomes ordinary loaded data (zero-initialized at boot, which is fine), the
 * template's NOLOAD section stays empty, and the .data copy is contiguous again.
 */

*(.uninitialized_data*)

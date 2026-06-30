# usb_ncm_net — CoAP server over a USB-NCM link

Bring-up firmware for the **ZBook** board (Raspberry Pi **RP2350B**, Cortex-**M33**)
that exposes the board to a host computer as a **USB CDC-NCM** virtual Ethernet
adapter and runs a **CoAP server** over that link. Tested against a macOS-ARM host.

The board takes the static IPv4 address **`192.0.2.1`**; the host configures its end
of the link (**`192.0.2.2`**) manually. A single `hello` CoAP resource plus the
standard `.well-known/core` discovery resource are served on UDP port **5683**.

This app is a sibling of `esp01_flasher` — same west workspace layout (T2), same
USB device_next stack, same OpenOCD flash flow.

## Architecture

- **USB**: the RP2350 native USB device controller (`&usbd`, left disabled by the
  board) is enabled in `app.overlay`, with a `zephyr,cdc-ncm-ethernet` function
  attached. `src/usbd_setup.c` builds the device_next context (single full-speed
  config, CDC-NCM class, IAD code triple) and `src/main.c` enables it.
- **Networking**: `CONFIG_NET_CONFIG_SETTINGS` assigns `192.0.2.1/24` to the NCM
  Ethernet interface at boot. No DHCP — the host is configured by hand.
- **CoAP**: `src/coap_server.c` declares the service with `COAP_SERVICE_DEFINE(...,
  COAP_SERVICE_AUTOSTART)`, which runs its own RX/TX thread and binds UDP :5683.
  Resources are declared at file scope with `COAP_RESOURCE_DEFINE`. `main()` does
  no socket work.
- **Console**: logging + the `net` and `coap_server` shells are on UART0
  (115200 baud) for diagnostics.

## Build & flash

Run from inside this directory.

```bash
west build -p always -b zbook/rp2350b/m33      # or: just build
west flash                                     # or: just flash
```

`-p always` is required when changing board/shield options on an existing `build/`.

**After flashing, reset the board (button or power-cycle).** Flashing over SWD
resets the MCU without a clean USB detach, so macOS keeps stale enumeration state
and never activates the NCM data interface. A real reset makes the host
re-enumerate and bring the link up. Each reset re-enumerates USB, so re-apply the
host IP (below) afterwards.

## Connect from macOS

1. Plug the board's USB port into the Mac. Find the new interface:
   ```bash
   networksetup -listallhardwareports     # look for a "USB ..." device -> en<N>
   # or diff `ifconfig` output before/after plugging in
   ```
2. Give the host its address on the link and bring the interface up:
   ```bash
   sudo ifconfig en<N> 192.0.2.2 255.255.255.0 up
   ping 192.0.2.1
   ```
3. Talk to the CoAP server (libcoap: `brew install libcoap`):
   ```bash
   coap-client -m get coap://192.0.2.1/hello              # -> "Hello from ZBook over USB-NCM"
   coap-client -m get coap://192.0.2.1/.well-known/core   # resource discovery
   ```
   Python alternative: `pip install aiocoap` then
   `aiocoap-client coap://192.0.2.1/hello`.

On the board console you can verify the link with `net iface` (shows the NCM
interface UP at 192.0.2.1) and `coap_server` (lists the running service).

## Adding resources

Add a new `COAP_RESOURCE_DEFINE(<name>, coap_server, { .get = <handler>, .path =
<path_array> })` block in `src/coap_server.c` (or a new `src/*.c` — `CMakeLists.txt`
globs `src/*.c`). Each handler builds a `coap_packet` response and sends it with
`coap_resource_send()`.

## Standalone checkout

This repo carries its own `west.yml` manifest (Zephyr v4.4.0 + the `zbook` board
module). To use it outside the shared workspace:

```bash
west init -m <repo-url> --mr main <workspace-dir>
cd <workspace-dir>/usb_ncm_net
west update
```

# usb_ncm_net — CoAP server over a USB-NCM link

Bring-up firmware for the **ZBook** board (Raspberry Pi **RP2350B**, Cortex-**M33**)
that exposes the board to a host computer as a **USB CDC-NCM** virtual Ethernet
adapter and runs a **CoAP server** over that link. Tested against a macOS-ARM host.

The board takes the static IPv4 address **`192.0.2.1`**; the host configures its end
of the link (**`192.0.2.2`**) manually. CoAP resources on UDP port **5683**:

- `hello` — GET, returns a greeting string.
- `led` — GET returns the board LED state (`0`/`1`); PUT `0`/`1` (or `off`/`on`)
  sets it. Drives the `led0` alias (blue LED).
- `.well-known/core` — standard resource discovery (dev build only; disabled in
  the minimal profile).

This app is a sibling of `esp01_flasher` — same west workspace layout (T2), same
USB device_next stack, same OpenOCD flash flow.

## Architecture

- **USB**: the RP2350 native USB device controller (`&usbd`, left disabled by the
  board) is enabled in `app.overlay`, with a `zephyr,cdc-ncm-ethernet` function
  attached. `src/usbd_setup.c` builds the device_next context (single full-speed
  config, CDC-NCM class, IAD code triple) and `src/main.c` enables it.
- **Networking**: IPv4-only, UDP-only — no IPv6, no TCP, no DHCP, no connection
  manager. `main()` assigns the static address `192.0.2.1/24` directly with
  `net_if_ipv4_addr_add()` (no net-config layer); the host is configured by hand.
  (`NET_MGMT` is pulled in unconditionally by the Ethernet L2 that NCM requires,
  but nothing here uses it.)
- **CoAP**: `src/coap_server.c` declares the service with `COAP_SERVICE_DEFINE(...,
  COAP_SERVICE_AUTOSTART)`, which runs its own RX/TX thread and binds UDP :5683.
  Resources are declared at file scope with `COAP_RESOURCE_DEFINE`. `main()` does
  no socket work.
- **Console** (UART0, 115200 baud): the **dev** profile has logging + the `net`,
  `coap_server` and general shells. The **minimal** profile drops the `net` and
  `coap_server` shells (the `net` shell alone pulls in ~14 KB flash and
  force-selects IGMP/IP-options) but keeps a bare `CONFIG_SHELL` console +
  logging.

## Build & flash

Run from inside this directory. Two profiles:

```bash
# Development baseline (shell + logging, easy to debug):
west build -p always -b zbook/rp2350b/m33                              # or: just build
west flash                                                             # or: just flash

# Minimal networking-stack profile (single CoAP client):
west build -p always -b zbook/rp2350b/m33 -d build-min \
      -- -DEXTRA_CONF_FILE=minimal.conf                                # or: just build-min
west flash -d build-min                                                # or: just flash-min
```

`minimal.conf` layers on top of `prj.conf` for a production, single-CoAP-client
build: it trims the net pools/contexts (`NET_MAX_CONTEXTS=1`), turns off IPv4
features unused by unicast CoAP (IGMP, IP header options, DSCP/ECN, gratuitous
ARP), trims the CoAP knobs (no `.well-known/core`, 64-byte blocks, no observers,
one pending message), and drops the `net` + `coap_server` shells. It keeps a bare
console + logging. On `zbook/rp2350b/m33` this takes the image from
192,396 / 53,904 B (flash/RAM) to **153,988 / 39,704** — see the doc below for
the per-layer breakdown. `-p always` is required when changing board/shield
options on an existing build dir.

See [`docs/memory-footprint.md`](docs/memory-footprint.md) for a measured
per-layer ROM/RAM breakdown — the cost of "network over USB" and the CoAP stack
isolated separately, and a side-by-side against the USB-ACM + UART approach.
Regenerate the underlying reports with `just footprint`.

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

   coap-client -m put -e 1 coap://192.0.2.1/led           # LED on  (-> 2.04 Changed)
   coap-client -m put -e 0 coap://192.0.2.1/led           # LED off
   coap-client -m get coap://192.0.2.1/led                # -> "1" or "0"
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

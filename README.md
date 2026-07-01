# usb_ncm_net — CoAP server over a USB-NCM link

Bring-up firmware for the **ZBook** board (Raspberry Pi **RP2350B**, Cortex-**M33**)
that exposes the board to a host computer as a **USB CDC-NCM** virtual Ethernet
adapter and runs a **CoAP server** over that link. Tested against a macOS-ARM host.

The board takes the static IPv4 address **`192.0.2.1`**; the host configures its end
of the link (**`192.0.2.2`**) manually. Two builds:

- **base** (`just build`) — plaintext CoAP on UDP **5683**.
- **DTLS** (`just build-dtls`) — CoAPS (DTLS 1.2, PSK auth) on UDP **5684**.

CoAP resources:

- `hello` — GET, returns a greeting string.
- `led` — GET returns the board LED state (`0`/`1`); PUT `0`/`1` (or `off`/`on`)
  sets it. Drives the `led0` alias (blue LED).

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
- **CoAP**: `src/coap_server.c` declares the service; the plaintext build uses
  `COAP_SERVICE_DEFINE(..., COAP_SERVICE_AUTOSTART)` (own RX/TX thread, binds UDP
  :5683). The DTLS build (`CONFIG_NET_SOCKETS_ENABLE_DTLS`) uses
  `COAPS_SERVICE_DEFINE`, registers the PSK, and starts CoAPS on :5684 from
  `coap_server_start()` (called by `main()`). Resources are file-scope
  `COAP_RESOURCE_DEFINE` blocks, shared by both.
- **Console** (UART0, 115200 baud): a bare `CONFIG_SHELL` console + logging. No
  `net`/`coap_server` shell commands (the `net` shell alone pulls ~14 KB flash and
  force-selects IGMP/IP-options).

## Build & flash

Run from inside this directory. `prj.conf` is the minimal base; `dtls.conf` is an
overlay for the CoAPS variant.

```bash
# Base: plaintext CoAP (UDP 5683)
west build -p always -b zbook/rp2350b/m33                              # or: just build
west flash                                                             # or: just flash

# DTLS/CoAPS variant: DTLS 1.2 + PSK, CoAPS on UDP 5684
west build -p always -b zbook/rp2350b/m33 -d build-dtls \
      -- -DEXTRA_CONF_FILE=dtls.conf -DMBEDTLS_FATAL_WARNINGS=OFF       # or: just build-dtls
west flash -d build-dtls                                               # or: just flash-dtls
```

`dtls.conf` layers mbedTLS 4.x (+ tf-psa-crypto) and turns the CoAP server into a
PSK-authenticated CoAPS server (`TLS_PSK_WITH_AES_128_CCM_8` — CoAP's RFC 7252
mandatory-to-implement suite; dev identity/key in `src/coap_server.c` — replace
for production). `-DMBEDTLS_FATAL_WARNINGS=OFF`
silences `-Werror` warnings in upstream tf-psa-crypto code. The DTLS build needs
the `mbedtls` + `tf-psa-crypto`
west modules (already in `west.yml`; run `west update` if not fetched).

See [`docs/benchmarks.md`](docs/benchmarks.md) for measured memory (per-layer
ROM/RAM, the cost of "network over USB" and the CoAP stack isolated, side-by-side
against the USB-ACM + UART approach) and performance (CoAP round-trip latency).
Regenerate with `just footprint` and `python3 scripts/coap_latency.py`.

Normal builds keep the shell + INFO logging on. For **latency measurement**, flash
the perf build instead (`just build-perf`, or `just build-dtls-perf` for CoAPS) —
it layers `perf.conf` to disable the shell and logging, whose synchronous UART I/O
would otherwise skew the timing.

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

### Running the DTLS / CoAPS variant

The `dtls.conf` build serves **CoAPS (DTLS 1.2) on UDP 5684**, authenticated with
a pre-shared key (`TLS_PSK_WITH_AES_128_CCM_8` — CoAP's RFC 7252
mandatory-to-implement suite, and the leanest AEAD for constrained devices). The
dev PSK is hardcoded in `src/coap_server.c` — replace it before shipping:

- identity: `zbook`
- key: `zbook-dtls-psk!!`  (16-byte ASCII)

> **Use the Python client (`scripts/coap_cli.py`), not `coap-client`, for DTLS.**
> macOS libcoap `coap-client` (OpenSSL) doesn't put PSK-CCM8 — nor ChaCha20 or
> GCM for PSK — in its ClientHello, and has no cipher-list flag, so `coaps://`
> there fails with `handshake failure`. `coap_cli.py` uses python-mbedtls, which
> negotiates PSK-CCM8 correctly.

1. Build, flash, then reset + set the host IP (same USB-NCM bring-up as above):
   ```bash
   just build-dtls && just flash-dtls        # then a 5 s USB power-cycle
   sudo ifconfig en<N> 192.0.2.2 255.255.255.0 up
   ```
2. Send requests over DTLS (coap-client-style; `pip install python-mbedtls` once):
   ```bash
   just coap --dtls get hello        # -> 2.05 Content 'Hello from ZBook over USB-NCM'
   just coap --dtls get led          # -> 2.05 Content '0' / '1'
   just coap --dtls put led -e 1     # LED on  -> 2.04 Changed
   just coap --dtls put led -e 0     # LED off
   # direct: python3 scripts/coap_cli.py --dtls get hello
   ```
   Plaintext works too — drop `--dtls` (talks to :5683): `just coap get hello`.
3. Latency over DTLS (one handshake, then timed requests):
   ```bash
   python3 scripts/coap_latency.py --dtls
   ```
   For clean numbers, flash the perf DTLS build first (shell + logging off):
   `just build-dtls-perf && just flash-dtls-perf`.

On the board console you can verify the link with `net iface` (shows the NCM
interface UP at 192.0.2.1) and `coap_server` (lists the running service).

### Troubleshooting: `Broken pipe` / ARP `(incomplete)`

If `coap-client` fails with `coap_socket_send: Broken pipe` on every port and
`arp -an | grep 192.0.2.1` shows `(incomplete)`, the board's CoAP server is
almost certainly fine — the **USB data path is down**. Look at the board console:
a loop of

```
<inf> cdc_ncm: USB configuration is not enabled
<inf> cdc_ncm: Cannot send speed change (-16)
```

means the host deconfigured the USB device (typically after repeated SWD
resets/reflashes or toggling the host interface `down`/`up`), so no Ethernet
frames flow and ARP for 192.0.2.1 never resolves.

Fix: **fully unplug the USB cable to drop VBUS, wait ~5 s, replug** — preferably a
direct Mac port, not a hub. A power-cycle forces a clean `SET_CONFIGURATION`
(better than an SWD reset). A healthy re-enumeration ends with `cdc_ncm:
Connected status done` and no repeating error lines. Then set `192.0.2.2` on the
board's `enN` and **don't bring the interface `down` afterward** — that re-drops
the config. When enumerated cleanly the board appears in
`networksetup -listallhardwareports` as **"ZBook USB-NCM CoAP"**.

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

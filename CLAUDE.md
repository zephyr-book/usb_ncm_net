# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code
in this repository.

## What this is

Bring-up firmware for the **ZBook** board (Raspberry Pi **RP2350B**,
Cortex-**M33**) that exposes the board to a host as a **USB CDC-NCM** virtual
Ethernet adapter and runs a **CoAP server** over that link. Tested against a
**macOS-ARM** host.

This is its **own git repo** (`git@github.com:zephyr-book/usb_ncm_net.git`) and its
own Zephyr **west workspace** (T2 topology — this repo carries `west.yml`, Zephyr
**v4.4.0**). It happens to live inside the shared `zephyr-book-workspace/` tree but
is **independent** of the sibling `bring-up-test-p1` app documented by the
workspace-root `CLAUDE.md`. The board target `zbook/rp2350b/m33` comes from the
`zbook` Zephyr module pulled in by `west.yml` — pass it with `-b`; there is **no
`BOARD_ROOT`**.

The board takes static IPv4 **`192.0.2.1`**; the host configures its end
(**`192.0.2.2/24`**) by hand. CoAP resources: `hello` (GET → greeting) and `led`
(GET → `0`/`1`; PUT `0`/`1`/`off`/`on` → drives the `led0` alias, the blue LED,
active-low).

## Working directory

**Run all build/flash/west commands from inside this directory** (`usb_ncm_net/`) —
it holds `west.yml`, `CMakeLists.txt`, `prj.conf`, and the build dirs. `.west/` is
at the workspace root, one level up. After editing `west.yml`, re-run `west update`.

## Build & flash

`prj.conf` is the minimal base; extra configs layer on via `-DEXTRA_CONF_FILE`.
**`-p always` (or a distinct `-d build-<name>`) is required** whenever you change
`-b`/`-S`/`EXTRA_CONF_FILE` on an existing build dir, or the cached value wins. The
`justfile` wraps every command below.

Three security profiles + a perf overlay:

```bash
# base — plaintext CoAP, UDP 5683                                   (just build)
west build -p always -b zbook/rp2350b/m33

# DTLS/CoAPS — DTLS 1.2 + PSK, UDP 5684                             (just build-dtls)
west build -p always -b zbook/rp2350b/m33 -d build-dtls \
      -- -DEXTRA_CONF_FILE=dtls.conf -DMBEDTLS_FATAL_WARNINGS=OFF

# EDHOC + OSCORE — app-layer security over plaintext UDP 5683       (just build-oscore)
just keys                                        # generate creds FIRST (see below)
west build -p always -b zbook/rp2350b/m33 -d build-oscore \
      -- -DEXTRA_CONF_FILE=oscore.conf

west flash [-d build-<name>]                     # OpenOCD/CMSIS-DAP (just flash[-dtls|-oscore])
```

- `-DMBEDTLS_FATAL_WARNINGS=OFF` silences `-Werror` in upstream tf-psa-crypto (not
  our code) — needed for any mbedTLS build (DTLS).
- **perf builds** (`just build-perf`, `just build-dtls-perf`) layer `perf.conf` to
  turn the **shell + logging OFF**; use them for `scripts/coap_latency.py` timing —
  the shell/`LOG_*` do synchronous UART I/O that inflates latency.
- `flash` targets pass a pinned `--openocd`/`--openocd-search` (see `justfile`).
- **After flashing, reset the board (button / VBUS power-cycle).** SWD reset alone
  doesn't cleanly detach USB, so macOS keeps stale enumeration and never activates
  the NCM data interface. Each reset re-enumerates → re-apply the host IP
  (`sudo ifconfig en<N> 192.0.2.2 255.255.255.0 up`). On a config-drop loop
  (`cdc_ncm: USB configuration is not enabled`), **unplug ~5 s to drop VBUS**; do
  **not** bring the host iface `down`. (See global memory `usb-ncm-macos-config-drop`.)

## Architecture

Four source files (`CMakeLists.txt` globs `src/*.c`, so new files need no edit):

- **`src/usbd_setup.c`** — builds the device_next context (single full-speed config,
  CDC-NCM class, IAD triple); the RP2350 native USB controller (`&usbd`, disabled by
  the board) is enabled in `app.overlay` with a `zephyr,cdc-ncm-ethernet` function.
- **`src/main.c`** — enables USB, assigns `192.0.2.1/24` directly via
  `net_if_ipv4_addr_add()` (no net-config layer, no DHCP), and for the DTLS build
  calls `coap_server_start()`.
- **`src/coap_server.c`** — the CoAP service + resources. Plaintext/OSCORE builds use
  `COAP_SERVICE_DEFINE(..., COAP_SERVICE_AUTOSTART)` (own RX/TX thread, binds :5683);
  the DTLS build (`CONFIG_NET_SOCKETS_ENABLE_DTLS`) uses `COAPS_SERVICE_DEFINE`,
  registers the PSK, and starts CoAPS :5684 from `coap_server_start()`. Resources are
  file-scope `COAP_RESOURCE_DEFINE` blocks; both handlers funnel through
  `coap_server_dispatch_inner()` (shared by the plaintext path **and** the OSCORE
  intercept).
- **`src/edhoc_oscore.c`** — EDHOC responder + OSCORE, entirely inside
  `#if defined(CONFIG_LIBEDHOC_ENABLE)` (inert in base/DTLS builds, which lack the
  headers). See next section.

Networking is **IPv4-only, UDP-only** — no IPv6/TCP/DHCP/conn-manager. Console is a
bare `CONFIG_SHELL` + logging on UART0 @115200; the `net`/`coap_server` shells are
**off** (the `net` shell alone costs ~14 KB flash + force-selects IGMP/IP-options).

### The three security variants

| Variant | Conf | Port | Security |
|---|---|---|---|
| base | `prj.conf` | 5683 | none (plaintext CoAP) |
| DTLS | `+dtls.conf` | 5684 | CoAPS DTLS 1.2, **PSK** `TLS_PSK_WITH_AES_128_CCM_8` (RFC 7252 MTI). Dev identity `zbook` / key `zbook-dtls-psk!!` hardcoded in `coap_server.c` — replace before shipping. |
| OSCORE | `+oscore.conf` | 5683 | **EDHOC** (RFC 9528) keys **OSCORE** (RFC 8613), over plaintext UDP. |

### EDHOC + OSCORE (the `oscore.conf` build)

**Profile: method 3 (static-DH both sides) / cipher suite 0 (X25519, SHA-256,
AES-CCM-16-64-128) / raw-public-key CCS credentials referenced by `kid` / no EAD.**
The device is the **Responder**; the peer is a native **C/C++ host client**
(`host_client/`). **HW-verified end-to-end** (fast handshake + OSCORE-protected
hello/led round-trip).

**Split-engine design** (the key architectural fact):
- **EDHOC = `libedhoc`** (kamil-kielbasa, v1.14.1; native Zephyr module). Its
  synchronous per-message responder API is driven **directly from `edhoc_post()`**
  (no dedicated thread): `message_1_process → message_2_compose` (2.04), then
  `message_3_process → message_4_compose → export_oscore_session` (2.04). One
  persistent `edhoc_context` spans the two POSTs, mutex-guarded, reset on each fresh
  `message_1`.
- **OSCORE = `uoscore-uedhoc`'s OSCORE half only** (`oscore2coap`/`coap2oscore`,
  `oscore_context_init`). Its **EDHOC half is disabled** (`CONFIG_UEDHOC` unset) —
  it has two unfixed RFC-9528 encoding bugs that break CCS-by-kid interop; that is
  the whole reason for the migration to libedhoc.
- **X25519 = `compact25519`** (DavyLandman, v1.1.0). libedhoc's suite-0 helper does
  ECDH with it — X-coordinate-only, so **no point decompression and no mbedTLS ECP**
  (unlike the retired suite-2/P-256 path). AES-CCM/SHA-256/HKDF still go through
  mbedTLS **PSA**.

**Transport (RFC 9668, sequential):** the host POSTs to `/.well-known/edhoc`;
`message_1` is prefixed with CBOR `true` (`0xf5`), `message_3` with the CBOR-encoded
`C_R`. `edhoc_post()` strips that framing. A `message_4` reply **is** composed (the
non-combined initiator flow requires it). On completion `edhoc_export_oscore_session`
yields the master secret/salt + correctly-oriented Sender/Recipient IDs →
`oscore_context_init`. A fresh handshake each boot re-keys OSCORE, so SSN starts at 0
(no NVM, no nonce reuse). `oscore_post()` intercepts OSCORE-protected requests
(outer POST to root, no Uri-Path), decrypts, dispatches to the shared
`coap_server_dispatch_inner()`, re-encrypts, sends.

**Credentials — one source of truth.** `scripts/edhoc_keys.py` (`just keys`) emits,
byte-for-byte matching, all three artifacts (the credential bytes feed the EDHOC
transcript hash, so they *must* match):
- `include/edhoc_creds.h` — device (Responder) side.
- `host_client/edhoc_creds_client.h` — host (Initiator) side (gitignored).
- `scripts/edhoc_creds.json`.

Rebuild + reflash the oscore variant **and** rebuild the host client after `just
keys`. X25519/OKP CCS (`kty=1`, `crv=4`), `KID_I=0x2a`, `KID_R=0x2b`, `C_R=-8`.

**Host client** (`host_client/`, its own CMake project — NOT Zephyr): built from the
**same libs** (libedhoc suite-0 + uoscore OSCORE + compact25519 + zcbor + cantcoap),
linking Homebrew **`mbedtls@3`** for host PSA (`brew install mbedtls@3`).
`host_client/edhoc_config.h` mirrors `oscore.conf`'s `CONFIG_LIBEDHOC_*` for the
non-Zephyr build. Run: `just build-host-client` then `just edhoc-client [host]`.

## Build wiring & non-obvious gotchas

- **Flat `edhoc.h` clash (both directions).** libedhoc (`include/edhoc.h`) and
  uoscore (`inc/edhoc.h`) both put a bare `edhoc.h` on the global include path.
  `CMakeLists.txt` fixes it with `target_include_directories(... BEFORE PRIVATE ...)`:
  libedhoc's dirs first for `app`; uoscore's `inc/` first for the `uoscore` /
  `uoscore_uedhoc_common` module targets (else uoscore's `crypto_wrapper.c` picks up
  libedhoc's header → `WEAK` undefined). The host client solves the same clash with
  per-lib static libraries, each with its own include order.
- **compact25519 is compiled by the app**, not the module (`file(GLOB ...)` over its
  `src/*.c` + `src/c25519/*.c`), and its suite-0 helper
  (`helpers/src/edhoc_cipher_suite_0.c`) is likewise added to `app` sources.
- **`PSA_KEY_HANDLE_INIT=PSA_KEY_ID_NULL`** — mbedTLS/tf-psa-crypto 4.x removed
  `PSA_KEY_HANDLE_INIT`; the suite-0 helper still uses it. Mapped via
  `target_compile_definitions`.
- **`CONFIG_COAP_SERVER_MESSAGE_SIZE=256`** in `oscore.conf` — it defaults to
  `COAP_SERVER_BLOCK_SIZE` (64), too small for the `message_1` POST (32-byte G_X,
  ~65 B) → server returns `4.13 Request Too Large` before the handler runs.
- **`sections-rwdata.ld`** — the RP2350 Pico-SDK RNG lives in
  `.uninitialized_data.*`; Zephyr's template `NOLOAD` gap misaligns the `.data` copy
  and corrupts `usbd_context` (USB init `-EPERM`). The linker fragment claims those
  inputs into `.data`. `sections-ram.ld` provides the `coap_resource_coap_server`
  iterable section.
- The suite-2/P-256 path (mbedTLS ECP, `DECLARE_PRIVATE_IDENTIFIERS`, a vendored
  helper) was **retired** — do not reintroduce it. Suite 0 removed all of that.

## Adding a CoAP resource

Add a `COAP_RESOURCE_DEFINE(<name>, coap_server, { .get = <h>, .path = <arr> })`
block in `src/coap_server.c` (or a new `src/*.c`; the glob picks it up). Each handler
builds a `coap_packet` and sends via `coap_resource_send()`. Route it through
`coap_server_dispatch_inner()` if it should also be reachable OSCORE-protected.

## Docs & source of truth

- **`docs/edhoc-oscore-plan.md`** — design record; **authoritative** for the EDHOC
  work, kept current with the suite-0 pivot.
- **`docs/benchmarks.md`** — measured ROM/RAM per layer + CoAP latency (base & DTLS).
  Regenerate with `just footprint` / `python3 scripts/coap_latency.py`.
- **`README.md`** — accurate for base/DTLS, but its **OSCORE section is stale**
  (predates the suite-0 / libedhoc / native-host-client pivot; still says suite-2 +
  aiocoap/lakers + "prototype"). Trust this file and the plan for OSCORE state.

## C code style

`.clang-format` is Zephyr upstream: LLVM base, **tabs**, 8-wide indent, **100-col**,
Linux braces, `InsertBraces: true`. Per the global rules: use `/* */` (and `/** */`
Doxygen) comments not `//`, always brace `if`/loops even single-line, and avoid
`typedef` — be explicit.

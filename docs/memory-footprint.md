# Memory footprint: USB-NCM + CoAP vs USB-ACM + UART + binary protocol

Board `zbook/rp2350b/m33` (RP2350B: 2 MB flash, 520 KB RAM), Zephyr v4.4.0,
zephyr-sdk arm-zephyr-eabi, `-Os`. All numbers are **bytes**, measured — not
estimated — from `west build -t rom_report` / `-t ram_report`
(`scripts/footprint/size_report`, which attributes every symbol to its
`ZEPHYR_BASE` source path) and cross-checked against `zephyr.map`. Regenerate with
`just footprint` (see bottom).

Two firmwares are compared:

| Firmware | Transport | App protocol | Diagnostics |
|---|---|---|---|
| `usb_ncm_net` (this repo) | USB CDC-NCM → IPv4/UDP | CoAP server | shell + logging |
| `esp01_flasher` (sibling repo) | USB CDC-ACM ↔ UART bridge | none (transparent) | logging only, no shell |

`usb_ncm_net` is measured in **two profiles**: the dev baseline (`prj.conf`) and
the minimal networking profile (`prj.conf + minimal.conf`, see `README.md`).

## TL;DR

- **Cost of "network over USB" itself** (CDC-NCM function class + IPv4/UDP stack +
  Ethernet L2, i.e. everything that turns a USB link into an IP socket):
  **~27.3 KB flash, ~14 KB RAM** (minimal profile; ~22 KB RAM on the dev profile).
- **Cost of the CoAP stack** (separated): **~5.1 KB flash + ~1 KB app, ~2.5 KB RAM**
  (minimal; ~7.2 KB RAM on the dev profile — it scales with CoAP thread stack,
  pending-message and block-size knobs).
- **Net-new price of NCM+CoAP over ACM+UART+binary**, comparing only the layers
  that differ (shared USB core and optional shell/logging excluded):
  **≈ +27 KB flash and +10 KB RAM** (minimal; +23 KB RAM on the dev profile).
- In absolute terms on the RP2350B that is **~1.3 % of flash and ~2 % of RAM** —
  negligible headroom impact. The maintainability win (standard CoAP/REST vs a
  hand-rolled binary framing) comes at a small, well-understood memory cost.

## Whole-image totals (context, not the comparison)

| | FLASH | RAM |
|---|---|---|
| `usb_ncm_net` dev (`prj.conf`) | 192,396 | 53,904 |
| `usb_ncm_net` minimal (`+minimal.conf`) | 190,832 | 40,264 |
| `esp01_flasher` (ACM+UART) | 111,432 | 25,032 |

⚠️ Do **not** read the raw whole-image difference as "the cost of CoAP": the two
images differ in more than the transport (e.g. `usb_ncm_net` keeps a shell,
`esp01_flasher` does not; `esp01_flasher` carries a 4 KB UART bridge ring buffer).
The per-layer tables below isolate the real cost.

## `usb_ncm_net` per layer (CoAP separated)

`size_report` path → bytes. "core" of `device_next` = the subsystem total minus
the function class.

| Layer | ROM | RAM (min) | RAM (dev) |
|---|--:|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ ACM)* | 15,019 | 5,674 | 5,674 |
| CDC-NCM function class — `device_next/class/usbd_cdc_ncm.c` | 3,577 | 4,374 | 4,374 |
| net/ip — IP, UDP, sockets, contexts, `net_pkt`, traffic classes | 20,846 | 9,742 | 17,902 |
| net L2 — `net/l2/ethernet` (ARP) | 3,488 | 201 | 201 |
| **➤ "Network over USB" subtotal** (NCM class + net/ip + L2) | **27,911** | **14,317** | **22,477** |
| **➤ CoAP stack** — `subsys/net/lib/coap` | **5,110** | **2,486** | **7,222** |
| CoAP app resources — `src/coap_server.c` (`hello`, `led`) | ~1,000 | 117 | 117 |
| *Optional diagnostics — `subsys/shell` + `net/lib/shell` + `logging`* | *~34,500* | *~6,900* | *~6,900* |

Biggest individual RAM line items (from `ram_report`):

- `net_buf_data_cdc_ncm_ep_pool` = **4,096** — the CDC-NCM USB↔Ethernet buffer;
  it dominates the NCM class RAM.
- `net_pkt` rx/tx packet data = **2×2,048**; `net_tc` rx/tx thread stacks =
  **1,504 + 1,200** (network traffic-class threads).
- CoAP server thread stack = **2,048** (minimal) / **4,096** (dev default).
- `usbd_stack` (USB thread) = 1,024; UDC endpoint configs = 2×640.

What `minimal.conf` trades away (all RAM, no functional loss for one CoAP client):
net buffers 14/36 → 4/16 pkt/buf, `NET_MAX_CONTEXTS` 6 → 1, CoAP thread stack
4096 → 2048, pending msgs 10 → 1, block size 256 → 64, observers 3 → 0. Net effect
net/ip 17,902 → 9,742 RAM and CoAP 7,222 → 2,486 RAM. Flash is barely affected
(config tunes pools, not code).

## `esp01_flasher` per layer (USB-ACM + UART baseline)

Sanity check passed: the ACM report contains **no `subsys/net` and no `coap`**
paths — the ACM path genuinely has no network stack.

| Layer | ROM | RAM |
|---|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ NCM)* | 15,639 | 5,390 |
| CDC-ACM function class — `device_next/class/usbd_cdc_acm.c` | 2,908 | 2,078 |
| UART driver — `drivers/serial/uart_pl011.c` | 1,792 | 4 |
| Ring-buffer lib — `sys/ring_buffer` | ~362 | 0 |
| App bridge ring buffers — `src/main.c` (2×2,048) † | 0 | 4,096 |
| *Logging (no shell)* | *~4,240* | *~3,480* |

† The 4 KB ring buffers are specific to the *transparent bridge*; a real
"ACM + binary protocol" app would size its own (typically smaller) buffers. They
are counted here because they exist in the measured firmware, and flagged so the
ACM column is not over-credited.

The shared USB device core is essentially identical on both sides
(**~15 KB ROM / ~5.5 KB RAM**), confirming it as common cost both approaches pay.

## The delta — price of NCM+CoAP over ACM+binary

Comparing only the layers that differ (shared USB core excluded; shell/logging
excluded as optional and not intrinsic to either transport):

| | NCM+CoAP-only | ACM+UART-only | Δ (NCM+CoAP − ACM) |
|---|--:|--:|--:|
| CDC function class (ROM) | 3,577 (NCM) | 2,908 (ACM) | +669 |
| net stack net/ip + L2 (ROM) | 24,334 | 0 | +24,334 |
| CoAP (ROM) | 5,110 | 0 | +5,110 |
| UART driver + ring lib (ROM) | 0 | 2,154 | −2,154 |
| **ΔROM** | **33,021** | **5,062** | **+27,959 (~27 KB)** |
| CDC function class (RAM) | 4,374 (NCM) | 2,078 (ACM) | +2,296 |
| net stack net/ip + L2 (RAM, min) | 9,943 | 0 | +9,943 |
| CoAP (RAM, min) | 2,486 | 0 | +2,486 |
| UART + bridge ring buffers (RAM) | 0 | 4,100 | −4,100 |
| **ΔRAM (minimal)** | **16,803** | **6,178** | **+10,625 (~10 KB)** |
| **ΔRAM (dev)** | 29,699 | 6,178 | +23,521 (~23 KB) |

**As a fraction of the RP2350B:** +27 KB flash ≈ 1.3 % of 2 MB; +10 KB RAM ≈ 2.0 %
of 520 KB (dev profile +23 KB RAM ≈ 4.5 %).

## Notes & caveats

- Report-tool totals reconcile with `west`: ROM within ~20 B; the `ram_report`
  tool total is ~2.5–3 KB below the `west` RAM figure (a small unattributed
  remainder), so headline whole-image RAM uses the `west` total and per-layer rows
  use the tool.
- Diagnostics (shell + net-shell + logging) cost ~34 KB flash / ~7 KB RAM in
  `usb_ncm_net` and are **kept in both of its profiles on purpose** (so the
  networking-stack footprint can be compared with everything else held equal).
  They are optional and excluded from the delta above. `esp01_flasher` ships no
  shell; drop `CONFIG_SHELL`/`NET_SHELL` (or set `CONFIG_LOG=n`) for a fair
  headless-vs-headless whole-image comparison.
- The "binary protocol" application layer for the ACM approach is not measured
  (esp01_flasher is a transparent bridge). It is assumed small (hundreds of bytes
  of parser + a buffer), comparable to the ~1 KB flash / ~120 B RAM of the CoAP
  app resources here — i.e. the app layer is a wash; the cost is in the stacks.
- Performance (CoAP round-trip latency / throughput over NCM vs raw UART framing)
  is **not** covered here — separate follow-up.

## Regenerate

```bash
# from inside usb_ncm_net/
just footprint          # builds both profiles and dumps ram/rom reports

# ACM baseline (from inside esp01_flasher/):
west build -p always -b zbook/rp2350b/m33 -d build
west build -d build -t rom_report
west build -d build -t ram_report
```

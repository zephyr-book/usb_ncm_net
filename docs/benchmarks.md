# Benchmarks: memory & performance — USB-NCM+CoAP vs USB-ACM+UART+binary protocol

Board `zbook/rp2350b/m33` (RP2350B: 2 MB flash, 520 KB RAM), Zephyr v4.4.0,
zephyr-sdk arm-zephyr-eabi, `-Os`. Numbers are **bytes**, measured (not estimated):
memory from `west build -t rom_report`/`-t ram_report`
(`scripts/footprint/size_report`, per `ZEPHYR_BASE` path) cross-checked against
`zephyr.map`; latency from `scripts/coap_latency.py`. Regenerate with
`just footprint` and `python3 scripts/coap_latency.py`.

Builds compared:

| Firmware / build | Transport | App |
|---|---|---|
| `usb_ncm_net` **base** (`prj.conf`, `just build`) | USB CDC-NCM → IPv4/UDP | plaintext CoAP |
| `usb_ncm_net` **DTLS** (`+dtls.conf`, `just build-dtls`) | USB CDC-NCM → IPv4/UDP | CoAPS (DTLS 1.2, X.509) |
| `esp01_flasher` (sibling) | USB CDC-ACM ↔ UART | none (transparent bridge) |

The base is minimal by construction (IPv4/UDP only, no IPv6/TCP/DHCP, trimmed
pools, bare console). The DTLS variant layers mbedTLS + a cert-authenticated
CoAPS service (ECDHE-ECDSA-AES128-GCM-SHA256, self-signed EC P-256, on UDP 5684).

## TL;DR

- **"Network over USB" itself** (CDC-NCM class + IPv4/UDP + Ethernet L2):
  **~22.9 KB flash, ~14 KB RAM**; the IPv4/UDP+L2 stack alone is ~19.3 KB flash,
  ~90 % irreducible core.
- **Plaintext CoAP stack**: **~4.6 KB flash + ~1 KB app, ~2.5 KB RAM**.
- **Net-new cost of plaintext NCM+CoAP over ACM+UART+binary**: **≈ +22 KB flash,
  +10 KB RAM** (~1.1 % flash, ~2 % RAM of the RP2350B).
- **Adding DTLS/CoAPS (X.509)** on top of the base: **+78 KB flash, +85 KB RAM** —
  dominated by the mbedTLS/tf-psa-crypto library (~65 KB flash) and the 60 KB
  mbedTLS heap. Still only ~13 % flash / ~24 % RAM of the part.
- **CoAP round-trip latency over USB-NCM**: **~2.1 ms median, ~3 ms p99, 0 % loss**
  (plaintext; transport-bound). DTLS per-request latency: not yet measured.

## Whole-image totals

| Build | FLASH | RAM |
|---|--:|--:|
| `usb_ncm_net` base (plaintext) | 154,036 | 39,704 |
| `usb_ncm_net` DTLS/CoAPS (X.509) | 232,020 | 124,848 |
| `esp01_flasher` (ACM+UART, headless) | 111,432 | 25,032 |

## `usb_ncm_net` base — per layer (CoAP separated)

`size_report` path → bytes. "core" of `device_next` = subsystem total minus the
CDC function class.

| Layer | ROM | RAM |
|---|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ ACM)* | 15,019 | 5,674 |
| CDC-NCM function class — `class/usbd_cdc_ncm.c` | 3,577 | 4,374 |
| net/ip — IP, UDP, sockets, contexts, `net_pkt`, traffic classes | 16,014 | 9,496 |
| net L2 — `net/l2/ethernet` (ARP) | 3,310 | 201 |
| **➤ "Network over USB" subtotal** (NCM class + net/ip + L2) | **22,901** | **14,071** |
| **➤ CoAP stack** — `subsys/net/lib/coap` | **4,610** | **2,486** |
| CoAP app resources — `src/coap_server.c` (`hello`, `led`) | ~1,000 | 117 |
| *General shell + logging (bare console; no net/coap shells)* | *~16,400* | *~4,400* |

Biggest RAM items: `net_buf_data_cdc_ncm_ep_pool` = 4,096 (hardcoded `2×2048` in
the driver); `net_pkt` rx/tx data 2×2,048; CoAP thread stack 2,048; shell stack
2,048.

## `esp01_flasher` — per layer (USB-ACM + UART baseline)

Sanity: the ACM report has **no `subsys/net` and no `coap`** paths (no network
stack), and no shell (headless).

| Layer | ROM | RAM |
|---|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ NCM)* | 15,639 | 5,390 |
| CDC-ACM function class — `class/usbd_cdc_acm.c` | 2,908 | 2,078 |
| UART driver — `drivers/serial/uart_pl011.c` | 1,792 | 4 |
| Ring-buffer lib + app bridge buffers (2×2,048) † | ~362 | 4,096 |

† The 4 KB ring buffers belong to the transparent bridge; a real ACM+binary app
would size its own. The shared USB device core is ~identical both sides
(~15 KB ROM / ~5.5 KB RAM) — common cost.

## Delta — plaintext NCM+CoAP vs ACM+binary

Only the layers that differ (shared USB core and optional shell/logging excluded).

| | ACM+UART-only | NCM+CoAP-only | Δ (NCM+CoAP − ACM) |
|---|--:|--:|--:|
| CDC function class (ROM) | 2,908 | 3,577 | +669 |
| net/ip + L2 (ROM) | 0 | 19,324 | +19,324 |
| CoAP (ROM) | 0 | 4,610 | +4,610 |
| UART driver + ring lib (ROM) | 2,154 | 0 | −2,154 |
| **ΔROM** | **5,062** | **27,511** | **+22,449 (~22 KB)** |
| CDC function class (RAM) | 2,078 | 4,374 | +2,296 |
| net/ip + L2 (RAM) | 0 | 9,697 | +9,697 |
| CoAP (RAM) | 0 | 2,486 | +2,486 |
| UART + bridge ring buffers (RAM) | 4,100 | 0 | −4,100 |
| **ΔRAM** | **6,178** | **16,557** | **+10,379 (~10 KB)** |

~1.1 % of flash, ~2 % of RAM. The ~19 KB net/ip+L2 is the fixed price of IP; the
ACM+binary path avoids it via raw byte framing.

## Cost of DTLS / CoAPS (X.509)

DTLS is layered on the base with `dtls.conf` (mbedTLS 4.x + tf-psa-crypto, DTLS
1.2, ECDHE-ECDSA-AES128-GCM-SHA256, self-signed EC P-256 server cert). Delta vs
the plaintext base:

| | base | DTLS | Δ |
|---|--:|--:|--:|
| FLASH | 154,036 | 232,020 | **+77,984 (~78 KB)** |
| RAM | 39,704 | 124,848 | **+85,144 (~85 KB)** |

Where it goes:

- **Flash (+78 KB):** mbedTLS + tf-psa-crypto library ≈ **64.8 KB** (X.509, ECDHE,
  ECDSA, AES-GCM, SHA-256, DTLS record/handshake), plus the TLS socket glue and
  larger buffers.
- **RAM (+85 KB):** the **60 KB mbedTLS heap** (`CONFIG_MBEDTLS_HEAP_SIZE`) is the
  single biggest item; ~9.4 KB of crypto state; the rest is DTLS-sized net buffers
  and the larger CoAP/main thread stacks the handshake needs (`dtls.conf` bumps
  pkt/buf pools, contexts, and stacks vs the base).

The mbedTLS heap is the obvious tuning knob — 60 KB is generous for one client and
can likely be trimmed. This is the standard software-crypto cost; the RP2350 has
no ECC accelerator, and its HW SHA-256 isn't wired into mbedTLS, so the handshake
(ECDHE + ECDSA-dominated) is all software.

## Performance — CoAP round-trip latency

Confirmable round trips over the live USB-NCM link, `scripts/coap_latency.py`
(one persistent socket, no per-request process overhead). **Plaintext** base,
1000 samples/endpoint after 50 warm-up, **0 % loss**:

| Endpoint | median | mean | p95 | p99 | max | stddev |
|---|--:|--:|--:|--:|--:|--:|
| `GET /hello` | 2.08 | 2.17 | 2.55 | 3.57 | 14.55 | 0.60 |
| `GET /led`   | 2.05 | 2.11 | 2.45 | 3.11 |  5.08 | 0.26 |
| `PUT /led=1` | 2.06 | 2.11 | 2.49 | 3.07 |  9.95 | 0.34 |

*(ms; full CON round trip: host → board IP/UDP/CoAP + handler → host)*

- **~2.1 ms median, transport-bound** — all three endpoints agree within ~0.06 ms,
  so the CoAP parse + handler cost is negligible; the ~2 ms floor is USB
  full-speed frame timing (1 ms intervals, ~two frames per round trip).
- Tail (5–15 ms) is host-side USB/NCM scheduling jitter, not the board.

Not yet measured: DTLS per-request latency (post-handshake AES-GCM overhead;
expected to add a small amount over the ~2 ms transport floor), the one-time DTLS
handshake time, sustained throughput, and the ACM+UART latency baseline.

## Notes & caveats

- Report-tool RAM total runs a few KB below the `west` figure (small unattributed
  remainder); whole-image RAM uses the `west` total, per-layer rows use the tool.
- The DTLS variant's certificate + key (`certs/coaps-server-*.der`, self-signed
  EC P-256, SAN `IP:192.0.2.1`) are DEV credentials — replace before shipping.
- `dtls.conf` builds with `-DMBEDTLS_FATAL_WARNINGS=OFF` (the pinned tf-psa-crypto
  has `-Werror` warnings in upstream library code, unrelated to this app).
- The "binary protocol" application layer for ACM is not measured (esp01_flasher
  is a transparent bridge); assumed small, comparable to the ~1 KB CoAP app.

## Regenerate

```bash
# memory (from usb_ncm_net/):
just footprint          # builds base + DTLS, dumps ram/rom reports

# ACM baseline (from esp01_flasher/):
west build -p always -b zbook/rp2350b/m33 -d build
west build -d build -t rom_report && west build -d build -t ram_report

# performance (host, NCM up at 192.0.2.2):
python3 scripts/coap_latency.py
```

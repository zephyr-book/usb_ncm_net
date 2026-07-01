# Benchmarks: memory & performance — USB-NCM+CoAP vs USB-ACM+UART+binary protocol

Board `zbook/rp2350b/m33` (RP2350B: 2 MB flash, 520 KB RAM), Zephyr v4.4.0,
zephyr-sdk arm-zephyr-eabi, `-Os`. Numbers are **bytes**, measured (not estimated):
memory from `west build -t rom_report`/`-t ram_report` (`size_report`, per
`ZEPHYR_BASE` path) cross-checked against `zephyr.map`; latency from
`scripts/coap_latency.py`. Regenerate with `just footprint` and
`python3 scripts/coap_latency.py [--dtls]`.

**Logging is OFF** in the firmware (`CONFIG_LOG=n`): per-request `LOG_*` calls do
synchronous UART work that perturbs transmission and inflates latency, so they're
compiled out. Builds compared:

| Firmware / build | Transport | App |
|---|---|---|
| `usb_ncm_net` **base** (`prj.conf`, `just build`) | USB CDC-NCM → IPv4/UDP | plaintext CoAP |
| `usb_ncm_net` **DTLS** (`+dtls.conf`, `just build-dtls`) | USB CDC-NCM → IPv4/UDP | CoAPS (DTLS 1.2, **PSK**) |
| `esp01_flasher` (sibling) | USB CDC-ACM ↔ UART | none (transparent bridge) |

The base is minimal (IPv4/UDP only, trimmed pools, bare console, no logging). The
DTLS variant adds mbedTLS + a PSK-authenticated CoAPS service
(`TLS_PSK_WITH_AES_128_CCM_8`, RFC 7252's mandatory mode) on UDP 5684.

## TL;DR

- **"Network over USB" itself** (CDC-NCM class + IPv4/UDP + Ethernet L2):
  **~21.9 KB flash, ~14 KB RAM**; the IPv4/UDP+L2 stack alone is ~19.3 KB flash,
  ~90 % irreducible core.
- **Plaintext CoAP stack**: **~4.6 KB flash + ~1 KB app, ~2.5 KB RAM**.
- **Net-new cost of plaintext NCM+CoAP over ACM+UART+binary**: **≈ +22 KB flash,
  +10 KB RAM** (~1.1 % flash, ~2 % RAM of the RP2350B).
- **Adding DTLS/CoAPS (PSK)** on top of the base: **+40 KB flash, +32 KB RAM** —
  the mbedTLS+tf-psa-crypto library is ~30 KB flash (AES-CCM, SHA-256, DTLS; **no
  X.509/ECDSA/ECDHE**), plus a 16 KB mbedTLS heap. (Cert-based X.509 was nearly
  double: +78 KB flash / +85 KB RAM — see note.)
- **Latency over USB-NCM** (0 % loss): plaintext **~1.7 ms median**, DTLS-PSK
  **~3.2 ms median** (+~1.5 ms for AES-CCM), DTLS handshake **~20 ms** one-time.
  **CON == NON** — confirmable adds no per-exchange latency.

## Whole-image totals

| Build | FLASH | RAM |
|---|--:|--:|
| `usb_ncm_net` base (plaintext) | 131,864 | 36,712 |
| `usb_ncm_net` DTLS/CoAPS (PSK) | 172,156 | 69,240 |
| `esp01_flasher` (ACM+UART) | 111,432 | 25,032 |

(`esp01_flasher` is measured as-shipped: logging on, no shell — so read the
per-layer tables, not the raw whole-image gap, for the real transport cost.)

## `usb_ncm_net` base — per layer (CoAP separated)

| Layer | ROM | RAM |
|---|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ ACM)* | 13,009 | 5,626 |
| CDC-NCM function class — `class/usbd_cdc_ncm.c` | 2,601 | 4,370 |
| net/ip — IP, UDP, sockets, contexts, `net_pkt`, traffic classes | 15,958 | 9,496 |
| net L2 — `net/l2/ethernet` (ARP) | 3,310 | 201 |
| **➤ "Network over USB" subtotal** (NCM class + net/ip + L2) | **21,869** | **14,067** |
| **➤ CoAP stack** — `subsys/net/lib/coap` | **4,610** | **2,486** |
| CoAP app resources — `src/coap_server.c` (`hello`, `led`) | ~1,000 | 117 |
| *Bare shell console (no logging)* | *15,250* | *3,771* |

Biggest RAM item is still `net_buf_data_cdc_ncm_ep_pool` = 4,096 (hardcoded
`2×2048` in the NCM driver); then `net_pkt` rx/tx 2×2,048 and the CoAP thread
stack 2,048.

## Delta — plaintext NCM+CoAP vs ACM+binary

The layers that differ (shared USB core excluded). net/ip+L2 and CoAP are
NCM-only; the ACM path instead carries a UART driver + ring buffer.

| | ACM+UART-only | NCM+CoAP-only | Δ |
|---|--:|--:|--:|
| net/ip + L2 (ROM) | 0 | 19,268 | +19,268 |
| CoAP (ROM) | 0 | 4,610 | +4,610 |
| UART driver + ring lib (ROM) | ~2,154 | 0 | −2,154 |
| **ΔROM (approx)** | | | **≈ +22 KB** |
| net/ip + L2 (RAM) | 0 | 9,697 | +9,697 |
| CoAP (RAM) | 0 | 2,486 | +2,486 |
| UART + bridge ring buffers (RAM) | ~4,100 | 0 | −4,100 |
| **ΔRAM (approx)** | | | **≈ +8–10 KB** |

The ~19 KB net/ip+L2 is the fixed price of a real IP stack; the ACM+binary path
avoids it with raw byte framing. On the RP2350B that's ~1.1 % flash / ~2 % RAM.

## Cost of DTLS / CoAPS (PSK)

DTLS layered on the base via `dtls.conf` (mbedTLS 4.x + tf-psa-crypto, DTLS 1.2,
`TLS_PSK_WITH_AES_128_CCM_8`). Delta vs the plaintext base:

| | base | DTLS-PSK | Δ |
|---|--:|--:|--:|
| FLASH | 131,864 | 172,156 | **+40,292 (~40 KB)** |
| RAM | 36,712 | 69,240 | **+32,528 (~32 KB)** |

- **Flash (+40 KB):** mbedTLS + tf-psa-crypto = **30.3 KB** (mbedTLS 16.4 KB +
  tf-psa-crypto 13.9 KB) — AES-CCM, SHA-256, HMAC, TLS-PRF, DTLS record/handshake.
  **Zero X.509/ECDSA/ECDH** (confirmed in the map). Plus the TLS socket glue.
- **RAM (+32 KB):** the **16 KB mbedTLS heap** (`CONFIG_MBEDTLS_HEAP_SIZE`) is the
  biggest item; the rest is DTLS-sized net buffers + TLS contexts.

**Why PSK, not X.509:** an earlier X.509 (ECDHE-ECDSA) build cost **+78 KB flash /
+85 KB RAM** — the crypto library alone was 64.8 KB and needed a 60 KB heap, plus
software ECDHE+ECDSA on the M33 (no ECC accelerator). For a controlled
point-to-point link, PSK (RFC 7252's mandatory-to-implement mode) is far smaller,
has a faster handshake, and is secure given a strong pre-shared key; the trade is
no forward secrecy and no per-device PKI identity.

## Performance — CoAP round-trip latency

Confirmable (CON) and Non-confirmable (NON) round trips over the live USB-NCM
link, `scripts/coap_latency.py` (one persistent connection — no per-request
process/handshake overhead). Logging off. 1000 samples/endpoint, 0 % loss.
Representative medians (all three endpoints agree within ~0.1 ms → transport-bound):

| Transport | CON median | NON median | p95 | p99 | one-time |
|---|--:|--:|--:|--:|--:|
| plaintext CoAP (`:5683`) | ~1.70 | ~1.70 | ~2.2 | ~3.0 | — |
| DTLS-PSK CoAPS (`:5684`) | ~3.2 | ~3.2 | ~3.9 | ~5 | handshake ~20 ms |

*(ms; full round trip: host → board IP/UDP/(DTLS)/CoAP + handler → host)*

- **Transport-bound.** `GET /hello`, `GET /led` and `PUT /led` are
  indistinguishable → the CoAP parse + resource handler cost is negligible.
- **CON == NON.** Confirmable reliability doesn't add latency for a single
  exchange on this clean link (retransmit only matters under loss).
- **Plaintext ~1.7 ms floor** = USB full-speed frame timing (~two 1 ms frames per
  round trip); removing logging took it from ~2.1 → ~1.7 ms.
- **DTLS-PSK adds ~1.5 ms/request** (AES-128-CCM-8 encrypt/decrypt + slightly
  larger records) on top of the transport, plus a **~20 ms one-time handshake**
  (fast because PSK has no ECC). Occasional host-side USB-scheduling outliers push
  the max to tens of ms; median/p95 are stable.

Not covered: sustained throughput, and a like-for-like ACM+UART latency baseline.

## Notes & caveats

- Logging is off; re-enable `CONFIG_LOG` only for debugging (it perturbs latency).
- The PSK identity/key in `src/coap_server.c` (id `zbook`, 16-byte key) and the
  matching values in `scripts/coap_latency.py` are DEV credentials — replace and
  provision out-of-band for production.
- `dtls.conf` builds with `-DMBEDTLS_FATAL_WARNINGS=OFF` (upstream tf-psa-crypto
  `-Werror` warnings, not this app). DTLS needs the `mbedtls` + `tf-psa-crypto`
  west modules (in `west.yml`).
- The X.509 latency couldn't be measured (the software ECDHE-ECDSA handshake
  wouldn't complete with the Python test client); PSK made it trivial.
- `esp01_flasher` ACM figures are its as-shipped config (logging on, headless);
  the "binary protocol" app layer isn't measured (it's a transparent bridge,
  assumed small).

## Regenerate

```bash
# memory (from usb_ncm_net/):
just footprint

# ACM baseline (from esp01_flasher/):
west build -p always -b zbook/rp2350b/m33 -d build
west build -d build -t rom_report && west build -d build -t ram_report

# performance (host, NCM up at 192.0.2.2):
python3 scripts/coap_latency.py          # plaintext, base build
python3 scripts/coap_latency.py --dtls   # CoAPS, DTLS build
```

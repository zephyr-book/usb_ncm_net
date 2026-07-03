# Benchmarks: memory & performance — base / DTLS / OSCORE

The three `usb_ncm_net` security profiles, measured on **board `zbook/rp2350b/m33`**
(RP2350B: 2 MB flash, 520 KB RAM), **Zephyr v4.4.0**, zephyr-sdk arm-zephyr-eabi,
`-Os`. Numbers are **bytes** and **milliseconds**, measured (not estimated):

- **memory** from `west build -t rom_report` / `-t ram_report` (`size_report`),
  cross-checked against the linker's `FLASH`/`RAM` region report;
- **latency** from `scripts/coap_latency.py` (plaintext, DTLS) and
  `host_client --bench` (OSCORE), 1000 samples/endpoint over one persistent
  connection.

**All figures are the PERF-config images** (`perf.conf`: `CONFIG_SHELL=n`,
`CONFIG_LOG=n`) — i.e. the *exact* firmware the latency runs use, so memory and
performance describe the same binary. The interactive shell + logging (the dev
console) is scaffolding, not part of a shipped link; adding it back costs
**~49 KB flash + ~7.5 KB RAM** on base/DTLS and **~63 KB flash** on OSCORE (extra
flash = libedhoc/uoscore log strings). Regenerate everything with `just footprint`
+ `just latency[-dtls|-oscore]`.

| Variant | Conf | Port | Security |
|---|---|---|---|
| **base**   | `prj.conf`      | 5683 | none (plaintext CoAP) |
| **DTLS**   | `+dtls.conf`    | 5684 | CoAPS DTLS 1.2, **PSK** `TLS_PSK_WITH_AES_128_CCM_8` (RFC 7252 MTI) |
| **OSCORE** | `+oscore.conf`  | 5683 | **EDHOC** (RFC 9528, method 3 / suite 0 / X25519) keys **OSCORE** (RFC 8613), over plaintext UDP |

## TL;DR

- **Whole-image (perf config):** base **106.6 KB flash / 32.3 KB RAM**, DTLS
  **146.8 / 66.1**, OSCORE **171.5 / 57.3**. All well under the RP2350B's budget
  (≤ 9.4 % flash, ≤ 12.8 % RAM).
- **Cost of DTLS-PSK over base:** **+40 KB flash / +34 KB RAM** — the mbedTLS +
  tf-psa-crypto library is ~30 KB flash (AES-CCM, SHA-256, HMAC, TLS-PRF, DTLS
  record/handshake; **no X.509/ECDSA/ECDHE**), and RAM is dominated by the 16 KB
  mbedTLS heap.
- **Cost of EDHOC+OSCORE over base:** **+65 KB flash / +25 KB RAM** — a ~62 KB
  crypto stack (tf-psa PSA 29.6 KB, libedhoc 14.8 KB, uoscore-OSCORE 9.3 KB,
  compact25519 X25519 6.0 KB, zcbor 2.2 KB).
- **OSCORE uses *less* RAM than DTLS** (57 vs 66 KB) despite far more code:
  `MBEDTLS_AES_ROM_TABLES=y` moves the ~8.7 KB AES tables to flash, X25519 runs
  off-heap via compact25519, libedhoc uses a stack (VLA) backend (0 static RAM),
  and OSCORE keeps the smaller base net pools. DTLS instead grows the pools and
  pays the AES RAM tables.
- **Latency (0 % loss, transport-bound):** plaintext **~1.6 ms** median, DTLS-PSK
  **~3.2 ms** (+~1.6 ms for AES-CCM), OSCORE **~3.8 ms** (+~2.2 ms; ~0.6 ms over
  DTLS). **CON == NON** — confirmable adds no per-exchange latency.
- **Handshake is where the two secure profiles diverge sharply:** DTLS-PSK is
  **~19 ms** (symmetric only), EDHOC is **~1.6 s** — a one-time, ~85× cost, almost
  entirely the **software X25519 static-DH on the M33** (no ECC accelerator). The
  trade: EDHOC gives real (elliptic-curve) key agreement and per-device raw-public-key
  identity; PSK gives a fast handshake from a pre-shared secret.

## Whole-image totals (perf config)

| Variant | FLASH | RAM | ΔFLASH vs base | ΔRAM vs base |
|---|--:|--:|--:|--:|
| base   | 106,584 | 32,256 | — | — |
| DTLS   | 146,848 | 66,120 | +40,264 | +33,864 |
| OSCORE | 171,532 | 57,312 | +64,948 | +25,056 |

OSCORE vs DTLS: **+24,684 flash, −8,808 RAM** (OSCORE is bigger in flash, smaller
in RAM). Dev build (shell+log on) adds, per variant: base +49,092 flash / +7,448
RAM, DTLS +49,288 / +7,456, OSCORE +62,956 / +7,464.

## Per-layer ROM (perf config)

| Layer | base | DTLS | OSCORE |
|---|--:|--:|--:|
| USB device controller (`drivers/usb/udc`) | 5,960 | 5,960 | 5,960 |
| USB `device_next` core + CDC-NCM class | 9,742 | 9,742 | 9,742 |
| net/ip (IP, UDP, sockets, contexts, `net_pkt`) | 15,894 | 16,026 | 16,038 |
| net L2 ethernet (ARP) | 3,310 | 3,310 | 3,310 |
| CoAP (`subsys/net/lib/coap`) | 4,732 | 4,804 | 4,740 |
| mbedTLS (SSL/TLS record + handshake) | 0 | 16,455 | 88 |
| tf-psa-crypto (PSA: AES-CCM/SHA-256/HKDF) | 0 | 13,914 | 29,574 |
| libedhoc (EDHOC engine + suite-0 helper) | 0 | 0 | 14,794 |
| uoscore-uedhoc (OSCORE half) | 0 | 0 | 9,284 |
| compact25519 (X25519) | 0 | 0 | 5,966 |
| zcbor | 0 | 0 | 2,194 |
| app (`src/*.c`) | 1,438 | 1,524 | 2,685 |
| **everything else** (kernel, picolibc, RP2350 HAL, net core, entropy, vector table) | ~65,488 | ~75,097 | ~67,141 |
| **TOTAL (rom_report Root)** | **106,564** | **146,832** | **171,516** |

## Per-layer RAM (perf config)

| Layer | base | DTLS | OSCORE |
|---|--:|--:|--:|
| USB device controller | 3,950 | 3,950 | 3,950 |
| USB `device_next` + CDC-NCM (incl. `net_buf_data_cdc_ncm_ep_pool` 4,096) | 5,918 | 5,918 | 5,918 |
| net/ip (pools scale with `NET_BUF`/`NET_PKT`/`MAX_CONTEXTS`) | 9,368 | 12,376 | 9,504 |
| net L2 ethernet | 201 | 201 | 201 |
| CoAP (server thread stack + service state) | 2,422 | 4,470 | 8,950 |
| mbedTLS PSA heap (`MBEDTLS_HEAP_SIZE`) | 0 | 16,408 | 16,384 |
| tf-psa-crypto (AES RAM tables etc.) | 0 | 9,424 | 680 |
| libedhoc (stack/VLA backend → no static RAM) | 0 | 0 | 0 |
| uoscore-uedhoc | 0 | 0 | 14 |
| app | 237 | 237 | 1,467 |
| **everything else** (kernel, stacks, BSS) | ~7,766 | ~10,714 | ~7,814 |
| **TOTAL (ram_report Root)** | **29,862** | **63,698** | **54,886** |

## Cost of DTLS / CoAPS (PSK)

DTLS layered via `dtls.conf` (mbedTLS 4.x + tf-psa-crypto, DTLS 1.2,
`TLS_PSK_WITH_AES_128_CCM_8`). Δ vs base: **+40,264 flash / +33,864 RAM**.

- **Flash (+40 KB):** mbedTLS 16,455 + tf-psa-crypto 13,914 = **30.4 KB** of
  crypto — AES-CCM, SHA-256, HMAC, TLS-PRF, DTLS record/handshake, **zero
  X.509/ECDSA/ECDH** — plus the TLS socket glue and bigger net buffers.
- **RAM (+34 KB):** the **16 KB mbedTLS PSA heap** dominates; then the AES RAM
  lookup tables inside tf-psa-crypto (~8.7 KB — this build does *not* set
  `MBEDTLS_AES_ROM_TABLES`), larger net pools (`NET_BUF` 24 vs 16, `NET_PKT` 6 vs
  4, `MAX_CONTEXTS` 3 vs 2 → +~3 KB in net/ip), and TLS contexts.

**Why PSK, not X.509:** an earlier X.509 (ECDHE-ECDSA) build cost **+78 KB flash /
+85 KB RAM** — the crypto library alone was ~65 KB and needed a 60 KB heap, plus
software ECDHE+ECDSA on the M33. For a controlled point-to-point link, PSK (RFC
7252's MTI mode) is far smaller and its handshake is trivial; the trade is no
forward secrecy and no per-device PKI identity.

## Cost of EDHOC + OSCORE

OSCORE layered via `oscore.conf` (libedhoc EDHOC engine + uoscore-uedhoc's OSCORE
half + compact25519 X25519, all over PSA). Δ vs base: **+64,948 flash /
+25,056 RAM**. The added crypto stack (perf config):

| | ROM | RAM |
|---|--:|--:|
| tf-psa-crypto (PSA AES-CCM / SHA-256 / HKDF; incl. AES ROM tables) | 29,574 | 680 |
| libedhoc (EDHOC engine + suite-0 helper) | 14,794 | 0 |
| uoscore-uedhoc (OSCORE half) | 9,284 | 14 |
| compact25519 (X25519 scalar mult) | 5,966 | 0 |
| zcbor (CBOR for libedhoc + uoscore) | 2,194 | 0 |
| mbedTLS (a thin residue; OSCORE uses PSA, not the TLS layer) | 88 | 16,384 |
| **crypto subtotal** | **~61,900** | **~17,082** |

- **Flash (+65 KB):** the crypto stack above (~62 KB) plus ~1.2 KB of app
  (`src/edhoc_oscore.c`). tf-psa-crypto is **2.1× the DTLS build's** (29.6 vs
  13.9 KB) because OSCORE adds HKDF-Extract/Expand and keeps the ~8.7 KB AES
  lookup tables in flash (`MBEDTLS_AES_ROM_TABLES=y`). X25519 is **not** in
  mbedTLS — compact25519 does the scalar multiplication (X-coordinate only, so no
  ECP/bignum/point-decompression), which is why there is no P-256-style ECC bloat.
- **RAM (+25 KB, i.e. *less* than DTLS):** the **16 KB PSA heap** again, plus a
  **+6.5 KB CoAP-server thread stack** (`COAP_SERVER_STACK_SIZE=8192` vs base
  2048 — the whole EDHOC handshake runs synchronously on that thread) and ~1.2 KB
  app. It stays below DTLS because `MBEDTLS_AES_ROM_TABLES=y` saves the ~8.7 KB
  AES RAM tables, libedhoc's stack/VLA backend adds no static RAM, and OSCORE
  keeps the base net-pool sizes rather than DTLS's larger ones.

## Performance — CoAP round-trip latency

Confirmable (CON) and Non-confirmable (NON) round trips over the live USB-NCM link,
one persistent connection, shell + logging off, 1000 samples/endpoint, 0 % loss.
Representative medians (all endpoints within ~0.1 ms → transport-bound):

| Transport | CON med | NON med | p95 | p99 | one-time handshake |
|---|--:|--:|--:|--:|--:|
| plaintext CoAP (`:5683`) | ~1.60 | ~1.60 | ~1.75 | ~1.9–2.2 | — |
| DTLS-PSK CoAPS (`:5684`) | ~3.20 | ~3.20 | ~3.35 | ~3.5 | **~19 ms** |
| EDHOC+OSCORE (`:5683`) | ~3.80 | — † | ~4.0 | ~4.1–4.35 | **~1.6 s** |

*(ms; full round trip host → board IP/UDP/(security)/CoAP + handler → host.
† OSCORE was measured with the CON-only `host_client`; on the clean link CON==NON
holds for the other two, and OSCORE has no reason to differ.)*

- **Transport-bound.** `GET /hello`, `GET /led` and `PUT /led` are
  indistinguishable → CoAP parse + resource-handler cost is negligible.
- **CON == NON.** Confirmable reliability adds no latency for a single exchange on
  this clean link (retransmit only matters under loss).
- **Plaintext ~1.6 ms floor** = USB full-speed frame timing (~two 1 ms frames per
  round trip).
- **DTLS-PSK adds ~1.6 ms/request** (AES-128-CCM-8 + slightly larger records) on a
  **~19 ms** symmetric handshake.
- **OSCORE adds ~2.2 ms/request** (~0.6 ms over DTLS — OSCORE option processing +
  AEAD both ways, on the CoAP-server thread). Its **handshake is ~1.6 s** and very
  stable (1.60–1.65 s across runs): this is the software **X25519 static-DH** on
  the Cortex-M33, not the network — EDHOC method 3 does multiple scalar
  multiplications per side. It is a *one-time, per-boot* cost (a fresh EDHOC run
  re-keys OSCORE, SSN from 0), amortized over the session.

Not covered: sustained throughput, and a like-for-like ACM+UART latency baseline.

## Notes & caveats

- Numbers refreshed 2026-07-03; they supersede an earlier revision (the app and
  module set changed, and that revision predated the OSCORE variant).
- Shell + logging are **off** in the measured images; the dev console adds the
  flash/RAM noted at the top and its synchronous UART I/O inflates latency.
- The PSK identity/key (`zbook` / `zbook-dtls-psk!!` in `src/coap_server.c`, mirrored
  in `scripts/coap_latency.py`) and the EDHOC credentials (`scripts/edhoc_keys.py`)
  are **DEV** values — replace and provision out-of-band for production.
- `dtls.conf` builds with `-DMBEDTLS_FATAL_WARNINGS=OFF` (upstream tf-psa-crypto
  `-Werror` warnings, not this app).
- The X.509 latency was never measured (the software ECDHE-ECDSA handshake wouldn't
  complete with the Python test client); PSK and EDHOC/X25519 both do.
- OSCORE latency is produced by the native `host_client --bench` (EDHOC Initiator +
  OSCORE client built from the same libedhoc/uoscore libraries the device runs), so
  interop is guaranteed at the encoder level.

## Regenerate

```bash
# memory — pristine-builds the three perf variants and dumps rom/ram reports
just footprint

# performance (host, NCM link up at 192.0.2.2; flash the matching perf build,
# power-cycle the board to re-enumerate USB, then):
just flash-perf         && just latency          # plaintext  (:5683)
just flash-dtls-perf    && just latency-dtls      # DTLS-PSK   (:5684)
just flash-oscore-perf  && just latency-oscore    # EDHOC+OSCORE (:5683)
```

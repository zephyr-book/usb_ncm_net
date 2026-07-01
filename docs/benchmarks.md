# Benchmarks: memory & performance — USB-NCM+CoAP vs USB-ACM+UART+binary protocol

Board `zbook/rp2350b/m33` (RP2350B: 2 MB flash, 520 KB RAM), Zephyr v4.4.0,
zephyr-sdk arm-zephyr-eabi, `-Os`. Two dimensions are measured:

- **Memory** — bytes, from `west build -t rom_report` / `-t ram_report`
  (`scripts/footprint/size_report`, which attributes every symbol to its
  `ZEPHYR_BASE` source path), cross-checked against `zephyr.map`.
- **Performance** — CoAP round-trip latency, from `scripts/coap_latency.py`
  (times Confirmable requests over the live USB-NCM link).

Regenerate with `just footprint` and `python3 scripts/coap_latency.py`
(see bottom).

Two firmwares are compared:

| Firmware | Transport | App protocol |
|---|---|---|
| `usb_ncm_net` (this repo) | USB CDC-NCM → IPv4/UDP | CoAP server |
| `esp01_flasher` (sibling repo) | USB CDC-ACM ↔ UART bridge | none (transparent) |

`usb_ncm_net` is measured in two profiles:

- **dev** (`prj.conf`): full diagnostics — `net` shell, `coap_server` shell,
  general shell, logging.
- **minimal** (`prj.conf + minimal.conf`, `just build-min`): production —
  trimmed net pools/contexts, IPv4 features unused by unicast CoAP turned off,
  and the `net` + `coap_server` shells dropped. A bare console (`CONFIG_SHELL`)
  and logging are kept.

## TL;DR

- **Cost of "network over USB" itself** (CDC-NCM function class + IPv4/UDP +
  Ethernet L2 — everything that turns a USB link into an IP socket, excluding
  diagnostics): **~22.9 KB flash, ~14 KB RAM** (minimal profile).
  - Of which the **IPv4/UDP + L2 stack alone is ~19.3 KB flash** — and ~90 % of
    that is irreducible core (contexts/sockets, packet mgmt, IPv4 in/out, ICMP
    errors, ARP, L2 framing, checksums). It's the price of speaking IP.
- **Cost of the CoAP stack** (separated): **~4.6 KB flash + ~1 KB app,
  ~2.5 KB RAM**.
- **Net-new price of NCM+CoAP over ACM+UART+binary** — comparing only the layers
  that differ (shared USB device core and optional shell/logging excluded from
  both): **≈ +22 KB flash and +10 KB RAM**.
- On the RP2350B that is **~1.1 % of flash and ~2 % of RAM** — negligible
  headroom impact. The interoperability/maintainability win of standard CoAP +
  IP comes at a small, fixed, well-understood memory cost.
- **CoAP round-trip latency over USB-NCM: ~2.1 ms median, ~3 ms p99, 0 % loss** —
  transport-bound (the CoAP handler cost is negligible; the floor is USB
  full-speed frame timing). See *Performance* below.

## Whole-image totals

| | FLASH | RAM |
|---|--:|--:|
| `usb_ncm_net` dev (`prj.conf`) | 192,396 | 53,904 |
| `usb_ncm_net` minimal (`+minimal.conf`) | 153,988 | 39,704 |
| `esp01_flasher` (ACM+UART, headless) | 111,432 | 25,032 |

⚠️ The raw whole-image difference is *not* "the cost of CoAP" — the images
differ in more than the transport (shells, logging, board peripherals). The
per-layer tables below isolate the real cost.

## `usb_ncm_net` per layer (CoAP separated)

`size_report` path → bytes. "core" of `device_next` = subsystem total minus the
function class.

| Layer | ROM (min) | ROM (dev) | RAM (min) | RAM (dev) |
|---|--:|--:|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ ACM)* | 15,019 | 15,019 | 5,674 | 5,674 |
| CDC-NCM function class — `class/usbd_cdc_ncm.c` | 3,577 | 3,577 | 4,374 | 4,374 |
| net/ip — IP, UDP, sockets, contexts, `net_pkt`, traffic classes | 16,014 | 20,978 | 9,496 | 17,902 |
| net L2 — `net/l2/ethernet` (ARP) | 3,310 | 3,488 | 201 | 201 |
| **➤ "Network over USB" subtotal** (NCM class + net/ip + L2) | **22,901** | 28,043 | **14,071** | 22,477 |
| **➤ CoAP stack** — `subsys/net/lib/coap` | **4,610** | 5,982 | **2,486** | 7,222 |
| CoAP app resources — `src/coap_server.c` (`hello`, `led`) | ~1,000 | ~1,000 | 117 | 117 |
| *General shell backend (bare console; kept in minimal)* | *16,394* | *16,514* | *4,383* | *4,383* |
| *`net` + `coap_server` shell commands (dev only)* | *0* | *~14,700* | *0* | *~440* |
| *Logging* | *~3,800* | *~3,800* | *2,242* | *2,242* |

The **dev → minimal** delta comes from: dropping the `net` shell (which also
force-selected IGMP/IP-options and pulled in `ping`/`icmp.c`), dropping the
`coap_server` shell, turning off IPv4 features unused by unicast CoAP (IGMP,
IP header options, DSCP/ECN, gratuitous ARP), and shrinking the net pools
(pkt/buf 14/36→4/16, `NET_MAX_CONTEXTS` 6→1) and CoAP knobs (thread stack
4096→2048, pending 10→1, block 256→64, observers 3→0, no `.well-known/core`).
net/ip flash falls 20,978 → 16,014 (~5 KB) and RAM 17,902 → 9,496 (~8 KB);
CoAP RAM 7,222 → 2,486.

Biggest individual RAM line items (minimal):

- `net_buf_data_cdc_ncm_ep_pool` = **4,096** — the CDC-NCM USB↔Ethernet buffer;
  dominates NCM class RAM. Hardcoded `2 × 2048` in the driver (not tunable
  without an upstream patch + MTU cap).
- `net_pkt` rx/tx data = **2×2,048**; CoAP server thread stack = **2,048**;
  general shell `shell_uart` stack = **2,048**; USB thread `usbd_stack` = 1,024;
  UDC endpoint configs = 2×640.

## `esp01_flasher` per layer (USB-ACM + UART baseline)

Sanity check: the ACM report contains **no `subsys/net` and no `coap`** paths —
the ACM path genuinely has no network stack. It is also headless (no shell).

| Layer | ROM | RAM |
|---|--:|--:|
| USB device core — `drivers/usb/udc` + `device_next` core *(shared w/ NCM)* | 15,639 | 5,390 |
| CDC-ACM function class — `class/usbd_cdc_acm.c` | 2,908 | 2,078 |
| UART driver — `drivers/serial/uart_pl011.c` | 1,792 | 4 |
| Ring-buffer lib — `sys/ring_buffer` | ~362 | 0 |
| App bridge ring buffers — `src/main.c` (2×2,048) † | 0 | 4,096 |

† The 4 KB ring buffers belong to the *transparent bridge*; a real
"ACM + binary protocol" app would size its own (typically smaller) buffers.
Counted here because they exist in the measured firmware, and flagged so the ACM
column isn't over-credited.

The shared USB device core is essentially identical on both sides
(**~15 KB ROM / ~5.5 KB RAM**) — common cost both approaches pay.

## The delta — price of NCM+CoAP over ACM+binary

Comparing only the layers that differ (shared USB core excluded; shell/logging
excluded as optional and not intrinsic to either transport). Uses the **minimal**
`usb_ncm_net` numbers.

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

**As a fraction of the RP2350B:** +22 KB flash ≈ 1.1 % of 2 MB; +10 KB RAM ≈
2.0 % of 520 KB.

## Can the net stack shrink further?

Mostly no — the ~19.3 KB of net/ip+L2 is ~90 % irreducible core. What was
reclaimed (dev → minimal) came almost entirely from **dropping the `net` shell**,
which force-selected IGMP (`net/lib/shell/Kconfig`), IGMP in turn selected
IP-header-options, and the shell also pulled in `ping`/`icmp.c` — together ~5 KB
of *diagnostic* net code, not data-path. Beyond that, only DSCP/ECN and
gratuitous-ARP (~0.3 KB) were freely toggleable; everything else optional
(fragmentation, PMTU, ACD, promiscuous, IGMPv3, `NET_CONTEXT_*` socket options)
was already off. `icmpv4.c` (~1.5 KB, ICMP error replies incl. port-unreachable)
can't be removed while keeping IPv4. The remaining core is the fixed cost of
having IP/UDP sockets over Ethernet — exactly what the ACM+binary approach avoids
by doing raw byte framing.

## Performance — CoAP round-trip latency

Confirmable (CON) CoAP round trips to the board over the live USB-NCM link,
measured from the macOS host with `scripts/coap_latency.py` (one persistent UDP
socket, so no per-request process overhead — unlike `coap-client` in a shell
loop). Minimal build, 1000 samples per endpoint after 50 warm-up, **0 % loss**:

| Endpoint | median | mean | p95 | p99 | max | stddev |
|---|--:|--:|--:|--:|--:|--:|
| `GET /hello` | 2.08 | 2.17 | 2.55 | 3.57 | 14.55 | 0.60 |
| `GET /led`   | 2.05 | 2.11 | 2.45 | 3.11 |  5.08 | 0.26 |
| `PUT /led=1` | 2.06 | 2.11 | 2.49 | 3.07 |  9.95 | 0.34 |

*(milliseconds; full CON round trip: host → board IP/UDP/CoAP + handler → host)*

- **~2.1 ms median, ~2.5 ms p95, ~3 ms p99, 0 % loss.**
- **Transport-bound, not compute-bound.** All three endpoints agree within
  ~0.06 ms — `GET /hello` (static string), `GET /led` (reads a flag) and
  `PUT /led=1` (parses a payload + drives a GPIO) are indistinguishable, so the
  CoAP parse + IP/UDP + resource-handler cost is negligible; essentially all
  ~2 ms is the USB-NCM transport.
- **The ~2 ms floor is USB full-speed timing.** RP2350 USB is full-speed (1 ms
  frame intervals); a round trip crosses the host→device and device→host bulk
  endpoints, each gated by the host's ~1 ms USB polling ≈ two frame times. It
  won't drop much without high-speed USB or host-side tuning.
- **Tail (5–15 ms outliers) is host-side jitter** — macOS USB/NCM scheduling and
  NTB buffering, not the board (p99 stays ~3 ms; only rare samples spike).

Not yet measured: sustained throughput, and the ACM+UART+binary latency baseline
for a direct comparison (a raw framed request/response over the CDC-ACM serial
port at a given baud). Those are the remaining performance follow-ups.

## Notes & caveats

- Report-tool totals reconcile with `west`: ROM within ~20 B; the `ram_report`
  tool total (~37.2 KB minimal) is ~2.5 KB below the `west` RAM figure (small
  unattributed remainder), so headline whole-image RAM uses the `west` total and
  per-layer rows use the tool.
- The dev profile keeps all three shells + full logging (~35 KB flash / ~7 KB
  RAM of diagnostics) for bring-up. The minimal profile drops the `net` and
  `coap_server` shells but keeps a bare console + logging. Both are optional and
  excluded from the delta above; `esp01_flasher` ships headless.
- The "binary protocol" application layer for the ACM approach is not measured
  (esp01_flasher is a transparent bridge). It is assumed small (a parser + a
  buffer), comparable to the ~1 KB flash / ~120 B RAM of the CoAP app resources
  here — i.e. the app layer is a wash; the cost is in the stacks.
- The 4 KB CDC-NCM endpoint pool is `2 × 2048` hardcoded in the driver; halving
  it needs an upstream patch plus capping the interface MTU below ~980 B — not
  worth 2 KB on this part.
- CoAP round-trip latency is covered above. Sustained throughput and the
  ACM+UART+binary latency baseline (for a direct performance comparison) are the
  remaining follow-ups.
- Latency figures are from one representative run on a macOS-ARM host; the tail
  reflects host USB scheduling, so absolute p99/max vary a little run to run
  while median/p95 are stable. Reproduce with `scripts/coap_latency.py`.

## Regenerate

```bash
# from inside usb_ncm_net/ --- memory
just footprint          # builds both profiles and dumps ram/rom reports

# ACM baseline (from inside esp01_flasher/):
west build -p always -b zbook/rp2350b/m33 -d build
west build -d build -t rom_report
west build -d build -t ram_report

# performance (host, with the NCM interface up at 192.0.2.2):
python3 scripts/coap_latency.py           # CoAP round-trip latency
```

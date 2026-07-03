# EDHOC (RFC 9528) + OSCORE (RFC 8613) on `usb_ncm_net` — design record

> **Status (2026-07): EDHOC engine = [`libedhoc`](https://github.com/kamil-kielbasa/libedhoc)
> v1.14.1; OSCORE half = `uoscore-uedhoc`.**
> - **Suite 2 / P-256 (first cut):** verified on hardware against `aiocoap`/`lakers`
>   — full handshake + OSCORE `hello`/`led` round-trip (which the old
>   `uoscore-uedhoc` engine could not do). But the handshake was **slow (~4 s)**:
>   software P-256 + point decompression on the RP2350 (no ECC accel), and it
>   dragged in mbedTLS's legacy ECP (a vendored+patched helper +
>   `DECLARE_PRIVATE_IDENTIFIERS`).
> - **Suite 0 / X25519 (current, verified on hardware):** to make the *device*
>   efficient, switched to cipher suite 0. X25519 ECDH is X-coordinate-only (no
>   point decompression, no mbedTLS ECP) via libedhoc's suite-0 helper +
>   `compact25519`, far faster on the RP2350 and drops all the legacy-ECP
>   machinery. The peer had to change too (aiocoap/lakers are P-256-only): the
>   host client is now a native **C/C++** client (`host_client/`) built from the
>   SAME libs (libedhoc + uoscore) so interop is guaranteed. **Verified on
>   hardware:** fast handshake + OSCORE `hello`/`led` round-trip. Device oscore
>   12.7% FLASH / 13.8% RAM (below the DTLS variant).
>   - Gotcha fixed on the way: `CONFIG_COAP_SERVER_MESSAGE_SIZE` defaults to
>     `COAP_SERVER_BLOCK_SIZE` (64), too small for the `message_1` POST (32-byte
>     G_X + CoAP framing ~65 B) -> server returned 4.13 before the handler ran.
>     Bumped to 256 in oscore.conf.
> The `uoscore-uedhoc` prototype below was re-applied and taken to hardware,
> where the live handshake against `aiocoap`/`lakers` failed. Root cause: **two
> unfixed RFC-9528 encoding bugs in `uoscore-uedhoc`** (see the next section) that
> make it non-interoperable with any RFC-correct peer for CCS-by-kid credentials.
> Decision: **replace only the EDHOC engine with `libedhoc`** (pure C, final
> RFC 9528, verified byte-exact against RFC 9529 §3 = our exact profile, Zephyr
> module, mbedTLS/PSA, MIT) and **keep `uoscore-uedhoc` for OSCORE**. The
> migration steps live in the plan file
> `~/.claude/plans/parallel-wobbling-prism.md`. Everything from `## Context`
> onward is preserved as reference — the **OSCORE intercept, CoAP dispatch, RFC
> 9668 framing, and `oscore_context_init` usage are retained**; only the EDHOC
> responder engine (§3, and the `uoscore-uedhoc`-specific build-time resolutions)
> is superseded.

## libedhoc integration — as-built notes (2026-07-02)

The libedhoc responder recipe in the plan file matched v1.14.1's API. What the
plan did **not** anticipate were integration frictions against **this** Zephyr's
mbedTLS (v4.4.0 = tf-psa-crypto 4.x). Keep these for next time:

- **`edhoc_post()` drives libedhoc synchronously** across the two POSTs:
  `edhoc_message_1_process` → `edhoc_message_2_compose` (reply); then
  `edhoc_message_3_process` → `edhoc_message_4_compose` (reply) →
  `edhoc_export_oscore_session` → `oscore_context_init`. One persistent
  `struct edhoc_context`, mutex-guarded, reset on each fresh `message_1`. No
  responder thread / no semaphores. The exporter returns Sender ID = C_I and
  Recipient ID = C_R already oriented and CBOR-encoded → fed straight into
  `oscore_init_params`.
- **Flow = RFC 9668 *sequential*, and it needs `message_4`.** The host client
  (`scripts/edhoc_oscore_client.py`) sets aiocoap `use_combined_edhoc=False`.
  aiocoap's default is the *combined* request (message_3 folded into the first
  OSCORE request via the EDHOC CoAP option) — the device does **not** implement
  that; a combined request lands on `oscore_post` before the handshake completes
  ("OSCORE packet before handshake"). aiocoap's non-combined initiator instead
  POSTs message_3 to `/.well-known/edhoc` and **processes a `message_4` reply**,
  so the device composes one (the plan's "no message_4" assumption did not hold
  against aiocoap). Combined-request support on the device remains a future option.
- **Client CoAP timeout must exceed the device compute.** The ~4 s/message
  latency exceeds aiocoap's default 2 s ACK timeout → it would retransmit
  message_1, and the responder regenerates its ephemeral key on every message_1,
  derailing the handshake. The client raises `TransportTuning.ACK_TIMEOUT` to 12 s
  (patched on the class so aiocoap's internal EDHOC wire requests inherit it).
- **Credentials:** bstr kids (I=`0x2a`, R=`0x2b`). A length-1 bstr kid in
  `0x20–0x37` is transported as a naked CBOR int (RFC 9528 §3.3.2), so the
  `verify` callback receives ID_CRED_I as an **integer** (`0x2a`→`-11`) — it must
  match on that yet write back the **byte-string** form so the MAC_3 context is
  `{4: h'2a'}`. `cred_fetch` sets the R kid byte-string + imports `ED_R_PRIV` via
  `edhoc_cipher_suite_2_key_import(EDHOC_KT_KEY_AGREEMENT)`. Peer key = 32-byte X
  (`ED_G_I`). C_R = one-byte int `-8`. All from `scripts/edhoc_keys.py`.
- **Two flat `edhoc.h`** (libedhoc `include/`, uoscore `inc/`) sit on the global
  include path. `CMakeLists.txt` fixes both directions with
  `target_include_directories(... BEFORE PRIVATE ...)`: libedhoc-first for `app`,
  uoscore-first for `uoscore`/`uoscore_uedhoc_common` (else `crypto_wrapper.c`
  gets libedhoc's header and `WEAK` is undefined).
- **libedhoc's cipher-suite-2 helper vs mbedTLS 4.x:** the helper decompresses
  the peer's 32-byte X with the classic mbedTLS ECP API, which 4.x made
  "private". Fixes: `CONFIG_MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS=y` (re-exposes the
  legacy API + generates a public `mbedtls/ecp.h`); the helper is
  **vendored+patched** to `vendor/edhoc_cipher_suite_2.c` because it also used
  `mbedtls_pk_ec()` (removed in 4.x) — the patch loads a standalone
  `mbedtls_ecp_group` instead; and CMakeLists maps the removed `PSA_KEY_HANDLE_INIT`
  to `PSA_KEY_ID_NULL`. The classic PK/X.509 modules are **not** enabled.
- **west.yml:** libedhoc pinned `v1.14.1` (remote `kamil-kielbasa`,
  `deps/modules/lib/libedhoc`). Its git submodules are unused on Zephyr.

## X25519 handshake cost — investigation (2026-07)

Suite 0 cut the handshake from suite-2/P-256's ~4 s to **~1.6 s**, but that is
still almost entirely **software X25519 static-DH on the M33** (no ECC accel): the
Responder does ~4 scalar multiplications per handshake (ephemeral keygen, `G_XY`,
`G_RX` for MAC_2, `G_IY` verifying MAC_3). On-board microbenchmark of
`compact25519`/`c25519` (zbook/rp2350b/m33 @ 150 MHz):

- **~383 ms per scalar mult at `-Os`** (~300 ms at `-O2`/`-O3`) ≈ **~45–57M
  cycles** — ~10× slower than c25519 typically runs on a Cortex-M, because its
  field arithmetic is **byte-limb (8-bit)**. That is the bottleneck, corroborated
  two ways (microbench, and the ~1.6 s end-to-end handshake ÷ ~4).
- **Algorithm-bound, not codegen- or memory-bound.** Two cheap levers were
  measured and **rejected**: recompiling only the c25519 sources at `-O2`/`-O3`
  (−22%); and relocating them from XIP flash into SRAM via `zephyr_code_relocate`
  (−6.5%, so **compute-bound, not flash-bound**). Neither closes the ~80× gap to
  DTLS-PSK's ~19.6 ms handshake.
- **Only lever with real upside: a faster pure-C X25519 backend** (wider limbs; no
  ARM asm, must stay constant-time). A formally-verified one is **already
  vendored** — HACL\* Curve25519 in
  `deps/modules/crypto/tf-psa-crypto/drivers/everest/` (enable via PSA + route the
  suite-0 ECDH through it — no new dependency); curve25519-donna `c32` / BearSSL
  `c25519_m31` are alternatives that need vendoring.

**Decision: closed without swapping the backend.** Under WFI/tickless idle the
session survives sleep, so the handshake is a **one-time, per-power-on** cost
(never re-paid on wake) — a weak factor in the DTLS-vs-OSCORE choice, which turns
on footprint + security posture instead. Numbers + rationale:
[`docs/benchmarks.md`](benchmarks.md).

## EDHOC engine: why `uoscore-uedhoc` was abandoned (2026-07)

Verified against **RFC 9528** (§3.3.2 identifiers, §5.3.2 message_2/context_2),
the official **RFC 9529 §3** trace (CCS + static-DH + kid — our exact profile),
and the library source at pin `dc0ab63` (identical at `eriptic` `main` `002de27`,
stale since 2026-03):

- **Bug #1 — `c_r_is_raw_int()`** (`src/edhoc/int_encode_decode.c:24`):
  `c_r->ptr[0] >= -24` is a no-op on `uint8_t`, so it matches only `0x00–0x17`
  and misses the negative one-byte ints `0x20–0x37` that §3.3.2 also requires be
  integer-encoded. The sibling `c_x_is_encoded_int()` (same file) is correct. The
  **plaintext** path (`ciphertext.c:200`) uses the correct predicate; the **MAC**
  path (`signature_or_mac_msg.c:67`) uses the buggy one *and* does
  `encode_int((const int32_t *)c_r->ptr, …)` — an **OOB read** of a 1-byte buffer.
  ⇒ for C_R in `0x20–0x37` the responder encodes C_R inconsistently (raw-int in
  the plaintext, bstr in the MAC context) so the MAC won't match an RFC peer; and
  for C_R in `0x00–0x17` it reads out of bounds (the `err 3` boot/handshake crash).
- **Bug #2 — `id_cred2kid()`** (`src/edhoc/plaintext_encode.c:35`): ignores
  `id_cred_x_map_kid_choice` and unconditionally re-encodes the **int** union
  member, so a **byte-string kid is mishandled** (library is int-kid-only).
  RFC 9529 §3 shows the canonical form: kid is a **bstr**, the MAC context uses
  the full map `a1 04 41 XX`, and the wire compresses to a naked int. lakers/
  aiocoap do exactly this. The device can match the MAC context (it uses
  `ID_CRED_R` verbatim) *only* with a bstr kid, but then bug #2 emits a garbage
  wire kid; or it emits a correct wire kid *only* with an int `ID_CRED`, but then
  the MAC context is `a1 04 XX` (int) ≠ the peer's `a1 04 41 XX` (bstr). **No
  identifier value satisfies both** ⇒ RFC-interop impossible without patching.

**Upstream status:** neither bug is fixed and `main` is stale since 2026-03.
Bug #2 has an **open-but-abandoned PR #59** ("Support for key ID as integer or
byte string", 2023, merge-conflicted, zero reviews) — authored by **Kamil
Kiełbasa, who is also the author of `libedhoc`**. Bug #1 has no PR/issue (→ file
one on `eriptic/uoscore-uedhoc`, referencing PR #59 for bug #2). That the person
who diagnosed this bug class upstream, was ignored, and then wrote a clean
replacement is the strongest argument for adopting `libedhoc`.

**`libedhoc` responder recipe, credential/crypto callback structs, Kconfig, and
the RFC 9529 ch.3 template** are captured in the plan file
`~/.claude/plans/parallel-wobbling-prism.md` (folded in from a research pass).
`libedhoc`'s synchronous per-message API (`edhoc_message_1_process` /
`_2_compose` / `_3_process` / `edhoc_export_oscore_session`) lets us **drop the
dedicated responder thread + semaphores** in §3 below and drive the state machine
directly from `edhoc_post()`; the exporter returns master_secret/salt +
sender_id/recipient_id, feeding `oscore_context_init` directly.

---

> **Everything below is the original `uoscore-uedhoc` design record**, retained
> as reference. The OSCORE / CoAP-intercept / framing parts are kept; the EDHOC
> engine (§3) and the `uoscore-uedhoc` build-time resolutions are superseded by
> the `libedhoc` migration above.

## Context

`usb_ncm_net` is a Zephyr CoAP **server** on the ZBook (RP2350B), reached by a host
PC over USB CDC-NCM (device `192.0.2.1`, host `192.0.2.2`). It exposes `hello` (GET)
and `led` (GET/PUT). Today it ships plaintext CoAP (`prj.conf`, UDP 5683) and an
optional DTLS/PSK variant (`dtls.conf`, UDP 5684). Goal: add **application-layer
security** — EDHOC to establish keys, OSCORE to protect CoAP — as a **third build
variant** so the plaintext/DTLS baselines stay intact and comparable (the app's
purpose is latency comparison via `scripts/coap_latency.py`).

Reality differs from the source brief: it's **one device + one host PC**, not two
Zephyr nodes. So **device = CoAP server = EDHOC Responder**; **host = client =
EDHOC Initiator** (Python). And the module ships its **own** `crypto_wrapper.c` — we
do **not** hand-write one.

## Decisions (locked with user)
- Scope this round: **through one OSCORE-protected round-trip**. SSN persistence deferred.
- Purpose: **working prototype** (robust demo, not production).
- Protocol: **suite 2 (P-256 / SHA-256 / AES-CCM-16-64-128)** — Zephyr glue selects P-256, not X25519 (documented fallback). **Method 3** (static DH both sides, no sigs). Credentials = **CCS/COSE_Key** (raw public key, no X.509), by `kid`. **Sequential** EDHOC-then-OSCORE flow; RFC 9668 combined request deferred.
- Peer: **host Python** (aiocoap OSCORE + lakers-python EDHOC Initiator).
- Server I/O: **keep `COAP_SERVICE`**; add an OSCORE intercept + an EDHOC resource.
- Packaging: new **`oscore.conf`** overlay variant.
- No SSN NVM now → **fresh EDHOC every boot** (SSN starts at 0, no nonce reuse).
- Access policy: `hello`/`led` reachable **plaintext + OSCORE during bring-up**, then flip to **OSCORE-only** (reject unprotected) once the round-trip is confirmed.
- Latency-comparison tooling (`just build-oscore`, `coap_latency.py --oscore`): **deferred** to a later round; this round ships the dedicated `edhoc_oscore_client.py` correctness proof.

## Key discovery facts (verified)
- Kconfig symbols: `CONFIG_UEDHOC`, `CONFIG_UOSCORE`; both `depends on ZCBOR && ZCBOR_CANONICAL`, `select UOSCORE_UEDHOC_CRYPTO_COMMON` → auto-selects `PSA_CRYPTO` + P-256/ECDH/ECDSA/CCM/HMAC/SHA-256/AES.
- Module `CMakeLists.txt` builds `src/common/crypto_wrapper.c` with `-DMBEDTLS`, links `mbedTLS`. **App writes no crypto wrapper.**
- `zcbor` and `uoscore-uedhoc` **sources not fetched** — only glue in `deps/zephyr/modules/`.
- `mbedtls` + `tf-psa-crypto` already in `usb_ncm_net/west.yml` allowlist; PSA supports P-256/CCM/SHA-256/ECDH/HKDF. RP2350 TRNG on, `CONFIG_ENTROPY_GENERATOR=y`.
- CoAP routing: empty-path `{NULL}` `.post` resource matches only no-Uri-Path requests (the OSCORE outer msg). `coap.c:1089 coap_uri_path_match`, dispatch `coap.c:1179`.
- `struct coap_packet` exposes raw bytes via `->data` / `->offset`; handlers already use `coap_packet_get_payload`, `coap_resource_send`.

## Implementation

### 1. Manifest + build (get a clean build first)
- `usb_ncm_net/west.yml`: add to zephyr import `name-allowlist`: `zcbor`, `uoscore-uedhoc`. Then `west update` (pins come from Zephyr v4.4.0's west.yml).
- New `usb_ncm_net/oscore.conf` (overlay, mirrors `dtls.conf` shape):
  - `CONFIG_UEDHOC=y`, `CONFIG_UOSCORE=y`, `CONFIG_ZCBOR=y`, `CONFIG_ZCBOR_CANONICAL=y`.
  - `CONFIG_MBEDTLS=y` + `CONFIG_MBEDTLS_ENABLE_HEAP=y`, `CONFIG_MBEDTLS_HEAP_SIZE=16384` (PSA/mbedTLS heap; lib itself is stack-only, PSA backend still wants heap as in dtls.conf).
  - PSA wants are auto-selected by Kconfig; add explicitly only if build complains.
  - Stack/pool bumps (EDHOC/CBOR/CCM temporaries are large): dedicated EDHOC thread stack **~10240**; `CONFIG_COAP_SERVER_STACK_SIZE=6144`; `CONFIG_MAIN_STACK_SIZE=4096`; net pools like dtls.conf (`NET_PKT_*=6`, `NET_BUF_*=24`, `NET_MAX_CONTEXTS=3`). Enable `CONFIG_THREAD_ANALYZER=y` + stack-usage check while bringing up; trim later.
  - Keep `CONFIG_LOG=y` **during bring-up** to see EDHOC/OSCORE errors; turn off for latency runs.
- Build: `west build -p always -b zbook/rp2350b/m33 -- -DEXTRA_CONF_FILE=oscore.conf` (`-p always` required when switching variant). Optional `just build-oscore`.
- Milestone A = compiles + links + boots with `CONFIG_UEDHOC/UOSCORE` on, `psa_crypto_init()` returns OK.

### 2. Keys / credentials (single source of truth)
- Generate **once** (host `scripts/edhoc_keys.py`, `cryptography` `ec.generate_private_key(ec.SECP256R1())`): two P-256 static-DH keypairs (Initiator `I`, Responder `R`). `d`=`private_value` (32B BE); `x`,`y`=public coords (32B each). kids 1 byte, e.g. `kid_I=0x2a`, `kid_R=0x2b`.
- **CCS/COSE_Key** (raw pubkey, no X.509), `cbor2`, **subject (outer key 2) must be a text string** (lakers rejects a bstr/int subject): `CCS = {2: "<subject text>", 8: {1: {1:2, 2:kid, -1:1, -2:x, -3:y}}}` (COSE_Key inside a CWT Claims Set). Canonical CBOR. lakers/aiocoap parse it via `lakers.Credential(bytes)`.
- `edhoc_keys.py` is the single source of truth; it emits (a) a C header `include/edhoc_creds.h` (device=R: private `R` + `ID_CRED_R`/`CRED_R` + peer public `CRED_I`, `g` fields as the 32-byte X-coordinate); (b) `scripts/edhoc_creds.json` for the host client. Private halves stay out of the header. **Byte-for-byte agreement is mandatory** (CRED bytes feed the transcript hash).
- Cross-check format against RFC 9528 App. C and the module `test/` vectors (note: vectors 5/6 are method 3 but use X.509 creds, not CCS).

### 3. Device: EDHOC responder (`src/edhoc_oscore.c`, new)
- **Resource** `edhoc`: `path = {".well-known","edhoc",NULL}`, `.post = edhoc_post`.
- uEDHOC is run-to-completion via `tx`/`rx` callbacks, but the 3 messages span **two** CoAP POSTs → bridge with a **dedicated EDHOC thread** + two semaphores (`in_ready`/`out_ready`) over shared in/out buffers:
  - EDHOC thread runs `edhoc_responder_run_extended(&ctx, &cred_i_array, &err_msg, &prk_out, &init_pk, &c_i, tx_cb, rx_cb, ead)`. `rx_cb` blocks on `in_ready`; `tx_cb` fills the out buffer + gives `out_ready`.
  - `edhoc_post`: strip RFC 9668 framing (detect by first byte: `0xf5` → message_1, strip 1 byte; else → message_3, strip the leading CBOR-encoded `C_R` item), hand payload to the thread, wait for the reply, send as `2.04 Changed`. On msg_3 the run completes with **no message_4**, so the thread posts an empty sentinel → handler sends an empty `2.04`.
- After completion: `prk_out2exporter(SHA_256, &prk_out, &prk_exp)` → `edhoc_exporter(SHA_256, OSCORE_MASTER_SECRET|SALT, &prk_exp, …)` → OSCORE **Master Secret** (16B) + **Master Salt** (8B) → `oscore_context_init(&params, &g_oscore_ctx)`; set `g_oscore_ready=true`.
- Seed the ephemeral via `ephemeral_dh_key_gen(P256, sys_rand32_get(), &y, &g_y)` (P-256/MBEDTLS ignores the seed and uses the PSA DRBG ← RP2350 TRNG).

### 4. Device: OSCORE intercept (`src/edhoc_oscore.c`)
- **Resource** `oscore_root`: `path = {NULL}`, `.post = oscore_post` (catches OSCORE outer msgs only — verified via `coap_uri_path_match`).
- `oscore_post`: `oscore2coap(request->data, request->offset, coap_buf, &coap_len, &g_oscore_ctx)` (return code `ok`/`not_oscore_pkt`/`first_request_after_reboot`) → `coap_packet_parse()` → shared dispatch to hello/led → `coap2oscore(resp, resp_len, out, &out_len, &g_oscore_ctx)` → send `out[0..out_len]` via `coap_resource_send` (wrap bytes in a `coap_packet` whose `.data`/`.offset` point at `out`; `coap_service_send` transmits `cpkt->data[0..offset]`).
- Refactor `hello`/`led` handlers into a shared `coap_server_dispatch_inner(request, buf, len, &response)` (exposed via `coap_server.h`) so the plaintext resources and the OSCORE path reuse the same builders.
- Reject/ignore if `!g_oscore_ready`.
- Enforcement: during bring-up leave `hello`/`led` reachable in the clear. Once the round-trip is confirmed, gate them so unprotected GET/PUT return `4.01 Unauthorized` and only the OSCORE path reaches them.

### 5. Device: boot wiring (`src/main.c`)
- Add `psa_crypto_init()` before `coap_server_start()` (EDHOC/OSCORE need PSA explicitly; DTLS relied on auto-init).
- Start the EDHOC responder thread at boot (blocks awaiting msg_1). Guard everything with `#if defined(CONFIG_UEDHOC)` so plaintext/DTLS builds are untouched (CMakeLists globs `src/*.c`).

### 6. Host: Initiator + OSCORE client (`scripts/edhoc_oscore_client.py`, new)
De-risked: **lakers-python** supports *only* suite 2 + Method 3 (STAT-STAT) + CCS creds — i.e. exactly our target (no C host client needed). **aiocoap ships a built-in EDHOC Initiator + OSCORE client** (`aiocoap/edhoc.py`) driven by a credentials map, so the host is mostly config.
- Env: Python **3.12** (pyenv). `scripts/requirements.txt`: `aiocoap[oscore]`, `lakers-python` (prebuilt macOS-arm64 wheels), `cbor2`, `cryptography`.
- **Path A (primary):** aiocoap built-in. Credentials type key is `edhoc-oscore` → `EdhocCredentials(suite, method, own_cred_style, own_cred, private_key|private_key_file, peer_cred)`. `own_cred`/`peer_cred` are `{14: <CCS-dict>}`; `private_key` is a COSE_Key map `{1:2, -1:1, -4:d}`; `own_cred_style="by-key-id"`. Load via `ctx.client_credentials.load_from_dict({"coap://192.0.2.1/*": {"edhoc-oscore": {...}}})`, then just send `GET /hello`, `GET /led`, `PUT /led`; aiocoap runs the handshake to `/.well-known/edhoc` on first use and protects follow-ups. (Verified offline: schema loads; `cbor2.dumps(own_cred[14], canonical=True)` matches the device header byte-for-byte.)
- **Path B (fallback / didactic):** explicit `lakers.EdhocInitiator` — `prepare_message_1` → POST (`0xf5`++msg_1, CF `application/cid-edhoc+cbor-seq`) → `parse_message_2`/`credential_check_or_fetch`/`verify_message_2` → `prepare_message_3` → POST → `edhoc_exporter(0,…,16)`/`(1,…,8)` → build `aiocoap.oscore.SecurityContext` (AES-CCM-16-64-128 / SHA-256, `sender_id=C_R`, `recipient_id=C_I`, `derive_keys(salt,ms)`).

## Implementation status (prototyped, then reverted)
The prototype was built and reached: all three builds compile/link (plaintext ~7% / DTLS ~9% / OSCORE ~12% FLASH), the `uoscore-uedhoc`+`zcbor` modules fetched and integrated, `edhoc_keys.py` generating matching CCS credentials (lakers-parsed, byte-identical to the device header), and the aiocoap client-credentials schema validated offline. **Not verified on hardware** (the live handshake + protected round-trip). Then the whole EDHOC/OSCORE addition was reverted; the repo returned to plaintext + DTLS.

## Verification (end-to-end) — for a future attempt
1. `west update`; build oscore variant; `west flash`.
2. macOS: assign host `192.0.2.2/24` on the USB-NCM iface (per README).
3. `python3 scripts/edhoc_oscore_client.py` → expect `2.04` on msg_2/msg_3, then a **decrypted** `Hello…` and LED state, no AEAD/decrypt errors; toggle LED via protected PUT and confirm the board LED.
4. Confirm plaintext (`prj.conf`) and DTLS (`dtls.conf`) builds still build/run unchanged.
5. Watch for stack overflow (`CONFIG_THREAD_ANALYZER`); confirm no heap growth over sustained protected traffic.
6. If the handshake fails: cross-check msg encodings vs RFC 9528 App. C and the module `test/` vectors.

## Risks (ranked)
1. **`kid` / connection-ID + CCS agreement** — `C_I`/`C_R`, kids, CCS bytes must be identical both ends; OSCORE Sender/Recipient = `C_I`/`C_R` with correct orientation. Enforce via the single `edhoc_keys.py`. This is the most likely first-run snag.
2. **CID ↔ OSCORE-ID encoding** between aiocoap and uoscore-uedhoc — uoscore-uedhoc uses the CBOR-encoded connection-id byte as the OSCORE ID (e.g. CID `-19` → `{0x32}`); confirm aiocoap derives the same bytes.
3. **CBOR framing** of `cid-edhoc+cbor-seq` (`0xf5` prefix on msg_1 / `C_R` prefix on msg_3) — Path A handles it; Path B must replicate.
4. **Stack sizes** — tune after first overflow.
- Non-risk (confirmed): lakers = suite 2 + Method 3 + CCS only = our exact target.

## Build-time resolutions (verified on the fetched module)
These were the non-obvious integration fixes discovered while building the prototype — keep them for next time:
- **API** (`inc/edhoc.h`+`inc/oscore.h`): `edhoc_responder_run_extended(&ctx,&cred_i_array,&err_msg,&prk_out,&initiator_pk,&c_i_bytes,tx,rx,ead)` → `prk_out2exporter(SHA_256,&prk_out,&prk_exp)` → `edhoc_exporter(SHA_256,OSCORE_MASTER_SECRET|SALT,&prk_exp,&out)` → `oscore_context_init(&params,&ctx)`. `oscore2coap()`/`coap2oscore()` are **5-arg** (no `oscore_present` out; use the return code `not_oscore_pkt`/`first_request_after_reboot`/`ok`). Ground-truth wiring: `samples/linux_edhoc_oscore/responder_server`.
- **Responder OSCORE IDs:** `sender_id = C_I` (learned via `_extended`), `recipient_id = C_R` (ours). `aead=OSCORE_AES_CCM_16_64_128`, `hkdf=OSCORE_SHA_256`, `fresh_master_secret_salt=true`. Enums: `SHA_256`(=-16), `P256`(=1).
- **Method 3 responder** does `rx(msg1)→tx(msg2)→rx(msg3)→done`; **no message_4** (`MESSAGE_4` undef) → the thread bridge must post an empty sentinel after the run so POST#2 gets its (empty) `2.04`.
- **`ephemeral_dh_key_gen`** P256/MBEDTLS ignores `seed`, uses `psa_generate_key()` (PSA DRBG ← RP2350 TRNG); outputs 32B priv + 32B pub-x; self-calls `psa_crypto_init()`.
- **`g` (peer static DH key)** must be the **32-byte X-coordinate** (`crypto_p256_uncompress_point` requires `ilen==32`; parity-independent for ECDH). Do *not* pass a 65-byte uncompressed point.
- **`cert.c` / mbedTLS 4.x gotcha (resolved):** the module unconditionally compiles `src/edhoc/cert.c` (X.509 path, dead code for CCS creds). To build it: `CONFIG_MBEDTLS_PK_PARSE_C/X509_USE_C/X509_CRT_PARSE_C=y`, keep the library DEBUG **off** (its `mbedtls_oid_*` debug call), and add a scoped `target_compile_definitions(uedhoc PRIVATE MBEDTLS_PK_ECDSA=MBEDTLS_PK_SIGALG_ECDSA)` in CMakeLists (mbedTLS 4.x moved that enum to a private header).
- **`crypto_p256.c` not compiled by the Zephyr glue** — the P-256 ECDH (`shared_secret_derive` → `crypto_p256_uncompress_point`) needs it. Add it to the build: `target_sources(uoscore_uedhoc_common PRIVATE ${ZEPHYR_UOSCORE_UEDHOC_MODULE_DIR}/src/common/crypto_p256.c)`. (It only surfaces as a link error once the EDHOC code is actually referenced.)
- **lakers CCS subject** must be a **text string** (a bstr/int subject → `EDHOCError::ParsingError`).
- **Boot crash (found on hardware):** the responder-thread rendez-vous semaphores must be `K_SEM_DEFINE`'d (compile-time init), **not** plain `struct k_sem` members of a zero-initialised static struct. The thread auto-starts at boot; `k_sem_reset`/`k_sem_take` on a zeroed sem walks an uninitialised wait-queue and faults the kernel (`arch_system_halt`), so USB never enumerates. Symptom: the OSCORE build halts at boot while plaintext/DTLS enumerate fine.
- **Responder thread stack:** size it ~16 KB. PSA P-256 keygen + the CBOR/CCM handshake temporaries are large (the library's own sample runs the responder on a 20 KB stack); 10 KB risks a stack-overflow fault after the sem fix. Keep `CONFIG_THREAD_ANALYZER` on during bring-up to measure the high-water mark.

## Suggested build order
A. manifest + Kconfig + `psa_crypto_init` → clean build/boot.
B. keys/creds header + `edhoc_keys.py`.
C. `/.well-known/edhoc` + thread bridge → handshake completes, log PRK/TH.
D. OSCORE context + intercept + host protected round-trip.

## Resolved identifiers
- `kid_I = C_I = 0x2a` (host), `kid_R = C_R = 0x2b` (device); kid tied to CID; source of truth = `edhoc_keys.py`.

## References
RFC 9528 (EDHOC) · RFC 8613 (OSCORE) · RFC 9668 (EDHOC + CoAP + OSCORE, `/.well-known/edhoc`, combined optimization) · [`zephyrproject-rtos/uoscore-uedhoc`](https://github.com/zephyrproject-rtos/uoscore-uedhoc) (README, `crypto_wrapper.c`, `inc/*.h`, `samples/linux_edhoc_oscore`, `test_vectors/`) · [lakers-python](https://pypi.org/project/lakers-python/) · [aiocoap EDHOC](https://aiocoap.readthedocs.io/en/latest/stateofedhoc.html).

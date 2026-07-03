alias c := clean
alias b := build
alias f := flash

openocd_bin := "~/.local/bin/usr/local/bin/openocd"
openodc_dir := "~/.local/openocd"

# USB-NCM CoAP server on the ZBook (RP2350B). The board enumerates on the host as
# a USB Ethernet adapter; a CoAP server runs over it.
#
# macOS host setup after flashing (the board is 192.0.2.1):
#   networksetup -listallhardwareports     # find the new "USB ... " -> en<N>
#   sudo ifconfig en<N> 192.0.2.2 255.255.255.0 up
#   coap-client -m get coap://192.0.2.1/hello               # base (plaintext), brew install libcoap
#   coap-client -m get coaps://192.0.2.1/led                # DTLS variant (port 5684)

clean:
    rip build build-dtls build-oscore build-perf build-dtls-perf build-oscore-perf

# Base build: plaintext CoAP, minimal IPv4/UDP stack (prj.conf).
build:
    west build -b zbook/rp2350b/m33

# DTLS / CoAPS (X.509) variant: prj.conf + dtls.conf, in its own build dir.
# MBEDTLS_FATAL_WARNINGS=OFF: the pinned tf-psa-crypto has -Werror warnings in
# upstream library code that aren't ours.
build-dtls:
    west build -p always -b zbook/rp2350b/m33 -d build-dtls -- -DEXTRA_CONF_FILE=dtls.conf -DMBEDTLS_FATAL_WARNINGS=OFF

# Performance-measurement builds: shell + logging off (perf.conf) so they don't
# perturb the latency timing. Plaintext, DTLS, and OSCORE variants -- these are
# also the images measured in docs/benchmarks.md.
build-perf:
    west build -p always -b zbook/rp2350b/m33 -d build-perf -- -DEXTRA_CONF_FILE=perf.conf

build-dtls-perf:
    west build -p always -b zbook/rp2350b/m33 -d build-dtls-perf -- -DEXTRA_CONF_FILE="dtls.conf;perf.conf" -DMBEDTLS_FATAL_WARNINGS=OFF

build-oscore-perf:
    west build -p always -b zbook/rp2350b/m33 -d build-oscore-perf -- -DEXTRA_CONF_FILE="oscore.conf;perf.conf"

# EDHOC (RFC 9528) + OSCORE (RFC 8613) variant: prj.conf + oscore.conf. Needs the
# generated credentials header (just keys) and the uoscore-uedhoc/zcbor west
# modules (run `west update` if not fetched). Application-layer security over
# plaintext UDP 5683 -- see docs/edhoc-oscore-plan.md.
build-oscore:
    west build -p always -b zbook/rp2350b/m33 -d build-oscore -- -DEXTRA_CONF_FILE=oscore.conf

flash:
    west flash --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-dtls:
    west flash -d build-dtls --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-perf:
    west flash -d build-perf --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-dtls-perf:
    west flash -d build-dtls-perf --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-oscore:
    west flash -d build-oscore --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-oscore-perf:
    west flash -d build-oscore-perf --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

# Send a CoAP request via the Python client (coap-client-style). Works over
# plaintext AND DTLS-PSK -- unlike libcoap coap-client, which can't negotiate the
# board's PSK-CCM8 suite. Uses scripts/.venv (see `just venv`). Examples:
#   just coap get hello
#   just coap get led
#   just coap --dtls put led -e 1
coap *args:
    scripts/.venv/bin/python scripts/coap_cli.py {{ args }}

# ---- CoAP round-trip latency probes (docs/benchmarks.md) ----
# Flash the matching perf build (flash-perf / flash-dtls-perf / flash-oscore-perf),
# power-cycle the board so USB re-enumerates, and re-apply the host IP (192.0.2.2)
# before running these.

# Plaintext CoAP latency (:5683). 1000 samples/endpoint, CON + NON.
latency:
    scripts/.venv/bin/python scripts/coap_latency.py

# DTLS-PSK CoAPS latency (:5684). Includes the one-time DTLS handshake time.
latency-dtls:
    scripts/.venv/bin/python scripts/coap_latency.py --dtls

# EDHOC+OSCORE latency (:5683): times the EDHOC handshake once, then the
# OSCORE-protected round trips, via the native host client's --bench mode.
latency-oscore host="192.0.2.1": build-host-client
    host_client/build/edhoc_oscore_client {{ host }} --bench

# ---- Host Python tooling (scripts/.venv) ----

# One-time: create the host venv + install all host deps (aiocoap, lakers,
# cbor2, cryptography, python-mbedtls).
venv:
    python3 -m venv scripts/.venv
    scripts/.venv/bin/pip install -q -r scripts/requirements.txt

# One-time (or to rotate keys): generate the shared EDHOC credentials. Writes
# include/edhoc_creds.h (device) + host_client/edhoc_creds_client.h (host) +
# scripts/edhoc_creds.json. Rebuild + reflash the oscore variant AND rebuild the
# host client (just build-host-client) after running this so both embed the keys.
keys:
    scripts/.venv/bin/python scripts/edhoc_keys.py

# Build the native host EDHOC (libedhoc, suite 0 / X25519) + OSCORE (uoscore)
# client. Needs Homebrew mbedtls@3 (brew install mbedtls@3) and `just keys` first.
build-host-client:
    cmake -S host_client -B host_client/build
    cmake --build host_client/build

# Run the host EDHOC handshake + OSCORE-protected round-trip against the device
# (flashed with build-oscore, USB-NCM link up; see docs/edhoc-oscore-plan.md).
edhoc-client host="192.0.2.1": build-host-client
    host_client/build/edhoc_oscore_client {{ host }}

erase:
    {{ openocd_bin }} -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset halt; flash erase_sector 0 0 last; shutdown"

# Regenerate the ROM/RAM reports behind docs/benchmarks.md (all three variants,
# perf config = shell+log off -- the same images the latency runs use).
# Pristine-builds each, then dumps the size_report trees to stdout.
footprint: build-perf build-dtls-perf build-oscore-perf
    west build -d build-perf -t rom_report
    west build -d build-perf -t ram_report
    west build -d build-dtls-perf -t rom_report
    west build -d build-dtls-perf -t ram_report
    west build -d build-oscore-perf -t rom_report
    west build -d build-oscore-perf -t ram_report

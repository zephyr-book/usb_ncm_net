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
    rip build build-dtls build-perf build-dtls-perf

# Base build: plaintext CoAP, minimal IPv4/UDP stack (prj.conf).
build:
    west build -b zbook/rp2350b/m33

# DTLS / CoAPS (X.509) variant: prj.conf + dtls.conf, in its own build dir.
# MBEDTLS_FATAL_WARNINGS=OFF: the pinned tf-psa-crypto has -Werror warnings in
# upstream library code that aren't ours.
build-dtls:
    west build -p always -b zbook/rp2350b/m33 -d build-dtls -- -DEXTRA_CONF_FILE=dtls.conf -DMBEDTLS_FATAL_WARNINGS=OFF

# Performance-measurement builds: shell + logging off (perf.conf) so they don't
# perturb the scripts/coap_latency.py timing. Plaintext and DTLS variants.
build-perf:
    west build -p always -b zbook/rp2350b/m33 -d build-perf -- -DEXTRA_CONF_FILE=perf.conf

build-dtls-perf:
    west build -p always -b zbook/rp2350b/m33 -d build-dtls-perf -- -DEXTRA_CONF_FILE="dtls.conf;perf.conf" -DMBEDTLS_FATAL_WARNINGS=OFF

flash:
    west flash --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-dtls:
    west flash -d build-dtls --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-perf:
    west flash -d build-perf --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

flash-dtls-perf:
    west flash -d build-dtls-perf --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

# Send a CoAP request via the Python client (coap-client-style). Works over
# plaintext AND DTLS-PSK -- unlike libcoap coap-client, which can't negotiate the
# board's PSK-CCM8 suite. DTLS needs `pip install python-mbedtls`. Examples:
#   just coap get hello
#   just coap get led
#   just coap --dtls put led -e 1
coap *args:
    python3 scripts/coap_cli.py {{ args }}

erase:
    {{ openocd_bin }} -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset halt; flash erase_sector 0 0 last; shutdown"

# Regenerate the ROM/RAM reports behind docs/benchmarks.md (both variants).
# Pristine-builds each, then dumps the size_report trees to stdout.
footprint:
    west build -p always -b zbook/rp2350b/m33 -d build
    west build -p always -b zbook/rp2350b/m33 -d build-dtls -- -DEXTRA_CONF_FILE=dtls.conf -DMBEDTLS_FATAL_WARNINGS=OFF
    west build -d build -t rom_report
    west build -d build -t ram_report
    west build -d build-dtls -t rom_report
    west build -d build-dtls -t ram_report

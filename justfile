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
#   ping 192.0.2.1
#   coap-client -m get coap://192.0.2.1/hello              # brew install libcoap
#   coap-client -m get coap://192.0.2.1/.well-known/core   # resource discovery

clean:
    rip build

build:
    west build -b zbook/rp2350b/m33

build-wifi:
    west build -b zbook/rp2350b/m33 --shield zbook_wifi

flash:
    west flash --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

erase:
    {{ openocd_bin }} -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset halt; flash erase_sector 0 0 last; shutdown"

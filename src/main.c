/**
 * @file main.c
 * @brief USB-NCM networking bring-up.
 *
 * Brings the RP2350 native USB device controller up as a CDC-NCM virtual
 * Ethernet adapter and assigns a static IPv4 address to it directly -- no DHCP,
 * no connection-manager, no net-config layer. The CoAP server
 * (src/coap_server.c) autostarts on its own thread, so main() only enables USB,
 * sets the address, and returns. The host configures its end (192.0.2.2) by
 * hand. See README.md.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "coap_server.h"
#include "usbd_setup.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_ncm_net, LOG_LEVEL_INF);

/* Static IPv4 on the NCM interface. The host uses 192.0.2.2 (configured by hand). */
#define BOARD_IPV4_ADDR    "192.0.2.1"
#define BOARD_IPV4_NETMASK "255.255.255.0"

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (!usbd_can_detect_vbus(ctx)) {
		return;
	}

	if (msg->type == USBD_MSG_VBUS_READY) {
		if (usbd_enable(ctx)) {
			LOG_ERR("Failed to enable device support");
		}
	} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
		if (usbd_disable(ctx)) {
			LOG_ERR("Failed to disable device support");
		}
	}
}

/* Assign the static IPv4 address directly -- no DHCP, no net_config layer. */
static int assign_static_ipv4(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_in_addr addr;
	struct net_in_addr netmask;

	if (iface == NULL) {
		LOG_ERR("No default network interface");
		return -ENODEV;
	}

	if (net_addr_pton(NET_AF_INET, BOARD_IPV4_ADDR, &addr) < 0 ||
	    net_addr_pton(NET_AF_INET, BOARD_IPV4_NETMASK, &netmask) < 0) {
		return -EINVAL;
	}

	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("Failed to add IPv4 address");
		return -EADDRNOTAVAIL;
	}

	(void)net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

	return 0;
}

int main(void)
{
	struct usbd_context *ctx;

	ctx = ncm_usbd_init(usbd_msg_cb);
	if (ctx == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return 0;
	}

	/* Controllers without VBUS detection must be enabled here; those with it
	 * are enabled from usbd_msg_cb() on USBD_MSG_VBUS_READY.
	 */
	if (!usbd_can_detect_vbus(ctx)) {
		if (usbd_enable(ctx)) {
			LOG_ERR("Failed to enable USB device support");
			return 0;
		}
	}

	(void)assign_static_ipv4();

	/* Plaintext CoAP autostarts; the DTLS variant registers X.509 creds and
	 * starts CoAPS here (after the interface exists).
	 */
	(void)coap_server_start();

	LOG_INF("USB-NCM up (board " BOARD_IPV4_ADDR ")");
	return 0;
}

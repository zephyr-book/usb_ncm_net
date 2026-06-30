/**
 * @file main.c
 * @brief USB-NCM networking bring-up.
 *
 * Brings the RP2350 native USB device controller up as a CDC-NCM virtual
 * Ethernet adapter. Static IPv4 (board = 192.0.2.1) is applied to that interface
 * by CONFIG_NET_CONFIG_SETTINGS, and the CoAP server (src/coap_server.c)
 * autostarts on its own thread. So main() only enables USB and reports link
 * state; the host configures its end (192.0.2.2) by hand. See README.md.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd_setup.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_ncm_net, LOG_LEVEL_INF);

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static struct net_mgmt_event_callback l4_cb;

static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network up -- CoAP server at coap://192.0.2.1/hello");
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("Network down");
	}
}

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

int main(void)
{
	struct usbd_context *ctx;

	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

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

	LOG_INF("USB-NCM up; CoAP server on UDP 5683 (board 192.0.2.1)");
	return 0;
}

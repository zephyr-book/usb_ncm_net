/**
 * @file usbd_setup.h
 * @brief Minimal USB device (device_next) context for the CDC-NCM ethernet link.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_NCM_NET_USBD_SETUP_H
#define USB_NCM_NET_USBD_SETUP_H

#include <zephyr/usb/usbd.h>

/**
 * @brief Build and initialize the USB device context (CDC-NCM class).
 *
 * Registers the string/configuration descriptors and all enabled USB classes
 * (including the CDC-NCM ethernet function declared in the devicetree), then
 * calls usbd_init(). The caller is responsible for usbd_enable().
 *
 * @param msg_cb USBD message callback (VBUS / class events), or NULL to skip
 *               registration.
 * @return Pointer to the initialized context, or NULL on failure.
 */
struct usbd_context *ncm_usbd_init(usbd_msg_cb_t msg_cb);

#endif /* USB_NCM_NET_USBD_SETUP_H */

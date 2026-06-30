/**
 * @file usbd_setup.c
 * @brief Minimal USB device (device_next) context for the CDC-NCM ethernet link.
 *
 * Distilled from samples/subsys/usb/common/sample_usbd_init.c: a single
 * full-speed configuration exposing the CDC-NCM class, with literal VID/PID and
 * string descriptors instead of the sample's Kconfig knobs. The NCM ethernet
 * function is instantiated from the devicetree node (compatible
 * "zephyr,cdc-ncm-ethernet") and picked up by usbd_register_all_classes().
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd_setup.h"

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usbd_setup, LOG_LEVEL_INF);

/*
 * Zephyr project test VID/PID. Fine for a local bench tool; obtain a real
 * VID/PID before distributing hardware.
 */
#define NCM_USBD_VID 0x2fe3
#define NCM_USBD_PID 0x0001

/* Bus-powered, no remote wakeup. */
#define NCM_USBD_ATTRIBUTES 0
/* Reported max draw, in 2 mA units (250 mA). */
#define NCM_USBD_MAX_POWER  125

USBD_DEVICE_DEFINE(ncm_usbd, DEVICE_DT_GET(DT_NODELABEL(usbd)), NCM_USBD_VID, NCM_USBD_PID);

USBD_DESC_LANG_DEFINE(ncm_lang);
USBD_DESC_MANUFACTURER_DEFINE(ncm_mfr, "Centro de Inovacao EDGE");
USBD_DESC_PRODUCT_DEFINE(ncm_product, "ZBook USB-NCM CoAP");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(ncm_sn)));

USBD_DESC_CONFIG_DEFINE(ncm_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(ncm_fs_config, NCM_USBD_ATTRIBUTES, NCM_USBD_MAX_POWER, &ncm_fs_cfg_desc);

struct usbd_context *ncm_usbd_init(usbd_msg_cb_t msg_cb)
{
	int err;

	err = usbd_add_descriptor(&ncm_usbd, &ncm_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&ncm_usbd, &ncm_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&ncm_usbd, &ncm_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return NULL;
	}

	IF_ENABLED(CONFIG_HWINFO, (err = usbd_add_descriptor(&ncm_usbd, &ncm_sn);))
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_configuration(&ncm_usbd, USBD_SPEED_FS, &ncm_fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration (%d)", err);
		return NULL;
	}

	err = usbd_register_all_classes(&ncm_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register classes (%d)", err);
		return NULL;
	}

	/* CDC-NCM owns an Interface Association Descriptor: use the IAD triple. */
	usbd_device_set_code_triple(&ncm_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	if (msg_cb != NULL) {
		err = usbd_msg_register_cb(&ncm_usbd, msg_cb);
		if (err) {
			LOG_ERR("Failed to register message callback (%d)", err);
			return NULL;
		}
	}

	err = usbd_init(&ncm_usbd);
	if (err) {
		LOG_ERR("Failed to initialize device support (%d)", err);
		return NULL;
	}

	return &ncm_usbd;
}

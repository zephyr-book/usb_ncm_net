/**
 * @file coap_server.h
 * @brief CoAP server startup hook.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_NCM_NET_COAP_SERVER_H
#define USB_NCM_NET_COAP_SERVER_H

#include <zephyr/net/coap.h>

/**
 * @brief Bring the CoAP server up.
 *
 * Plaintext build: the service is autostarted by the CoAP server thread, so
 * this only logs. DTLS build (CONFIG_NET_SOCKETS_ENABLE_DTLS): registers the
 * X.509 server credentials and starts the CoAPS service on UDP 5684.
 *
 * @return 0 on success, negative errno on failure.
 */
int coap_server_start(void);

/**
 * @brief Route a parsed CoAP request to an app resource and build its response.
 *
 * Dispatches @p request to the "hello"/"led" resources and writes the CoAP
 * response into @p response (backed by @p buf / @p buf_len). Shared by the
 * plaintext COAP_SERVICE handlers and by the OSCORE intercept
 * (src/edhoc_oscore.c), which dispatches the decrypted inner request.
 *
 * @return 0 on success, negative errno on failure.
 */
int coap_server_dispatch_inner(struct coap_packet *request, uint8_t *buf, uint16_t buf_len,
			       struct coap_packet *response);

#endif /* USB_NCM_NET_COAP_SERVER_H */

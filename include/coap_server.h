/**
 * @file coap_server.h
 * @brief CoAP server startup hook.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_NCM_NET_COAP_SERVER_H
#define USB_NCM_NET_COAP_SERVER_H

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

#endif /* USB_NCM_NET_COAP_SERVER_H */

/**
 * @file coap_server.c
 * @brief Minimal CoAP server exposed over the USB-NCM link.
 *
 * Declares a CoAP service that autostarts its own UDP server on port 5683 plus a
 * single "hello" GET resource. The stack provides .well-known/core discovery
 * (CONFIG_COAP_SERVER_WELL_KNOWN_CORE). The service binds to any local address,
 * so it answers on the board's NCM IPv4 address (192.0.2.1).
 *
 * Modeled on samples/net/sockets/coap_server/src/core.c.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_link_format.h>
#include <zephyr/net/coap_service.h>

LOG_MODULE_REGISTER(coap_server, LOG_LEVEL_INF);

/* Standard CoAP UDP port. */
static uint16_t coap_port = 5683;

static int hello_get(struct coap_resource *resource, struct coap_packet *request,
		     struct sockaddr *addr, socklen_t addr_len)
{
	static const char payload[] = "Hello from ZBook over USB-NCM\n";
	uint8_t data[CONFIG_COAP_SERVER_MESSAGE_SIZE];
	struct coap_packet response;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint16_t id;
	uint8_t tkl;
	int r;

	id = coap_header_get_id(request);
	tkl = coap_header_get_token(request, token);

	LOG_INF("GET /hello");

	r = coap_packet_init(&response, data, sizeof(data), COAP_VERSION_1, COAP_TYPE_ACK, tkl,
			     token, COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) {
		return r;
	}

	r = coap_packet_append_payload_marker(&response);
	if (r < 0) {
		return r;
	}

	r = coap_packet_append_payload(&response, (const uint8_t *)payload, strlen(payload));
	if (r < 0) {
		return r;
	}

	return coap_resource_send(resource, &response, addr, addr_len, NULL);
}

static const char *const hello_path[] = {"hello", NULL};
static const char *const hello_attributes[] = {
	"title=\"Hello\"",
	"rt=text",
	NULL,
};

COAP_RESOURCE_DEFINE(hello, coap_server, {
	.get = hello_get,
	.path = hello_path,
	.metadata = &((struct coap_core_metadata){
		.attributes = hello_attributes,
	}),
});

/*
 * Bind to the IPv4 wildcard address. With a NULL host the service prefers IPv6
 * when CONFIG_NET_IPV6 is enabled (default) and binds in6addr_any only, so IPv4
 * CoAP requests to 192.0.2.1 get no listener (ICMP port-unreachable). "0.0.0.0"
 * forces the IPv4 branch in coap_service_start() -> bind INADDR_ANY:5683.
 */
COAP_SERVICE_DEFINE(coap_server, "0.0.0.0", &coap_port, COAP_SERVICE_AUTOSTART);

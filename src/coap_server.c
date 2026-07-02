/**
 * @file coap_server.c
 * @brief Minimal CoAP server exposed over the USB-NCM link.
 *
 * Declares a CoAP service that autostarts its own UDP server on port 5683 with:
 *   - "hello" : GET, returns a greeting string.
 *   - "led"   : GET returns the board LED state ("0"/"1"); PUT "0"/"1" (or
 *               "off"/"on") sets it. Drives the led0 alias (blue LED).
 * The stack provides .well-known/core discovery (CONFIG_COAP_SERVER_WELL_KNOWN_CORE).
 * The service binds to the IPv4 wildcard, so it answers on the board's NCM IPv4
 * address (192.0.2.1).
 *
 * Modeled on samples/net/sockets/coap_server/src/core.c.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "coap_server.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>
#if defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
#include <zephyr/net/tls_credentials.h>
#endif

LOG_MODULE_REGISTER(coap_server, LOG_LEVEL_INF);

#if defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)
/* CoAPS (secure) UDP port. */
static uint16_t coap_port = 5684;
#else
/* Standard CoAP UDP port. */
static uint16_t coap_port = 5683;
#endif

/*
 * Response builders. Each fills @p response (backed by @p buf) for one resource
 * without transmitting it, so the same logic serves both the plaintext
 * COAP_SERVICE handlers below and the OSCORE intercept (src/edhoc_oscore.c),
 * which encrypts the built response before sending.
 */

static int build_hello(struct coap_packet *response, uint8_t *buf, uint16_t buf_len,
			uint8_t tkl, const uint8_t *token, uint16_t id)
{
	static const char payload[] = "Hello from ZBook over USB-NCM\n";
	int r;

	LOG_INF("GET /hello");

	r = coap_packet_init(response, buf, buf_len, COAP_VERSION_1, COAP_TYPE_ACK, tkl, token,
			     COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) {
		return r;
	}

	r = coap_packet_append_payload_marker(response);
	if (r < 0) {
		return r;
	}

	return coap_packet_append_payload(response, (const uint8_t *)payload, strlen(payload));
}

/* Board LED behind the "led" resource (led0 alias = blue LED, active-low). */
static const struct gpio_dt_spec led_gpio = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_state;

static int led_init(void)
{
	if (!gpio_is_ready_dt(&led_gpio)) {
		LOG_ERR("LED %s not ready", led_gpio.port ? led_gpio.port->name : "(none)");
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);
}
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/** Parse an on/off request payload: "1"/"on" -> 1, "0"/"off" -> 0, else -1. */
static int parse_onoff(const uint8_t *payload, uint16_t len)
{
	if (len >= 1 && payload[0] == '1') {
		return 1;
	}
	if (len >= 1 && payload[0] == '0') {
		return 0;
	}
	if (len >= 2 && strncmp((const char *)payload, "on", 2) == 0) {
		return 1;
	}
	if (len >= 3 && strncmp((const char *)payload, "off", 3) == 0) {
		return 0;
	}

	return -1;
}

static int build_led_get(struct coap_packet *response, uint8_t *buf, uint16_t buf_len,
			 uint8_t tkl, const uint8_t *token, uint16_t id)
{
	const uint8_t value = led_state ? '1' : '0';
	int r;

	LOG_INF("GET /led -> %c", value);

	r = coap_packet_init(response, buf, buf_len, COAP_VERSION_1, COAP_TYPE_ACK, tkl, token,
			     COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) {
		return r;
	}

	r = coap_packet_append_payload_marker(response);
	if (r < 0) {
		return r;
	}

	return coap_packet_append_payload(response, &value, sizeof(value));
}

static int build_led_put(struct coap_packet *request, struct coap_packet *response, uint8_t *buf,
			 uint16_t buf_len, uint8_t tkl, const uint8_t *token, uint16_t id)
{
	const uint8_t *payload;
	uint16_t payload_len;
	uint8_t code;
	int value;

	payload = coap_packet_get_payload(request, &payload_len);
	value = (payload != NULL) ? parse_onoff(payload, payload_len) : -1;

	if (value < 0) {
		LOG_WRN("PUT /led: bad payload");
		code = COAP_RESPONSE_CODE_BAD_REQUEST;
	} else {
		(void)gpio_pin_set_dt(&led_gpio, value);
		led_state = value;
		LOG_INF("PUT /led -> %d", value);
		code = COAP_RESPONSE_CODE_CHANGED;
	}

	return coap_packet_init(response, buf, buf_len, COAP_VERSION_1, COAP_TYPE_ACK, tkl, token,
				code, id);
}

static const char *const hello_path[] = {"hello", NULL};
static const char *const led_path[] = {"led", NULL};

int coap_server_dispatch_inner(struct coap_packet *request, uint8_t *buf, uint16_t buf_len,
			       struct coap_packet *response)
{
	struct coap_option options[2];
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint16_t id = coap_header_get_id(request);
	uint8_t tkl = coap_header_get_token(request, token);
	uint8_t code = coap_header_get_code(request);
	int opt_num = coap_find_options(request, COAP_OPTION_URI_PATH, options, ARRAY_SIZE(options));

	if (opt_num < 0) {
		opt_num = 0;
	}

	if (code == COAP_METHOD_GET && coap_uri_path_match(hello_path, options, opt_num)) {
		return build_hello(response, buf, buf_len, tkl, token, id);
	}

	if (coap_uri_path_match(led_path, options, opt_num)) {
		if (code == COAP_METHOD_GET) {
			return build_led_get(response, buf, buf_len, tkl, token, id);
		}
		if (code == COAP_METHOD_PUT) {
			return build_led_put(request, response, buf, buf_len, tkl, token, id);
		}
	}

	LOG_WRN("unhandled request (code %u)", code);
	return coap_packet_init(response, buf, buf_len, COAP_VERSION_1, COAP_TYPE_ACK, tkl, token,
				COAP_RESPONSE_CODE_NOT_FOUND, id);
}

/*
 * Thin COAP_SERVICE handler used by the plaintext hello/led resources: dispatch
 * to the shared builder, then transmit. (In the OSCORE build these resources
 * stay reachable in the clear during bring-up.)
 */
static int app_request(struct coap_resource *resource, struct coap_packet *request,
		       struct net_sockaddr *addr, net_socklen_t addr_len)
{
	uint8_t data[CONFIG_COAP_SERVER_MESSAGE_SIZE];
	struct coap_packet response;
	int r = coap_server_dispatch_inner(request, data, sizeof(data), &response);

	if (r < 0) {
		return r;
	}

	return coap_resource_send(resource, &response, addr, addr_len, NULL);
}

COAP_RESOURCE_DEFINE(hello, coap_server, {
	.get = app_request,
	.path = hello_path,
});

COAP_RESOURCE_DEFINE(led, coap_server, {
	.get = app_request,
	.put = app_request,
	.path = led_path,
});

#if defined(CONFIG_NET_SOCKETS_ENABLE_DTLS)

/*
 * Pre-shared key + identity (TLS_PSK_WITH_AES_128_CCM_8). DEV credentials --
 * replace and provision out-of-band before shipping. The client must use the
 * same identity + key (see scripts/coap_latency.py).
 */
#define PSK_TAG 1

static const uint8_t psk_key[] = {
	'z', 'b', 'o', 'o', 'k', '-', 'd', 't',
	'l', 's', '-', 'p', 's', 'k', '!', '!',
}; /* 16-byte (128-bit) DEV key */
static const char psk_id[] = "zbook";
static const sec_tag_t sec_tags[] = {PSK_TAG};

/*
 * CoAPS is NOT autostarted: the credentials must be registered before the
 * socket is created, so coap_server_start() adds them and then starts the
 * service. Bind the IPv4 wildcard (host "0.0.0.0").
 */
/* Last arg is the sec-tag list size in BYTES (sizeof), not the element count. */
COAPS_SERVICE_DEFINE(coap_server, "0.0.0.0", &coap_port, 0, sec_tags, sizeof(sec_tags));

int coap_server_start(void)
{
	int err;

	err = tls_credential_add(PSK_TAG, TLS_CREDENTIAL_PSK, psk_key, sizeof(psk_key));
	if (err) {
		LOG_ERR("Failed to add PSK (%d)", err);
		return err;
	}

	err = tls_credential_add(PSK_TAG, TLS_CREDENTIAL_PSK_ID, psk_id, sizeof(psk_id) - 1);
	if (err) {
		LOG_ERR("Failed to add PSK identity (%d)", err);
		return err;
	}

	err = coap_service_start(&coap_server);
	if (err) {
		LOG_ERR("Failed to start CoAPS service (%d)", err);
		return err;
	}

	LOG_INF("CoAPS server on UDP %u (DTLS, PSK AES-128-CCM-8)", coap_port);
	return 0;
}

#else /* plaintext CoAP */

/*
 * Bind the IPv4 wildcard ("0.0.0.0") so unicast CoAP stays reachable at
 * 192.0.2.1. The plaintext service autostarts on the CoAP server thread.
 */
COAP_SERVICE_DEFINE(coap_server, "0.0.0.0", &coap_port, COAP_SERVICE_AUTOSTART);

int coap_server_start(void)
{
	LOG_INF("CoAP server on UDP %u", coap_port);
	return 0;
}

#endif /* CONFIG_NET_SOCKETS_ENABLE_DTLS */

/**
 * @file edhoc_oscore.c
 * @brief EDHOC (RFC 9528) responder + OSCORE (RFC 8613) protection for the CoAP
 *        server. The EDHOC engine is libedhoc; OSCORE uses the uoscore-uedhoc
 *        OSCORE half. Both run over the mbedTLS/PSA backend.
 *
 * Only compiled when CONFIG_LIBEDHOC_ENABLE is set (the oscore.conf variant).
 * The split (libedhoc for EDHOC, uoscore for OSCORE) exists because
 * uoscore-uedhoc's own EDHOC half has two unfixed RFC-9528 encoding bugs that
 * break CCS-by-kid interop -- see docs/edhoc-oscore-plan.md.
 *
 * Flow (RFC 9668 transport binding, sequential):
 *   1. The host Initiator POSTs to /.well-known/edhoc. message_1 is prefixed
 *      with CBOR `true` (0xf5); message_3 is prefixed with the CBOR-encoded C_R.
 *      edhoc_post() strips that framing, then drives libedhoc's synchronous,
 *      per-message responder API directly (no dedicated thread): message_1 ->
 *      edhoc_message_1_process + edhoc_message_2_compose (returned as 2.04
 *      Changed); message_3 -> edhoc_message_3_process + edhoc_message_4_compose +
 *      OSCORE export (message_4 returned as 2.04). One persistent edhoc_context
 *      spans the two POSTs, serialised by a mutex and reset whenever a fresh
 *      message_1 arrives. This is the RFC 9668 *sequential* flow -- the host runs
 *      aiocoap with use_combined_edhoc=false, whose non-combined initiator
 *      processes message_4 to finish the handshake (so message_4 is required).
 *   2. edhoc_export_oscore_session() yields the OSCORE Master Secret/Salt plus
 *      the correctly-oriented Sender/Recipient IDs, which initialise an OSCORE
 *      security context. A fresh EDHOC run every boot re-keys OSCORE, so the
 *      sender sequence number always starts at 0 (no NVM / no nonce reuse).
 *   3. oscore_post() catches OSCORE-protected requests (outer POST to root, no
 *      Uri-Path), decrypts with oscore2coap(), dispatches the inner request to
 *      the shared hello/led handler, re-encrypts with coap2oscore(), and sends.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

/*
 * The whole file is inert unless the libedhoc EDHOC engine is enabled
 * (oscore.conf variant). CMakeLists globs every source under src, so this guard
 * keeps the plaintext and DTLS builds -- which lack edhoc.h/oscore.h -- building.
 */
#if defined(CONFIG_LIBEDHOC_ENABLE)

#include <zephyr/logging/log.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_service.h>

#include <string.h>

#include "coap_server.h"
#include "edhoc_creds.h"

/* libedhoc EDHOC engine + the ready-made cipher-suite-0 crypto/keys helper
 * (AES-CCM-16-64-128 / SHA-256 over PSA; X25519 via compact25519). We write zero
 * crypto. X25519 ECDH is X-coordinate-only, so there is no point decompression
 * and no dependency on mbedTLS's (legacy) ECP -- unlike suite 2 / P-256.
 */
#include "edhoc.h"
#include "edhoc_cipher_suite_0.h"

/* uoscore-uedhoc, OSCORE half only: byte_array, oscore_context_init(), and the
 * oscore2coap()/coap2oscore() pipeline. Its EDHOC half is not built.
 */
#include "oscore.h"

LOG_MODULE_REGISTER(edhoc_oscore, LOG_LEVEL_INF);

#define EDHOC_MSG_MAX  256 /* largest EDHOC message / CoAP payload we handle */
#define OSCORE_BUF_MAX 512 /* OSCORE encrypt/decrypt scratch */

/* ---- Derived OSCORE security context ---- */

/* Valid once a handshake completes. */
static struct context oscore_ctx;
static volatile bool oscore_ready;

/*
 * OSCORE key material exported from the finished EDHOC handshake. Held at file
 * scope because oscore_context_init() keeps the Sender ID *by reference* (only
 * the Recipient ID is deep-copied), so the backing store must outlive the call.
 * A single OSCORE context is active at a time (mutex-guarded), so one set
 * suffices.
 */
static uint8_t oscore_master_secret[16];
static uint8_t oscore_master_salt[8];
static uint8_t oscore_sender_id[8];
static uint8_t oscore_recipient_id[8];

/* ---- libedhoc responder state ---- */

/*
 * One persistent EDHOC context drives the two-POST handshake. edhoc_ctx_active
 * tracks whether a handshake is in flight (message_1 seen, message_3 pending).
 * edhoc_mtx serialises concurrent POSTs and guards the reset-on-message_1.
 */
static struct edhoc_context edhoc_ctx;
static bool edhoc_ctx_active;
K_MUTEX_DEFINE(edhoc_mtx);

/*
 * Supply this device's Responder credential (CRED_R) and import its static-DH
 * private key for the ECDH key agreement. The kid is a byte string (h'2b').
 */
static int cred_fetch(void *user_ctx, struct edhoc_auth_creds *creds)
{
	ARG_UNUSED(user_ctx);

	creds->label = EDHOC_COSE_HEADER_KID;
	creds->key_id.cred = ED_CRED_R;
	creds->key_id.cred_len = sizeof(ED_CRED_R);
	creds->key_id.cred_is_cbor = true;
	creds->key_id.encode_type = EDHOC_ENCODE_TYPE_BYTE_STRING;
	creds->key_id.key_id_bstr[0] = ED_KID_R;
	creds->key_id.key_id_bstr_length = 1;

	if (edhoc_cipher_suite_0_key_import(NULL, EDHOC_KT_KEY_AGREEMENT, ED_R_PRIV,
					    sizeof(ED_R_PRIV), creds->priv_key_id) != EDHOC_SUCCESS) {
		LOG_ERR("responder static-DH private key import failed");
		return EDHOC_ERROR_CREDENTIALS_FAILURE;
	}

	return EDHOC_SUCCESS;
}

/*
 * Verify the peer Initiator's credential by kid and hand libedhoc the peer's
 * static-DH public key (G_I, the 32-byte X25519 u-coordinate).
 */
static int cred_verify(void *user_ctx, struct edhoc_auth_creds *creds,
		       const uint8_t **public_key_reference, size_t *public_key_length)
{
	ARG_UNUSED(user_ctx);

	if (creds->label != EDHOC_COSE_HEADER_KID) {
		return EDHOC_ERROR_CREDENTIALS_FAILURE;
	}

	/*
	 * The peer's kid (h'2a') is a length-1 byte string whose value lies in
	 * the 0x20-0x37 range, so RFC 9528 3.3.2 transports it as the equivalent
	 * one-byte CBOR integer; libedhoc therefore presents it in the integer
	 * encoding. The byte-string branch covers a non-compressible kid.
	 */
	if (creds->key_id.encode_type == EDHOC_ENCODE_TYPE_INTEGER) {
		if (creds->key_id.key_id_int != ED_KID_I_INT) {
			LOG_WRN("unknown peer kid (int %d)", (int)creds->key_id.key_id_int);
			return EDHOC_ERROR_CREDENTIALS_FAILURE;
		}
	} else {
		if (creds->key_id.key_id_bstr_length != 1 ||
		    creds->key_id.key_id_bstr[0] != ED_KID_I) {
			LOG_WRN("unknown peer kid (bstr)");
			return EDHOC_ERROR_CREDENTIALS_FAILURE;
		}
	}

	/*
	 * MAC_3 verification rebuilds ID_CRED_I as {4: kid} from the fields set
	 * below, so it must carry the byte-string kid form (matching the peer's
	 * transcript) regardless of how the kid arrived on the wire.
	 */
	creds->key_id.encode_type = EDHOC_ENCODE_TYPE_BYTE_STRING;
	creds->key_id.key_id_bstr[0] = ED_KID_I;
	creds->key_id.key_id_bstr_length = 1;
	creds->key_id.cred = ED_CRED_I;
	creds->key_id.cred_len = sizeof(ED_CRED_I);
	creds->key_id.cred_is_cbor = true;

	*public_key_reference = ED_G_I;
	*public_key_length = sizeof(ED_G_I);

	return EDHOC_SUCCESS;
}

/* Initialise a fresh responder context for one handshake. */
static int edhoc_setup(struct edhoc_context *ctx)
{
	static const enum edhoc_method methods[] = {EDHOC_METHOD_3};
	static const struct edhoc_credentials creds = {
		.fetch = cred_fetch,
		.verify = cred_verify,
	};
	const struct edhoc_connection_id cid = {
		.encode_type = EDHOC_CID_TYPE_ONE_BYTE_INTEGER,
		.int_value = ED_C_R_INT,
	};
	int ret;

	ret = edhoc_context_init(ctx);
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	ret = edhoc_set_methods(ctx, methods, ARRAY_SIZE(methods));
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	/* Single supported suite (0); use the helper's canonical descriptor. */
	ret = edhoc_set_cipher_suites(ctx, edhoc_cipher_suite_0_get_suite(), 1);
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	ret = edhoc_set_connection_id(ctx, &cid);
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	ret = edhoc_bind_keys(ctx, edhoc_cipher_suite_0_get_keys());
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	ret = edhoc_bind_crypto(ctx, edhoc_cipher_suite_0_get_crypto());
	if (ret != EDHOC_SUCCESS) {
		return ret;
	}

	return edhoc_bind_credentials(ctx, &creds);
}

/*
 * After message_3, export the OSCORE session and stand up the OSCORE context.
 * Returns 0 on success, negative on failure.
 */
static int derive_and_init_oscore(struct edhoc_context *ctx)
{
	size_t sender_id_len = 0;
	size_t recipient_id_len = 0;
	int ret;

	ret = edhoc_export_oscore_session(ctx, oscore_master_secret, sizeof(oscore_master_secret),
					  oscore_master_salt, sizeof(oscore_master_salt),
					  oscore_sender_id, sizeof(oscore_sender_id),
					  &sender_id_len, oscore_recipient_id,
					  sizeof(oscore_recipient_id), &recipient_id_len);
	if (ret != EDHOC_SUCCESS) {
		LOG_ERR("EDHOC OSCORE export failed (%d)", ret);
		return -EIO;
	}

	/*
	 * RFC 9668: the responder's OSCORE Sender ID = C_I and Recipient ID =
	 * C_R. libedhoc returns them already in that orientation and CBOR-encoded
	 * as the OSCORE identifiers, so they feed oscore_context_init() directly
	 * (no manual C_I/C_R juggling).
	 */
	struct oscore_init_params params = {
		.master_secret = {.len = sizeof(oscore_master_secret), .ptr = oscore_master_secret},
		.sender_id = {.len = (uint32_t)sender_id_len, .ptr = oscore_sender_id},
		.recipient_id = {.len = (uint32_t)recipient_id_len, .ptr = oscore_recipient_id},
		.id_context = {.len = 0, .ptr = NULL},
		.master_salt = {.len = sizeof(oscore_master_salt), .ptr = oscore_master_salt},
		.aead_alg = OSCORE_AES_CCM_16_64_128,
		.hkdf = OSCORE_SHA_256,
		.fresh_master_secret_salt = true, /* re-keyed every boot via EDHOC */
	};

	if (oscore_context_init(&params, &oscore_ctx) != ok) {
		LOG_ERR("oscore_context_init failed");
		return -EIO;
	}

	oscore_ready = true;
	LOG_INF("OSCORE context ready (fresh keys, SSN=0)");
	return 0;
}

/* ---- /.well-known/edhoc responder resource ---- */

/** Length in bytes of the leading CBOR data item (the C_R prefix on message_3). */
static uint32_t cbor_item_len(const uint8_t *p, uint32_t len)
{
	uint8_t major;
	uint8_t ai;
	uint32_t hdr = 1;
	uint32_t payload = 0;

	if (len == 0) {
		return 0;
	}

	major = p[0] >> 5;
	ai = p[0] & 0x1f;

	if (ai == 24) {
		hdr = 2;
	} else if (ai == 25) {
		hdr = 3;
	} else if (ai == 26) {
		hdr = 5;
	} else if (ai == 27) {
		hdr = 9;
	}

	/* Byte/text strings carry a payload after the header. */
	if (major == 2 || major == 3) {
		if (ai < 24) {
			payload = ai;
		} else if (ai == 24 && len >= 2) {
			payload = p[1];
		} else if (ai == 25 && len >= 3) {
			payload = ((uint32_t)p[1] << 8) | p[2];
		}
	}

	return hdr + payload;
}

static int edhoc_post(struct coap_resource *resource, struct coap_packet *request,
		      struct net_sockaddr *addr, net_socklen_t addr_len)
{
	uint8_t data[EDHOC_MSG_MAX];
	uint8_t out_buf[EDHOC_MSG_MAX];
	struct coap_packet response;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	const uint8_t *payload;
	uint16_t payload_len;
	const uint8_t *msg;
	uint32_t msg_len;
	size_t out_len = 0;
	bool is_msg_1;
	uint16_t id;
	uint8_t tkl;
	int rc;
	int r;

	payload = coap_packet_get_payload(request, &payload_len);
	if (payload == NULL || payload_len == 0) {
		LOG_WRN("empty EDHOC POST");
		return -EINVAL;
	}

	/* Strip the RFC 9668 transport framing to recover the raw EDHOC message. */
	is_msg_1 = (payload[0] == 0xf5);
	if (is_msg_1) {
		/* message_1 is prefixed with CBOR true (0xf5). */
		msg = payload + 1;
		msg_len = payload_len - 1;
	} else {
		/* message_3 is prefixed with the CBOR-encoded C_R. */
		uint32_t skip = cbor_item_len(payload, payload_len);

		if (skip == 0 || skip >= payload_len) {
			LOG_WRN("bad message_3 C_R prefix");
			return -EINVAL;
		}
		msg = payload + skip;
		msg_len = payload_len - skip;
	}

	LOG_INF("EDHOC %s: %u bytes, first=0x%02x", is_msg_1 ? "message_1" : "message_3", msg_len,
		msg[0]);

	/*
	 * Drive the libedhoc responder state machine. The mutex serialises
	 * concurrent POSTs; a fresh message_1 resets any half-finished handshake.
	 * On success, message_1 yields message_2 in out_buf; message_3 completes
	 * the handshake and produces an empty reply (message_4 is not used).
	 */
	k_mutex_lock(&edhoc_mtx, K_FOREVER);

	if (is_msg_1) {
		if (edhoc_ctx_active) {
			(void)edhoc_context_deinit(&edhoc_ctx);
			edhoc_ctx_active = false;
		}
		oscore_ready = false;

		rc = edhoc_setup(&edhoc_ctx);
		if (rc == EDHOC_SUCCESS) {
			edhoc_ctx_active = true;
			rc = edhoc_message_1_process(&edhoc_ctx, msg, msg_len);
		}
		if (rc == EDHOC_SUCCESS) {
			rc = edhoc_message_2_compose(&edhoc_ctx, out_buf, sizeof(out_buf), &out_len);
		}
		if (rc != EDHOC_SUCCESS) {
			LOG_ERR("EDHOC message_1/2 failed (%d)", rc);
			if (edhoc_ctx_active) {
				(void)edhoc_context_deinit(&edhoc_ctx);
				edhoc_ctx_active = false;
			}
		}
	} else {
		if (!edhoc_ctx_active) {
			LOG_WRN("message_3 without a pending handshake");
			rc = -EINVAL;
		} else {
			rc = edhoc_message_3_process(&edhoc_ctx, msg, msg_len);
			if (rc != EDHOC_SUCCESS) {
				LOG_ERR("EDHOC message_3 process failed (%d)", rc);
			}
			/*
			 * RFC 9668 sequential flow: the initiator (aiocoap,
			 * use_combined_edhoc=false) processes an EDHOC message_4 as
			 * the reply to the message_3 POST. Compose it before deriving
			 * OSCORE -- message_4_compose advances the state to PERSISTED,
			 * and the OSCORE export still succeeds afterwards.
			 */
			if (rc == EDHOC_SUCCESS) {
				rc = edhoc_message_4_compose(&edhoc_ctx, out_buf,
							     sizeof(out_buf), &out_len);
				if (rc != EDHOC_SUCCESS) {
					LOG_ERR("EDHOC message_4 compose failed (%d)", rc);
				}
			}
			if (rc == EDHOC_SUCCESS) {
				rc = derive_and_init_oscore(&edhoc_ctx);
			}
			(void)edhoc_context_deinit(&edhoc_ctx);
			edhoc_ctx_active = false;
		}
	}

	k_mutex_unlock(&edhoc_mtx);

	if (rc != EDHOC_SUCCESS) {
		return -EBADMSG;
	}

	if (is_msg_1) {
		LOG_INF("EDHOC message_2 composed (%u bytes)", (unsigned int)out_len);
	} else {
		LOG_INF("EDHOC handshake complete, message_4 sent (%u bytes)",
			(unsigned int)out_len);
	}

	id = coap_header_get_id(request);
	tkl = coap_header_get_token(request, token);

	r = coap_packet_init(&response, data, sizeof(data), COAP_VERSION_1, COAP_TYPE_ACK, tkl,
			     token, COAP_RESPONSE_CODE_CHANGED, id);
	if (r < 0) {
		return r;
	}

	if (out_len > 0) {
		r = coap_packet_append_payload_marker(&response);
		if (r < 0) {
			return r;
		}
		r = coap_packet_append_payload(&response, out_buf, out_len);
		if (r < 0) {
			return r;
		}
	}

	return coap_resource_send(resource, &response, addr, addr_len, NULL);
}

static const char *const edhoc_path[] = {".well-known", "edhoc", NULL};

COAP_RESOURCE_DEFINE(edhoc, coap_server, {
	.post = edhoc_post,
	.path = edhoc_path,
});

/* ---- OSCORE intercept resource (outer POST to root, no Uri-Path) ---- */

static int oscore_post(struct coap_resource *resource, struct coap_packet *request,
		       struct net_sockaddr *addr, net_socklen_t addr_len)
{
	uint8_t coap_buf[OSCORE_BUF_MAX];
	uint8_t rsp_buf[EDHOC_MSG_MAX];
	uint8_t oscore_out[OSCORE_BUF_MAX];
	uint32_t coap_len = sizeof(coap_buf);
	uint32_t oscore_len = sizeof(oscore_out);
	struct coap_packet inner_req;
	struct coap_packet inner_rsp;
	struct coap_packet out_pkt;
	enum err e;
	int r;

	if (!oscore_ready) {
		LOG_WRN("OSCORE packet before handshake");
		return -ENOTCONN;
	}

	e = oscore2coap(request->data, request->offset, coap_buf, &coap_len, &oscore_ctx);
	if (e == not_oscore_pkt) {
		return -ENOENT; /* let the stack answer 4.04 */
	}
	if (e != ok && e != first_request_after_reboot) {
		LOG_ERR("oscore2coap failed (err %d)", e);
		return -EBADMSG;
	}

	r = coap_packet_parse(&inner_req, coap_buf, coap_len, NULL, 0);
	if (r < 0) {
		LOG_ERR("inner CoAP parse failed (%d)", r);
		return r;
	}

	r = coap_server_dispatch_inner(&inner_req, rsp_buf, sizeof(rsp_buf), &inner_rsp);
	if (r < 0) {
		return r;
	}

	e = coap2oscore(inner_rsp.data, inner_rsp.offset, oscore_out, &oscore_len, &oscore_ctx);
	if (e != ok) {
		LOG_ERR("coap2oscore failed (err %d)", e);
		return -EBADMSG;
	}

	/* Transmit the protected response verbatim (coap_service_send() writes
	 * out_pkt.data[0 .. out_pkt.offset]).
	 */
	out_pkt = (struct coap_packet){
		.data = oscore_out,
		.offset = (uint16_t)oscore_len,
		.max_len = sizeof(oscore_out),
	};

	return coap_resource_send(resource, &out_pkt, addr, addr_len, NULL);
}

/* Empty path matches only requests with no Uri-Path -- i.e. the OSCORE outer
 * message -- and never the hello/led/.well-known resources.
 */
static const char *const oscore_root_path[] = {NULL};

COAP_RESOURCE_DEFINE(oscore_root, coap_server, {
	.post = oscore_post,
	.path = oscore_root_path,
});

#endif /* CONFIG_LIBEDHOC_ENABLE */

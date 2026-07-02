/**
 * @file edhoc_config.h
 * @brief Build-time configuration of libedhoc for the host client (non-Zephyr).
 *
 * libedhoc's headers include "edhoc_config.h" on non-Zephyr targets (on Zephyr
 * the same CONFIG_LIBEDHOC_* macros come from Kconfig/autoconf). These values
 * mirror the device build (usb_ncm_net/oscore.conf) so both ends agree on the
 * compile-time working set: method 3, cipher suite 0 (X25519), one-byte
 * connection ids and kids, no EAD, stack memory backend.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EDHOC_CONFIG_H
#define EDHOC_CONFIG_H

#define CONFIG_LIBEDHOC_ENABLE			      1
#define CONFIG_LIBEDHOC_KEY_ID_LEN		      4
#define CONFIG_LIBEDHOC_MAX_NR_OF_CIPHER_SUITES	      1
#define CONFIG_LIBEDHOC_MAX_LEN_OF_CONN_ID	      1
#define CONFIG_LIBEDHOC_MAX_LEN_OF_ECC_KEY	      32
#define CONFIG_LIBEDHOC_MAX_LEN_OF_MAC		      32
#define CONFIG_LIBEDHOC_MAX_NR_OF_EAD_TOKENS	      0
#define CONFIG_LIBEDHOC_MAX_LEN_OF_CRED_KEY_ID	      1
#define CONFIG_LIBEDHOC_MAX_LEN_OF_HASH_ALG	      1
#define CONFIG_LIBEDHOC_MAX_NR_OF_CERTS_IN_X509_CHAIN 2
#define CONFIG_LIBEDHOC_LOG_LEVEL		      2 /* WRN */
#define CONFIG_LIBEDHOC_MEM_BACKEND		      0 /* stack (C99 VLAs) */

#endif /* EDHOC_CONFIG_H */

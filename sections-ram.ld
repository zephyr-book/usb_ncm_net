/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Per-service iterable section that collects the COAP_RESOURCE_DEFINE() entries
 * for the "coap_server" service (see src/coap_server.c). The section name is
 * coap_resource_<service-name>; this fragment provides the list start/end
 * boundary symbols the COAP_SERVICE_DEFINE() macro references.
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_RAM(coap_resource_coap_server, Z_LINK_ITERABLE_SUBALIGN)

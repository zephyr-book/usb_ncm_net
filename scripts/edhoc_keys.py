#!/usr/bin/env python3
"""Generate the EDHOC static-DH key material shared by the device and host.

EDHOC (RFC 9528) method 3 (static DH both sides), cipher suite 0
(X25519 / SHA-256 / AES-CCM-16-64-128). Credentials are CCS (CWT Claims Set)
wrapping a COSE_Key -- raw public key, no X.509 -- referenced by `kid`.

This is the single source of truth so the device firmware and the host client
agree byte-for-byte (the credential bytes feed the EDHOC transcript hash, so any
mismatch breaks the handshake). Run once:

    scripts/.venv/bin/python scripts/edhoc_keys.py     (or: just keys)

It writes:
  - include/edhoc_creds.h    -- C arrays for the device (Responder role)
  - scripts/edhoc_creds.json -- key material for the host client (Initiator role)

Re-running regenerates fresh keys; rebuild the firmware and rerun the client.
"""
import json
import pathlib

import cbor2
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import x25519

HERE = pathlib.Path(__file__).resolve().parent
APP = HERE.parent

# Identifiers (freely chosen; must match both ends -- see docs/edhoc-oscore-plan.md).
KID_I = b"\x2a"  # initiator (host) credential key id (byte string)
KID_R = b"\x2b"  # responder (device) credential key id (byte string)
# Responder connection identifier, chosen by the device. Encoded as a one-byte
# CBOR integer (RFC 9528 3.3.2): the wire value is the naked int and the OSCORE
# Recipient ID is its 1-byte CBOR encoding. -8 -> 0x27 (the RFC 9529 3 value).
# libedhoc drives C_R via struct edhoc_connection_id.int_value; the peer
# (node-edhoc) learns C_R from message_2, so this value is device-local.
C_R_INT = -8
# Initiator connection identifier, chosen by the host client. One-byte CBOR int;
# the device (responder) learns it from message_1. -14 -> 0x2d.
C_I_INT = -14
SUITE = 0
METHOD = 3


def kid_wire_int(kid):
    """Return the CBOR one-byte integer a length-1 byte-string kid compresses to.

    RFC 9528 3.3.2: a kid whose single byte is a valid one-byte CBOR integer
    (0x00..0x17 or 0x20..0x37) is transported as that naked integer, so the peer
    sends -- and libedhoc's verify callback receives -- the integer form. Returns
    None if the kid does not compress (stays a byte string on the wire).
    """
    v = kid[0]
    if v <= 0x17:
        return v
    if 0x20 <= v <= 0x37:
        return -1 - (v - 0x20)
    return None


def gen_x25519():
    """Return (priv, pub) as 32-byte raw values for a fresh X25519 key pair.

    priv is the raw private scalar (unclamped; X25519 clamps at use, per
    RFC 7748); pub is the u-coordinate of the public point.
    """
    sk = x25519.X25519PrivateKey.generate()
    priv = sk.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    pub = sk.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    return priv, pub


def ccs(subject, kid, pub):
    """CCS = {2: subject(text), 8: {1: COSE_Key}} with an X25519 OKP COSE_Key.

    COSE_Key = {1(kty):1(OKP), 2(kid):bstr, -1(crv):4(X25519), -2(x):pub}.
    OKP keys carry only the public value in -2 (no -3/y). Subject is a text
    string. Canonical CBOR so both ends produce identical bytes.
    """
    cose_key = {1: 1, 2: kid, -1: 4, -2: pub}
    return cbor2.dumps({2: subject, 8: {1: cose_key}}, canonical=True)


def id_cred(kid):
    """ID_CRED_x = {4: kid} (COSE header, credential referenced by kid)."""
    return cbor2.dumps({4: kid}, canonical=True)


def c_array(name, data, comment=None):
    body = ", ".join(f"0x{b:02x}" for b in data)
    line = f"static const uint8_t {name}[] = {{{body}}};"
    if comment:
        return f"\t/* {comment} */\n\t{line}"
    return f"\t{line}"


def main():
    di, pi = gen_x25519()  # initiator (host)
    dr, pr = gen_x25519()  # responder (device)

    cred_i = ccs("usb-ncm-initiator", KID_I, pi)
    cred_r = ccs("usb-ncm-responder", KID_R, pr)
    idc_i = id_cred(KID_I)
    idc_r = id_cred(KID_R)
    kid_i_int = kid_wire_int(KID_I)  # int the peer sends on the wire (compact form)
    kid_r_int = kid_wire_int(KID_R)

    # --- C header for the device (Responder), libedhoc layout ---
    # libedhoc builds ID_CRED_x itself from the raw kid byte + the KID label, so
    # the header only carries: the raw kid bytes (+ the initiator's compact wire
    # integer, which is what libedhoc's verify callback receives), the responder
    # private key, both CCS credentials, and the peer's static-DH public key.
    header = f"""/**
 * @file edhoc_creds.h
 * @brief EDHOC static-DH credentials (suite {SUITE}, method {METHOD}) -- GENERATED.
 *
 * Produced by scripts/edhoc_keys.py -- do not edit by hand. Holds this device's
 * Responder key material plus the peer Initiator's public credential, in the
 * layout libedhoc's credential fetch/verify callbacks consume. The host
 * counterpart lives in scripts/edhoc_creds.json.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EDHOC_CREDS_H
#define EDHOC_CREDS_H

#include <stdint.h>

/* Responder connection identifier C_R, as a one-byte CBOR integer. */
#define ED_C_R_INT ({C_R_INT})

/* Credential key identifiers (kids), raw byte-string values. */
#define ED_KID_R ({KID_R[0]:#04x})
#define ED_KID_I ({KID_I[0]:#04x})
/* Compact wire integer the initiator's kid compresses to (RFC 9528 3.3.2), i.e.
 * the form libedhoc's verify callback receives for ID_CRED_I. */
#define ED_KID_I_INT ({kid_i_int})

/* ---- This device: EDHOC Responder (R) ---- */
{c_array("ED_R_PRIV", dr, "static DH private key, X25519 (32B, imported for key agreement)")}
{c_array("ED_CRED_R", cred_r, "CRED_R (CCS/COSE_Key), used verbatim in the transcript")}

/* ---- Peer: EDHOC Initiator (I), public material only ---- */
{c_array("ED_G_I", pi, "peer static DH public key, X25519 u-coordinate (32B)")}
{c_array("ED_CRED_I", cred_i, "CRED_I (CCS/COSE_Key), used verbatim in the transcript")}

#endif /* EDHOC_CREDS_H */
"""
    hpath = APP / "include" / "edhoc_creds.h"
    hpath.write_text(header)

    # --- C header for the host client (Initiator role), mirror of the device ---
    client_header = f"""/**
 * @file edhoc_creds_client.h
 * @brief EDHOC static-DH credentials (suite {SUITE}, method {METHOD}) -- GENERATED.
 *
 * Produced by scripts/edhoc_keys.py -- do not edit by hand. The host-client
 * (EDHOC Initiator) counterpart of include/edhoc_creds.h: this host's Initiator
 * key material plus the peer Responder's (device) public credential.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EDHOC_CREDS_CLIENT_H
#define EDHOC_CREDS_CLIENT_H

#include <stdint.h>

/* Connection identifiers, as one-byte CBOR integers. */
#define ED_C_I_INT ({C_I_INT})
#define ED_C_R_INT ({C_R_INT})

/* Credential key identifiers (kids) + the compact wire integer each compresses
 * to (RFC 9528 3.3.2), i.e. the form libedhoc's verify callback receives. */
#define ED_KID_I ({KID_I[0]:#04x})
#define ED_KID_R ({KID_R[0]:#04x})
#define ED_KID_R_INT ({kid_r_int})

/* ---- This host: EDHOC Initiator (I) ---- */
{c_array("ED_I_PRIV", di, "static DH private key, X25519 (32B, imported for key agreement)")}
{c_array("ED_CRED_I", cred_i, "CRED_I (CCS/COSE_Key), used verbatim in the transcript")}

/* ---- Peer: EDHOC Responder (R, the device), public material only ---- */
{c_array("ED_G_R", pr, "peer static DH public key, X25519 u-coordinate (32B)")}
{c_array("ED_CRED_R", cred_r, "CRED_R (CCS/COSE_Key), used verbatim in the transcript")}

#endif /* EDHOC_CREDS_CLIENT_H */
"""
    cpath = APP / "host_client" / "edhoc_creds_client.h"
    cpath.parent.mkdir(parents=True, exist_ok=True)
    cpath.write_text(client_header)

    # --- JSON for the host client (Initiator) ---
    host = {
        "suite": SUITE,
        "method": METHOD,
        "c_i_int": C_I_INT,
        "c_r_int": C_R_INT,
        "initiator": {
            "kid": KID_I.hex(),
            "priv": di.hex(),
            "pub": pi.hex(),
            "ccs": cred_i.hex(),
            "id_cred": idc_i.hex(),
        },
        "responder": {
            "kid": KID_R.hex(),
            "pub": pr.hex(),
            "ccs": cred_r.hex(),
            "id_cred": idc_r.hex(),
        },
    }
    jpath = HERE / "edhoc_creds.json"
    jpath.write_text(json.dumps(host, indent=2) + "\n")

    print(f"wrote {hpath.relative_to(APP)}")
    print(f"wrote {cpath.relative_to(APP)}")
    print(f"wrote {jpath.relative_to(APP)}")
    print(f"CRED_I ({len(cred_i)}B): {cred_i.hex()}")
    print(f"CRED_R ({len(cred_r)}B): {cred_r.hex()}")
    print(f"ID_CRED_I: {idc_i.hex()} (wire int {kid_i_int})   "
          f"ID_CRED_R: {idc_r.hex()} (wire int {kid_r_int})")
    print(f"C_I: {C_I_INT}   C_R: {C_R_INT}   suite: {SUITE}")


if __name__ == "__main__":
    main()

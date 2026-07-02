#!/usr/bin/env python3
"""Generate the EDHOC static-DH key material shared by the device and host.

EDHOC (RFC 9528) method 3 (static DH both sides), cipher suite 2
(P-256 / SHA-256 / AES-CCM-16-64-128). Credentials are CCS (CWT Claims Set)
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
from cryptography.hazmat.primitives.asymmetric import ec

HERE = pathlib.Path(__file__).resolve().parent
APP = HERE.parent

# Identifiers (freely chosen; must match both ends -- see docs/edhoc-oscore-plan.md).
KID_I = b"\x2a"  # initiator (host) credential key id (byte string)
KID_R = b"\x2b"  # responder (device) credential key id (byte string)
# Responder connection identifier, chosen by the device. Encoded as a one-byte
# CBOR integer (RFC 9528 3.3.2): the wire value is the naked int and the OSCORE
# Recipient ID is its 1-byte CBOR encoding. -8 -> 0x27 (the RFC 9529 3 value).
# libedhoc drives C_R via struct edhoc_connection_id.int_value; the peer (aiocoap)
# learns C_R from message_2, so this value is device-local.
C_R_INT = -8
SUITE = 2
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


def gen_p256():
    """Return (d, x, y) as 32-byte big-endian for a fresh P-256 key pair."""
    sk = ec.generate_private_key(ec.SECP256R1())
    d = sk.private_numbers().private_value
    pub = sk.public_key().public_numbers()
    return (
        d.to_bytes(32, "big"),
        pub.x.to_bytes(32, "big"),
        pub.y.to_bytes(32, "big"),
    )


def ccs(subject, kid, x, y):
    """CCS = {2: subject(text), 8: {1: COSE_Key}} with a P-256 COSE_Key.

    COSE_Key = {1(kty):2(EC2), 2(kid):bstr, -1(crv):1(P-256), -2(x), -3(y)}.
    Subject must be a text string (lakers rejects a bstr/int subject).
    Canonical CBOR so both ends produce identical bytes.
    """
    cose_key = {1: 2, 2: kid, -1: 1, -2: x, -3: y}
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
    di, xi, yi = gen_p256()  # initiator (host)
    dr, xr, yr = gen_p256()  # responder (device)

    cred_i = ccs("usb-ncm-initiator", KID_I, xi, yi)
    cred_r = ccs("usb-ncm-responder", KID_R, xr, yr)
    idc_i = id_cred(KID_I)
    idc_r = id_cred(KID_R)
    kid_i_int = kid_wire_int(KID_I)  # int the peer sends on the wire (compact form)

    # --- C header for the device (Responder), libedhoc layout ---
    # libedhoc builds ID_CRED_x itself from the raw kid byte + the KID label, so
    # the header only carries: the raw kid bytes (+ the initiator's compact wire
    # integer, which is what libedhoc's verify callback receives), the responder
    # private key, both CCS credentials, and the peer's static-DH public X.
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
{c_array("ED_R_PRIV", dr, "static DH private key (imported for ECDH key agreement)")}
{c_array("ED_CRED_R", cred_r, "CRED_R (CCS/COSE_Key), used verbatim in the transcript")}

/* ---- Peer: EDHOC Initiator (I), public material only ---- */
{c_array("ED_G_I", xi, "peer static DH public key, X-coordinate (32B, for ECDH)")}
{c_array("ED_CRED_I", cred_i, "CRED_I (CCS/COSE_Key), used verbatim in the transcript")}

#endif /* EDHOC_CREDS_H */
"""
    hpath = APP / "include" / "edhoc_creds.h"
    hpath.write_text(header)

    # --- JSON for the host client (Initiator) ---
    host = {
        "suite": SUITE,
        "method": METHOD,
        "c_r_int": C_R_INT,
        "initiator": {
            "kid": KID_I.hex(),
            "priv": di.hex(),
            "x": xi.hex(),
            "y": yi.hex(),
            "ccs": cred_i.hex(),
            "id_cred": idc_i.hex(),
        },
        "responder": {
            "kid": KID_R.hex(),
            "x": xr.hex(),
            "y": yr.hex(),
            "ccs": cred_r.hex(),
            "id_cred": idc_r.hex(),
        },
    }
    jpath = HERE / "edhoc_creds.json"
    jpath.write_text(json.dumps(host, indent=2) + "\n")

    print(f"wrote {hpath.relative_to(APP)}")
    print(f"wrote {jpath.relative_to(APP)}")
    print(f"CRED_I ({len(cred_i)}B): {cred_i.hex()}")
    print(f"CRED_R ({len(cred_r)}B): {cred_r.hex()}")
    print(f"ID_CRED_I: {idc_i.hex()} (wire int {kid_i_int})   ID_CRED_R: {idc_r.hex()}")
    print(f"C_R (one-byte int): {C_R_INT}")


if __name__ == "__main__":
    main()

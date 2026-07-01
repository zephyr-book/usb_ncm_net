#!/usr/bin/env python3
"""Minimal CoAP client for the ZBook over USB-NCM -- a coap-client-style CLI.

Sends one CoAP request and prints the response code + payload. Supports
plaintext CoAP (UDP 5683) and CoAPS/DTLS-PSK (UDP 5684). The DTLS path uses
python-mbedtls (via coap_latency.DtlsTransport), which negotiates the board's
TLS_PSK_WITH_AES_128_CCM_8 suite -- unlike libcoap's `coap-client`, whose OpenSSL
backend won't offer PSK-CCM8, so `coaps://` there fails the handshake.

    python3 scripts/coap_cli.py get hello
    python3 scripts/coap_cli.py get led
    python3 scripts/coap_cli.py put led -e 1
    python3 scripts/coap_cli.py --dtls get hello        # DTLS (needs python-mbedtls)
    python3 scripts/coap_cli.py --dtls put led -e 0

(or via just:  just coap get hello  /  just coap --dtls put led -e 1)
"""
import argparse
import os
import struct
import time

# Reuse the transports + request builder from the latency probe (same directory).
from coap_latency import DtlsTransport, PlainTransport, build_request

METHODS = {"get": 0x01, "post": 0x02, "put": 0x03, "delete": 0x04}
TYPES = {"con": 0, "non": 1}
CODE_NAMES = {
    "2.01": "Created",
    "2.02": "Deleted",
    "2.03": "Valid",
    "2.04": "Changed",
    "2.05": "Content",
    "4.00": "Bad Request",
    "4.01": "Unauthorized",
    "4.04": "Not Found",
    "4.05": "Method Not Allowed",
    "5.00": "Internal Server Error",
}


def parse_response(data):
    """Return (code 'C.DD', name, payload bytes) from a CoAP response datagram.

    The board's responses carry no options, so the payload is whatever follows
    the 0xFF payload marker after the header + token.
    """
    if len(data) < 4:
        return "(short)", "", b""
    code = data[1]
    code_str = f"{code >> 5}.{code & 0x1f:02d}"
    marker = data.find(b"\xff", 4 + (data[0] & 0x0f))
    payload = data[marker + 1:] if marker != -1 else b""
    return code_str, CODE_NAMES.get(code_str, ""), payload


def close_quietly(tp):
    """Best-effort close. On DTLS this sends close_notify, letting the board free
    its session context immediately (it only has CONFIG_NET_SOCKETS_TLS_MAX_CONTEXTS)."""
    try:
        tp.s.close()
    except Exception:  # noqa: BLE001 -- cleanup must never raise
        pass


def open_transport(args, port):
    """Create the transport and complete the handshake, retrying transient
    failures. The DTLS handshake occasionally times out over USB-NCM (a dropped
    flight, or the board's small DTLS context pool still busy from a prior run);
    a fresh attempt almost always succeeds."""
    last = None
    for attempt in range(1, args.retries + 1):
        tp = (DtlsTransport if args.dtls else PlainTransport)(args.host, port, args.timeout)
        try:
            return tp, tp.connect()
        except OSError as exc:  # TimeoutError, ConnectionRefused, ...
            last = exc
            close_quietly(tp)
            if attempt < args.retries:
                time.sleep(0.3)
    raise SystemExit(f"connect to {args.host}:{port} failed after {args.retries} tries ({last})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("method", choices=METHODS, help="CoAP method")
    ap.add_argument("path", help="resource path, e.g. hello or led")
    ap.add_argument("-e", "--payload", help="request payload (text)")
    ap.add_argument("--dtls", action="store_true", help="CoAPS over DTLS 1.2 (PSK)")
    ap.add_argument("--host", default="192.0.2.1")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--type", choices=TYPES, default="con", help="CON or NON (default: con)")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--retries", type=int, default=3, help="handshake/connect attempts")
    args = ap.parse_args()

    port = args.port if args.port is not None else (5684 if args.dtls else 5683)
    payload = args.payload.encode() if args.payload is not None else None

    tp, hs = open_transport(args, port)
    if args.dtls:
        print(f"DTLS handshake: {hs:.1f} ms")

    try:
        mid = int.from_bytes(os.urandom(2), "big")
        tp.send(build_request(METHODS[args.method], args.path, TYPES[args.type], mid,
                              os.urandom(4), payload))

        # Read until the response with our message ID arrives (or the socket times out).
        while True:
            try:
                data = tp.recv()
            except OSError as exc:
                raise SystemExit(f"no response from {args.host}:{port} ({exc})")
            if len(data) >= 4 and struct.unpack("!H", data[2:4])[0] == mid:
                break

        code_str, name, payload = parse_response(data)
        scheme = "coaps" if args.dtls else "coap"
        print(f"{args.method.upper()} {scheme}://{args.host}/{args.path} -> "
              f"{code_str} {name}".rstrip())
        if payload:
            print(f"  payload: {payload.decode('utf-8', 'replace')!r}")
    finally:
        close_quietly(tp)


if __name__ == "__main__":
    main()

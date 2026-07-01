#!/usr/bin/env python3
"""Measure CoAP round-trip latency to the ZBook over USB-NCM.

Sends CoAP requests and times each one until its response arrives, over a single
persistent connection so there is no per-request process/handshake overhead
(unlike `coap-client` in a shell loop). Reports both message types:

  - CON (Confirmable): request answered by an ACK carrying the response.
  - NON (Non-confirmable): request answered by a NON response.

Transports:
  - plaintext CoAP over UDP (default, port 5683)
  - CoAPS over DTLS 1.2 (--dtls, port 5684) -- one handshake, then timed requests

DTLS needs python-mbedtls (pip install python-mbedtls). Certificate verification
is skipped (this is a latency probe, not an auth test).

    python3 scripts/coap_latency.py                 # plaintext, base build
    python3 scripts/coap_latency.py --dtls           # CoAPS, DTLS build
"""
import argparse
import os
import socket
import statistics
import struct
import time

COAP_VER = 1
TYPE_CON = 0
TYPE_NON = 1
CODE_GET = 0x01
CODE_PUT = 0x03


def build_request(code, uri_path, mtype, mid, token, payload=None):
    tkl = len(token)
    out = bytearray([(COAP_VER << 6) | (mtype << 4) | tkl, code])
    out += struct.pack("!H", mid)
    out += token
    last = 0
    for seg in uri_path.split("/"):
        if not seg:
            continue
        b = seg.encode()
        out.append(((11 - last) << 4) | len(b))  # Uri-Path = option 11
        out += b
        last = 11
    if payload is not None:
        out.append(0xFF)
        out += payload
    return bytes(out)


class PlainTransport:
    scheme = "coap"

    def __init__(self, host, port, timeout):
        self.addr = (host, port)
        self.timeout = timeout
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(timeout)

    def connect(self):
        self.s.connect(self.addr)
        return 0.0

    def send(self, d):
        self.s.send(d)

    def recv(self, n=1500):
        return self.s.recv(n)


class DtlsTransport:
    scheme = "coaps"

    def __init__(self, host, port, timeout):
        from mbedtls import tls
        self._tls = tls
        self.addr = (host, port)
        self._req_timeout = timeout
        # Pin DTLS 1.2 + the server's single ciphersuite so the ClientHello
        # matches exactly (ECDHE-ECDSA-AES128-GCM-SHA256, EC P-256).
        # PSK identity + key must match the server (src/coap_server.c).
        conf = tls.DTLSConfiguration(
            pre_shared_key=("zbook", b"zbook-dtls-psk!!"),
            ciphers=("TLS-PSK-WITH-AES-128-CCM-8",),
            lowest_supported_version=tls.DTLSVersion.DTLSv1_2,
            highest_supported_version=tls.DTLSVersion.DTLSv1_2,
            validate_certificates=False,
        )
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp.settimeout(15.0)  # generous during the handshake
        self.s = tls.ClientContext(conf).wrap_socket(self._udp, server_hostname=None)

    def connect(self):
        self.s.connect(self.addr)
        t0 = time.perf_counter()
        while True:
            try:
                self.s.do_handshake()
                break
            except (self._tls.WantReadError, self._tls.WantWriteError):
                continue
        hs = (time.perf_counter() - t0) * 1e3
        self._udp.settimeout(self._req_timeout)  # tighter for request timing
        return hs

    def send(self, d):
        self.s.send(d)

    def recv(self, n=1500):
        return self.s.recv(n)


def measure(tp, code, path, mtype, payload, count, warmup):
    rtts = []
    losses = 0
    mid = int.from_bytes(os.urandom(2), "big")
    for i in range(count + warmup):
        mid = (mid + 1) & 0xFFFF
        req = build_request(code, path, mtype, mid, os.urandom(4), payload)
        t0 = time.perf_counter()
        rtt = None
        try:
            tp.send(req)
            while True:
                data = tp.recv()
                if len(data) >= 4 and struct.unpack("!H", data[2:4])[0] == mid:
                    rtt = (time.perf_counter() - t0) * 1e3
                    break
        except (socket.timeout, OSError):
            pass
        if i < warmup:
            continue
        if rtt is None:
            losses += 1
        else:
            rtts.append(rtt)
    return rtts, losses


def report(name, rtts, losses):
    if not rtts:
        print(f"{name:<20} no responses (loss={losses})")
        return
    s = sorted(rtts)

    def pct(p):
        return s[min(len(s) - 1, round(p / 100 * (len(s) - 1)))]

    print(f"{name:<20} n={len(s):<5} min={s[0]:6.3f}  mean={statistics.fmean(s):6.3f}  "
          f"med={statistics.median(s):6.3f}  p95={pct(95):6.3f}  p99={pct(99):6.3f}  "
          f"max={s[-1]:7.3f}  sd={statistics.pstdev(s):5.3f}  loss={losses}  (ms)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.0.2.1")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--dtls", action="store_true", help="CoAPS over DTLS 1.2")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--warmup", type=int, default=50)
    ap.add_argument("--timeout", type=float, default=2.0)
    args = ap.parse_args()

    port = args.port if args.port is not None else (5684 if args.dtls else 5683)
    tp = (DtlsTransport if args.dtls else PlainTransport)(args.host, port, args.timeout)
    hs = tp.connect()

    print(f"CoAP RTT to {tp.scheme}://{args.host}:{port}  "
          f"({args.count} samples, {args.warmup} warmup)")
    if args.dtls:
        print(f"DTLS handshake: {hs:.3f} ms (one-time)")
    print()

    cases = [
        ("GET /hello", CODE_GET, "hello", None),
        ("GET /led", CODE_GET, "led", None),
        ("PUT /led=1", CODE_PUT, "led", b"1"),
    ]
    for mtype, tag in ((TYPE_CON, "CON"), (TYPE_NON, "NON")):
        for label, code, path, payload in cases:
            rtts, losses = measure(tp, code, path, mtype, payload, args.count, args.warmup)
            report(f"{tag} {label}", rtts, losses)
        print()


if __name__ == "__main__":
    main()

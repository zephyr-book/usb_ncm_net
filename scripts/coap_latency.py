#!/usr/bin/env python3
"""Measure CoAP round-trip latency to the ZBook over the USB-NCM link.

Holds a single UDP socket open and sends Confirmable (CON) CoAP requests to the
board, timing each request until its ACK + piggybacked response arrives. This
measures the true on-wire round trip (host USB -> board IP/UDP/CoAP + handler ->
host USB), with no per-request process-startup overhead (unlike `coap-client`
in a shell loop). Sequential: one request outstanding at a time.

No external dependencies (stdlib only). Run from the host with the NCM
interface already configured (e.g. `sudo ifconfig en11 192.0.2.2 255.255.255.0 up`).

    python3 scripts/coap_latency.py                 # default: 192.0.2.1, 1000x
    python3 scripts/coap_latency.py --count 2000
"""
import argparse
import os
import socket
import statistics
import struct
import time

COAP_VER = 1
TYPE_CON = 0
TYPE_ACK = 2
CODE_GET = 0x01
CODE_PUT = 0x03


def build_request(code, uri_path, mid, token, payload=None):
    """Encode a minimal CoAP CON request (Uri-Path only, short options)."""
    tkl = len(token)
    out = bytearray([(COAP_VER << 6) | (TYPE_CON << 4) | tkl, code])
    out += struct.pack("!H", mid)
    out += token

    last_opt = 0
    for seg in uri_path.split("/"):
        if not seg:
            continue
        seg_b = seg.encode()
        delta = 11 - last_opt  # Uri-Path is option 11
        length = len(seg_b)
        if delta > 12 or length > 12:
            raise ValueError("extended option encoding not implemented")
        out.append((delta << 4) | length)
        out += seg_b
        last_opt = 11

    if payload is not None:
        out.append(0xFF)  # payload marker
        out += payload
    return bytes(out)


def measure(sock, addr, code, path, payload, count, warmup):
    """Return (list of RTTs in ms, loss count) for `count` timed requests."""
    rtts = []
    losses = 0
    mid = int.from_bytes(os.urandom(2), "big")

    for i in range(count + warmup):
        mid = (mid + 1) & 0xFFFF
        token = os.urandom(4)
        req = build_request(code, path, mid, token, payload)

        t0 = time.perf_counter()
        sock.sendto(req, addr)

        rtt = None
        while True:
            try:
                data, _ = sock.recvfrom(1500)
            except socket.timeout:
                break
            # Match our message ID (piggybacked ACK carries the request's MID).
            if len(data) >= 4 and struct.unpack("!H", data[2:4])[0] == mid:
                rtt = (time.perf_counter() - t0) * 1e3
                break
        if rtt is None:
            if i >= warmup:
                losses += 1
        elif i >= warmup:
            rtts.append(rtt)
    return rtts, losses


def report(name, rtts, losses):
    if not rtts:
        print(f"{name:<16} no responses (loss={losses})")
        return
    s = sorted(rtts)

    def pct(p):
        return s[min(len(s) - 1, round(p / 100 * (len(s) - 1)))]

    print(
        f"{name:<16} n={len(s):<5} "
        f"min={s[0]:6.3f}  mean={statistics.fmean(s):6.3f}  "
        f"med={statistics.median(s):6.3f}  p95={pct(95):6.3f}  "
        f"p99={pct(99):6.3f}  max={s[-1]:7.3f}  "
        f"sd={statistics.pstdev(s):5.3f}  loss={losses}  (ms)"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.0.2.1")
    ap.add_argument("--port", type=int, default=5683)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--warmup", type=int, default=50)
    ap.add_argument("--timeout", type=float, default=1.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)
    addr = (args.host, args.port)

    print(f"CoAP RTT to {args.host}:{args.port}  "
          f"({args.count} samples, {args.warmup} warmup, CON)\n")

    cases = [
        ("GET /hello", CODE_GET, "hello", None),
        ("GET /led", CODE_GET, "led", None),
        ("PUT /led=1", CODE_PUT, "led", b"1"),
    ]
    for name, code, path, payload in cases:
        rtts, losses = measure(sock, addr, code, path, payload,
                               args.count, args.warmup)
        report(name, rtts, losses)


if __name__ == "__main__":
    main()

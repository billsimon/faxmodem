#!/usr/bin/env python3
"""A fake peer that answers the call and then sends no RTP at all — the
failure seen in production when media cannot reach the negotiated port.

Without the media watchdog, faxmodem sits on the call until --timeout (ten
minutes by default) while the far end redials into a busy signal. With it, the
call should be cleared within --media-timeout.

    scripts/test-media-timeout.py --port 5099 --expect-bye-within 30

Exit 0 only if a BYE arrives inside the expected window.
"""
import argparse
import re
import socket
import sys
import time

SDP = ("v=0\r\n"
       "o=- 1 1 IN IP4 127.0.0.1\r\n"
       "s=silent\r\n"
       "c=IN IP4 127.0.0.1\r\n"
       "t=0 0\r\n"
       "m=audio {port} RTP/AVP 0\r\n"
       "a=rtpmap:0 PCMU/8000\r\n"
       "a=sendrecv\r\n")


def header(msg, name):
    for line in msg.split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def reply(sock, addr, msg, status, body=None):
    lines = [f"SIP/2.0 {status}"]
    for name in ("Via", "From", "Call-ID", "CSeq"):
        value = header(msg, name)
        if value is not None:
            lines.append(f"{name}: {value}")
    to = header(msg, "To") or ""
    if ";tag=" not in to:
        to += ";tag=silentpeer"
    lines.append(f"To: {to}")
    lines.append("Contact: <sip:silent@127.0.0.1:%d>" % SOCK_PORT)
    if body:
        lines.append("Content-Type: application/sdp")
        lines.append(f"Content-Length: {len(body)}")
        head = "\r\n".join(lines) + "\r\n\r\n"
        sock.sendto((head + body).encode(), addr)
    else:
        lines.append("Content-Length: 0")
        sock.sendto(("\r\n".join(lines) + "\r\n\r\n").encode(), addr)


def main():
    global SOCK_PORT
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5099)
    ap.add_argument("--rtp-port", type=int, default=6000)
    ap.add_argument("--expect-bye-within", type=float, default=30.0)
    args = ap.parse_args()
    SOCK_PORT = args.port

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.settimeout(args.expect_bye_within + 15)

    # Bind the RTP port so the far end's packets are absorbed rather than
    # answered with ICMP port unreachable. We never send anything back.
    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("127.0.0.1", args.rtp_port))

    print(f"silent peer on 127.0.0.1:{args.port}, RTP sink on {args.rtp_port}", flush=True)
    answered_at = None

    while True:
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            print("FAIL: no BYE — the call was never cleared")
            return 1

        msg = data.decode(errors="replace")
        request = msg.split("\r\n", 1)[0]
        method = request.split(" ", 1)[0]

        if method == "INVITE":
            if answered_at is None:
                print(f"<- {request}", flush=True)
                reply(sock, addr, msg, "200 OK", SDP.format(port=args.rtp_port))
                answered_at = time.monotonic()
                print("-> 200 OK, then total silence", flush=True)
            continue

        if method == "ACK":
            continue

        if method == "BYE":
            held = time.monotonic() - (answered_at or time.monotonic())
            reply(sock, addr, msg, "200 OK")
            print(f"<- BYE after {held:.1f}s on a silent call", flush=True)
            if held > args.expect_bye_within:
                print(f"FAIL: took {held:.1f}s, expected under {args.expect_bye_within:.0f}s")
                return 1
            print("PASS: the media watchdog cleared the channel")
            return 0

        reply(sock, addr, msg, "200 OK")


if __name__ == "__main__":
    SOCK_PORT = 5099
    sys.exit(main())

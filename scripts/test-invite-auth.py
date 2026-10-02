#!/usr/bin/env python3
"""A one-shot fake SIP trunk that challenges the INVITE, to prove faxmodem
authenticates outbound calls with its credentials and never registers.

It answers the first INVITE with 407 Proxy Authentication Required, checks the
digest on the retry, and then declines the call so the test ends immediately.

    scripts/test-invite-auth.py --port 5099 --user 1001 --password secret

Exit 0 only if a correctly authenticated INVITE arrived and no REGISTER did.
"""
import argparse
import hashlib
import re
import socket
import sys

REALM = "faxtrunk"
NONCE = "3d8f2c1ab9e4470e"


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def header(msg, name):
    for line in msg.split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def reply(sock, addr, msg, status, extra=()):
    lines = [f"SIP/2.0 {status}"]
    for name in ("Via", "From", "Call-ID", "CSeq"):
        value = header(msg, name)
        if value is not None:
            lines.append(f"{name}: {value}")
    to = header(msg, "To") or ""
    if ";tag=" not in to:
        to += ";tag=faketrunk"
    lines.append(f"To: {to}")
    lines.extend(extra)
    lines.append("Content-Length: 0")
    sock.sendto(("\r\n".join(lines) + "\r\n\r\n").encode(), addr)


def digest_params(value):
    return dict((k, quoted or bare) for k, quoted, bare in
                re.findall(r'(\w+)=(?:"([^"]*)"|([^,\s]+))', value or ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5099)
    ap.add_argument("--user", default="1001")
    ap.add_argument("--password", default="secret")
    ap.add_argument("--timeout", type=float, default=45.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.settimeout(args.timeout)
    print(f"fake trunk listening on 127.0.0.1:{args.port}", flush=True)

    challenged = False
    while True:
        try:
            data, addr = sock.recvfrom(65535)
        except socket.timeout:
            print("FAIL: nothing arrived before the timeout")
            return 1

        msg = data.decode(errors="replace")
        request = msg.split("\r\n", 1)[0]
        method = request.split(" ", 1)[0]
        print(f"<- {request}", flush=True)

        if method == "REGISTER":
            print("FAIL: faxmodem sent a REGISTER")
            return 1
        if method == "ACK":
            continue
        if method != "INVITE":
            reply(sock, addr, msg, "405 Method Not Allowed")
            continue

        auth = header(msg, "Proxy-Authorization")
        if auth is None:
            if challenged:
                print("FAIL: retried INVITE still carried no credentials")
                return 1
            challenged = True
            reply(sock, addr, msg, "407 Proxy Authentication Required",
                  [f'Proxy-Authenticate: Digest realm="{REALM}", '
                   f'nonce="{NONCE}", algorithm=MD5'])
            print("-> 407 with a digest challenge", flush=True)
            continue

        p = digest_params(auth)
        uri = p.get("uri", "")
        expected = md5(f'{md5(f"{args.user}:{REALM}:{args.password}")}'
                       f':{p.get("nonce", "")}:{md5(f"INVITE:{uri}")}')
        print(f"-> 603 Decline (username={p.get('username')!r}, uri={uri!r})", flush=True)
        reply(sock, addr, msg, "603 Decline")

        if p.get("username") != args.user:
            print(f"FAIL: username was {p.get('username')!r}, expected {args.user!r}")
            return 1
        if p.get("realm") != REALM or p.get("nonce") != NONCE:
            print("FAIL: the credentials answered a different challenge")
            return 1
        if p.get("response") != expected:
            print(f"FAIL: digest mismatch, got {p.get('response')} want {expected}")
            return 1
        print("PASS: INVITE authenticated with the configured credentials, no REGISTER sent")
        return 0


if __name__ == "__main__":
    sys.exit(main())

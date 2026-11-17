#!/usr/bin/env python3
"""Mock Proxmox VE server for trying Portal Mux's Proxmox view.

    python3 -I tests/proxmox_mock.py --dir DIR            # writes DIR/{ca-cert,server-cert,server-key}.pem once
    qemu-system-x86_64 -nodefaults -vga qxl -display none -m 128 \
        -object secret,id=spw,data=t1ck3t -spice tls-port=15931,addr=127.0.0.1,x509-dir=DIR,password-secret=spw \
        -object secret,id=vpw,data=PVEVNC:6 -vnc 127.0.0.1:20,password-secret=vpw

Logins: root@pam / pvepass (ticket), root@pam!rdc / tok-secret (API token), tfa@pam / pvepass (asks for 2FA).
Container 200's console (termproxy) is a bash in a pty, started in DIR.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import pty
import random
import re
import secrets
import signal
import select
import socket
import ssl
import struct
import subprocess
import termios
import threading
import time
import urllib.parse
import fcntl

SUBJECT = "/OU=PVE Cluster Node/O=Proxmox Virtual Environment/CN=pve1.mock"
HOST_SUBJECT = "OU=PVE Cluster Node,O=Proxmox Virtual Environment,CN=pve1.mock"
TICKET, CSRF = "PVE:root@pam:6738A1B2::mocksignature", "6738A1B2:mockcsrf"
TOKEN = "root@pam!rdc=tok-secret"
SPICE_PASSWORD = "t1ck3t"
VNC_TICKET = "PVEVNC:6738A1B2::bW9jayB2bmMgdGlja2V0"
VNC_PROXY_PORT = 5901  # the websocket must ask for it
TERM_TICKET = "PVEVNC:6738A1C0::dGVybXByb3h5IHRpY2tldA=="
TERM_PROXY_PORT = 5902
STARTED = time.time()

GUESTS = {  # vmid: guest; "spice": its display is SPICE
    100: dict(type="qemu", node="pve1", name="win11", status="running", spice=True),
    101: dict(type="qemu", node="pve1", name="debian", status="stopped", spice=False),
    102: dict(type="qemu", node="pve1", name="ubuntu-template", status="stopped", template=1),
    200: dict(type="lxc", node="pve1", name="nginx", status="running"),
    110: dict(type="qemu", node="pve2", name="backup", status="stopped", spice=True),
}
NODES = {"pve1": "online", "pve2": "online"}
lock = threading.Lock()


def make_certs(d):
    if os.path.exists(os.path.join(d, "server-cert.pem")):
        return
    os.makedirs(d, exist_ok=True)
    p = lambda name: os.path.join(d, name)
    run = lambda *a: subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30", "-subj", "/CN=Proxmox Virtual Environment/OU=mock/O=PVE Cluster Manager CA",
        "-keyout", p("ca-key.pem"), "-out", p("ca-cert.pem"))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", SUBJECT, "-keyout", p("server-key.pem"), "-out", p("server.csr"))
    with open(p("ext.cnf"), "w") as f:
        f.write("subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:pve1.mock\n")
    run("openssl", "x509", "-req", "-in", p("server.csr"), "-CA", p("ca-cert.pem"), "-CAkey", p("ca-key.pem"), "-CAcreateserial",
        "-days", "30", "-extfile", p("ext.cnf"), "-out", p("server-cert.pem"))
    os.chmod(p("server-key.pem"), 0o644)  # qemu reads it


def resources():
    out = []
    for n, s in NODES.items():
        node = dict(type="node", id="node/" + n, node=n, status=s)
        if s == "online":
            node.update(cpu=random.uniform(0.02, 0.35), maxcpu=8, mem=random.randint(9 << 30, 11 << 30), maxmem=32 << 30,
                        uptime=int(time.time() - STARTED) + 273600)
        out.append(node)
    for vmid, g in GUESTS.items():
        running = g["status"] == "running"
        out.append(dict(type=g["type"], id=f"{g['type']}/{vmid}", vmid=vmid, node=g["node"], name=g["name"], status=g["status"],
                        template=g.get("template", 0), cpu=random.uniform(0.03, 0.6) if running else 0, mem=(1536 << 20) if running else 0,
                        maxmem=4096 << 20))
    return out


class Api(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    ca = ""
    proxy_port = 0
    tls_port = 61000
    vnc_port = 5920

    def reply(self, status, data=None, message=None, reason=None):
        body = json.dumps({"data": data, **({"message": message} if message else {})}).encode()
        self.send_response(status, reason)
        self.send_header("Content-Type", "application/json;charset=UTF-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def user(self):
        return "root@pam!rdc" if self.headers.get("Authorization") == "PVEAPIToken=" + TOKEN else "root@pam"

    def auth(self, post):
        if self.headers.get("Authorization") == "PVEAPIToken=" + TOKEN:
            return True
        cookie = self.headers.get("Cookie", "")
        if "PVEAuthCookie=" + urllib.parse.quote(TICKET, safe="") not in cookie and "PVEAuthCookie=" + TICKET not in cookie:
            self.reply(401, reason="No ticket")
            return False
        if post and self.headers.get("CSRFPreventionToken") != CSRF:
            self.reply(401, reason="Permission check failed (invalid csrf token)")
            return False
        return True

    def form(self):
        n = int(self.headers.get("Content-Length") or 0)
        return dict(urllib.parse.parse_qsl(self.rfile.read(n).decode()))

    def do_GET(self):
        if not self.auth(False):
            return
        url = urllib.parse.urlsplit(self.path)
        parts = url.path.split("/")
        if url.path == "/api2/json/cluster/resources":
            with lock:
                return self.reply(200, resources())
        if len(parts) == 9 and parts[3] == "nodes" and parts[7:] == ["status", "current"]:
            with lock:
                g = GUESTS.get(int(parts[6]))
                if not g or g["node"] != parts[4] or g["type"] != parts[5]:
                    return self.reply(500, message="does not exist\n")
                status = {"status": g["status"], "vmid": int(parts[6]), "name": g["name"], "qmpstatus": g["status"]}
                if g.get("spice"):
                    status["spice"] = 1
                return self.reply(200, status)
        if len(parts) == 8 and parts[3] == "nodes" and parts[7] == "vncwebsocket":
            return self.websocket(dict(urllib.parse.parse_qsl(url.query)))
        self.reply(501, reason=f"Method 'GET {self.path}' not implemented")

    def websocket(self, query):
        """vncwebsocket: protocol "binary", raw RFB in binary frames; or termproxy's messages."""
        term = query.get("vncticket") == TERM_TICKET and query.get("port") == str(TERM_PROXY_PORT)
        if not term and (query.get("vncticket") != VNC_TICKET or query.get("port") != str(VNC_PROXY_PORT)):
            return self.reply(401, reason="permission denied - invalid vnc ticket")
        protocols = [p.strip() for p in (self.headers.get("Sec-WebSocket-Protocol") or "").split(",")]
        if self.headers.get("Upgrade", "").lower() != "websocket" or "binary" not in protocols:
            return self.reply(400, reason="unsupported websocket-protocol protocol")
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.send_header("Sec-WebSocket-Protocol", "binary")
        self.end_headers()
        self.close_connection = True
        print("websocket: open", "(termproxy)" if term else "", flush=True)
        if term:
            return self.termproxy()
        upstream = socket.create_connection(("127.0.0.1", self.vnc_port))
        client = self.connection
        buf = b""
        try:
            while True:
                readable = select.select([client, upstream], [], [], 0.2)[0]
                if client.pending():
                    readable.append(client)
                if upstream in readable:
                    data = upstream.recv(65536)
                    if not data:
                        return client.sendall(ws_frame(8, b""))
                    client.sendall(ws_frame(2, data))
                if client in readable:
                    data = client.recv(65536)
                    if not data:
                        return
                    buf += data
                    while (frame := ws_parse(buf)) is not None:
                        op, payload, buf = frame
                        if op in (0, 2):
                            upstream.sendall(payload)
                        elif op == 9:
                            client.sendall(ws_frame(10, payload))
                        elif op == 8:
                            client.sendall(ws_frame(8, payload[:2]))
                            return
        except OSError:
            pass
        finally:
            upstream.close()
            print("websocket: closed", flush=True)

    def termproxy(self):
        """As Proxmox's termproxy: "user:ticket\n", then "OK"; 0:LEN:DATA input, 1:COLS:ROWS: resize, 2 ping."""
        client, buf, data, pid, fd = self.connection, b"", b"", 0, -1
        try:
            while True:
                readable = select.select([client] + ([fd] if fd >= 0 else []), [], [], 0.2)[0]
                if client.pending():
                    readable.append(client)
                if fd in readable:
                    try:
                        out = os.read(fd, 65536)
                    except OSError:
                        out = b""
                    if not out:
                        return client.sendall(ws_frame(8, b"\x03\xe8"))
                    client.sendall(ws_frame(2, out))
                if client not in readable:
                    continue
                chunk = client.recv(65536)
                if not chunk:
                    return
                buf += chunk
                while (frame := ws_parse(buf)) is not None:
                    op, payload, buf = frame
                    if op == 8:
                        return client.sendall(ws_frame(8, payload[:2]))
                    if op == 9:
                        client.sendall(ws_frame(10, payload))
                    if op in (0, 1, 2):
                        data += payload
                if pid == 0:
                    if b"\n" not in data:
                        continue
                    line, data = data.split(b"\n", 1)
                    print("termproxy: login", line.split(b":", 1)[0].decode(errors="replace"), flush=True)
                    if line not in (b"root@pam:" + TERM_TICKET.encode(), b"root@pam!rdc:" + TERM_TICKET.encode()):
                        return client.sendall(ws_frame(8, b"\x03\xe8"))
                    pid, fd = pty.fork()
                    if pid == 0:
                        env = dict(os.environ, TERM="xterm-256color", PS1="root@nginx:\\w# ")
                        os.chdir(Api.home)
                        os.execvpe("bash", ["bash", "--norc", "--noprofile", "-i"], env)
                    client.sendall(ws_frame(2, b"OK"))
                while data:
                    if data[:1] == b"2":
                        data = data[1:]
                    elif m := re.match(rb"1:(\d+):(\d+):", data):
                        print("termproxy: resize", m[1].decode(), m[2].decode(), flush=True)
                        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", int(m[2]), int(m[1]), 0, 0))
                        data = data[m.end():]
                    elif m := re.match(rb"0:(\d+):", data):
                        if len(data) < m.end() + int(m[1]):
                            break  # the rest comes in the next frame
                        os.write(fd, data[m.end():m.end() + int(m[1])])
                        data = data[m.end() + int(m[1]):]
                    elif re.fullmatch(rb"[01](:\d*)?(:\d*)?", data):
                        break  # the rest comes in the next frame
                    else:
                        print("termproxy: bad message", data[:20], flush=True)
                        return client.sendall(ws_frame(8, b"\x03\xea"))
        except OSError:
            pass
        finally:
            if pid:
                os.kill(pid, signal.SIGHUP)
                os.waitpid(pid, 0)
                os.close(fd)
            print("websocket: closed (termproxy)", flush=True)

    def do_POST(self):
        form = self.form()
        parts = self.path.split("/")  # ['', 'api2', 'json', 'nodes', node, type, vmid, ...]
        if self.path == "/api2/json/access/ticket":
            if form.get("password") != "pvepass" or form.get("username") not in ("root@pam", "tfa@pam"):
                return self.reply(401, reason="authentication failure")
            if form["username"] == "tfa@pam":
                return self.reply(200, {"username": "tfa@pam", "ticket": "PVE:!tfa!...", "NeedTFA": 1})
            return self.reply(200, {"username": "root@pam", "ticket": TICKET, "CSRFPreventionToken": CSRF})
        if not self.auth(True):
            return
        if len(parts) >= 8 and parts[3] == "nodes":
            node, kind, vmid = parts[4], parts[5], int(parts[6])
            with lock:
                g = GUESTS.get(vmid)
                if not g or g["node"] != node or g["type"] != kind:
                    return self.reply(500, message=f"Configuration file 'nodes/{node}/{kind}-server/{vmid}.conf' does not exist\n")
                if parts[7] == "status" and len(parts) == 9:
                    action = parts[8]
                    if g.get("template"):
                        return self.reply(500, message="you can't start a vm if it's a template\n")
                    if action == "start":
                        g["status"] = "running"
                    elif action in ("stop", "shutdown"):
                        g["status"] = "stopped"
                    elif action not in ("reboot", "reset", "suspend", "resume"):
                        return self.reply(501, reason=f"Method 'POST {self.path}' not implemented")
                    return self.reply(200, f"UPID:{node}:0001:{action}:{vmid}:root@pam:")
                if parts[7] == "vncproxy":
                    if g["status"] != "running":
                        return self.reply(500, message=f"CT {vmid} not running\n" if kind == "lxc" else f"VM {vmid} not running\n")
                    if form.get("websocket") != "1":
                        return self.reply(501, reason="only websocket=1 is mocked")
                    return self.reply(200, {"port": str(VNC_PROXY_PORT), "ticket": VNC_TICKET, "user": "root@pam", "cert": "-----BEGIN CERTIFICATE-----\n…",
                                            "upid": f"UPID:{node}:0002:vncproxy:{vmid}:root@pam:"})
                if parts[7] == "termproxy":
                    if g["status"] != "running":
                        return self.reply(500, message=f"CT {vmid} not running\n" if kind == "lxc" else f"VM {vmid} not running\n")
                    return self.reply(200, {"port": TERM_PROXY_PORT, "ticket": TERM_TICKET, "user": self.user(),
                                            "upid": f"UPID:{node}:0003:vncterm:{vmid}:root@pam:"})
                if parts[7] == "spiceproxy" and kind == "qemu":
                    if g["status"] != "running":
                        return self.reply(500, message=f"VM {vmid} not running\n")
                    if not g.get("spice"):
                        return self.reply(500, message="No spice port\n")
                    proxy_host = form.get("proxy") or "127.0.0.1"
                    return self.reply(200, {
                        "type": "spice", "title": f"VM {vmid} - {g['name']}", "password": SPICE_PASSWORD,
                        "host": f"pvespiceproxy:{secrets.token_hex(4)}:{vmid}:{node}::{secrets.token_hex(8)}",
                        "proxy": f"http://{proxy_host}:{self.proxy_port}", "tls-port": self.tls_port,
                        "host-subject": HOST_SUBJECT, "ca": self.ca.replace("\n", "\\n"), "delete-this-file": 1,
                        "toggle-fullscreen": "Shift+F11", "release-cursor": "Ctrl+Alt+R", "secure-attention": "Ctrl+Alt+Ins"})
        self.reply(501, reason=f"Method 'POST {self.path}' not implemented")

    def log_message(self, fmt, *args):
        print("api:", self.command if hasattr(self, "command") else "", fmt % args, flush=True)


def ws_parse(buf):
    if len(buf) < 2:
        return None
    op, masked, n, i = buf[0] & 0x0F, buf[1] & 0x80, buf[1] & 0x7F, 2
    if n == 126:
        if len(buf) < 4:
            return None
        n, i = struct.unpack(">H", buf[2:4])[0], 4
    elif n == 127:
        if len(buf) < 10:
            return None
        n, i = struct.unpack(">Q", buf[2:10])[0], 10
    mask = buf[i:i + 4] if masked else b"\0\0\0\0"
    i += 4 if masked else 0
    if len(buf) < i + n:
        return None
    payload = bytes(b ^ mask[k % 4] for k, b in enumerate(buf[i:i + n]))
    return op, payload, buf[i + n:]


def ws_frame(op, payload):
    n = len(payload)
    head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
    return head + payload


def proxy(port, spice_port):
    """CONNECT pvespiceproxy:…:<tls port> -> local SPICE TLS port."""
    server = socket.create_server(("127.0.0.1", port))

    def serve(client):
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = client.recv(4096)
            if not chunk:
                return client.close()
            head += chunk
        line = head.split(b"\r\n", 1)[0].decode(errors="replace")
        print("proxy:", line, flush=True)
        if not line.startswith("CONNECT pvespiceproxy:"):
            client.sendall(b"HTTP/1.0 501 proxy not implemented\r\n\r\n")
            return client.close()
        upstream = socket.create_connection(("127.0.0.1", spice_port))
        client.sendall(b"HTTP/1.0 200 OK\r\n\r\n")
        rest = head.split(b"\r\n\r\n", 1)[1]
        if rest:
            upstream.sendall(rest)
        socks = [client, upstream]
        while True:
            for s in select.select(socks, [], [])[0]:
                data = s.recv(65536)
                if not data:
                    client.close(), upstream.close()
                    return
                (upstream if s is client else client).sendall(data)

    while True:
        c, _ = server.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("--dir", required=True, help="certificates (made if missing)")
    a.add_argument("--port", type=int, default=18006)
    a.add_argument("--proxy-port", type=int, default=13128)
    a.add_argument("--spice-port", type=int, default=15931, help="the local SPICE server's TLS port")
    a.add_argument("--vnc-port", type=int, default=5920, help="the local VNC server, password PVEVNC:6")
    args = a.parse_args()
    make_certs(args.dir)
    with open(os.path.join(args.dir, "ca-cert.pem")) as f:
        Api.ca = f.read()
    Api.proxy_port = args.proxy_port
    Api.home = os.path.abspath(args.dir)
    Api.vnc_port = args.vnc_port
    threading.Thread(target=proxy, args=(args.proxy_port, args.spice_port), daemon=True).start()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Api)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(args.dir, "server-cert.pem"), os.path.join(args.dir, "server-key.pem"))
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
    print(f"mock Proxmox on https://127.0.0.1:{args.port}, spiceproxy on {args.proxy_port} → SPICE TLS {args.spice_port}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Mock Azure sign-in and Resource Manager for trying Portal Mux's Azure view, plain HTTP on 127.0.0.1.

    python3 -I tests/azure_mock.py [--port 18443] [--linux-ip 127.0.0.1] [--linux-admin azureuser] [--windows-ip 127.0.0.1] [--expire 3599] [--deny]
    PORTAL_MUX_AZURE_LOGIN=http://127.0.0.1:18443 PORTAL_MUX_AZURE_ARM=http://127.0.0.1:18443 portal-mux

The sign-in page answers at once with a redirect to the app's local port (code + PKCE checked); --deny refuses it.
Refresh tokens rotate on every use; access tokens last --expire seconds. Subscription "Pay-As-You-Go" lists three VMs
over two pages (web-linux running, win-app deallocated, db-private stopped with only a private IP); "Old Trial" is
disabled. Power actions take 4 s through starting/stopping/deallocating.
Serial console (--serial-disabled: switched off for the subscription): win-app has no boot diagnostics; the others
answer the token with a getty-like "login:" (any user, password "serial"), then a bash in a pty in --dir.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import pty
import re
import secrets
import select
import signal
import struct
import sys
import tempfile
import threading
import time
import urllib.parse

TENANT_ID = "11111111-2222-3333-4444-555555555555"
USER = "ann@contoso.example"
lock = threading.Lock()
codes, access, refresh = {}, {}, set()  # code -> (challenge, redirect); token -> expiry
vms = {}  # name -> dict
LOGIN = r"""
import getpass, os, sys
name = sys.argv[1]
print(f"\nUbuntu 24.04.1 LTS {name} ttyS0\n")
while True:
    user = input(f"{name} login: ")
    if getpass.getpass("Password: ") == "serial" and user:
        break
    print("\nLogin incorrect\n")
os.execvp("bash", ["bash", "--norc", "--noprofile", "-i"])
"""


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def vm(name, system, admin, power, private, public, boot=True):
    return {"name": name, "os": system, "admin": admin, "power": power, "until": 0, "next": power, "private": private, "public": public,
            "boot": boot}


def vm_id(name, lower=False):
    rid = f"/subscriptions/sub-1/resourceGroups/RG-Demo/providers/Microsoft.Compute/virtualMachines/{name}"
    return rid.lower() if lower else rid


def tick():
    for v in vms.values():
        if v["until"] and time.time() >= v["until"]:
            v["power"], v["until"] = v["next"], 0


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # a websocket upgrade needs it

    def log_message(self, fmt, *args):
        print(self.command, self.path.split("?")[0], fmt % args, flush=True)

    def send_json(self, status, body, headers=()):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def arm_error(self, status, code, message):
        self.send_json(status, {"error": {"code": code, "message": message}})

    def tokens(self):
        token = secrets.token_urlsafe(24)
        access[token] = time.time() + args.expire
        new_refresh = "rt+" + secrets.token_urlsafe(24) + "/="  # characters a form must encode
        refresh.add(new_refresh)
        claims = b64url(json.dumps({"preferred_username": USER, "tid": TENANT_ID}).encode())
        return {"token_type": "Bearer", "expires_in": args.expire, "access_token": token, "refresh_token": new_refresh,
                "id_token": f"{b64url(b'{}')}.{claims}.sig"}

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        if m := re.fullmatch(r"/serial/([^/]+)/client", url.path):
            return self.serial(m.group(1))
        if re.fullmatch(r"/[^/]+/oauth2/v2\.0/authorize", url.path):
            redirect = q.get("redirect_uri", "")
            if not re.fullmatch(r"http://localhost:\d+", redirect) or q.get("code_challenge_method") != "S256":
                return self.send_json(400, {"error": "invalid_request", "error_description": "bad redirect_uri or PKCE"})
            if args.deny:
                target = redirect + "/?" + urllib.parse.urlencode({"error": "access_denied", "error_description": "The user said no", "state": q["state"]})
            else:
                code = "0.A" + secrets.token_urlsafe(16)
                codes[code] = (q["code_challenge"], redirect)
                target = redirect + "/?" + urllib.parse.urlencode({"code": code, "state": q["state"], "session_state": "x"})
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if not self.authorized():
            return
        with lock:
            tick()
            if url.path == "/subscriptions":
                return self.send_json(200, {"value": [
                    {"subscriptionId": "sub-1", "displayName": "Pay-As-You-Go", "state": "Enabled", "tenantId": TENANT_ID},
                    {"subscriptionId": "sub-2", "displayName": "Old Trial", "state": "Disabled", "tenantId": TENANT_ID}]})
            if url.path == "/subscriptions/sub-1/providers/Microsoft.SerialConsole/consoleServices/default":
                return self.send_json(200, {"properties": {"disabled": args.serial_disabled}})
            if re.fullmatch(r"/subscriptions/sub-2/.*", url.path):
                return self.arm_error(409, "ReadOnlyDisabledSubscription", "The subscription is disabled.")
            if url.path == "/subscriptions/sub-1/providers/Microsoft.Compute/virtualMachines":
                if q.get("statusOnly") == "true":
                    return self.send_json(200, {"value": [
                        {"id": vm_id(v["name"], lower=True), "name": v["name"], "properties": {"instanceView": {"statuses": [
                            {"code": "ProvisioningState/succeeded"}, {"code": "PowerState/" + v["power"]}]}}} for v in vms.values()]})
                page = [self.model(v) for v in vms.values()]
                if q.get("$skiptoken") == "2":
                    return self.send_json(200, {"value": page[1:]})
                return self.send_json(200, {"value": page[:1], "nextLink": f"http://127.0.0.1:{args.port}{url.path}?api-version={q['api-version']}&$skiptoken=2"})
            if url.path == "/subscriptions/sub-1/providers/Microsoft.Network/networkInterfaces":
                return self.send_json(200, {"value": [{"id": f"/subscriptions/sub-1/resourceGroups/rg-demo/providers/Microsoft.Network/networkInterfaces/{v['name']}-nic",
                    "properties": {"ipConfigurations": [{"properties": {"primary": True, "privateIPAddress": v["private"], **(
                        {"publicIPAddress": {"id": f"/subscriptions/sub-1/resourceGroups/rg-demo/providers/Microsoft.Network/publicIPAddresses/{v['name']}-ip"}}
                        if v["public"] is not None else {})}}]}} for v in vms.values()]})
            if url.path == "/subscriptions/sub-1/providers/Microsoft.Network/publicIPAddresses":
                return self.send_json(200, {"value": [{"id": f"/subscriptions/sub-1/resourceGroups/RG-DEMO/providers/Microsoft.Network/publicIPAddresses/{v['name']}-ip",
                    "properties": {"ipAddress": v["public"] if v["power"] in ("running", "starting", "stopped") else None}}
                    for v in vms.values() if v["public"] is not None]})
        self.arm_error(404, "NotFound", "No such resource: " + url.path)

    def model(self, v):
        return {"id": vm_id(v["name"]), "name": v["name"], "location": "westeurope", "properties": {
            "hardwareProfile": {"vmSize": "Standard_B2s"}, "storageProfile": {"osDisk": {"osType": v["os"]}},
            "osProfile": {"computerName": v["name"], "adminUsername": v["admin"]},
            "diagnosticsProfile": {"bootDiagnostics": {"enabled": v["boot"]}},
            "networkProfile": {"networkInterfaces": [{"id": f"/subscriptions/sub-1/resourceGroups/RG-Demo/providers/Microsoft.Network/networkInterfaces/{v['name']}-nic"}]}}}

    def authorized(self):
        token = self.headers.get("Authorization", "").removeprefix("Bearer ")
        if access.get(token, 0) < time.time():
            self.arm_error(401, "ExpiredAuthenticationToken", "The access token expired.")
            return False
        return True

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        form = dict(urllib.parse.parse_qsl(body))
        if re.fullmatch(r"/[^/]+/oauth2/v2\.0/token", url.path):
            with lock:
                if form.get("grant_type") == "authorization_code":
                    challenge, redirect = codes.pop(form.get("code"), (None, None))
                    if challenge != b64url(hashlib.sha256(form.get("code_verifier", "").encode()).digest()) or redirect != form.get("redirect_uri"):
                        return self.send_json(400, {"error": "invalid_grant", "error_description": "AADSTS70008: Bad code or verifier.\r\nTrace ID: 1"})
                    return self.send_json(200, self.tokens())
                if form.get("grant_type") == "refresh_token" and form.get("refresh_token") in refresh:
                    refresh.discard(form["refresh_token"])
                    return self.send_json(200, self.tokens())
            return self.send_json(400, {"error": "invalid_grant", "error_description": "AADSTS700082: The refresh token has expired.\r\nTrace ID: 2"})
        if not self.authorized():
            return
        if self.headers.get("Content-Length") is None:
            return self.arm_error(411, "LengthRequired", "Content-Length is required.")
        if m := re.fullmatch(r"/subscriptions/sub-1/resourcegroups/rg-demo/providers/microsoft\.compute/virtualmachines/([^/]+)"
                             r"/providers/Microsoft\.SerialConsole/serialPorts/0/connect", url.path, re.IGNORECASE):
            if m.group(1) not in vms:
                return self.arm_error(404, "NotFound", "No such VM.")
            if self.headers.get("Content-Type") != "application/json":  # as the real one
                return self.send_json(415, {})
            return self.send_json(200, {"connectionString": f"ws://127.0.0.1:{args.port}/serial/{m.group(1)}/client"})
        m = re.fullmatch(r"/subscriptions/sub-1/resourcegroups/rg-demo/providers/microsoft\.compute/virtualmachines/([^/]+)/(start|restart|deallocate|poweroff)",
                         url.path, re.IGNORECASE)
        with lock:
            tick()
            v = vms.get(m.group(1)) if m else None
            if not v:
                return self.arm_error(404, "NotFound", "No such VM.")
            action = m.group(2).lower()
            if action == "start" and v["power"] in ("deallocated", "stopped"):
                v.update(power="starting", next="running", until=time.time() + 4)
            elif action == "deallocate" and v["power"] != "deallocated":
                v.update(power="deallocating", next="deallocated", until=time.time() + 4)
            elif action == "poweroff" and v["power"] == "running":
                v.update(power="stopping", next="stopped", until=time.time() + 4)
            elif action == "restart" and v["power"] == "running":
                pass
            else:
                return self.arm_error(409, "OperationNotAllowed", f"Cannot {action} a VM that is {v['power']}.")
        self.send_json(202, {}, [("Azure-AsyncOperation", "http://127.0.0.1/op"), ("Retry-After", "4")])

    def serial(self, name):
        """As the serial console's websocket: a first message, the token as the answer, then the port's text both ways."""
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True  # no HTTP after it
        client, buf, pid, fd = self.connection, b"", 0, -1
        v = vms.get(name)
        if not v or v["power"] not in ("running", "starting"):
            print("serial: not running", name, flush=True)
            return client.sendall(ws_frame(8, struct.pack(">H", 1011) + b"The VM is not running."))
        client.sendall(ws_frame(1, b'{"status":"connected"}'))
        try:
            while True:
                readable = select.select([client] + ([fd] if fd >= 0 else []), [], [], 0.5)[0]
                if fd in readable:
                    try:
                        out = os.read(fd, 65536)
                    except OSError:
                        out = b""
                    if not out:
                        return client.sendall(ws_frame(8, struct.pack(">H", 1000)))
                    try:  # text frames when they can be, as the real one may
                        out.decode()
                        client.sendall(ws_frame(1, out))
                    except UnicodeDecodeError:
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
                    if op not in (0, 1, 2):
                        continue
                    if pid == 0:
                        if access.get(payload.decode(errors="replace"), 0) < time.time():
                            print("serial: bad token", flush=True)
                            return client.sendall(ws_frame(8, struct.pack(">H", 1008) + b"Unauthorized"))
                        print("serial: token accepted", name, flush=True)
                        pid, fd = pty.fork()
                        if pid == 0:
                            os.chdir(args.dir)
                            os.execvpe(sys.executable, [sys.executable, "-I", "-c", LOGIN, name],
                                       dict(os.environ, TERM="xterm-256color", PS1=f"azureuser@{name}:\\w$ "))
                    else:
                        os.write(fd, payload)
        except OSError:
            pass
        finally:
            if pid:
                os.kill(pid, signal.SIGHUP)
                os.waitpid(pid, 0)
                os.close(fd)
            print("serial: closed", name, flush=True)


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


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=18443)
    p.add_argument("--linux-ip", default="127.0.0.1")
    p.add_argument("--linux-admin", default="azureuser")
    p.add_argument("--windows-ip", default="127.0.0.1")
    p.add_argument("--expire", type=int, default=3599)
    p.add_argument("--deny", action="store_true")
    p.add_argument("--serial-disabled", action="store_true")
    p.add_argument("--dir", default=None, help="where the serial console's shell starts (a new temporary folder)")
    args = p.parse_args()
    args.dir = args.dir or tempfile.mkdtemp(prefix="azure-mock-")
    for v in (vm("web-linux", "Linux", args.linux_admin, "running", "10.0.0.4", args.linux_ip),
              vm("win-app", "Windows", "portaladmin", "deallocated", "10.0.0.5", args.windows_ip, boot=False),
              vm("db-private", "Linux", "azureuser", "stopped", "10.0.0.6", None)):
        vms[v["name"]] = v
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()

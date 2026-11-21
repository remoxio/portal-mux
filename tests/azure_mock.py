#!/usr/bin/env python3
"""Mock Azure sign-in and Resource Manager for trying Portal Mux's Azure view, plain HTTP on 127.0.0.1.

    python3 -I tests/azure_mock.py [--port 18443] [--linux-ip 127.0.0.1] [--linux-admin azureuser] [--windows-ip 127.0.0.1] [--expire 3599] [--deny]
    PORTAL_MUX_AZURE_LOGIN=http://127.0.0.1:18443 PORTAL_MUX_AZURE_ARM=http://127.0.0.1:18443 portal-mux

The sign-in page answers at once with a redirect to the app's local port (code + PKCE checked); --deny refuses it.
Refresh tokens rotate on every use; access tokens last --expire seconds. Subscription "Pay-As-You-Go" lists three VMs
over two pages (web-linux running, win-app deallocated, db-private stopped with only a private IP); "Old Trial" is
disabled. Power actions take 4 s through starting/stopping/deallocating.
"""
import argparse
import base64
import hashlib
import http.server
import json
import re
import secrets
import threading
import time
import urllib.parse

TENANT_ID = "11111111-2222-3333-4444-555555555555"
USER = "ann@contoso.example"
lock = threading.Lock()
codes, access, refresh = {}, {}, set()  # code -> (challenge, redirect); token -> expiry
vms = {}  # name -> dict


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def vm(name, os, admin, power, private, public):
    return {"name": name, "os": os, "admin": admin, "power": power, "until": 0, "next": power, "private": private, "public": public}


def vm_id(name, lower=False):
    rid = f"/subscriptions/sub-1/resourceGroups/RG-Demo/providers/Microsoft.Compute/virtualMachines/{name}"
    return rid.lower() if lower else rid


def tick():
    for v in vms.values():
        if v["until"] and time.time() >= v["until"]:
            v["power"], v["until"] = v["next"], 0


class Handler(http.server.BaseHTTPRequestHandler):
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


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=18443)
    p.add_argument("--linux-ip", default="127.0.0.1")
    p.add_argument("--linux-admin", default="azureuser")
    p.add_argument("--windows-ip", default="127.0.0.1")
    p.add_argument("--expire", type=int, default=3599)
    p.add_argument("--deny", action="store_true")
    args = p.parse_args()
    for v in (vm("web-linux", "Linux", args.linux_admin, "running", "10.0.0.4", args.linux_ip),
              vm("win-app", "Windows", "portaladmin", "deallocated", "10.0.0.5", args.windows_ip),
              vm("db-private", "Linux", "azureuser", "stopped", "10.0.0.6", None)):
        vms[v["name"]] = v
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()

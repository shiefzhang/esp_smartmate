#!/usr/bin/env python3
"""
智能家居 + 代理 综合面板的反向代理。

解决的问题：
  1. HA 的 REST API 不返回 Access-Control-Allow-Origin -> 浏览器跨域被拦
  2. mihomo 启用 secret 后，UI 页面拿不到 Authorization 头
  3. 令牌/secret 不应该出现在前端源码里

方案：本进程持有 HA 令牌与 mihomo secret，前端只调同源的 /ha/* 与 /mihomo/*。

路由：
  GET  /                     -> 面板页面 index.html
  GET  /app.css /app.js      -> 静态资源
  GET  /ha/states            -> HA 全部实体
  GET  /ha/states/<id>       -> HA 单个实体
  POST /ha/services/<domain>/<service> -> HA 调用服务
  ANY  /mihomo/<path>        -> mihomo REST API（自动带 secret）

部署到 Orange Pi（armv7）：
  /home/orangepi/docker/panel/
    panel_proxy.py
    panel-secrets.json     {"ha_token": "...", "mihomo_secret": "..."}  chmod 600
    index.html
  /etc/systemd/system/panel-proxy.service

只用标准库，Orange Pi 上直接跑。
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SECRETS_FILE = os.path.join(BASE_DIR, "panel-secrets.json")
HA_BASE = "http://127.0.0.1:8123"
MIHOMO_BASE = "http://127.0.0.1:9090"
PORT = 8090

_secrets = {}


def load_secrets():
    global _secrets
    try:
        with open(SECRETS_FILE, "r", encoding="utf-8") as f:
            _secrets = json.load(f)
    except Exception as e:
        print("[FATAL] 无法读取 %s: %s" % (SECRETS_FILE, e), file=sys.stderr)
        sys.exit(1)
    if not _secrets.get("ha_token"):
        print("[FATAL] secrets 里缺少 ha_token", file=sys.stderr)
        sys.exit(1)


class Handler(BaseHTTPRequestHandler):
    server_version = "HomePanel/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # ---------- 响应工具 ----------
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj, ensure_ascii=False))

    def _static(self, name, ctype):
        path = os.path.join(BASE_DIR, name)
        if not os.path.isfile(path):
            self._json(404, {"error": "not found: " + name})
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    def _proxy(self, url, method="GET", body=None, headers=None, timeout=20):
        req = urllib.request.Request(url, data=body, method=method)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read(), resp.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), "application/json; charset=utf-8"
        except Exception as e:
            return 502, json.dumps({"error": "upstream failed: %s" % e}).encode(), \
                "application/json; charset=utf-8"

    # ---------- 路由 ----------
    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path

        if path in ("/", "/index.html"):
            return self._static("index.html", "text/html; charset=utf-8")
        if path == "/app.css":
            return self._static("app.css", "text/css; charset=utf-8")
        if path == "/app.js":
            return self._static("app.js", "application/javascript; charset=utf-8")

        if path == "/ha/states":
            code, body, ct = self._proxy(
                HA_BASE + "/api/states", headers=self._ha_headers())
            return self._send(code, body, ct)

        if path.startswith("/ha/states/"):
            eid = urllib.parse.unquote(path[len("/ha/states/"):])
            code, body, ct = self._proxy(
                HA_BASE + "/api/states/" + urllib.parse.quote(eid),
                headers=self._ha_headers())
            return self._send(code, body, ct)

        if path.startswith("/mihomo/"):
            return self._mihomo(path[len("/mihomo"):])

        return self._json(404, {"error": "no route: " + path})

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None

        if path.startswith("/ha/services/"):
            rest = path[len("/ha/services/"):]
            code, b, ct = self._proxy(
                HA_BASE + "/api/services/" + rest, method="POST", body=body,
                headers=self._ha_headers())
            return self._send(code, b, ct)

        if path.startswith("/mihomo/"):
            return self._mihomo(path[len("/mihomo"):], method="POST", body=body)

        return self._json(404, {"error": "no route: " + path})

    def do_PUT(self):
        return self._mihomo_or_404(urllib.parse.urlparse(self.path).path)

    def do_DELETE(self):
        return self._mihomo_or_404(urllib.parse.urlparse(self.path).path)

    def _mihomo_or_404(self, path):
        if path.startswith("/mihomo/"):
            return self._mihomo(path[len("/mihomo"):])
        return self._json(404, {"error": "no route: " + path})

    def _ha_headers(self):
        return {
            "Authorization": "Bearer " + _secrets["ha_token"],
            "Content-Type": "application/json",
        }

    def _mihomo(self, path, method="GET", body=None):
        secret = _secrets.get("mihomo_secret", "")
        headers = {"Content-Type": "application/json"}
        if secret:
            headers["Authorization"] = "Bearer " + secret
        code, b, ct = self._proxy(
            MIHOMO_BASE + path, method=method, body=body, headers=headers)
        return self._send(code, b, ct)


def main():
    load_secrets()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("panel proxy listening on :%d" % PORT, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
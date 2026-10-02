#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""serve-pkg.py — PS5 Nexus 的 PC 端 PKG 文件服务器（单文件、零依赖）。

在 PS5 的「PKG管理 → 从 PC 安装」里填本机 IP:端口即可列出卡片并安装；
系统安装器直接从 PC 分段下载，PKG 全程不落 PS5 硬盘。

为什么不用 `python -m http.server`：Sony 下载器发 Range 请求（206 Partial
Content），标准库那套忽略 Range → PS5 中止安装（0x80B211C8）。本脚本实现了
Range/HEAD，并额外提供 /catalog（JSON 列表，带 CORS 头，供 PS5 浏览器跨域
fetch）。

用法（放在装着 .pkg 的文件夹里运行）：
    python serve-pkg.py [端口]        # 默认 9898
然后在 PS5 Nexus 网页里填: <本机IP>:9898

取自 Loopayeh/pkg-sender 的 serve_pkg.py（MIT License），按本项目需要加了
/catalog、CORS 与中文文件名处理。
"""
import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9898
CHUNK = 1024 * 1024
PKG_EXTS = (".pkg", ".ffpkg", ".ffpfsc", ".exfat")


class Handler(BaseHTTPRequestHandler):
    server_version = "PS5-Nexus-serve-pkg/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("%s - %s\n" % (self.client_address[0], fmt % args))
        sys.stdout.flush()

    # ---- 通用响应（统一带 CORS：PS5 浏览器跨域 fetch catalog/图标） ----
    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # ---- /catalog：文件列表（PS5 端卡片数据源） ----
    def _catalog(self):
        items = []
        for name in sorted(os.listdir(".")):
            if not name.lower().endswith(PKG_EXTS):
                continue
            try:
                if not os.path.isfile(name):
                    continue
                size = os.path.getsize(name)
            except OSError:
                continue
            items.append({
                "id": name,
                "name": os.path.splitext(name)[0],
                "size": size,
                "is_fpkg": name.lower().endswith((".ffpkg", ".ffpfsc", ".exfat")),
                "url": "/pkg/" + urllib.parse.quote(name),
            })
        body = json.dumps({"ok": True, "items": items}).encode("utf-8")
        self._send(200, "application/json; charset=utf-8", body)

    # ---- PKG 文件：Range/HEAD 都支持 ----
    def _send_file(self, path, start, end, total, partial):
        try:
            f = open(path, "rb")
        except OSError:
            self._send(404, "text/plain; charset=utf-8",
                       b"not found")
            return
        f.seek(start)
        length = end - start + 1
        self.send_response(206 if partial else 200)
        if partial:
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, end, total))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        try:
            while length > 0:
                buf = f.read(min(CHUNK, length))
                if not buf:
                    break
                self.wfile.write(buf)
                length -= len(buf)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            f.close()

    def do_GET(self):
        # Sony 下载器会在 URL 后附加 query（&product=..&serverIpAddr=..），剥掉
        raw = self.path.split("?", 1)[0]
        path = urllib.parse.unquote(raw)
        if path in ("/catalog", "/catalog/"):
            return self._catalog()
        if path.startswith("/pkg/"):
            # 只取文件名，不进子目录、不许路径穿越
            name = os.path.basename(path[len("/pkg/"):])
            if not name or not os.path.isfile(name):
                return self._send(404, "text/plain; charset=utf-8",
                                  ("not found: %s" % name).encode("utf-8"))
            total = os.path.getsize(name)
            start, end = 0, total - 1
            partial = False
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                try:
                    spec = rng[6:].split("-", 1)
                    start = int(spec[0]) if spec[0] else 0
                    if len(spec) > 1 and spec[1]:
                        end = min(int(spec[1]), total - 1)
                    partial = True
                except ValueError:
                    pass
            if start >= total:
                self.send_error(416, "range past EOF")
                return
            return self._send_file(name, start, end, total, partial)
        self._send(404, "text/plain; charset=utf-8", b"not found")

    def do_HEAD(self):
        raw = self.path.split("?", 1)[0]
        path = urllib.parse.unquote(raw)
        if path.startswith("/pkg/"):
            name = os.path.basename(path[len("/pkg/"):])
            if not name or not os.path.isfile(name):
                self.send_error(404)
                return
            total = os.path.getsize(name)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            return
        self.send_error(404)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    cwd = os.getcwd()
    n = sum(1 for x in os.listdir(cwd)
            if x.lower().endswith(PKG_EXTS) and os.path.isfile(x))
    print("PS5 Nexus serve-pkg on port %d — %d 个 PKG — 目录: %s"
          % (PORT, n, cwd), flush=True)
    print("在 PS5 Nexus「PKG管理 → 从 PC 安装」里填: <本机IP>:%d" % PORT,
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

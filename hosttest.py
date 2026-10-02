#!/usr/bin/env python3
"""Host test for the merged owendswang-backend + NEXUS-frontend tree.

Emulates the NEXUS front-end exactly:
  * apiJson(path, obj)  -> POST application/json, body = JSON.stringify(obj)
  * apiPost(path, obj)  -> POST application/x-www-form-urlencoded
  * uploadOne()         -> POST /api/upload-file with X-WFM-* headers + raw body
and checks the response shapes the front-end actually consumes.

Run inside WSL:  python3 hosttest.py
"""
import base64
import http.client
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.parse

WORKDIR = os.path.dirname(os.path.abspath(__file__))


def _makefile_version():
    """Read VERSION_TAG out of the Makefile.

    $(LINUX_BIN) is PS5-Nexus-linux-$(VERSION_TAG), so any hard-coded name goes
    stale on every version bump. Reading it back also gives the suite a second
    thing to check: that the running binary reports the version this tree claims
    — which is what catches a stale build that silently kept its old -D.
    """
    try:
        with open(os.path.join(WORKDIR, "Makefile"), encoding="utf-8") as f:
            m = re.search(r"^VERSION_TAG\s*:?=\s*(\S+)", f.read(), re.M)
        return m.group(1) if m else None
    except OSError:
        return None


VERSION_TAG = _makefile_version()


def _host_bin():
    name = "PS5-Nexus-linux-" + VERSION_TAG if VERSION_TAG else "PS5-Nexus-linux"
    return os.path.join(WORKDIR, name)


# WFM_BIN lets a counterfactual run point the whole suite at a deliberately
# re-broken binary (see .build/nas-counterfactual-wsl.sh) without touching the
# tree. Default is the normal build.
BIN = os.environ.get("WFM_BIN") or _host_bin()
ROOT = "/tmp/ps5-nexus-hosttest"
# SMB fixture port. Must stay OUT of the Windows reserved ranges
# (`netsh interface ipv4 show excludedportrange protocol=tcp` lists 1390-1489);
# see .build/nas-fixture.sh.
NAS_PORT = int(os.environ.get("WFM_NAS_PORT", "1500"))

PASS = 0
FAIL = 0
SKIP = []
FAILED = []


def nas_port_open(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=1)
        s.close()
        return True
    except Exception:
        return False


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok   %s" % name)
    else:
        FAIL += 1
        FAILED.append(name)
        print("  FAIL %s   %s" % (name, detail))


def encode_uri_component(s):
    # Matches JS encodeURIComponent (does not escape !'()*-._~ A-Za-z0-9)
    return urllib.parse.quote(s, safe="!'()*-._~")


def wait_port(ports=range(2026, 2041), timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for p in ports:
            try:
                c = http.client.HTTPConnection("127.0.0.1", p, timeout=2)
                c.request("GET", "/api/status")
                r = c.getresponse()
                r.read()
                c.close()
                return p
            except Exception:
                pass
        time.sleep(0.2)
    return None


class Client:
    def __init__(self, port):
        self.port = port

    def raw(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        conn.request(method, path, body=body, headers=headers or {})
        r = conn.getresponse()
        data = r.read()
        st = r.status
        conn.close()
        return st, data

    def j(self, st, data):
        try:
            return json.loads(data.decode("utf-8"))
        except Exception:
            return None

    def get(self, path):
        return self.raw("GET", path)

    def get_json(self, path):
        st, d = self.raw("GET", path)
        return st, self.j(st, d)

    def api_json(self, path, obj):
        b = json.dumps(obj, ensure_ascii=False).encode("utf-8")  # JSON.stringify
        st, d = self.raw("POST", path, b, {"Content-Type": "application/json"})
        return st, self.j(st, d)

    def api_post(self, path, obj):
        b = urllib.parse.urlencode(obj).encode("utf-8")
        st, d = self.raw("POST", path, b,
                         {"Content-Type": "application/x-www-form-urlencoded"})
        return st, self.j(st, d)

    def upload(self, dst, name, data, rel=None, size=None):
        h = {
            "Content-Type": "application/octet-stream",
            "X-WFM-Dir": encode_uri_component(dst),
            "X-WFM-Name": encode_uri_component(name),
            "X-WFM-Size": str(len(data) if size is None else size),
        }
        if rel is not None:
            h["X-WFM-Rel"] = encode_uri_component(rel)
        st, d = self.raw("POST", "/api/upload-file", data, h)
        return st, self.j(st, d)

    def wait_idle(self, timeout=40):
        end = time.time() + timeout
        while time.time() < end:
            st, j = self.api_json("/api/tasks", {})
            tasks = (j or {}).get("tasks", [])
            active = [t for t in tasks
                      if t.get("state_name") in ("queued", "running", "paused")]
            if not active:
                return tasks
            time.sleep(0.2)
        return None


def list_names(c, path):
    st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": path})
    if not j or not j.get("ok"):
        return None
    return {e["name"] for e in j.get("entries", [])}


def rmtree_rw(path):
    """Remove a tree even when a fixture deliberately made parts of it read-only.

    ⚠️ Why plain `shutil.rmtree(..., ignore_errors=True)` is not enough: the save
    / snapshot fixtures chmod directories to 0555 on purpose to exercise the
    "cannot write here" paths. unlinking a child needs write permission on the
    *directory*, so rmtree fails on that child, and `ignore_errors=True` turns
    that failure into silence — the read-only tree survives the wipe. The run
    then dies ~20 lines later with a PermissionError while merely opening
    `snaps/<title>/111/sd.bin`, which reads like an unrelated product bug. That
    is a "looks healthy but never actually cleans" criterion, so: retry with the
    permission fixed, and (see main) assert the wipe really happened.
    """
    def onerror(func, p, exc):
        for target in (os.path.dirname(p), p):
            try:
                os.chmod(target, 0o700)
            except OSError:
                pass
        try:
            func(p)
        except OSError:
            pass

    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, onerror=onerror)
    else:
        os.remove(path)


def main():
    # ---- start server ----
    os.makedirs(ROOT, exist_ok=True)
    for n in os.listdir(ROOT):
        try:
            rmtree_rw(os.path.join(ROOT, n))
        except OSError as e:
            print("FATAL: cannot clear %s/%s: %s" % (ROOT, n, e), file=sys.stderr)
            return 9
    leftover = os.listdir(ROOT)
    if leftover:
        print("FATAL: %s not empty after wipe: %s" % (ROOT, leftover), file=sys.stderr)
        return 9

    save_root = os.path.join(ROOT, "saveroot")
    snap_dir = os.path.join(ROOT, "snaps")
    brew_dir = os.path.join(ROOT, "homebrew")
    # A title's live save dir + an old snapshot, so backup/restore have fixtures.
    title = "CUSA00001"
    os.makedirs(os.path.join(save_root, title, "savedata_prospero"), exist_ok=True)
    with open(os.path.join(save_root, title, "savedata_prospero", "sd.bin"), "wb") as f:
        f.write(b"LIVE-V2")
    os.makedirs(os.path.join(snap_dir, title, "111"), exist_ok=True)
    with open(os.path.join(snap_dir, title, "111", "sd.bin"), "wb") as f:
        f.write(b"OLD-V1")
    os.makedirs(brew_dir, exist_ok=True)

    # Device-root fixtures for the mount check (WFM_DEVICE_ROOT_PREFIX below).
    #   devroot/empty  -> an empty directory on the SAME filesystem as the prefix,
    #                     i.e. exactly what an unpopulated /mnt/usb0 looks like
    #   devroot/full   -> has content, so it counts as attached
    devroot = os.path.join(ROOT, "devroot")
    os.makedirs(os.path.join(devroot, "empty"), exist_ok=True)
    os.makedirs(os.path.join(devroot, "full", "games"), exist_ok=True)
    with open(os.path.join(devroot, "full", "games", "x.pkg"), "wb") as f:
        f.write(b"pkg")

    env = dict(os.environ)
    env["WFM_SAVE_ROOT"] = save_root
    env["WFM_SNAPSHOT_DIR"] = snap_dir
    env["WFM_HOMEBREW_DIR"] = brew_dir
    env["WFM_DEVICE_ROOT_PREFIX"] = devroot

    proc = subprocess.Popen([BIN], cwd=WORKDIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        port = wait_port()
        if not port:
            print("FATAL: server never came up")
            out = proc.stdout.read(4000).decode("utf-8", "replace")
            print(out)
            return 1
        print("server on port %d\n" % port)
        c = Client(port)

        # ---- 1. status ----
        st, j = c.get_json("/api/status")
        check("GET /api/status ok+port", st == 200 and j and j.get("ok")
              and j.get("port") == port, "st=%s j=%s" % (st, j))
        check("GET /api/status has http flag", bool(j and j.get("http") is True))
        # The version the running binary was compiled with must equal the one
        # the Makefile claims. This is the assertion that makes a stale build
        # (flags changed but nothing relinked) impossible to miss.
        check("GET /api/status reports the Makefile's VERSION_TAG",
              j and j.get("version") == VERSION_TAG,
              "got %r want %r" % (j.get("version") if j else None, VERSION_TAG))

        # ---- 2. tasks (GET + JSON POST) ----
        st, j = c.get_json("/api/tasks")
        check("GET /api/tasks ok+tasks[]", st == 200 and j and j.get("ok")
              and isinstance(j.get("tasks"), list), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/tasks", {})
        check("POST /api/tasks (json) ok", st == 200 and j and j.get("ok"))

        # ---- 3. list ----
        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": ROOT})
        check("POST /api/fs/list ok,path,entries", j and j.get("ok")
              and j.get("path") == ROOT and isinstance(j.get("entries"), list),
              "j=%s" % j)

        # ---- 4. space ----
        st, j = c.api_json("/api/space", {"path": ROOT})
        check("POST /api/space top-level free/total/path", j and j.get("ok")
              and isinstance(j.get("free"), int) and isinstance(j.get("total"), int)
              and j.get("path") == ROOT, "j=%s" % j)

        # ---- 5. text create/save/read (JSON, like the front-end) ----
        st, j = c.api_json("/api/text/create", {"path": ROOT, "name": "note.txt"})
        check("POST /api/text/create (json)", j and j.get("ok"), "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/note.txt"})
        check("POST /api/fs/read new file -> version+empty", j and j.get("ok")
              and "version" in j and j.get("data") == "", "j=%s" % j)
        version = (j or {}).get("version")

        body = "hello world\n"
        st, j = c.api_json("/api/text/save",
                           {"path": ROOT + "/note.txt", "content": body,
                            "version": version})
        check("POST /api/text/save (json, CAS)", j and j.get("ok"), "j=%s" % j)

        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/note.txt"})
        got = ""
        try:
            got = base64.b64decode(j["data"]).decode()
        except Exception:
            pass
        check("POST /api/fs/read round-trips content", got == body, "got=%r" % got)

        # ---- 5b. text endpoints also accept form encoding (req_param fix) ----
        st, j = c.api_post("/api/text/create", {"path": ROOT, "name": "form.txt"})
        okform = bool(j and j.get("ok"))
        if okform:
            st, j = c.api_post("/api/text/save",
                               {"path": ROOT + "/form.txt", "content": "x"})
            okform = bool(j and j.get("ok"))
        check("text create/save also accepts form encoding", okform, "j=%s" % j)

        # ---- 6. rename ----
        st, j = c.api_json("/api/rename", {"path": ROOT + "/note.txt", "name": "real.txt"})
        check("POST /api/rename ok", j and j.get("ok"), "j=%s" % j)
        names = list_names(c, ROOT) or set()
        check("rename took effect on disk", "real.txt" in names and "note.txt" not in names,
              "names=%s" % names)

        # ---- 7. mkdir + errno report ----
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": "sub"})
        check("POST /api/mkdir ok", j and j.get("ok"), "j=%s" % j)
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": "sub"})
        err = (j or {}).get("error", "")
        check("mkdir EEXIST reports a real errno, not 'Success'", j
              and not j.get("ok") and err and err.lower() != "success",
              "j=%s" % j)

        # ---- 8. copy then move (task-driven) ----
        st, j = c.api_json("/api/copy", {"paths": ROOT + "/real.txt",
                                         "dst": ROOT + "/sub", "overwrite": "0"})
        check("POST /api/copy returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        check("copy produced sub/real.txt", "real.txt" in (list_names(c, ROOT + "/sub") or set()))

        st, j = c.api_json("/api/move", {"paths": ROOT + "/real.txt",
                                         "dst": ROOT + "/sub", "overwrite": "1"})
        check("POST /api/move returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        names = list_names(c, ROOT) or set()
        check("move removed source from ROOT", "real.txt" not in names, "names=%s" % names)

        # ---- 9. upload (raw streaming, like XHR) ----
        payload = b"0123456789ABCDEF"          # 16 bytes
        st, j = c.upload(ROOT, "up.bin", payload)
        check("POST /api/upload-file (raw, 16B) ok", j and j.get("ok"), "st=%s j=%s" % (st, j))
        ok_on_disk = False
        try:
            with open(os.path.join(ROOT, "up.bin"), "rb") as f:
                ok_on_disk = f.read() == payload
        except Exception:
            pass
        check("uploaded bytes match on disk", ok_on_disk)

        # ---- 9b. folder upload (X-WFM-Rel subtree) ----
        st, j = c.upload(ROOT, "kid.txt", b"abc", rel="folder/kid.txt")
        c.wait_idle()
        check("folder upload recreates subtree",
              "kid.txt" in (list_names(c, ROOT + "/folder") or set()),
              "j=%s" % j)

        # ---- 10. chinese filename round-trip (json_escape codepoint fix) ----
        zh = "测试目录"
        st, j = c.api_json("/api/mkdir", {"path": ROOT, "name": zh})
        check("mkdir chinese name ok", j and j.get("ok"), "j=%s" % j)
        names = list_names(c, ROOT)
        check("chinese name round-trips through json_escape", names is not None
              and zh in names, "names=%s" % names)

        # ---- 11. .log read (user-reported crash case) ----
        logpath = os.path.join(ROOT, "big.log")
        logdata = b"line one\n\x00\x01\x02 binary\n" + b"x" * 5000
        with open(logpath, "wb") as f:
            f.write(logdata)
        st, j = c.api_json("/api/fs/read", {"path": ROOT + "/big.log"})
        check("read .log (with NUL bytes) ok", j and j.get("ok")
              and j.get("size") == len(logdata),
              "size=%s want=%s" % (j.get("size") if j else None, len(logdata)))

        # ---- 12. delete (task) ----
        st, j = c.api_json("/api/delete", {"paths": ROOT + "/sub/real.txt"})
        check("POST /api/delete returns task_id", j and j.get("ok")
              and j.get("task_id") is not None, "j=%s" % j)
        c.wait_idle()
        check("delete removed the file",
              "real.txt" not in (list_names(c, ROOT + "/sub") or set()))

        # ---- 13. chmod (task) ----
        st, j = c.api_json("/api/chmod", {"paths": ROOT + "/up.bin",
                                          "mode": "0755", "recursive": "0"})
        check("POST /api/chmod returns task_id", j and j.get("ok"), "j=%s" % j)
        c.wait_idle()

        # ---- 14. extract rejects a non-archive gracefully ----
        st, j = c.api_json("/api/extract", {"path": ROOT + "/up.bin"})
        check("extract non-archive -> clean error", j and not j.get("ok")
              and j.get("error"), "j=%s" % j)

        # ---- 14b. extract honours the front-end's dst_dir / subdir names ----
        # The dialog posts `dst_dir` (the 解压到 picker) + `subdir` (以压缩包名新建
        # 子目录), while this back-end used to read only `destination` / `separate`.
        # Probe: pointing dst_dir at a FILE must be rejected as
        # "destination is not a directory" — that verdict can only be reached if
        # the alias was really read.
        with open(os.path.join(ROOT, "afile.bin"), "wb") as f:
            f.write(b"x")
        with open(os.path.join(ROOT, "arch.rar"), "wb") as f:
            f.write(b"not a real rar")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/arch.rar",
                                            "dst_dir": ROOT + "/afile.bin",
                                            "subdir": "1", "conflict": "overwrite"})
        check("extract reads dst_dir (file as destination -> 409)",
              st == 409 and j and j.get("error_code") == "destination_must_be_directory",
              "st=%s j=%s" % (st, j))

        # In-process engine: a bogus archive is ACCEPTED as a task and the worker
        # fails it with a format verdict (no more 503 helper-missing).
        st, j = c.api_json("/api/extract", {"path": ROOT + "/arch.rar",
                                            "dst_dir": ROOT + "/sub", "subdir": "1"})
        bogus_id = (j or {}).get("task_id")
        check("bogus archive accepted as a task (in-process engine)",
              j and j.get("ok") and bogus_id is not None, "st=%s j=%s" % (st, j))
        settled = None
        for _ in range(80):
            st, pj = c.api_post("/api/install/poll", {"id": str(bogus_id)})
            if pj and pj.get("ok") and pj.get("state_name") in (
                    "done", "failed", "cancelled"):
                settled = pj
                break
            time.sleep(0.25)
        check("bogus archive task fails with a format verdict",
              settled is not None and settled.get("state_name") == "failed",
              "settled=%s" % (settled,))

        # ---- 14c. multi-part RAR: members collapse to the first volume ----
        mp = os.path.join(ROOT, "mpgame.part1.rar")
        with open(mp, "wb") as f:
            f.write(b"rar-part1")
        with open(os.path.join(ROOT, "mpgame.part2.rar"), "wb") as f:
            f.write(b"rar-part2")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/mpgame.part2.rar",
                                            "dst_dir": ROOT + "/sub"})
        bogus2 = (j or {}).get("task_id")
        check("extract .part2.rar alone -> collapses to part1 (task, not 400)",
              j and j.get("ok") and bogus2 is not None, "st=%s j=%s" % (st, j))
        with open(os.path.join(ROOT, "mpgame.r00"), "wb") as f:
            f.write(b"rar-legacy-volume")
        st, j = c.api_json("/api/extract", {"path": ROOT + "/mpgame.r00",
                                            "dst_dir": ROOT + "/sub"})
        check("extract legacy .r00 with .rar missing -> first-part error",
              st == 400 and j and j.get("error_code") == "archive_first_part_missing",
              "st=%s j=%s" % (st, j))

        # ---- 14d. REAL in-process extraction: zip / rar / 7z / volumes ----
        # Fixtures come from ps5-nexus-legacy's engine suite (tests/fixtures/).
        # These are the "host-tested leaves" the helper design could never
        # exercise end-to-end: the unpack runs inside THIS linux binary.
        import shutil
        fx = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "tests", "fixtures")

        def fx_extract(names, dst_name, password=None):
            """Copy fixture(s) into the sandbox, enqueue, wait for the task."""
            if isinstance(names, str):
                names = [names]
            for n in names:
                shutil.copyfile(os.path.join(fx, n), os.path.join(ROOT, n))
            dst = os.path.join(ROOT, dst_name)
            os.makedirs(dst, exist_ok=True)
            body = {"path": os.path.join(ROOT, names[0]), "dst_dir": dst,
                    "conflict": "overwrite", "subdir": "0"}
            if password:
                body["password"] = password
            st, j = c.api_json("/api/extract", body)
            tid = (j or {}).get("task_id")
            if tid is None:
                return st, j, None
            for _ in range(240):
                st, pj = c.api_post("/api/install/poll", {"id": str(tid)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    return st, j, pj
                time.sleep(0.25)
            return st, j, None

        # ZIP (minizip-ng + zlib): content-asserted
        st, j, pj = fx_extract("basic.zip", "xz-basic")
        ok = pj is not None and pj.get("state_name") == "done"
        root_txt = nested_txt = None
        if ok:
            root_txt = open(os.path.join(ROOT, "xz-basic", "root.txt"), "rb").read()
            nested_txt = open(os.path.join(ROOT, "xz-basic", "dir", "nested.txt"), "rb").read()
        check("real zip unpack: root.txt content matches",
              bool(ok and root_txt == b"root content"), "got=%r pj=%s" % (root_txt, pj))
        check("real zip unpack: nested dirs created",
              bool(ok and nested_txt == b"nested content"), "got=%r" % (nested_txt,))

        # RAR (unrar7, C++, RARDLL mode)
        st, j, pj = fx_extract("basic-v6.rar", "xz-rar")
        ok = pj is not None and pj.get("state_name") == "done"
        got = open(os.path.join(ROOT, "xz-rar", "root.txt"), "rb").read() if ok else None
        # fixture's root.txt carries a banner line before the payload text
        check("real rar unpack (unrar7 in-process)",
              bool(ok and got is not None and
                   b"rar v1.9 fixture root content" in got),
              "got=%r pj=%s" % (got, pj))

        # 7z (LZMA SDK, incl. -maes AesOpt TUs)
        st, j, pj = fx_extract("lzma.7z", "xz-7z")
        ok = pj is not None and pj.get("state_name") == "done"
        got = os.path.isfile(os.path.join(ROOT, "xz-7z", "_src", "readme.txt")) if ok else False
        check("real 7z unpack (lzma)", bool(ok and got), "pj=%s" % (pj,))

        # Encrypted: without a password -> archive_password_required; with the
        # right one -> done. Covers PKCRYPT (ZipCrypto) and RAR5 AES-256.
        st, j, pj = fx_extract("enc-zipcrypto.zip", "xz-enc-zc")
        check("zipcrypto zip without password -> password_required failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") == "archive_password_required",
              "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-zipcrypto.zip", "xz-enc-zc2", password="secret123")
        check("zipcrypto zip with password -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-v6.rar", "xz-enc-rar")
        check("encrypted rar without password -> password_required failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") == "archive_password_required",
              "pj=%s" % (pj,))
        st, j, pj = fx_extract("enc-v6.rar", "xz-enc-rar2", password="secret123")
        check("encrypted rar with password -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))

        # Volume sets: part1 alone works; selecting ANY later member also works
        # because the entry layer collapses members to the first volume; a
        # missing first volume fails loudly instead of hanging.
        st, j, pj = fx_extract(["vol.part1.rar", "vol.part2.rar", "vol.part3.rar"],
                               "xz-vol1")
        check("rar volume set from part1 -> done",
              pj is not None and pj.get("state_name") == "done", "pj=%s" % (pj,))
        st, j, pj = fx_extract(["vol.part1.rar", "vol.part2.rar", "vol.part3.rar"],
                               "xz-vol2")
        # enqueue from part2 only: same outcome, the collapse already ran
        body = {"path": os.path.join(ROOT, "vol.part2.rar"),
                "dst_dir": os.path.join(ROOT, "xz-vol3"),
                "conflict": "overwrite", "subdir": "0"}
        os.makedirs(os.path.join(ROOT, "xz-vol3"), exist_ok=True)
        st, j = c.api_json("/api/extract", body)
        tid = (j or {}).get("task_id")
        pj2 = None
        if tid is not None:
            for _ in range(240):
                st, pj2 = c.api_post("/api/install/poll", {"id": str(tid)})
                if pj2 and pj2.get("ok") and pj2.get("state_name") in (
                        "done", "failed", "cancelled"):
                    break
                time.sleep(0.25)
        check("rar volume set enqueued from part2 -> still done (collapse)",
              pj2 is not None and pj2.get("state_name") == "done", "pj2=%s" % (pj2,))
        st, j, pj = fx_extract("broken.zip.001", "xz-broken")
        check("incomplete zip volume set -> missing-volume failure",
              pj is not None and pj.get("state_name") == "failed"
              and pj.get("error_code") in ("archive_missing_volume",
                                           "archive_extract_failed"),
              "pj=%s" % (pj,))

        # ---- 15. NEXUS-only stubs must be graceful ----
        st, j = c.api_post("/api/save/list", {"title_id": "CUSA00000"})
        check("POST /api/save/list ok (empty)", j and j.get("ok"), "j=%s" % j)
        st, j = c.api_post("/api/pkg/scan", {"root": ROOT})
        check("POST /api/pkg/scan ok (empty)", j and j.get("ok"), "j=%s" % j)
        st, j = c.api_post("/api/app/register", {})
        # PS5-only; on the host it must be a clean, routed error (never 404/405).
        check("POST /api/app/register routed (PS5-only on host)",
              st == 501 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        # ---- fetch: a REAL NAS pull (it was a 501 stub until 2026-10-01) ----
        st, j = c.api_post("/api/fetch", {"src": "127.0.0.1/share/x.pkg",
                                          "dst": ROOT, "scheme": "ftp"})
        check("POST /api/fetch with a backend we do not have -> 501",
              st == 501 and j and not j.get("ok")
              and j.get("error_code") == "fetch_unsupported", "st=%s j=%s" % (st, j))

        st, j = c.api_post("/api/fetch", {"src": "127.0.0.1/share/game.pkg",
                                          "dst": ROOT, "scheme": "smb"})
        fetch_id = (j or {}).get("task_id")
        check("POST /api/fetch queues a real pull task", j and j.get("ok")
              and fetch_id is not None, "st=%s j=%s" % (st, j))
        if fetch_id is not None:
            # Nothing listens on 127.0.0.1:445 here, so the worker must fail FAST
            # and settle. A queued task that never terminates would wedge the
            # single-active-task queue and block every later operation.
            # /api/install/poll is used (not /api/tasks) because it looks a task up
            # by id directly — /api/tasks removes finished rows on the next call.
            settled = None
            for _ in range(80):
                st, pj = c.api_post("/api/install/poll", {"id": str(fetch_id)})
                if pj and pj.get("ok") and pj.get("state_name") in (
                        "done", "failed", "cancelled"):
                    settled = pj
                    break
                time.sleep(0.25)
            check("fetch task settles (fails) instead of hanging",
                  settled is not None and settled.get("state_name") == "failed",
                  "settled=%s" % settled)
        c.wait_idle()
        st, j = c.api_post("/api/install/poll", {"id": "99999"})
        check("POST /api/install/poll unknown id -> not found json",
              st == 404 and j and not j.get("ok"), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/task/cancel", {"id": 99999})
        check("POST /api/task/cancel unknown id -> error json", j and not j.get("ok"),
              "st=%s j=%s" % (st, j))

        # ---- 15b. status returns a real address ----
        st, j = c.get_json("/api/status")
        check("status reports a non-empty addr", j and j.get("addr"),
              "addr=%r" % (j.get("addr") if j else None))

        # ---- 15c. PKG library scan / enqueue / poll ----
        pkgdir = os.path.join(ROOT, "pkgs")
        os.makedirs(os.path.join(pkgdir, "deep"), exist_ok=True)
        with open(os.path.join(pkgdir, "game.pkg"), "wb") as f:
            f.write(b"not-a-real-pkg")           # meta parse fails -> stem fallback
        with open(os.path.join(pkgdir, "deep", "image.ffpkg"), "wb") as f:
            f.write(b"FPKG-IMAGE-BYTES")

        st, j = c.api_post("/api/pkg/scan", {"root": pkgdir})
        pkgs = (j or {}).get("packages", [])
        by_path = {p["path"]: p for p in pkgs}
        gp = by_path.get(os.path.join(pkgdir, "game.pkg"))
        ip = by_path.get(os.path.join(pkgdir, "deep", "image.ffpkg"))
        check("pkg/scan finds .pkg recursively", j and j.get("ok") and gp is not None,
              "j=%s" % j)
        check("pkg/scan labels .pkg by stem fallback", gp is not None
              and gp.get("title") == "game" and gp.get("is_fpkg") is False,
              "gp=%s" % gp)
        check("pkg/scan marks .ffpkg as fPKG image", ip is not None
              and ip.get("is_fpkg") is True and ip.get("title") == "image",
              "ip=%s" % ip)

        st, j = c.api_post("/api/pkg/enqueue", {"path": ip["path"] if ip else "",
                                                "title": "image", "version": ""})
        tid = (j or {}).get("task_id")
        check("pkg/enqueue (fPKG) returns task_id", j and j.get("ok") and tid is not None,
              "j=%s" % j)
        # poll until terminal, then confirm the image landed in homebrew
        polled = None
        for _ in range(60):
            st, pj = c.api_post("/api/install/poll", {"id": str(tid)})
            if not pj or not pj.get("ok"):
                break
            polled = pj
            if pj.get("state_name") in ("done", "failed", "cancelled"):
                break
            time.sleep(0.2)
        check("install/poll reports state_name/progress/total",
              polled and polled.get("state_name") and "progress" in polled
              and "total" in polled, "polled=%s" % polled)
        check("install/poll reached done", polled and polled.get("state_name") == "done",
              "polled=%s" % polled)
        check("fPKG image copied into homebrew",
              os.path.exists(os.path.join(brew_dir, "image.ffpkg")))
        c.wait_idle()

        # ---- 15d. /api/pkg/install-url (PC 网络直装入队) ----
        st, j = c.api_post("/api/pkg/install-url", {"url": "ftp://pc/game.pkg"})
        check("pkg/install-url rejects non-http scheme (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/install-url", {"url": "http://"})
        check("pkg/install-url rejects empty url (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_post("/api/pkg/install-url",
                           {"url": "http://" + "9" * 520 + "/x.pkg"})
        check("pkg/install-url rejects oversized url (400)",
              st == 400 and j and j.get("error_code") == "pkg_url_invalid",
              "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/pkg/install-url",
                           {"url": "http://192.168.1.10:9898/pkg/game.pkg"})
        # 宿主（__linux__）没有安装层 ⇒ 501；真机才会入队返回 task_id
        check("pkg/install-url valid url -> 501 on host (pkg_install_unsupported)",
              st == 501 and j and j.get("error_code") == "pkg_install_unsupported",
              "st=%s j=%s" % (st, j))
        st, _ = c.raw("GET", "/api/pkg/install-url")
        check("pkg/install-url GET -> 405", st == 405, "st=%s" % st)

        # ---- 15d. save domain: list / backup / restore ----
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps = (j or {}).get("snapshots", [])
        check("save/list returns existing snapshots", j and j.get("ok") and "111" in snaps,
              "j=%s" % j)

        st, j = c.api_post("/api/save/backup", {"title_id": title})
        bk_id = (j or {}).get("task_id")
        check("save/backup returns task_id", j and j.get("ok") and bk_id is not None,
              "j=%s" % j)
        c.wait_idle()
        st, j = c.api_post("/api/save/list", {"title_id": title})
        snaps2 = (j or {}).get("snapshots", [])
        new_ids = [s for s in snaps2 if s not in snaps]
        check("save/backup created a new snapshot", len(new_ids) == 1,
              "before=%s after=%s" % (snaps, snaps2))
        if new_ids:
            snap_file = os.path.join(snap_dir, title, new_ids[0], "sd.bin")
            ok_snap = False
            try:
                with open(snap_file, "rb") as f:
                    ok_snap = f.read() == b"LIVE-V2"
            except Exception:
                pass
            check("snapshot content equals the live save", ok_snap)

        # restore the OLD snapshot over the live dir, then verify the live bytes went back
        st, j = c.api_post("/api/save/restore", {"title_id": title, "snapshot": "111"})
        rs_id = (j or {}).get("task_id")
        check("save/restore returns task_id", j and j.get("ok") and rs_id is not None,
              "j=%s" % j)
        c.wait_idle()
        live_file = os.path.join(save_root, title, "savedata_prospero", "sd.bin")
        ok_restore = False
        try:
            with open(live_file, "rb") as f:
                ok_restore = f.read() == b"OLD-V1"
        except Exception:
            pass
        check("restore wrote the snapshot back over the live save", ok_restore)

        # backup of an unknown title is a clean 404, not a hang
        st, j = c.api_post("/api/save/backup", {"title_id": "CUSA99999"})
        check("save/backup unknown title -> not found", st == 404 and j
              and not j.get("ok"), "st=%s j=%s" % (st, j))

        # ---- 15e. pkg info via form POST (front-end style) must not be 405 ----
        st, j = c.api_post("/api/pkg/info", {"path": ROOT + "/pkgs/game.pkg"})
        check("POST /api/pkg/info (form) is routed, not 405", st != 405,
              "st=%s" % st)

        # ---- 15f. request ledger (/api/diag) ----
        st, j = c.get_json("/api/diag")
        reqs = (j or {}).get("requests", [])
        check("GET /api/diag returns a request ledger", j and j.get("ok")
              and isinstance(reqs, list) and len(reqs) > 0, "j=%s" % j)
        check("ledger records completed requests with done=true",
              any(r.get("url") == "/api/status" and r.get("done") is True
                  for r in reqs),
              "sample=%s" % reqs[:3])

        # ---- 15g. NAS browsing: scheme routing on /api/fs/list ----
        # The reported bug: clicking a NAS bookmark sent {scheme:"smb",
        # path:"nas.lan/games"} and the back-end ran opendir() on that literal
        # string -> ENOENT -> "No such file or directory". The guard is that a
        # non-local scheme is routed to the NAS backend and the failure names
        # something the user can act on.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb",
                                            "path": "nonexistent.invalid/games"})
        err = (j or {}).get("error", "")
        check("NAS list is routed to the NAS backend (not local ENOENT)",
              j and not j.get("ok") and "No such file or directory" not in err,
              "st=%s j=%s" % (st, j))
        check("NAS list failure carries nas_list_failed",
              j and j.get("error_code") == "nas_list_failed", "j=%s" % j)

        # Unreachable-but-fast: nothing listens on 127.0.0.1:445.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1/share"})
        check("unreachable SMB -> 502 nas_list_failed",
              st == 502 and j and not j.get("ok")
              and j.get("error_code") == "nas_list_failed", "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "nfs", "path": "127.0.0.1/export"})
        check("unreachable NFS -> 502 nas_list_failed",
              st == 502 and j and not j.get("ok")
              and j.get("error_code") == "nas_list_failed", "st=%s j=%s" % (st, j))

        # A malformed address (non-numeric port) must say so, not "not found".
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1:abc/share"})
        check("malformed NAS address -> nas_bad_address",
              j and not j.get("ok") and j.get("error_code") == "nas_bad_address",
              "st=%s j=%s" % (st, j))

        # Share-less SMB address: rejected at the door with an actionable
        # message. Without this guard libsmb2 connects to IPC$ instead and many
        # NAS/Samba boxes answer the root listing by dropping the TCP
        # connection, which surfaces as CONNECTION_REFUSED and sends the user
        # off to check the network while the real problem is the address.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1"})
        check("SMB address without share name -> nas_bad_address naming the share",
              st == 400 and j and not j.get("ok")
              and j.get("error_code") == "nas_bad_address"
              and "共享名" in (j or {}).get("error", ""), "st=%s j=%s" % (st, j))
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "\\192.168.1.3\\"})
        check("SMB share-less UNC form also rejected as nas_bad_address",
              st == 400 and j and not j.get("ok")
              and j.get("error_code") == "nas_bad_address", "st=%s j=%s" % (st, j))

        # Credentials / port are spliced into the URL, never rejected as bad input.
        st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": "127.0.0.1/share",
                                            "user": "guest", "pass": "x", "port": "1500"})
        check("NAS list with credentials+port still reaches the backend",
              j and not j.get("ok") and j.get("error_code") == "nas_list_failed",
              "st=%s j=%s" % (st, j))

        # ---- 15g-bis. 反斜杠是路径分隔符，不是主机名的一部分 ----
        # 用户实测（2026-10-01）：地址栏填 Windows 形状的
        # "192.168.1.3\PS5_Games" ⇒
        #   NAS 连接失败：Invalid address:192.168.1.3\PS5_Games
        #                 Can not resolve into IPv4/v6.
        # 即整串（含反斜杠）被当成主机名交给了 getaddrinfo。
        #
        # 判据是**两种失败的形状不同**：解析阶段失败 ⇒ nas_bad_address；
        # 到了后端才失败 ⇒ nas_list_failed。修复前是后者**且消息里带
        # "Invalid address" / "Can not resolve"**；修复后仍然是后者，
        # 但主机名已经干净（127.0.0.1 秒拒，不走 DNS，所以断言离线且快）。
        # 故意不打 192.168.1.3：真去连一个不存在的内网地址会挂到超时。
        for label, naspath in (
            ("host\\share", "127.0.0.1\\PS5_Games"),
            ("UNC \\\\host\\share", "\\\\127.0.0.1\\PS5_Games"),
            ("forward-slash control", "127.0.0.1/PS5_Games"),
        ):
            st, j = c.api_json("/api/fs/list", {"scheme": "smb", "path": naspath})
            msg = (j or {}).get("error", "") or ""
            check("NAS %s -> backslash is a separator, host not the whole string" % label,
                  j and not j.get("ok")
                  and j.get("error_code") == "nas_list_failed"
                  and "Can not resolve" not in msg and "Invalid address" not in msg,
                  "path=%r st=%s j=%s" % (naspath, st, j))

        # ---- 15h. NAS end-to-end against a real SMB2 server ----
        # Nothing above proves a share can actually be listed; it only proves the
        # failure is NAS-flavoured. This block points the client at a real smbd
        # (started by .build/nas-fixture.sh) and asserts the entries come back.
        if nas_port_open(NAS_PORT):
            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/games",
                                "port": str(NAS_PORT)})
            ents = (j or {}).get("entries", []) if j else []
            names = {e["name"] for e in ents}
            check("NAS smb: lists a real share", j and j.get("ok")
                  and names == {"readme.txt", "subdir"}, "st=%s j=%s" % (st, j))
            isdir = {e["name"]: e.get("is_dir") for e in ents}
            check("NAS smb: subdir marked is_dir", isdir.get("subdir") is True,
                  "isdir=%s" % isdir)
            sizes = {e["name"]: e.get("size") for e in ents}
            check("NAS smb: file size carried through",
                  sizes.get("readme.txt") == len(b"hello from nas\n"),
                  "sizes=%s" % sizes)

            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/games/subdir",
                                "port": str(NAS_PORT)})
            names = {e["name"] for e in ((j or {}).get("entries", []) if j else [])}
            check("NAS smb: descends into a subdirectory", j and j.get("ok")
                  and names == {"inner.txt"}, "st=%s j=%s" % (st, j))

            st, j = c.api_json("/api/fs/list",
                               {"scheme": "smb", "path": "127.0.0.1/nosuchshare",
                                "port": str(NAS_PORT)})
            check("NAS smb: unknown share -> clean backend error, not a crash",
                  j and not j.get("ok") and j.get("error_code") == "nas_list_failed",
                  "st=%s j=%s" % (st, j))
        else:
            SKIP.append("NAS end-to-end (nothing listening on 127.0.0.1:%d; "
                        "run .build/nas-fixture.sh as root first)" % NAS_PORT)

        # Local scheme unchanged (regression guard for the owendswang path).
        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": "/nonexistent-xyz"})
        check("local scheme missing path -> 404 (unchanged)",
              st == 404 and j and not j.get("ok"), "st=%s j=%s" % (st, j))

        # ---- 15i. device roots: an unpopulated mount point is not "attached" ----
        # PS5 pre-creates /mnt/usb0, /mnt/usb1 and /mnt/ext1 as empty directories,
        # so listing them SUCCEEDS with nothing plugged in — and the front-end's
        # probeRoots shows a shortcut for a drive that is not there. That is the
        # reported bug. WFM_DEVICE_ROOT_PREFIX points the check at devroot/ so both
        # branches are testable without a console.
        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "empty")})
        check("unmounted device root -> 404 device_not_mounted",
              st == 404 and j and not j.get("ok")
              and j.get("error_code") == "device_not_mounted", "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "full")})
        check("populated device root still lists",
              j and j.get("ok")
              and {e["name"] for e in j.get("entries", [])} == {"games"},
              "st=%s j=%s" % (st, j))

        # Anything deeper is an ordinary directory INSIDE a device: never gated,
        # otherwise browsing an attached drive would break.
        st, j = c.api_json("/api/fs/list", {"scheme": "local",
                                            "path": os.path.join(devroot, "full", "games")})
        check("paths deeper than a device root are never gated",
              j and j.get("ok"), "st=%s j=%s" % (st, j))

        st, j = c.api_json("/api/fs/list", {"scheme": "local", "path": devroot})
        check("the prefix directory itself is never gated",
              j and j.get("ok"), "st=%s j=%s" % (st, j))

        # ---- 16. still alive after all of it ----
        st, j = c.get_json("/api/status")
        check("server still alive after full sweep", st == 200 and j and j.get("ok"))

        # ---- 17. shutdown last ----
        st, j = c.api_json("/api/shutdown", {})
        check("POST /api/shutdown ok", j and j.get("ok"), "st=%s j=%s" % (st, j))
        gone = False
        for _ in range(30):
            try:
                c.get_json("/api/status")
                time.sleep(0.2)
            except Exception:
                gone = True
                break
        check("server exited after shutdown", gone or proc.poll() is not None)

    finally:
        try:
            proc.send_signal(signal.SIGTERM)
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    print("\n%d passed, %d failed" % (PASS, FAIL))
    if SKIP:
        print("SKIPPED (fixture missing — this is NOT a pass):")
        for s in SKIP:
            print("  - " + s)
    if FAILED:
        print("failed: " + ", ".join(FAILED))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())

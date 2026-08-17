# ---------------------------------------------------------------------------
#  revsynth_server.py -- revsynth local web UI
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  python3 scripts/revsynth_server.py [port]
#  Then open http://localhost:8765 (default port) in a browser -- do NOT
#  open revsynth.html directly as a file; the page must be served so its
#  requests reach this process. Binds 127.0.0.1 only. Dependencies:
#  python3 + matplotlib.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""revsynth local web UI.

    python3 scripts/revsynth_server.py [port]

Then open  http://localhost:8765  (default port) in a browser -- do NOT open
revsynth.html directly as a file; the page must be served so its requests reach
this process. Binds 127.0.0.1 only. Dependencies: python3 + matplotlib.

Asynchronous protocol (Safari-safe: no request outlives a poll interval):
    POST /synth  {filename, content_b64, mode, out_format, lut_k}
        -> {job: <id>}            (synthesis runs in a background thread)
    GET  /result?job=<id>
        -> {state: "running"} | {state: "done", ...full result...}
           | {state: "error", error: ...}
"""
import sys, os, io, json, base64, tempfile, contextlib, traceback
import threading, uuid, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

import revsynth

HTML_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "revsynth.html")
JOBS = {}
JOBS_LOCK = threading.Lock()


def do_synth(req):
    fname = os.path.basename(req.get("filename", "circuit.v"))
    data = base64.b64decode(req["content_b64"])
    mode = req.get("mode", "auto")
    fmt = req.get("out_format", "real")
    lut_k = int(req.get("lut_k", 10))
    recompute = int(req.get("recompute", 0))
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, fname)
        with open(inp, "wb") as f:
            f.write(data)
        base = os.path.splitext(fname)[0]
        out = os.path.join(td, base + "_rev." + fmt)
        pdf = os.path.join(td, base + "_rev.pdf")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ckt, stats = revsynth.run(inp, out=out, pdf=pdf, mode=mode,
                                      lut_k=lut_k, recompute=recompute)
        svgs = revsynth.circuit_svgs(ckt, base, gates_per_page=60, max_pages=24)
        return dict(ok=True, log=buf.getvalue(),
                    stats={str(k): str(v) for k, v in stats.items()},
                    netlist_name=os.path.basename(out),
                    netlist_text=open(out).read(),
                    svgs=svgs,
                    pdf_name=os.path.basename(pdf),
                    pdf_b64=base64.b64encode(open(pdf, "rb").read()).decode())


def worker(job_id, req):
    t0 = time.time()
    print(f"[job {job_id[:8]}] start: {req.get('filename')} "
          f"mode={req.get('mode')}", file=sys.stderr)
    try:
        res = do_synth(req)
        res["state"] = "done"
    except SystemExit as e:
        res = dict(state="error", error=str(e))
    except Exception:
        res = dict(state="error", error=traceback.format_exc(limit=6))
    with JOBS_LOCK:
        JOBS[job_id] = res
    print(f"[job {job_id[:8]}] {res['state']} in {time.time()-t0:.1f}s",
          file=sys.stderr)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            self._send(200, open(HTML_PATH, "rb").read(), "text/html")
        elif u.path == "/result":
            job = parse_qs(u.query).get("job", [""])[0]
            with JOBS_LOCK:
                res = JOBS.get(job)
            if res is None:
                self._send(200, json.dumps(dict(state="unknown")).encode())
            elif res == "running":
                self._send(200, json.dumps(dict(state="running")).encode())
            else:
                self._send(200, json.dumps(res).encode())
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path != "/synth":
            self._send(404, b"{}")
            return
        try:
            n = int(self.headers["Content-Length"])
            req = json.loads(self.rfile.read(n))
            job_id = uuid.uuid4().hex
            with JOBS_LOCK:
                JOBS[job_id] = "running"
            threading.Thread(target=worker, args=(job_id, req),
                             daemon=True).start()
            self._send(200, json.dumps(dict(job=job_id)).encode())
        except Exception:
            self._send(200, json.dumps(
                dict(state="error", error=traceback.format_exc(limit=4))).encode())


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    print(f"revsynth {revsynth.__version__} UI:  http://localhost:{port}   (Ctrl-C to stop)")
    print("open that URL in the browser; do NOT open revsynth.html as a file")
    print("dependencies: python3 + matplotlib (pip3 install matplotlib)")
    srv.serve_forever()


if __name__ == "__main__":
    main()

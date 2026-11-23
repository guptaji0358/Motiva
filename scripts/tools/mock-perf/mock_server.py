"""Local mock of the performance-intelligence backend (DEV ONLY, loopback).
  python mock_server.py --port 8765 --profile valid_v1.json --records out.jsonl [--delay 0] [--fail-upload]
GET /v1/profile -> serves the profile file with an ETag (304 on If-None-Match, 404 if none)
POST /v1/records -> appends the received records to --records (500 with --fail-upload)"""
import argparse, hashlib, json, time
from http.server import BaseHTTPRequestHandler, HTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8765)
ap.add_argument("--profile")
ap.add_argument("--records", default="records.jsonl")
ap.add_argument("--delay", type=float, default=0.0)
ap.add_argument("--fail-upload", action="store_true")
A = ap.parse_args()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        time.sleep(A.delay)
        if self.path != "/v1/profile" or not A.profile:
            self.send_response(404); self.end_headers(); return
        body = open(A.profile, "rb").read()
        etag = '"%s"' % hashlib.sha256(body).hexdigest()[:16]
        if self.headers.get("If-None-Match") == etag:
            self.send_response(304); self.end_headers(); return
        self.send_response(200); self.send_header("ETag", etag)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        time.sleep(A.delay)
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if A.fail_upload or self.path != "/v1/records":
            self.send_response(500); self.end_headers(); return
        with open(A.records, "ab") as f:
            for r in json.loads(data).get("records", []): f.write(json.dumps(r).encode() + b"\n")
        self.send_response(204); self.end_headers()

HTTPServer(("127.0.0.1", A.port), H).serve_forever()

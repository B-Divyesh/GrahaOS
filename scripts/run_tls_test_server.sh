#!/usr/bin/env bash
# scripts/run_tls_test_server.sh — Phase 29 Session B (FU28.A).
#
# Local TLS test server consumed by user/tests/libtls_handshake.tap.
# Listens on 127.0.0.1:8443 with a self-signed certificate.  GrahaOS's
# libtls test treats this cert as a trust anchor (the gate runs offline,
# no public CA path required).
#
# Usage:
#   scripts/run_tls_test_server.sh &           # background
#   PID=$!
#   ...
#   kill $PID
#
# Implementation: stdlib http.server + ssl.SSLContext.  The hostname
# 'localhost' matches the certificate's CN so SNI verification succeeds.

set -euo pipefail

PORT="${TLS_PORT:-8443}"
CERT_DIR="$(mktemp -d)"
CERT="$CERT_DIR/cert.pem"
KEY="$CERT_DIR/key.pem"

cleanup() { rm -rf "$CERT_DIR"; }
trap cleanup EXIT

# Generate a fresh self-signed cert (RSA-2048, 1-day validity).
openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
    -subj "/CN=localhost/O=GrahaOS-Test/C=US" \
    -keyout "$KEY" -out "$CERT" >/dev/null 2>&1

python3 - "$PORT" "$CERT" "$KEY" <<'PY'
import http.server, ssl, sys
port = int(sys.argv[1]); cert = sys.argv[2]; key = sys.argv[3]

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass  # quiet
    def do_GET(self):
        body = b"hello from GrahaOS TLS test server\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        _ = self.rfile.read(n) if n else b""
        body = b"echo from GrahaOS TLS test server\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

httpd = http.server.HTTPServer(("127.0.0.1", port), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
print("TLS test server listening on 127.0.0.1:%d" % port, file=sys.stderr)
httpd.serve_forever()
PY

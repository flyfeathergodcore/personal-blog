#!/usr/bin/env bash
# Usage: tcp_http_smoke.sh <tcp_server_bin> <port> [tcp_server options]
set -euo pipefail

BIN=$1
PORT=$2
shift 2

"$BIN" -p "$PORT" -m http "$@" > /tmp/webcpp_tcp_http_server.log 2>&1 &
PID=$!
cleanup() {
    kill -TERM "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
}
trap cleanup EXIT

sleep 0.3
python3 - "$PORT" <<'PYEOF'
import socket
import sys

request = (
    b"GET /benchmark HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Connection: keep-alive\r\n"
    b"\r\n"
)


def receive_response(stream):
    buffer = bytearray()
    while b"\r\n\r\n" not in buffer:
        chunk = stream.recv(4096)
        assert chunk, "connection closed before HTTP headers"
        buffer.extend(chunk)
    header_end = buffer.index(b"\r\n\r\n") + 4
    headers = bytes(buffer[:header_end])
    body = bytearray(buffer[header_end:])
    assert headers.startswith(b"HTTP/1.1 200 OK\r\n"), headers
    assert b"Content-Length: 2\r\n" in headers, headers
    while len(body) < 2:
        chunk = stream.recv(4096)
        assert chunk, "connection closed before HTTP body"
        body.extend(chunk)
    assert body[:2] == b"OK", body


with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3) as stream:
    stream.sendall(request)
    receive_response(stream)
    stream.sendall(request)
    receive_response(stream)

print("tcp http keep-alive: OK")
PYEOF

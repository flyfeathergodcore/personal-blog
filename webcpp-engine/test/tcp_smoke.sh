#!/usr/bin/env bash
# Usage: tcp_smoke.sh <tcp_server_bin> <port>
set -euo pipefail

BIN=$1
PORT=$2
shift 2

"$BIN" -p "$PORT" "$@" > /tmp/webcpp_tcp_server.log 2>&1 &
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

with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3) as stream:
    payload = b"tcp-echo-check"
    stream.sendall(payload)
    received = stream.recv(len(payload))
    assert received == payload, (received, payload)
print("tcp echo: OK")
PYEOF

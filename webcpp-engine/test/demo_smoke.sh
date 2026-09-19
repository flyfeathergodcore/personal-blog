#!/usr/bin/env bash
# 用法：./test/demo_smoke.sh <http_server_bin> <port> <cert> <key>
#
# H2（Task 10）已接入：curl --http2 经 ALPN 协商 h2。h1 用例显式加
# --http1.1 强制走 HTTP/1.1；新增 h2 用例（curl 支持 --http2 时跑，
# 否则打印 SKIP 不记 FAIL）。
#
# 反向代理用例：ReverseProxy 原样转发完整路径（不做前缀剥离），故上游
# python http.server 需能在 /api/ 下返回目录列表页——我们在临时目录下
# 建一个 api/ 子目录作为 http.server 根，/api/ 即命中该目录的列表页。
set -e
# 无论从何处调用，均回到仓库根目录，使相对路径（./www、build/demo_server、test/certs）稳定
cd "$(dirname "$0")/.."
BIN=$1; PORT=$2; CERT=$3; KEY=$4

# 清掉可能残留的旧 demo_server：SO_REUSEPORT 下新旧进程可同时绑同一端口，
# 旧进程（无 /api/ 路由）会分走连接导致代理用例 404 抖动。强制清理保证确定性。
if [ -n "$BIN" ] && [ -x "$BIN" ]; then
    # 只匹配 demo 服务器自身的调用（"$BIN -c …"），避免误杀当前脚本进程
    # （脚本 cmdline 含 "build/demo_server 8443 …"，不含 " -c "）。
    pkill -9 -f "$BIN -c " 2>/dev/null || true
    pkill -9 -f "http.server 8080" 2>/dev/null || true   # 清残留 python 上游
    sleep 0.3
fi

# ── 启动代理上游（python http.server，根为带 api/ 子目录的临时目录）──
UPSTREAM_ROOT=/tmp/webcpp_proxy_upstream
rm -rf "$UPSTREAM_ROOT"
mkdir -p "$UPSTREAM_ROOT/api"
echo "hello-from-upstream" > "$UPSTREAM_ROOT/api/hello.txt"
(cd "$UPSTREAM_ROOT" && exec python3 -m http.server 8080 --bind 127.0.0.1) \
    &> /tmp/webcpp_proxy_upstream.log & UPSTREAM_PID=$!
# 等待上游就绪：最多 20 次，每次 0.5s
upstream_ready=0
for _ in $(seq 1 20); do
    if curl -s -o /dev/null --max-time 1 http://127.0.0.1:8080/ 2>/dev/null; then
        upstream_ready=1
        break
    fi
    sleep 0.5
done
if [ "$upstream_ready" != "1" ]; then
    echo "UPSTREAM READY FAIL"
    kill $UPSTREAM_PID 2>/dev/null || true
    exit 1
fi
echo "upstream: ready"

cleanup() {
    [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null || true
    [ -n "${UPSTREAM_PID:-}" ] && kill "$UPSTREAM_PID" 2>/dev/null || true
}

cat > /tmp/demo_cfg.yaml <<EOF
host: 127.0.0.1
port: 0
tls_port: $PORT
threads: 2
doc_root: ./www
tls_cert: $CERT
tls_key: $KEY
proxy:
  - prefix: /api/
    upstreams:
      - "127.0.0.1:8080"
log:
  dir: /tmp/webcpp_demo_log
  level: info
EOF
$BIN -c /tmp/demo_cfg.yaml & PID=$!
sleep 1
# h1 普通请求
CODE=$(curl -sk --http1.1 -o /tmp/demo_body -w '%{http_code}' "https://127.0.0.1:$PORT/")
echo "h1: $CODE"
[ "$CODE" = "200" ] || { echo "H1 FAIL"; cleanup; exit 1; }

# h2 普通请求（Task 10：curl --http2 经 ALPN 协商 h2；若本机 curl 无 --http2 打印 SKIP）
if curl --help all 2>/dev/null | grep -q -- "--http2"; then
    H2_CODE=$(curl -sk --http2 -o /tmp/h2body -w '%{http_code}' "https://127.0.0.1:$PORT/")
    echo "h2: $H2_CODE"
    [ "$H2_CODE" = "200" ] || { echo "H2 FAIL"; cleanup; exit 1; }
    grep -q "webcpp" /tmp/h2body || { echo "H2 FAIL (body)"; cleanup; exit 1; }
else
    echo "h2: SKIP (curl no --http2)"
fi
# SSE
timeout 3 curl -sk --http1.1 -N "https://127.0.0.1:$PORT/sse" | grep -q "tick=" || { echo "SSE FAIL"; cleanup; exit 1; }
# health / metrics
curl -sk --http1.1 "https://127.0.0.1:$PORT/health" | grep -q '"ok"' || { echo "HEALTH FAIL"; cleanup; exit 1; }
# ── 反向代理用例：/api/ 应返回上游 python http.server 目录列表页 ──
PROXY_CODE=$(curl -sk --http1.1 -o /tmp/demo_proxy_body -w '%{http_code}' "https://127.0.0.1:$PORT/api/")
echo "proxy: $PROXY_CODE"
[ "$PROXY_CODE" = "200" ] || { echo "PROXY FAIL (code)"; cleanup; exit 1; }
grep -qi "Directory listing" /tmp/demo_proxy_body || { echo "PROXY FAIL (marker)"; cleanup; exit 1; }
grep -q "hello.txt" /tmp/demo_proxy_body || { echo "PROXY FAIL (entry)"; cleanup; exit 1; }

# 优雅关闭
kill -TERM $PID
wait $PID || true       # 信号退出是预期的清理结果
kill $UPSTREAM_PID 2>/dev/null || true
wait $UPSTREAM_PID 2>/dev/null || true
echo "SMOKE-OK"

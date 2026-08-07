#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════
# 构建并启动 webcpp-engine 博客容器（含前端静态产物）
# 用法：在 webcpp-engine 目录下执行 ./build-run.sh
#   - 先在宿主机构建前端 my-web → dist（复用本机 node，Docker 内不跑 node，
#     避免拉 node 镜像依赖外网代理）
#   - 构建镜像 webcpp-blog-engine（context 为上级的 github 根，含连接池源码）
#   - 后端网关与 MySQL（mysql1 容器）都在 Docker，共处 user-defined 网络 blog-net，
#     通过容器名 mysql1 互连（默认 bridge 网络不支持容器名 DNS，故必须建自定义网络）
#   - 前端静态产物由网关 doc_root 直接 serve，浏览器同源访问 http://localhost:8443
# ═══════════════════════════════════════════════════════════════════
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GITHUB_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"   # webcpp-engine → vue-web → github
MYWEB_DIR="$(cd "$SCRIPT_DIR/../my-web" && pwd)"

echo "==> 构建前端（my-web → dist，复用宿主机 node）"
(cd "$MYWEB_DIR" && npm run build)

echo "==> 准备 Docker 网络（blog-net，后端网关与 mysql1 共处）"
docker network create blog-net 2>/dev/null || true
docker network connect blog-net mysql1 2>/dev/null || true

echo "==> 确保 mysql1 允许网络远程登录（root@'%'，幂等）"
docker exec mysql1 mysql -uroot -p123456 -e \
  "CREATE USER IF NOT EXISTS 'root'@'%' IDENTIFIED BY '123456'; \
   GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION; \
   FLUSH PRIVILEGES;" 2>&1 | grep -v "Using a password" || true

echo "==> 构建镜像（context=$GITHUB_DIR）"
docker build -f "$SCRIPT_DIR/Dockerfile" -t webcpp-blog-engine "$GITHUB_DIR"

echo "==> 重启容器"
# 宿主机局域网 IP → 注入容器，/api/network/lan 返回它（前端另用 WebRTC 实时覆盖）。
# 取「默认路由网卡」（实际联网的接口），避免写死 en0/en1 在换网卡时取错；
# 取不到再遍历 en0-en5 兜底。
DEFAULT_IF="$(route get default 2>/dev/null | awk '/interface:/{print $2}')"
HOST_LAN_IP="$(ipconfig getifaddr "${DEFAULT_IF:-en0}" 2>/dev/null)"
if [ -z "$HOST_LAN_IP" ]; then
  for i in en0 en1 en2 en3 en4 en5; do
    ip="$(ipconfig getifaddr "$i" 2>/dev/null)" && [ -n "$ip" ] && HOST_LAN_IP="$ip" && break
  done
fi
echo "   宿主机局域网 IP: ${HOST_LAN_IP:-（未检测到）}"
docker rm -f webcpp-blog 2>/dev/null || true
# 容器映射到本机内网 9443：对外统一走宿主机 lan-proxy（0.0.0.0:8443）。
# 转发器能取到真实访客 IP 并上报（容器内只能看到网桥 IP），仪表盘据此统计访问者；
# 容器不再对外直连，"局域网访问"关闭时的 403 拦截由 webcpp 按 Host 头完成。
docker run -d --name webcpp-blog --network blog-net -p 127.0.0.1:9443:8443 \
  -e HOST_LAN_IP="$HOST_LAN_IP" webcpp-blog-engine

echo "==> 启动访问者 IP 转发器（lan-proxy.py，监听 0.0.0.0:8443 → 127.0.0.1:9443）"
pkill -f "lan-proxy.py" 2>/dev/null || true
sleep 1
(cd "$SCRIPT_DIR" && nohup python3 lan-proxy.py > lan-proxy.log 2>&1 &)
sleep 2
HTTP_CODE="$(curl -s -o /dev/null -w '%{http_code}' --noproxy '*' http://localhost:8443/ || echo '无法访问')"
echo "   转发器就绪（http://localhost:8443 → HTTP $HTTP_CODE）"

echo "==> 应用已启动"
echo "   访问 : http://localhost:8443  （经宿主机转发器 → 容器 9443，记录访问者 IP）"
echo "   MySQL: mysql1（容器间通过容器名互连，Docker 内网）"
echo "   日志 : docker logs -f webcpp-blog"
echo "   转发器日志 : webcpp-engine/lan-proxy.log"

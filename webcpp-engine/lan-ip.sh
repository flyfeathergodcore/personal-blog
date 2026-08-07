#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
# lan-ip.sh — 宿主机局域网 IP 探测脚本（供 Docker 部署的博客后端使用）
#
# 背景：Docker bridge 网络下容器看不到宿主机网卡，容器内 DetectLanIp()
# 只能拿到自己的 eth0（172.17.x.x）与网关（172.17.0.1），不是宿主机在
# 局域网内的地址。本脚本在宿主机上探测真实局域网 IP：单次打印，或常驻
# 写入文件；容器把该文件 bind-mount 进来（后端 LAN_IP_FILE 每请求实时
# 读），宿主机换网后脚本自动更新文件 → 后端下次请求即拿到新 IP，无需
# 重启容器。
#
# 用法：
#   ./lan-ip.sh                       # 单次打印宿主机局域网 IP（失败输出空）
#   ./lan-ip.sh -w /run/lan-ip        # 常驻：每 2 秒重探，变化时原子写入文件
#   ./lan-ip.sh -w /run/lan-ip -i 5   # 自定义重探间隔（秒）
#
# 探测优先级（逐级回退）：
#   ① python3 UDP connect 8.8.8.8 取默认路由接口源 IP —— 与后端 C++ 的
#     DetectLanIp() 方法③同一思路，不发包仅做路由查找，macOS/Linux 最可靠
#   ② iproute2（Linux）：ip -4 route show default → 接口 → 接口地址
#   ③ macOS route + ipconfig：route -n get default → 接口 → ipconfig getifaddr
#
# Docker 部署示例（容器只暴露 8443）：
#   方式一（Linux 推荐，零改动）：--network host，容器直用宿主机网卡，
#     后端 DetectLanIp() 方法③直接嗅到宿主机 IP：
#       docker run --network host webcpp-blog
#
#   方式二（macOS / Windows / Linux 通用）：挂载文件，本脚本常驻写入：
#       ./lan-ip.sh -w /run/lan-ip &
#       docker run -p 8443:8443 -v /run/lan-ip:/var/run/lan-ip webcpp-blog
#
#   方式三（最简单，但换网需重启容器）：启动时注入一次：
#       docker run -p 8443:8443 -e HOST_LAN_IP=$(./lan-ip.sh) webcpp-blog
# ═══════════════════════════════════════════════════════════════════

# 探测宿主机局域网 IPv4，成功打印并 return 0，失败 return 1
detect() {
    # ① python3 UDP connect 法（与后端 C++ 同思路，默认路由接口源 IP，最可靠）
    if command -v python3 >/dev/null 2>&1; then
        ip=$(python3 -c '
import socket
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))   # 仅路由查找，不实际发包
    print(s.getsockname()[0])
except Exception:
    pass
' 2>/dev/null)
        [ -n "$ip" ] && echo "$ip" && return 0
    fi

    # ② Linux iproute2：默认路由所在接口的 inet 地址
    if command -v ip >/dev/null 2>&1; then
        iface=$(ip -4 route show default 2>/dev/null | awk '/default/{print $5; exit}')
        if [ -n "$iface" ]; then
            ip=$(ip -4 addr show dev "$iface" 2>/dev/null \
                 | awk '/inet /{gsub(/\/.*/, "", $2); print $2; exit}')
            [ -n "$ip" ] && echo "$ip" && return 0
        fi
    fi

    # ③ macOS：默认路由接口 → ipconfig getifaddr
    if command -v route >/dev/null 2>&1 && command -v ipconfig >/dev/null 2>&1; then
        iface=$(route -n get default 2>/dev/null | awk '/interface:/{print $2; exit}')
        if [ -n "$iface" ]; then
            ip=$(ipconfig getifaddr "$iface" 2>/dev/null)
            [ -n "$ip" ] && echo "$ip" && return 0
        fi
    fi
    return 1
}

# 解析参数
MODE=once
OUT=""
INTERVAL=2
while [ $# -gt 0 ]; do
    case "$1" in
        -w) MODE=watch; OUT="$2"; shift 2 ;;
        -i) INTERVAL="$2"; shift 2 ;;
        *) echo "用法: $0 [-w <输出文件>] [-i <间隔秒>]" >&2; exit 1 ;;
    esac
done

if [ "$MODE" = once ]; then
    # 单次模式：打印 IP
    detect
    exit $?
fi

if [ -z "$OUT" ]; then
    echo "错误：-w 需要输出文件路径" >&2
    exit 1
fi

# 常驻模式：每 INTERVAL 秒重探一次，IP 变化时原子写入文件。
# 先写临时文件再 mv 替换，容器读到的一定是完整一行，不会读到半行。
prev=""
while :; do
    ip=$(detect)
    if [ -n "$ip" ] && [ "$ip" != "$prev" ]; then
        tmp="$OUT.$$"
        printf '%s\n' "$ip" > "$tmp" && mv "$tmp" "$OUT"
        prev="$ip"
        echo "[lan-ip] 已更新 $OUT -> $ip"
    fi
    sleep "$INTERVAL"
done

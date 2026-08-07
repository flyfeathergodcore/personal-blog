#!/usr/bin/env python3
# ═══════════════════════════════════════════════════════════════════
# lan-proxy — 宿主机 TCP 转发器（记录访问者真实 IP）
#
# 背景：webcpp 容器在 Docker 内，docker-proxy NAT 后只能看到网桥 IP，
# 拿不到真实访客 IP。本脚本在宿主机监听对外端口，取每个连接的**真实
# 对端 IP**（本机 127.0.0.1 / 局域网 192.168.x.x），再透明转发给容器；
# 连接建立/断开时上报后端 /api/network/visitor，供仪表盘统计访问者。
#
# 转发是 TCP 层字节流透传，HTTP/WebSocket/SSE 长连接原样支持；
# 「局域网访问」关闭时容器返回的 403 也会原样透传（拦截逻辑在 webcpp）。
#
# 用法：python3 lan-proxy.py [listen_port] [target_host] [target_port]
#   默认：监听 0.0.0.0:8443 → 转发 127.0.0.1:9443（容器映射）
# ═══════════════════════════════════════════════════════════════════
import json
import socket
import sys
import threading
import urllib.request

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
TARGET_HOST = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
TARGET_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9443
# 上报接口：直连容器映射端口（不经转发器，避免循环上报）
API_URL = "http://127.0.0.1:%d/api/network/visitor" % TARGET_PORT


def report(ip, action):
    """异步上报连接事件到后端（失败静默，不影响转发）"""
    try:
        data = json.dumps({"ip": ip, "action": action}).encode("utf-8")
        req = urllib.request.Request(
            API_URL, data=data, headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass


def pump(src, dst):
    """单向字节流转发，直到任一端关闭"""
    try:
        while True:
            buf = src.recv(65536)
            if not buf:
                break
            dst.sendall(buf)
    except Exception:
        pass
    finally:
        try:
            src.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            dst.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass


def handle(conn, addr):
    """一个客户端连接：上报建立 → 双向转发 → 上报断开"""
    ip = addr[0]
    report(ip, "connect")
    try:
        up = socket.create_connection((TARGET_HOST, TARGET_PORT), timeout=5)
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        report(ip, "disconnect")
        return

    t1 = threading.Thread(target=pump, args=(conn, up), daemon=True)
    t2 = threading.Thread(target=pump, args=(up, conn), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    try:
        conn.close()
    except Exception:
        pass
    try:
        up.close()
    except Exception:
        pass
    report(ip, "disconnect")


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((LISTEN_HOST, LISTEN_PORT))
    except OSError as e:
        print("[lan-proxy] 监听 %s:%d 失败：%s（端口被占用？先杀旧进程）"
              % (LISTEN_HOST, LISTEN_PORT, e), flush=True)
        sys.exit(1)
    s.listen(256)
    print("[lan-proxy] 监听 %s:%d → 转发 %s:%d，已记录访问者 IP"
          % (LISTEN_HOST, LISTEN_PORT, TARGET_HOST, TARGET_PORT), flush=True)
    while True:
        try:
            conn, addr = s.accept()
        except OSError:
            break
        threading.Thread(target=handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Keep-alive HTTP/1.1 QPS client for tcp_server -m http.

This deliberately sends one outstanding request per connection. It measures the
server's TCP accept, coroutine scheduling, read and write path without HTTP
pipelining or a third-party benchmark dependency.
"""

import argparse
import asyncio
import time


REQUEST = (
    b"GET /benchmark HTTP/1.1\r\n"
    b"Host: benchmark\r\n"
    b"Connection: keep-alive\r\n"
    b"\r\n"
)


def content_length(headers: bytes) -> int:
    for line in headers.split(b"\r\n")[1:]:
        name, separator, value = line.partition(b":")
        if separator and name.strip().lower() == b"content-length":
            return int(value.strip())
    raise ValueError("response has no Content-Length")


async def connection_worker(index, host, port, start, state, ready, timeout):
    completed = 0
    errors = 0
    writer = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=timeout
        )
        ready.put_nowait(True)
        await start.wait()

        while time.perf_counter() < state["deadline"]:
            writer.write(REQUEST)
            await writer.drain()
            headers = await reader.readuntil(b"\r\n\r\n")
            if not headers.startswith(b"HTTP/1.1 200 OK\r\n"):
                raise RuntimeError("unexpected HTTP status: " + repr(headers[:64]))
            body = await reader.readexactly(content_length(headers))
            if body != b"OK":
                raise RuntimeError("unexpected HTTP body: " + repr(body))
            completed += 1
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, OSError,
            TimeoutError, ValueError, RuntimeError) as error:
        errors += 1
        if not start.is_set():
            ready.put_nowait(False)
        print("connection {}: {}".format(index, error))
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
    return completed, errors


async def run(args):
    start = asyncio.Event()
    ready = asyncio.Queue()
    state = {"deadline": 0.0}
    workers = [
        asyncio.create_task(
            connection_worker(i, args.host, args.port, start, state, ready, args.timeout)
        )
        for i in range(args.connections)
    ]

    connected = 0
    for _ in workers:
        if await ready.get():
            connected += 1
    if connected == 0:
        await asyncio.gather(*workers)
        raise RuntimeError("no benchmark connection was established")

    started = time.perf_counter()
    state["deadline"] = started + args.duration
    start.set()
    results = await asyncio.gather(*workers)
    elapsed = time.perf_counter() - started
    requests = sum(result[0] for result in results)
    errors = sum(result[1] for result in results)

    print("host={}:{} connections={}/{}".format(
        args.host, args.port, connected, args.connections
    ))
    print("duration={:.3f}s requests={} errors={}".format(elapsed, requests, errors))
    print("qps={:.2f}".format(requests / elapsed if elapsed else 0.0))
    return 0 if errors == 0 else 1


def parse_args():
    parser = argparse.ArgumentParser(description="QPS benchmark for tcp_server -m http")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("-c", "--connections", type=int, default=64)
    parser.add_argument("-d", "--duration", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=3.0,
                        help="connection setup timeout in seconds")
    args = parser.parse_args()
    if args.port < 1 or args.port > 65535:
        parser.error("port must be in 1..65535")
    if args.connections < 1:
        parser.error("connections must be positive")
    if args.duration <= 0:
        parser.error("duration must be positive")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(run(parse_args())))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except RuntimeError as error:
        print("benchmark failed: {}".format(error))
        raise SystemExit(2)

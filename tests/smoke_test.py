#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
import time


def recv_text(sock: socket.socket) -> str:
    return sock.recv(4096).decode("utf-8", errors="replace")
# 从 socket 接收最多 4096 字节的数据，返回的是 bytes 对象
#.decode("utf-8", errors="replace")：把接收到的字节按 UTF-8 编码解码成 Python 字符串（str 类型）。
# errors="replace" 的意思是如果遇到无法解码的字节，就用 � 替代而不是抛出异常。

def send_cmd(sock: socket.socket, cmd: str) -> str:
    sock.sendall((cmd + "\n").encode("utf-8"))
    # sendall 会保证把所有数据都发送完，内部会循环调用底层的 send 直到全部数据送出。
    return recv_text(sock)
    # 发送后，立即调用 recv_text(sock) 读取服务器的响应并返回。

def connect_with_retry(host: str, port: int, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None

    while time.monotonic() < deadline:
        try:
            return socket.create_connection((host, port), timeout=1.0)
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)

    raise RuntimeError(f"server did not become ready in {timeout} seconds: {last_error}")


def run_smoke_test(host: str, port: int, timeout: float) -> None:
    with connect_with_retry(host, port, timeout) as sock:
        welcome = recv_text(sock)
        assert "WELCOME" in welcome, welcome

        assert send_cmd(sock, "PING") == "PONG\n"
        assert send_cmd(sock, "SET 23 zhao") == "OK\n"
        assert send_cmd(sock, "GET 23") == "VALUE zhao\n"
        assert send_cmd(sock, "SIZE") == "SIZE 1\n"
        assert send_cmd(sock, "DEL 23") == "DELETED\n"
        assert send_cmd(sock, "GET 23") == "NOT_FOUND\n"
        assert send_cmd(sock, "EXIT") == "BYE\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run TCP smoke test against mini_kv_server.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_smoke_test(args.host, args.port, args.timeout)
    print("smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

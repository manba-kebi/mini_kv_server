#! /usr/bin/env python3

from __future__ import  annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

# 解析逗号分隔的数字
def parse_csv_ints(text: str) -> list[int]:
    values = [int(item.strip()) for item in text.split(",") if item.strip()]
    if not values:
        raise argparse.ArgumentTypeError("list must not be empty")
    return values

# 死等服务器端口开
def wait_for_port(host: str,port: int,timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None

    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host,port),timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)

        raise RuntimeError(f"server did not listen on {host}:{port}: {last_error}")

# 解析文本里的键值对。
def parse_key_values(output: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in output.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=",1)
        result[key.strip()] = value.strip()
    return result

# 跑一轮完整的“启动服务器 -> 压测 -> 关闭服务器”流程（这是核心函数）。
def run_case(args: argparse.Namespace, workers: int, queue: int, backlog: int) -> dict[str, str]:
    server_cmd = [
        str(args.server),
        "--port",
        str(args.port),
        "--workers",
        str(workers),
        "--queue",
        str(queue),
        "--backlog",
        str(backlog),
    ]

    server = subprocess.Popen(
        server_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        wait_for_port(args.host, args.port, args.start_timeout)
        bench_cmd = [
            sys.executable,
            str(args.benchmark),
            "--host",
            args.host,
            "--port",
            str(args.port),
            "--clients",
            str(args.clients),
            "--rounds",
            str(args.rounds),
            "--timeout",
            str(args.request_timeout),
        ]
        completed = subprocess.run(
            bench_cmd,
            check=True,
            text=True,
            capture_output=True,
        )
        values = parse_key_values(completed.stdout)
        values["workers"] = str(workers)
        values["queue"] = str(queue)
        values["backlog"] = str(backlog)
        return values
    finally:
        server.terminate()
        try:
            server.wait(timeout = 3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=3)

# 把数据打印成 Markdown 格式的表格。
def print_markdown_table(rows: list[dict[str,str]]) -> None:
    headers = [
        "workers",
        "queue",
        "backlog",
        "clients",
        "rounds",
        "ok",
        "failed",
        "qps",
        "avg_ms",
        "p95_ms",
        "p99_ms",
    ]
    print("| " + " | ".join(headers) +" |")
    print("| " + " | ".join(["---"] * len(headers)) + " |")
    for row in rows:
        print("| " + " | ".join(row.get(header, "") for header in headers) + " |")

# 定义矩阵测试的命令行参数（比 benchmark.py 复杂一些）。
def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Run benchmark matrix for mini_kv_server.")
    parser.add_argument("--server", type=Path, default=root / "build" / "mini_kv_server")   # 它会自动往上一级目录找 build 文件夹里的 C++ 服务器可执行文件，你不需要手动指定绝对路径。
    parser.add_argument("--benchmark", type=Path, default=root / "tools" / "benchmark.py")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--workers", type=parse_csv_ints, default=[1, 4, 8])
    parser.add_argument("--queues", type=parse_csv_ints, default=[16, 64])
    parser.add_argument("--backlogs", type=parse_csv_ints, default=[64])
    parser.add_argument("--clients", type=int, default=16)
    parser.add_argument("--rounds", type=int, default=1000)
    parser.add_argument("--start-timeout", type=float, default=5.0)
    parser.add_argument("--request-timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows: list[dict[str, str]] = []

    for workers in args.workers:
        for queue in args.queues:
            for backlog in args.backlogs:
                print(
                    f"running workers={workers}, queue={queue}, backlog={backlog}",
                    file=sys.stderr,
                )
                rows.append(run_case(args, workers, queue, backlog))

    print_markdown_table(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
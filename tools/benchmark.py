#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
import statistics
import threading
import time
from dataclasses import dataclass, field


@dataclass
class WorkerResult:
    ok: int = 0             # 成功请求
    failed: int = 0         # 失败请求
    latencies_ns: list[int] = field(default_factory=list)       # 所有成功请求的耗时
    errors: list[str] = field(default_factory=list)             # 报错信息列表

# TCP 流读取器。从 socket 里一个字节一个字节地读数据，直到读到一个换行符 \n 才停下，把这一整行字符串返回。
# pending 用来暂存还没处理完的“半截数据”（处理粘包）。
def recv_line(sock: socket.socket, pending: bytearray) -> str:
    while True:
        newline = pending.find(b"\n")
        if newline != -1:
            raw = pending[: newline + 1]
            del pending[: newline + 1]
            return raw.decode("utf-8", errors="replace")

        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("connection closed while waiting for response")
        pending.extend(chunk)

# 发送单条命令并计时。把命令（如 SET k1 v1）发出去，调用 recv_line 等回复，同时用高精度计时器算一下这条命令耗了多少纳秒。
def request(sock: socket.socket, pending: bytearray, cmd: str) -> tuple[str, int]:
    started = time.perf_counter_ns()
    sock.sendall((cmd + "\n").encode("utf-8"))
    response = recv_line(sock, pending)
    elapsed = time.perf_counter_ns() - started
    return response, elapsed

# 连上服务器 -> 检查欢迎词 -> 循环 rounds 次（每次发一个 SET，再发一个 GET，并记录每次的耗时）-> 最后发 EXIT 退出。
def worker(worker_id: int, host: str, port: int, rounds: int, timeout: float) -> WorkerResult:
    result = WorkerResult()
    pending = bytearray()

    try:
        # 每个 worker 线程创建一个 socket 连接。
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            welcome = recv_line(sock, pending)
            if not welcome.startswith("WELCOME"):
                result.failed += 1
                result.errors.append(f"unexpected welcome: {welcome.strip()}")
                return result

            for i in range(rounds):
                key = f"k{worker_id}_{i}"

                response, latency = request(sock, pending, f"SET {key} value")
                if response == "OK\n":
                    result.ok += 1
                    result.latencies_ns.append(latency)
                else:
                    result.failed += 1
                    result.errors.append(f"SET failed: {response.strip()}")

                response, latency = request(sock, pending, f"GET {key}")
                if response == "VALUE value\n":
                    result.ok += 1
                    result.latencies_ns.append(latency)
                else:
                    result.failed += 1
                    result.errors.append(f"GET failed: {response.strip()}")

            try:
                request(sock, pending, "EXIT")
            except OSError:
                pass

    except OSError as exc:
        result.failed += rounds * 2
        result.errors.append(str(exc))

    return result

# 百分位数计算器。把传进来的所有耗时（纳秒）排序，取出指定百分比（如 0.95）位置的那个值，并换算成毫秒返回。
def percentile(values: list[int], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int((len(ordered) - 1) * pct)       # -1原因：因为索引从 0 开始，最大索引是 N-1；百分位是一把从 0 到 1 的尺子，末端必须对准 N-1。
    return ordered[index] / 1_000_000.0

# 拿到所有线程的 results，汇总总请求数、成功/失败数，算出总 QPS、平均延迟，调用 percentile 算 p95/p99，最后把结果打印到终端上。
def print_summary(clients: int, rounds: int, elapsed: float, results: list[WorkerResult]) -> None:
    ok = sum(item.ok for item in results)
    failed = sum(item.failed for item in results)
    attempted = clients * rounds * 2
    latencies = [latency for item in results for latency in item.latencies_ns]

    avg_ms = (statistics.mean(latencies) / 1_000_000.0) if latencies else 0.0
    qps = ok / elapsed if elapsed > 0 else 0.0

    print(f"clients={clients}")
    print(f"rounds={rounds}")
    print(f"attempted={attempted}")
    print(f"ok={ok}")
    print(f"failed={failed}")
    print(f"seconds={elapsed:.6f}")
    print(f"qps={qps:.2f}")
    print(f"avg_ms={avg_ms:.4f}")
    print(f"p50_ms={percentile(latencies, 0.50):.4f}")
    print(f"p95_ms={percentile(latencies, 0.95):.4f}")
    print(f"p99_ms={percentile(latencies, 0.99):.4f}")

    errors = [error for item in results for error in item.errors]       # 把两层 for 循环拍扁成一行
    if errors:
        print("sample_errors:")
        for error in errors[:5]:        # 因为压测可能产生海量错误，全部打印会“刷屏”且毫无意义。所以这里只取前5个错误信息
            print(f"- {error}")

# 读取终端输入的参数（如 --clients 10），转换成 Python 变量，并附带上默认值（如没传 --port 就用 9000）。
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark mini_kv_server with concurrent TCP clients.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--rounds", type=int, default=500)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.clients <= 0:
        raise SystemExit("--clients must be positive")
    if args.rounds <= 0:
        raise SystemExit("--rounds must be positive")

    started = time.perf_counter()
    results: list[WorkerResult] = []
    results_lock = threading.Lock()

    # 调用 worker，并在拿到结果时加了一把线程锁（Lock），防止多个线程同时往 results 列表里塞数据导致混乱。
    def run_one(worker_id: int) -> None:
        result = worker(worker_id, args.host, args.port, args.rounds, args.timeout)
        with results_lock:
            results.append(result)

    threads = [threading.Thread(target=run_one, args=(i,)) for i in range(args.clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    elapsed = time.perf_counter() - started
    print_summary(args.clients, args.rounds, elapsed, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
#!/usr/bin/env python3
"""
Backpressure test for mini_kv_server.

This script is different from benchmark.py:
- benchmark.py measures throughput by repeatedly sending commands.
- this script verifies the bounded queue behavior by creating many clients
  at the same time and checking how many are accepted versus rejected as BUSY.

Suggested server command:
  ./build/mini_kv_server --port 9000 --workers 1 --queue 1 --backlog 64

Suggested test command:
  python3 tools/backpressure_test.py --clients 20 --hold-seconds 2
"""

from __future__ import annotations

import argparse
import socket
import threading
import time
from dataclasses import dataclass
from typing import Counter as CounterType
from collections import Counter


@dataclass
class ClientResult:
    status: str
    detail: str = ""


def recv_once(sock: socket.socket, timeout: float) -> str:
    sock.settimeout(timeout)
    data = sock.recv(4096)
    return data.decode("utf-8", errors="replace")


def run_client(
    host: str,
    port: int,
    start_barrier: threading.Barrier,
    hold_seconds: float,
    timeout: float,
) -> ClientResult:
    try:
        start_barrier.wait()
    except threading.BrokenBarrierError:
        return ClientResult("barrier_error")

    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            first_response = recv_once(sock, timeout)

            if first_response.startswith("WELCOME"):
                # Keep this accepted connection alive for a while, so it occupies
                # one worker thread. This is what makes the queue fill up quickly.
                time.sleep(hold_seconds)

                try:
                    sock.sendall(b"EXIT\n")
                    recv_once(sock, timeout)
                except BrokenPipeError:
                    return ClientResult("broken_pipe", "server closed accepted client before EXIT")
                except ConnectionResetError:
                    return ClientResult("connection_reset", "accepted client reset before EXIT")
                except socket.timeout:
                    return ClientResult("timeout", "accepted client timed out on EXIT")

                return ClientResult("welcome")

            if first_response.startswith("BUSY"):
                # This is the expected backpressure signal when the user-space
                # thread-pool queue is full.
                return ClientResult("busy")

            if first_response == "":
                return ClientResult("closed_without_response")

            return ClientResult("unexpected_response", first_response.strip())

    except BrokenPipeError as exc:
        return ClientResult("broken_pipe", str(exc))
    except ConnectionRefusedError as exc:
        return ClientResult("connect_refused", str(exc))
    except ConnectionResetError as exc:
        return ClientResult("connection_reset", str(exc))
    except socket.timeout as exc:
        return ClientResult("timeout", str(exc))
    except OSError as exc:
        return ClientResult("os_error", str(exc))


def print_summary(results: list[ClientResult], elapsed: float) -> None:
    counts: CounterType[str] = Counter(result.status for result in results)

    print("backpressure test result")
    print(f"clients={len(results)}, seconds={elapsed:.3f}")
    print(f"welcome={counts['welcome']}")
    print(f"busy={counts['busy']}")
    print(f"connect_refused={counts['connect_refused']}")
    print(f"connection_reset={counts['connection_reset']}")
    print(f"broken_pipe={counts['broken_pipe']}")
    print(f"timeout={counts['timeout']}")
    print(f"closed_without_response={counts['closed_without_response']}")
    print(f"unexpected_response={counts['unexpected_response']}")
    print(f"os_error={counts['os_error']}")

    interesting_details = [
        result for result in results
        if result.status not in {"welcome", "busy"} and result.detail
    ]
    if interesting_details:
        print()
        print("details:")
        for result in interesting_details[:10]:
            print(f"- {result.status}: {result.detail}")

    if counts["busy"] > 0:
        print()
        print("conclusion: BUSY appeared, so the bounded queue backpressure is visible.")
    else:
        print()
        print("conclusion: BUSY did not appear. Try increasing --clients or lowering server --workers/--queue.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create concurrent clients and count WELCOME/BUSY/backpressure results."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--clients", type=int, default=20)
    parser.add_argument("--hold-seconds", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=8.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.clients <= 0:
        raise SystemExit("--clients must be positive")
    if args.hold_seconds < 0:
        raise SystemExit("--hold-seconds must not be negative")
    if args.timeout <= 0:
        raise SystemExit("--timeout must be positive")

    start_barrier = threading.Barrier(args.clients)
    results: list[ClientResult] = []
    results_lock = threading.Lock()

    def worker() -> None:
        result = run_client(
            host=args.host,
            port=args.port,
            start_barrier=start_barrier,
            hold_seconds=args.hold_seconds,
            timeout=args.timeout,
        )
        with results_lock:
            results.append(result)

    threads = [threading.Thread(target=worker) for _ in range(args.clients)]

    started_at = time.perf_counter()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started_at

    print_summary(results, elapsed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

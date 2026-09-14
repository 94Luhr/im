#!/usr/bin/env python3
"""IM 服务端 asyncio 并发连接与心跳压测工具。"""

import argparse
import asyncio
import json
import random
import struct
import time
from collections import deque
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Deque, List, Optional


PROTOCOL_BASE = 1000
HEARTBEAT_REQUEST = PROTOCOL_BASE + 11
HEARTBEAT_RESPONSE = PROTOCOL_BASE + 12
MAX_PACKET_SIZE = 1024 * 1024


@dataclass
class Statistics:
    requested_connections: int = 0
    connected: int = 0
    connection_failures: int = 0
    unexpected_disconnects: int = 0
    requests_sent: int = 0
    responses_received: int = 0
    protocol_errors: int = 0
    write_failures: int = 0
    latencies_ms: List[float] = field(default_factory=list)


def build_frame(protocol_type: int) -> bytes:
    # 协议体中的 int 沿用 x86 小端布局，外层帧长度固定为网络字节序。
    body = struct.pack("<i", protocol_type)
    return struct.pack("!I", len(body)) + body


def percentile(values: List[float], ratio: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * ratio))
    return ordered[index]


async def write_with_optional_fragmentation(
    writer: asyncio.StreamWriter,
    payload: bytes,
    fragment_ratio: float,
) -> None:
    if len(payload) <= 1 or random.random() >= fragment_ratio:
        writer.write(payload)
        await writer.drain()
        return

    # 随机切成多个小片段，验证服务端输入缓存能正确处理 TCP 半包。
    offset = 0
    while offset < len(payload):
        part_length = random.randint(1, min(7, len(payload) - offset))
        writer.write(payload[offset : offset + part_length])
        await writer.drain()
        offset += part_length
        await asyncio.sleep(0)


async def read_responses(
    reader: asyncio.StreamReader,
    pending: Deque[float],
    statistics: Statistics,
    stopping: asyncio.Event,
) -> None:
    try:
        while not stopping.is_set():
            header = await reader.readexactly(4)
            body_length = struct.unpack("!I", header)[0]
            if body_length <= 0 or body_length > MAX_PACKET_SIZE:
                statistics.protocol_errors += 1
                return
            body = await reader.readexactly(body_length)
            if body_length < 4 or struct.unpack_from("<i", body)[0] != HEARTBEAT_RESPONSE:
                statistics.protocol_errors += 1
                continue
            statistics.responses_received += 1
            if pending:
                statistics.latencies_ms.append((time.perf_counter() - pending.popleft()) * 1000)
            else:
                statistics.protocol_errors += 1
    except asyncio.IncompleteReadError:
        if not stopping.is_set():
            statistics.unexpected_disconnects += 1
    except (ConnectionError, OSError):
        if not stopping.is_set():
            statistics.unexpected_disconnects += 1


async def run_connection(
    host: str,
    port: int,
    deadline: float,
    interval: float,
    burst: int,
    fragment_ratio: float,
    connect_semaphore: asyncio.Semaphore,
    statistics: Statistics,
) -> None:
    writer: Optional[asyncio.StreamWriter] = None
    try:
        async with connect_semaphore:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=5.0
            )
        statistics.connected += 1
    except (asyncio.TimeoutError, ConnectionError, OSError):
        statistics.connection_failures += 1
        return

    pending: Deque[float] = deque()
    stopping = asyncio.Event()
    reader_task = asyncio.create_task(read_responses(reader, pending, statistics, stopping))
    heartbeat_frame = build_frame(HEARTBEAT_REQUEST)

    try:
        while time.monotonic() < deadline and not reader_task.done():
            # 多帧在一次 write 中提交，用于覆盖 TCP 粘包场景。
            payload = heartbeat_frame * burst
            now = time.perf_counter()
            pending.extend(now for _ in range(burst))
            await write_with_optional_fragmentation(writer, payload, fragment_ratio)
            statistics.requests_sent += burst
            if interval > 0:
                await asyncio.sleep(interval)
            else:
                await asyncio.sleep(0)
        # 给已经发出的请求短暂时间返回，避免结束瞬间低估成功率。
        grace_deadline = time.monotonic() + 2.0
        while pending and time.monotonic() < grace_deadline and not reader_task.done():
            await asyncio.sleep(0.01)
    except (ConnectionError, OSError):
        statistics.write_failures += 1
    finally:
        stopping.set()
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, OSError):
            pass
        reader_task.cancel()
        await asyncio.gather(reader_task, return_exceptions=True)


def build_report(statistics: Statistics, duration: float, arguments: argparse.Namespace) -> dict:
    latency_values = statistics.latencies_ms
    report = asdict(statistics)
    report.pop("latencies_ms", None)
    report.update(
        {
            "host": arguments.host,
            "port": arguments.port,
            "configured_duration_seconds": arguments.duration,
            "actual_duration_seconds": round(duration, 3),
            "burst": arguments.burst,
            "fragment_ratio": arguments.fragment_ratio,
            "responses_per_second": round(
                statistics.responses_received / duration if duration > 0 else 0.0, 2
            ),
            "response_success_percent": round(
                statistics.responses_received * 100.0 / statistics.requests_sent
                if statistics.requests_sent
                else 0.0,
                3,
            ),
            "latency_ms": {
                "average": round(sum(latency_values) / len(latency_values), 3)
                if latency_values
                else 0.0,
                "p50": round(percentile(latency_values, 0.50), 3),
                "p95": round(percentile(latency_values, 0.95), 3),
                "p99": round(percentile(latency_values, 0.99), 3),
                "maximum": round(max(latency_values), 3) if latency_values else 0.0,
            },
        }
    )
    return report


async def run(arguments: argparse.Namespace) -> dict:
    statistics = Statistics(requested_connections=arguments.connections)
    semaphore = asyncio.Semaphore(arguments.connect_concurrency)
    started_at = time.monotonic()
    deadline = started_at + arguments.duration
    tasks = [
        asyncio.create_task(
            run_connection(
                arguments.host,
                arguments.port,
                deadline,
                arguments.interval,
                arguments.burst,
                arguments.fragment_ratio,
                semaphore,
                statistics,
            )
        )
        for _ in range(arguments.connections)
    ]
    await asyncio.gather(*tasks)
    return build_report(statistics, time.monotonic() - started_at, arguments)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="IM 服务端 asyncio 心跳压测")
    parser.add_argument("--host", default="127.0.0.1", help="服务端地址")
    parser.add_argument("--port", type=int, default=56789, help="服务端端口")
    parser.add_argument("--connections", type=int, default=1000, help="并发连接数")
    parser.add_argument("--duration", type=float, default=30.0, help="压测持续秒数")
    parser.add_argument("--interval", type=float, default=1.0, help="每轮发送间隔秒数")
    parser.add_argument("--burst", type=int, default=2, help="每次写入合并的心跳帧数")
    parser.add_argument(
        "--fragment-ratio", type=float, default=0.1, help="随机拆分写入的概率，范围0到1"
    )
    parser.add_argument(
        "--connect-concurrency", type=int, default=200, help="同时执行连接握手的数量"
    )
    parser.add_argument("--output", default="load-test-report.json", help="JSON报告路径")
    arguments = parser.parse_args()
    if arguments.connections <= 0 or arguments.duration <= 0 or arguments.interval < 0:
        parser.error("连接数和持续时间必须大于0，间隔不能小于0")
    if arguments.burst <= 0 or arguments.connect_concurrency <= 0:
        parser.error("burst和连接并发数必须大于0")
    if not 0.0 <= arguments.fragment_ratio <= 1.0:
        parser.error("fragment-ratio必须在0到1之间")
    return arguments


def main() -> None:
    arguments = parse_arguments()
    try:
        report = asyncio.run(run(arguments))
    except KeyboardInterrupt:
        print("压测已由用户中止")
        return
    report_text = json.dumps(report, ensure_ascii=False, indent=2)
    print(report_text)
    Path(arguments.output).write_text(report_text + "\n", encoding="utf-8")
    print(f"报告已写入: {arguments.output}")


if __name__ == "__main__":
    main()

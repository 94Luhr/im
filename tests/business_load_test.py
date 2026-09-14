#!/usr/bin/env python3
"""IM服务端登录、消息持久化和离线投递业务压测工具。"""

import argparse
import asyncio
import json
import struct
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple


PROTOCOL_BASE = 1000
LOGIN_REQUEST = PROTOCOL_BASE + 3
LOGIN_RESPONSE = PROTOCOL_BASE + 4
CHAT_REQUEST = PROTOCOL_BASE + 7
CHAT_RESPONSE = PROTOCOL_BASE + 8
CHAT_MESSAGE = PROTOCOL_BASE + 13
MESSAGE_ACK = PROTOCOL_BASE + 14

LOGIN_SUCCESS = 0
SEND_SUCCESS = 0
MAX_PACKET_SIZE = 1024 * 1024
MAX_STRING_LENGTH = 15
CHAT_CONTENT_LENGTH = 8 * 1024
DEFAULT_PASSWORD = "LoadTest123!"


@dataclass
class BusinessStatistics:
    requested_pairs: int = 0
    sender_login_successes: int = 0
    receiver_login_successes: int = 0
    login_failures: int = 0
    connection_failures: int = 0
    messages_requested: int = 0
    messages_persisted: int = 0
    persist_failures: int = 0
    offline_messages_received: int = 0
    acknowledgements_sent: int = 0
    protocol_errors: int = 0
    task_failures: int = 0
    persist_latencies_ms: List[float] = field(default_factory=list)
    receiver_login_latencies_ms: List[float] = field(default_factory=list)


def fixed_string(value: str, length: int) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) >= length:
        raise ValueError(f"字符串编码后必须少于{length}字节: {value}")
    return encoded + bytes(length - len(encoded))


def frame(body: bytes) -> bytes:
    # 外层长度使用网络字节序，协议结构体字段沿用x86小端布局。
    return struct.pack("!I", len(body)) + body


def login_body(telephone: str, password: str) -> bytes:
    # 原登录结构体按4字节对齐，末尾需要补2字节。
    return struct.pack(
        "<i15s15s2x",
        LOGIN_REQUEST,
        fixed_string(telephone, MAX_STRING_LENGTH),
        fixed_string(password, MAX_STRING_LENGTH),
    )


def chat_body(sender_id: int, receiver_id: int, content: str) -> bytes:
    return struct.pack(
        "<iii8192s",
        CHAT_REQUEST,
        sender_id,
        receiver_id,
        fixed_string(content, CHAT_CONTENT_LENGTH),
    )


def acknowledgement_body(message_id: int) -> bytes:
    # ACK结构体采用1字节对齐，协议体长度固定为12字节。
    return struct.pack("<iQ", MESSAGE_ACK, message_id)


async def read_frame(reader: asyncio.StreamReader, timeout: float) -> bytes:
    header = await asyncio.wait_for(reader.readexactly(4), timeout=timeout)
    body_length = struct.unpack("!I", header)[0]
    if body_length < 4 or body_length > MAX_PACKET_SIZE:
        raise ValueError(f"非法协议体长度: {body_length}")
    return await asyncio.wait_for(reader.readexactly(body_length), timeout=timeout)


async def read_until_type(
    reader: asyncio.StreamReader,
    expected_type: int,
    timeout: float,
) -> bytes:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise asyncio.TimeoutError
        body = await read_frame(reader, remaining)
        protocol_type = struct.unpack_from("<i", body)[0]
        if protocol_type == expected_type:
            return body


async def open_and_login(
    host: str,
    port: int,
    telephone: str,
    password: str,
    timeout: float,
) -> Tuple[asyncio.StreamReader, asyncio.StreamWriter, int]:
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(host, port), timeout=timeout
    )
    writer.write(frame(login_body(telephone, password)))
    await writer.drain()
    response = await read_until_type(reader, LOGIN_RESPONSE, timeout)
    if len(response) != 12:
        raise ValueError(f"登录响应长度错误: {len(response)}")
    _, user_id, result = struct.unpack("<iii", response)
    if result != LOGIN_SUCCESS or user_id <= 0:
        raise PermissionError(f"账号{telephone}登录失败，结果码{result}")
    return reader, writer, user_id


async def close_writer(writer: Optional[asyncio.StreamWriter]) -> None:
    if writer is None:
        return
    writer.close()
    try:
        await writer.wait_closed()
    except (ConnectionError, OSError):
        pass


def account(prefix: str, pair_index: int) -> str:
    return f"{prefix}{pair_index:06d}"


async def persist_pair_messages(
    pair_index: int,
    arguments: argparse.Namespace,
    run_token: str,
    semaphore: asyncio.Semaphore,
    statistics: BusinessStatistics,
) -> None:
    writer: Optional[asyncio.StreamWriter] = None
    try:
        async with semaphore:
            reader, writer, sender_id = await open_and_login(
                arguments.host,
                arguments.port,
                account("load_s", pair_index),
                arguments.password,
                arguments.timeout,
            )
            statistics.sender_login_successes += 1

            # 接收者ID不能由测试脚本猜测，通过接收者登录结果获得会干扰离线场景。
            # 测试账号按SQL脚本成对连续创建，但自增ID可能受现有数据影响，
            # 因此从登录后服务端发送的好友信息中读取真实好友ID。
            friend_id = 0
            friend_deadline = time.monotonic() + arguments.timeout
            while friend_id <= 0:
                body = await read_frame(reader, friend_deadline - time.monotonic())
                protocol_type = struct.unpack_from("<i", body)[0]
                if protocol_type == PROTOCOL_BASE + 10 and len(body) == 48:
                    candidate_id = struct.unpack_from("<i", body, 4)[0]
                    candidate_name = body[16:31].split(b"\0", 1)[0].decode(
                        "utf-8", errors="replace"
                    )
                    if candidate_name == account("load_r", pair_index):
                        friend_id = candidate_id

            for message_index in range(arguments.messages_per_pair):
                content = f"load:{run_token}:{pair_index}:{message_index}"
                started_at = time.perf_counter()
                writer.write(frame(chat_body(sender_id, friend_id, content)))
                await writer.drain()
                statistics.messages_requested += 1

                response = await read_until_type(
                    reader, CHAT_RESPONSE, arguments.timeout
                )
                if len(response) != 12:
                    raise ValueError(f"聊天响应长度错误: {len(response)}")
                _, response_friend_id, result = struct.unpack("<iii", response)
                if result != SEND_SUCCESS or response_friend_id != friend_id:
                    statistics.persist_failures += 1
                    continue
                statistics.messages_persisted += 1
                statistics.persist_latencies_ms.append(
                    (time.perf_counter() - started_at) * 1000
                )
    except PermissionError:
        statistics.login_failures += 1
    except (asyncio.TimeoutError, ConnectionError, OSError):
        statistics.connection_failures += 1
        statistics.task_failures += 1
    except (UnicodeError, ValueError, struct.error):
        statistics.protocol_errors += 1
        statistics.task_failures += 1
    finally:
        await close_writer(writer)


async def receive_pair_messages(
    pair_index: int,
    expected_count: int,
    arguments: argparse.Namespace,
    run_token: str,
    semaphore: asyncio.Semaphore,
    statistics: BusinessStatistics,
) -> None:
    writer: Optional[asyncio.StreamWriter] = None
    try:
        async with semaphore:
            started_at = time.perf_counter()
            reader, writer, receiver_id = await open_and_login(
                arguments.host,
                arguments.port,
                account("load_r", pair_index),
                arguments.password,
                arguments.timeout,
            )
            statistics.receiver_login_successes += 1
            statistics.receiver_login_latencies_ms.append(
                (time.perf_counter() - started_at) * 1000
            )

            received_for_run = 0
            deadline = time.monotonic() + arguments.delivery_timeout
            while received_for_run < expected_count:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise asyncio.TimeoutError
                body = await read_frame(reader, remaining)
                protocol_type = struct.unpack_from("<i", body)[0]
                if protocol_type != CHAT_MESSAGE:
                    continue
                if len(body) != 8224:
                    raise ValueError(f"离线消息长度错误: {len(body)}")

                _, _, message_receiver_id, source, message_id, _ = struct.unpack_from(
                    "<iiiiQq", body
                )
                content = body[32:].split(b"\0", 1)[0].decode(
                    "utf-8", errors="replace"
                )
                if message_receiver_id != receiver_id or source != 1:
                    statistics.protocol_errors += 1
                    continue

                # 对收到的合法消息都确认，只有本轮令牌匹配的消息计入结果。
                writer.write(frame(acknowledgement_body(message_id)))
                await writer.drain()
                statistics.acknowledgements_sent += 1
                if content.startswith(f"load:{run_token}:{pair_index}:"):
                    received_for_run += 1
                    statistics.offline_messages_received += 1
    except PermissionError:
        statistics.login_failures += 1
    except (asyncio.TimeoutError, ConnectionError, OSError):
        statistics.connection_failures += 1
        statistics.task_failures += 1
    except (UnicodeError, ValueError, struct.error):
        statistics.protocol_errors += 1
        statistics.task_failures += 1
    finally:
        await close_writer(writer)


def percentile(values: List[float], ratio: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * ratio))
    return ordered[index]


def latency_report(values: List[float]) -> dict:
    return {
        "average": round(sum(values) / len(values), 3) if values else 0.0,
        "p50": round(percentile(values, 0.50), 3),
        "p95": round(percentile(values, 0.95), 3),
        "p99": round(percentile(values, 0.99), 3),
        "maximum": round(max(values), 3) if values else 0.0,
    }


async def run(arguments: argparse.Namespace) -> dict:
    statistics = BusinessStatistics(requested_pairs=arguments.pairs)
    semaphore = asyncio.Semaphore(arguments.concurrency)
    run_token = uuid.uuid4().hex[:8]

    persist_started_at = time.monotonic()
    await asyncio.gather(
        *(
            persist_pair_messages(
                pair_index, arguments, run_token, semaphore, statistics
            )
            for pair_index in range(1, arguments.pairs + 1)
        )
    )
    persist_duration = time.monotonic() - persist_started_at

    # 发送连接关闭后短暂等待Reactor完成异步断线清理。
    await asyncio.sleep(1.0)

    delivery_started_at = time.monotonic()
    await asyncio.gather(
        *(
            receive_pair_messages(
                pair_index,
                arguments.messages_per_pair,
                arguments,
                run_token,
                semaphore,
                statistics,
            )
            for pair_index in range(1, arguments.pairs + 1)
        )
    )
    delivery_duration = time.monotonic() - delivery_started_at

    report = asdict(statistics)
    report.pop("persist_latencies_ms")
    report.pop("receiver_login_latencies_ms")
    report.update(
        {
            "host": arguments.host,
            "port": arguments.port,
            "run_token": run_token,
            "messages_per_pair": arguments.messages_per_pair,
            "persist_duration_seconds": round(persist_duration, 3),
            "delivery_duration_seconds": round(delivery_duration, 3),
            "persisted_messages_per_second": round(
                statistics.messages_persisted / persist_duration, 2
            ),
            "delivered_messages_per_second": round(
                statistics.offline_messages_received / delivery_duration, 2
            ),
            "persist_latency_ms": latency_report(statistics.persist_latencies_ms),
            "receiver_login_latency_ms": latency_report(
                statistics.receiver_login_latencies_ms
            ),
        }
    )
    return report


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="IM服务端数据库业务场景压测")
    parser.add_argument("--host", default="127.0.0.1", help="服务端地址")
    parser.add_argument("--port", type=int, default=56789, help="服务端端口")
    parser.add_argument("--pairs", type=int, default=100, help="参与测试的账号对数")
    parser.add_argument(
        "--messages-per-pair", type=int, default=10, help="每对账号发送的消息数"
    )
    parser.add_argument("--concurrency", type=int, default=50, help="并发业务连接数")
    parser.add_argument("--timeout", type=float, default=10.0, help="单次操作超时秒数")
    parser.add_argument(
        "--delivery-timeout", type=float, default=30.0, help="每个接收者补发超时秒数"
    )
    parser.add_argument("--password", default=DEFAULT_PASSWORD, help="压测账号密码")
    parser.add_argument(
        "--output", default="business-load-report.json", help="JSON报告路径"
    )
    arguments = parser.parse_args()
    if not 1 <= arguments.pairs <= 1000:
        parser.error("pairs必须在1到1000之间")
    if arguments.messages_per_pair <= 0 or arguments.messages_per_pair > 1000:
        parser.error("messages-per-pair必须在1到1000之间")
    if arguments.concurrency <= 0 or arguments.timeout <= 0:
        parser.error("并发数和超时时间必须大于0")
    if arguments.delivery_timeout <= 0:
        parser.error("离线投递超时时间必须大于0")
    fixed_string(arguments.password, MAX_STRING_LENGTH)
    return arguments


def main() -> None:
    arguments = parse_arguments()
    try:
        report = asyncio.run(run(arguments))
    except KeyboardInterrupt:
        print("业务压测已由用户中止")
        return
    report_text = json.dumps(report, ensure_ascii=False, indent=2)
    print(report_text)
    Path(arguments.output).write_text(report_text + "\n", encoding="utf-8")
    print(f"报告已写入: {arguments.output}")


if __name__ == "__main__":
    main()


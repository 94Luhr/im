#!/usr/bin/env python3
"""IM服务端核心业务的一键自动化集成测试。"""

import argparse
import asyncio
import json
import struct
import time
import uuid
from pathlib import Path
from typing import Callable, Dict, Optional, Tuple

from business_load_test import (
    CHAT_MESSAGE,
    CHAT_RESPONSE,
    LOGIN_RESPONSE,
    SEND_SUCCESS,
    close_writer,
    fixed_string,
    frame,
    login_body,
    open_and_login,
    read_frame,
)


PROTOCOL_BASE = 1000
ADD_FRIEND_REQUEST = PROTOCOL_BASE + 5
ADD_FRIEND_RESPONSE = PROTOCOL_BASE + 6
FRIEND_INFO = PROTOCOL_BASE + 10
MESSAGE_ACK = PROTOCOL_BASE + 14
HISTORY_REQUEST = PROTOCOL_BASE + 15
HISTORY_END = PROTOCOL_BASE + 16
FRIEND_RESULT_NOTIFY = PROTOCOL_BASE + 17
FRIEND_RESULT_ACK = PROTOCOL_BASE + 18

FRIEND_PENDING = 4
FRIEND_REFUSED = 2
PASSWORD = "Integration1!"
NAME_LENGTH = 15
CONTENT_LENGTH = 8 * 1024


def text_field(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def packet_type(body: bytes) -> int:
    if len(body) < 4:
        raise ValueError(f"协议体过短: {len(body)}")
    return struct.unpack_from("<i", body)[0]


async def read_matching(
    reader: asyncio.StreamReader,
    predicate: Callable[[bytes], bool],
    timeout: float,
) -> bytes:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise asyncio.TimeoutError("等待目标协议包超时")
        body = await read_frame(reader, remaining)
        if predicate(body):
            return body


async def assert_packet_absent(
    reader: asyncio.StreamReader,
    predicate: Callable[[bytes], bool],
    duration: float,
) -> None:
    deadline = time.monotonic() + duration
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        try:
            body = await read_frame(reader, remaining)
        except asyncio.TimeoutError:
            return
        if predicate(body):
            raise AssertionError("已确认的数据在重新登录后仍被重复投递")


def chat_request(sender_id: int, receiver_id: int, content: str) -> bytes:
    return struct.pack(
        "<iii8192s",
        PROTOCOL_BASE + 7,
        sender_id,
        receiver_id,
        fixed_string(content, CONTENT_LENGTH),
    )


def message_ack(message_id: int) -> bytes:
    return struct.pack("<iQ", MESSAGE_ACK, message_id)


def history_request(friend_id: int) -> bytes:
    return struct.pack("<iiiQ", HISTORY_REQUEST, friend_id, 30, 0)


def friend_request(
    requester_id: int, requester_name: str, target_name: str
) -> bytes:
    # 旧协议结构体使用4字节对齐，两个15字节昵称后有2字节尾部填充。
    return struct.pack(
        "<ii15s15s2x",
        ADD_FRIEND_REQUEST,
        requester_id,
        fixed_string(requester_name, NAME_LENGTH),
        fixed_string(target_name, NAME_LENGTH),
    )


def friend_response(
    result: int,
    requester_id: int,
    target_id: int,
    requester_name: str,
    target_name: str,
) -> bytes:
    return struct.pack(
        "<iiii15s15s2x",
        ADD_FRIEND_RESPONSE,
        result,
        requester_id,
        target_id,
        fixed_string(requester_name, NAME_LENGTH),
        fixed_string(target_name, NAME_LENGTH),
    )


def friend_result_ack(request_id: int) -> bytes:
    return struct.pack("<iQ", FRIEND_RESULT_ACK, request_id)


async def login(
    arguments: argparse.Namespace, account: str
) -> Tuple[asyncio.StreamReader, asyncio.StreamWriter, int]:
    return await open_and_login(
        arguments.host,
        arguments.port,
        account,
        arguments.password,
        arguments.timeout,
    )


async def find_friend_id(
    reader: asyncio.StreamReader, expected_name: str, timeout: float
) -> int:
    def is_expected_friend(body: bytes) -> bool:
        return (
            packet_type(body) == FRIEND_INFO
            and len(body) == 48
            and text_field(body[16:31]) == expected_name
        )

    body = await read_matching(reader, is_expected_friend, timeout)
    return struct.unpack_from("<i", body, 4)[0]


async def verify_message_flow(
    arguments: argparse.Namespace, token: str, steps: Dict[str, bool]
) -> int:
    sender_writer: Optional[asyncio.StreamWriter] = None
    receiver_writer: Optional[asyncio.StreamWriter] = None
    second_receiver_writer: Optional[asyncio.StreamWriter] = None
    try:
        sender_reader, sender_writer, sender_id = await login(
            arguments, "itest_sender"
        )
        receiver_id = await find_friend_id(
            sender_reader, "itest_receiver", arguments.timeout
        )
        steps["sender_login"] = True

        content = f"integration:{token}"
        sender_writer.write(frame(chat_request(sender_id, receiver_id, content)))
        await sender_writer.drain()
        response = await read_matching(
            sender_reader,
            lambda body: packet_type(body) == CHAT_RESPONSE,
            arguments.timeout,
        )
        if len(response) != 12:
            raise ValueError(f"聊天响应长度错误: {len(response)}")
        _, response_friend_id, result = struct.unpack("<iii", response)
        if result != SEND_SUCCESS or response_friend_id != receiver_id:
            raise AssertionError(f"消息持久化失败，结果码: {result}")
        steps["message_persisted"] = True
    finally:
        await close_writer(sender_writer)

    await asyncio.sleep(arguments.settle_delay)
    try:
        receiver_reader, receiver_writer, logged_receiver_id = await login(
            arguments, "itest_receiver"
        )

        def is_current_message(body: bytes) -> bool:
            return (
                packet_type(body) == CHAT_MESSAGE
                and len(body) == 8224
                and text_field(body[32:]) == content
            )

        message = await read_matching(
            receiver_reader, is_current_message, arguments.timeout
        )
        _, message_sender_id, message_receiver_id, source, message_id, _ = (
            struct.unpack_from("<iiiiQq", message)
        )
        if message_receiver_id != logged_receiver_id or source != 1:
            raise AssertionError("离线消息的接收者或来源字段错误")
        steps["offline_message_delivered"] = True

        receiver_writer.write(frame(message_ack(message_id)))
        await receiver_writer.drain()
        steps["message_ack_sent"] = True
    finally:
        await close_writer(receiver_writer)

    await asyncio.sleep(arguments.settle_delay)
    try:
        second_reader, second_receiver_writer, _ = await login(
            arguments, "itest_receiver"
        )
        await assert_packet_absent(
            second_reader,
            lambda body: packet_type(body) == CHAT_MESSAGE
            and len(body) == 8224
            and text_field(body[32:]) == content
            and struct.unpack_from("<i", body, 12)[0] == 1,
            arguments.absence_timeout,
        )
        steps["message_ack_prevents_redelivery"] = True

        second_receiver_writer.write(frame(history_request(message_sender_id)))
        await second_receiver_writer.drain()
        history_message_seen = False
        while True:
            body = await read_frame(second_reader, arguments.timeout)
            current_type = packet_type(body)
            if current_type == CHAT_MESSAGE and len(body) == 8224:
                if text_field(body[32:]) == content:
                    if struct.unpack_from("<i", body, 12)[0] != 2:
                        raise AssertionError("历史消息来源字段错误")
                    history_message_seen = True
            elif current_type == HISTORY_END:
                if len(body) != 24:
                    raise ValueError(f"历史结束包长度错误: {len(body)}")
                break
        if not history_message_seen:
            raise AssertionError("历史分页中没有找到本轮消息")
        steps["history_query"] = True
        return message_id
    finally:
        await close_writer(second_receiver_writer)


async def verify_friend_flow(
    arguments: argparse.Namespace, steps: Dict[str, bool]
) -> int:
    requester_writer: Optional[asyncio.StreamWriter] = None
    target_writer: Optional[asyncio.StreamWriter] = None
    result_writer: Optional[asyncio.StreamWriter] = None
    verify_writer: Optional[asyncio.StreamWriter] = None
    try:
        requester_reader, requester_writer, requester_id = await login(
            arguments, "itest_request"
        )
        requester_writer.write(
            frame(friend_request(requester_id, "itest_request", "itest_target"))
        )
        await requester_writer.drain()
        response = await read_matching(
            requester_reader,
            lambda body: packet_type(body) == ADD_FRIEND_RESPONSE,
            arguments.timeout,
        )
        if len(response) != 48:
            raise ValueError(f"好友申请响应长度错误: {len(response)}")
        _, result, response_requester_id, target_id = struct.unpack_from(
            "<iiii", response
        )
        if result != FRIEND_PENDING or response_requester_id != requester_id:
            raise AssertionError(f"好友申请未进入待处理状态，结果码: {result}")
        steps["offline_friend_request_saved"] = True
    finally:
        await close_writer(requester_writer)

    await asyncio.sleep(arguments.settle_delay)
    try:
        target_reader, target_writer, logged_target_id = await login(
            arguments, "itest_target"
        )
        request = await read_matching(
            target_reader,
            lambda body: packet_type(body) == ADD_FRIEND_REQUEST,
            arguments.timeout,
        )
        if len(request) != 40:
            raise ValueError(f"好友申请包长度错误: {len(request)}")
        _, delivered_requester_id, raw_requester_name, raw_target_name = (
            struct.unpack("<ii15s15s2x", request)
        )
        requester_name = text_field(raw_requester_name)
        target_name = text_field(raw_target_name)
        if (
            delivered_requester_id != requester_id
            or logged_target_id != target_id
            or requester_name != "itest_request"
            or target_name != "itest_target"
        ):
            raise AssertionError("补发的好友申请身份字段错误")
        steps["offline_friend_request_delivered"] = True

        target_writer.write(
            frame(
                friend_response(
                    FRIEND_REFUSED,
                    requester_id,
                    target_id,
                    requester_name,
                    target_name,
                )
            )
        )
        await target_writer.drain()
        steps["friend_request_handled"] = True
    finally:
        await close_writer(target_writer)

    await asyncio.sleep(arguments.settle_delay)
    try:
        result_reader, result_writer, logged_requester_id = await login(
            arguments, "itest_request"
        )
        notification = await read_matching(
            result_reader,
            lambda body: packet_type(body) == FRIEND_RESULT_NOTIFY,
            arguments.timeout,
        )
        if len(notification) != 62:
            raise ValueError(f"好友结果通知长度错误: {len(notification)}")
        (
            _,
            request_id,
            result,
            notified_requester_id,
            notified_target_id,
            handled_at,
            raw_requester_name,
            raw_target_name,
        ) = struct.unpack("<iQiiiq15s15s", notification)
        if (
            request_id <= 0
            or result != FRIEND_REFUSED
            or notified_requester_id != logged_requester_id
            or notified_target_id != target_id
            or handled_at <= 0
            or text_field(raw_requester_name) != "itest_request"
            or text_field(raw_target_name) != "itest_target"
        ):
            raise AssertionError("好友申请结果通知字段错误")
        steps["friend_result_delivered"] = True

        result_writer.write(frame(friend_result_ack(request_id)))
        await result_writer.drain()
        steps["friend_result_ack_sent"] = True
    finally:
        await close_writer(result_writer)

    await asyncio.sleep(arguments.settle_delay)
    try:
        verify_reader, verify_writer, _ = await login(arguments, "itest_request")
        await assert_packet_absent(
            verify_reader,
            lambda body: packet_type(body) == FRIEND_RESULT_NOTIFY
            and len(body) == 62
            and struct.unpack_from("<Q", body, 4)[0] == request_id,
            arguments.absence_timeout,
        )
        steps["friend_result_ack_prevents_redelivery"] = True
        return request_id
    finally:
        await close_writer(verify_writer)


async def failed_login_result(arguments: argparse.Namespace) -> int:
    writer: Optional[asyncio.StreamWriter] = None
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(arguments.host, arguments.port),
            timeout=arguments.timeout,
        )
        writer.write(frame(login_body("itest_missing", "WrongPassword!")))
        await writer.drain()
        response = await read_matching(
            reader,
            lambda body: packet_type(body) == LOGIN_RESPONSE,
            arguments.timeout,
        )
        if len(response) != 12:
            raise ValueError(f"失败登录响应长度错误: {len(response)}")
        return struct.unpack("<iii", response)[2]
    finally:
        await close_writer(writer)


async def verify_login_rate_limit(
    arguments: argparse.Namespace, steps: Dict[str, bool]
) -> None:
    # 前五次失败用于触发封禁，第六次应在查询数据库前被统一拒绝。
    for _ in range(5):
        result = await failed_login_result(arguments)
        if result not in (1, 2):
            raise AssertionError(f"失败登录返回了意外结果码: {result}")
    if await failed_login_result(arguments) != 2:
        raise AssertionError("达到失败阈值后没有返回限流结果")
    steps["login_rate_limit"] = True


async def run(arguments: argparse.Namespace) -> dict:
    started_at = time.monotonic()
    token = uuid.uuid4().hex[:8]
    steps: Dict[str, bool] = {}
    report = {
        "passed": False,
        "host": arguments.host,
        "port": arguments.port,
        "run_token": token,
        "steps": steps,
        "message_id": 0,
        "friend_request_id": 0,
        "error": "",
    }
    try:
        report["message_id"] = await verify_message_flow(arguments, token, steps)
        report["friend_request_id"] = await verify_friend_flow(arguments, steps)
        await verify_login_rate_limit(arguments, steps)
        report["passed"] = True
    except Exception as error:  # 测试报告需要保留失败类型和原因。
        report["error"] = f"{type(error).__name__}: {error}"
    report["duration_seconds"] = round(time.monotonic() - started_at, 3)
    return report


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="IM服务端核心业务自动化集成测试")
    parser.add_argument("--host", default="127.0.0.1", help="服务端地址")
    parser.add_argument("--port", type=int, default=56789, help="服务端端口")
    parser.add_argument("--password", default=PASSWORD, help="专用测试账号密码")
    parser.add_argument("--timeout", type=float, default=10.0, help="协议步骤超时秒数")
    parser.add_argument(
        "--absence-timeout",
        type=float,
        default=1.0,
        help="确认消息不再重投递的观察秒数",
    )
    parser.add_argument(
        "--settle-delay",
        type=float,
        default=0.3,
        help="断线后等待服务端顺序任务完成的秒数",
    )
    parser.add_argument(
        "--output", default="integration-report.json", help="JSON报告路径"
    )
    arguments = parser.parse_args()
    if arguments.timeout <= 0 or arguments.absence_timeout <= 0:
        parser.error("超时时间必须大于0")
    if arguments.settle_delay < 0:
        parser.error("等待时间不能小于0")
    fixed_string(arguments.password, NAME_LENGTH)
    return arguments


def main() -> None:
    arguments = parse_arguments()
    try:
        report = asyncio.run(run(arguments))
    except KeyboardInterrupt:
        print("集成测试已由用户中止")
        raise SystemExit(130)
    report_text = json.dumps(report, ensure_ascii=False, indent=2)
    print(report_text)
    Path(arguments.output).write_text(report_text + "\n", encoding="utf-8")
    print(f"报告已写入: {arguments.output}")
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()

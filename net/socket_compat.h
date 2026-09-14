#pragma once

// Linux 服务端统一使用文件描述符表示 socket。
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;

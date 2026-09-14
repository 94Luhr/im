#include "CKernel.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace {
// 信号处理函数只修改原子标志，主线程据此完成网络资源的有序回收。
std::atomic_bool g_running{true};
void stop(int) { g_running = false; }
}

int main() {
    // 接收 Ctrl+C 或 systemd 的终止信号时停止主循环。
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    CKernel kernel;
    if (!kernel.startServer()) return 1;
    while (g_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    kernel.endSrever();
    return 0;
}

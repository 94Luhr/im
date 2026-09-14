#pragma once

#include <atomic>
#include <cstdint>

// 全局指标均为原子计数，采集过程不会阻塞 Reactor 或业务线程。
struct ServerMetrics
{
    std::atomic<uint64_t> activeConnections{0};
    std::atomic<uint64_t> acceptedConnections{0};
    std::atomic<uint64_t> closedConnections{0};
    std::atomic<uint64_t> receivedPackets{0};
    std::atomic<uint64_t> sentPackets{0};
    std::atomic<uint64_t> receivedBytes{0};
    std::atomic<uint64_t> sentBytes{0};
    std::atomic<uint64_t> pendingJobs{0};
    std::atomic<uint64_t> rejectedJobs{0};
    std::atomic<uint64_t> rateLimitedLogins{0};
    std::atomic<uint64_t> databasePoolSize{0};
    std::atomic<uint64_t> databaseBusyConnections{0};
    std::atomic<uint64_t> databaseOperations{0};
    std::atomic<uint64_t> databaseFailures{0};
    std::atomic<uint64_t> databaseAcquireTimeouts{0};

    static ServerMetrics& instance()
    {
        static ServerMetrics metrics;
        return metrics;
    }
};
